// The Generate palette: prompt, size, sampling, LoRAs and reference layers,
// and the three things that can be asked of an image model -- make a new
// layer from words, fill a selection, or edit the layer by instruction.
//
// Everything the panel offers is read from the server's own capabilities
// rather than hardcoded. A build of stable-diffusion.cpp ships whatever
// samplers and schedulers it was compiled with, the LoRA list is whatever is
// in its folder, and the size limits and the ability to take a mask or
// reference images follow the loaded model. A list written out here would be
// wrong the first time either side was updated.
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <future>
#include <string>

#include "App.h"
#include "BackgroundJob.h"
#include "GenerateBackend.h"
#include "firn/generate.h"
#include "imgui.h"
#include "ui/GenerateState.h"

using namespace firn;

namespace {

// Sizes go to the model in multiples of 64: every model here has a latent
// grid, and a request that does not line up with it is either refused or
// quietly rounded somewhere the user cannot see.
int snap_size(int v, const genhttp::Capabilities& c, bool horizontal) {
    const int lo = (horizontal ? c.min_width : c.min_height) > 0 ? (horizontal ? c.min_width : c.min_height) : 64;
    const int hi = (horizontal ? c.max_width : c.max_height) > 0 ? (horizontal ? c.max_width : c.max_height) : 4096;
    v = (v + 32) / 64 * 64;
    return std::clamp(v, (lo + 63) / 64 * 64, hi / 64 * 64);
}

// The parameters the panel adds to whatever the request already carries.
json::Value panel_params(const GenerateState& g) {
    json::Value p = json::Value::object();
    if (g.negative[0]) p.set("negative_prompt", json::Value::string(g.negative));
    if (g.seed >= 0) p.set("seed", json::Value::number(static_cast<double>(g.seed)));
    if (g.batch > 1) p.set("batch_count", json::Value::number(g.batch));

    json::Value sample = json::Value::object();
    if (g.steps > 0) sample.set("sample_steps", json::Value::number(g.steps));
    if (g.sampler >= 0 && g.sampler < static_cast<int>(g.caps.samplers.size()))
        sample.set("sample_method", json::Value::string(g.caps.samplers[static_cast<size_t>(g.sampler)]));
    if (g.scheduler >= 0 && g.scheduler < static_cast<int>(g.caps.schedulers.size()))
        sample.set("scheduler", json::Value::string(g.caps.schedulers[static_cast<size_t>(g.scheduler)]));
    if (g.cfg > 0.0f) {
        json::Value guidance = json::Value::object();
        guidance.set("txt_cfg", json::Value::number(g.cfg));
        sample.set("guidance", std::move(guidance));
    }
    if (!sample.obj.empty()) p.set("sample_params", std::move(sample));

    // Only the LoRAs actually turned up are sent, so the list can stay on
    // screen without every entry being applied.
    json::Value loras = json::Value::array();
    for (size_t i = 0; i < g.caps.loras.size() && i < g.lora_strength.size(); ++i) {
        if (g.lora_strength[i] == 0.0f) continue;
        json::Value e = json::Value::object();
        e.set("name", json::Value::string(g.caps.loras[i]));
        e.set("strength", json::Value::number(g.lora_strength[i]));
        loras.push(std::move(e));
    }
    if (loras.size() > 0) p.set("lora", std::move(loras));
    return p;
}

// Asking the server what it is takes a round trip through curl, which must
// not happen on the interface thread: a wrong address waits for a timeout.
void refresh_capabilities(App& app, GenerateState& g) {
    if (g.asking || app.config.generate_url.empty()) return;
    g.asking = true;
    g.asked_url = app.config.generate_url;
    const std::string url = g.asked_url;
    g.ask = std::async(std::launch::async, [url]() {
        genhttp::Capabilities c;
        std::string err;
        const bool ok = genhttp::capabilities(url, &c, &err);
        return std::pair<genhttp::Capabilities, std::string>{std::move(c), ok ? std::string() : err};
    });
}

void collect_capabilities(App& app, GenerateState& g) {
    if (!g.asking || !g.ask.valid()) return;
    if (g.ask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto [caps, err] = g.ask.get();
    g.asking = false;
    g.caps_error = err;
    g.have_caps = err.empty();
    if (!g.have_caps) return;
    g.caps = std::move(caps);
    g.lora_strength.assign(g.caps.loras.size(), 0.0f);
    // The server's own defaults are a better starting point than anything
    // written here: they follow whichever model is loaded.
    if (g.steps == 0) g.steps = g.caps.steps;
    if (g.cfg == 0.0f) g.cfg = g.caps.txt_cfg;
    if (app.doc) {
        g.width = snap_size(app.doc->width(), g.caps, true);
        g.height = snap_size(app.doc->height(), g.caps, false);
    } else if (g.caps.width > 0) {
        g.width = g.caps.width;
        g.height = g.caps.height;
    }
}

}  // namespace

void draw_generate_panel(App& app) {
    if (!ImGui::Begin("Generate")) { ImGui::End(); return; }
    app.config.right_palette = "Generate";
    GenerateState& g = *app.generate_state;

    // A changed address means the answers on screen belong to a different
    // server, so they are asked for again.
    if (!app.config.generate_url.empty() && app.config.generate_url != g.asked_url && !g.asking) refresh_capabilities(app, g);
    collect_capabilities(app, g);

    if (app.config.generate_url.empty()) {
        ImGui::TextDisabled("No image model.");
        ImGui::TextWrapped("Set a server address in File > Preferences to generate, fill and edit with a model.");
        ImGui::End();
        return;
    }
    // The panel docks into a narrow column, so everything here has to read
    // at that width: names wrap rather than running under the next control.
    if (g.asking) ImGui::TextDisabled("Asking...");
    else if (!g.have_caps) ImGui::TextWrapped("%s", g.caps_error.empty() ? "Not connected." : g.caps_error.c_str());
    else ImGui::TextWrapped("%s", g.caps.model.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", g.caps.model.c_str(), app.config.generate_url.c_str());
    ImGui::BeginDisabled(g.asking);
    if (ImGui::SmallButton("Refresh")) { g.asked_url.clear(); refresh_capabilities(app, g); }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Prompt");
    ImGui::InputTextMultiline("##prompt", g.prompt, sizeof g.prompt,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f));
    const bool ctrl_enter = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                            ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false);

    const bool busy = app.job != nullptr;
    const bool has_layer = app.doc && app.active_is_raster();
    const bool has_selection = app.doc && app.doc->has_selection() && app.doc->selection().any();

    ImGui::Separator();
    const bool have_prompt = g.prompt[0] != 0;
    ImGui::BeginDisabled(busy || !have_prompt || !app.doc);
    const bool make = ImGui::Button("Generate") || (ctrl_enter && have_prompt && !busy);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A new layer from the prompt alone.");
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < 110) ImGui::NewLine();
    ImGui::BeginDisabled(busy || !has_layer || !has_selection || !g.caps.takes_mask);
    const bool fill = ImGui::Button("Fill");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(g.caps.takes_mask ? "Replace the selection, leaving the rest of the picture alone."
                                            : "This model does not take a mask.");
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < 90) ImGui::NewLine();
    ImGui::BeginDisabled(busy || !has_layer || !have_prompt || !g.caps.takes_refs);
    const bool edit = ImGui::Button("Edit layer");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(g.caps.takes_refs ? "Change the whole layer by instruction."
                                            : "This model does not take a reference image.");

    if (busy) { ImGui::SameLine(); ImGui::TextDisabled("%s...", app.job->name.c_str()); }

    // Everything below is settings, and it scrolls: expanding the LoRA or
    // reference list must never push the buttons out of reach, which is what
    // happens to a panel that simply grows downwards in a short dock.
    ImGui::Separator();
    ImGui::BeginChild("##settings", ImVec2(0, 0), false);

    // Until it is set by hand, the size follows the image: a panel offering
    // 512x512 over a 4000 pixel photograph is offering the wrong thing.
    if (!g.size_touched && app.doc) {
        g.width = snap_size(app.doc->width(), g.caps, true);
        g.height = snap_size(app.doc->height(), g.caps, false);
    }

    // The numbers go in a two-column table so their labels keep their room
    // however narrow the dock is, rather than sliding under the next field.
    if (ImGui::BeginTable("##numbers", 2, ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* label) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        row("Size");
        const float half = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
        ImGui::SetNextItemWidth(half);
        if (ImGui::InputInt("##w", &g.width, 0)) { g.width = snap_size(g.width, g.caps, true); g.size_touched = true; }
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(half);
        if (ImGui::InputInt("##h", &g.height, 0)) { g.height = snap_size(g.height, g.caps, false); g.size_touched = true; }

        row("Steps");
        ImGui::DragInt("##steps", &g.steps, 0.2f, 1, 100);
        row("Guidance");
        ImGui::DragFloat("##cfg", &g.cfg, 0.05f, 1.0f, 20.0f, "%.1f");

        // A seed of -1 is a fresh picture every time; pinning one is how a
        // prompt is refined without the composition moving underneath it.
        row("Seed");
        if (g.seed < 0) {
            if (ImGui::Button("Random each time", ImVec2(-FLT_MIN, 0))) g.seed = 0;
        } else {
            int seed = static_cast<int>(g.seed);
            const float btn = ImGui::CalcTextSize("Unpin").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - btn - 6.0f));
            if (ImGui::InputInt("##seed", &seed, 0)) g.seed = std::max(0, seed);
            ImGui::SameLine(0, 6);
            if (ImGui::Button("Unpin")) g.seed = -1;
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!app.doc);
    if (ImGui::SmallButton("Match image") && app.doc) {
        g.width = snap_size(app.doc->width(), g.caps, true);
        g.height = snap_size(app.doc->height(), g.caps, false);
        g.size_touched = false;
    }
    ImGui::EndDisabled();

    if (ImGui::TreeNode("More")) {
        ImGui::TextUnformatted("Negative prompt");
        ImGui::InputTextMultiline("##negative", g.negative, sizeof g.negative,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 2.0f));
        auto combo = [](const char* label, int* index, const std::vector<std::string>& items) {
            if (items.empty()) return;
            const char* current = *index >= 0 && *index < static_cast<int>(items.size())
                                      ? items[static_cast<size_t>(*index)].c_str() : "(server default)";
            ImGui::SetNextItemWidth(160);
            if (!ImGui::BeginCombo(label, current)) return;
            if (ImGui::Selectable("(server default)", *index < 0)) *index = -1;
            for (size_t i = 0; i < items.size(); ++i)
                if (ImGui::Selectable(items[i].c_str(), *index == static_cast<int>(i))) *index = static_cast<int>(i);
            ImGui::EndCombo();
        };
        combo("Sampler", &g.sampler, g.caps.samplers);
        combo("Scheduler", &g.scheduler, g.caps.schedulers);
        if (g.caps.max_batch > 1) {
            ImGui::SetNextItemWidth(70);
            ImGui::DragInt("At once", &g.batch, 0.1f, 1, g.caps.max_batch);
        }
        ImGui::TreePop();
    }

    if (g.caps.takes_lora && !g.caps.loras.empty()) {
        if (ImGui::TreeNode("LoRAs")) {
            g.lora_strength.resize(g.caps.loras.size(), 0.0f);
            for (size_t i = 0; i < g.caps.loras.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                bool on = g.lora_strength[i] != 0.0f;
                if (ImGui::Checkbox("##on", &on)) g.lora_strength[i] = on ? 1.0f : 0.0f;
                ImGui::SameLine();
                ImGui::BeginDisabled(!on);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##strength", &g.lora_strength[i], 0.05f, 2.0f,
                                   (g.caps.loras[i] + "  %.2f").c_str());
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    // Further pictures for the model to look at. The layer being edited is
    // always the first one it sees, so these are numbered from two, which is
    // how the instruction refers to them.
    if (g.caps.takes_refs && app.doc && app.doc->layer_count() > 1) {
        if (ImGui::TreeNode("Reference layers")) {
            ImGui::TextDisabled("Sent alongside the layer being edited.");
            for (int i = static_cast<int>(app.doc->layer_count()) - 1; i >= 0; --i) {
                const Layer& L = app.doc->layer(static_cast<size_t>(i));
                if (!L.is_raster() || i == app.active_layer()) continue;
                const auto at = std::find(g.refs.begin(), g.refs.end(), i);
                bool on = at != g.refs.end();
                ImGui::PushID(i);
                char label[300];
                const size_t order = on ? static_cast<size_t>(at - g.refs.begin()) + 2 : 0;
                if (on) std::snprintf(label, sizeof label, "%zu. %s", order, L.name.c_str());
                else std::snprintf(label, sizeof label, "%s", L.name.c_str());
                if (ImGui::Checkbox(label, &on)) {
                    if (on) g.refs.push_back(i);
                    else g.refs.erase(std::remove(g.refs.begin(), g.refs.end(), i), g.refs.end());
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    ImGui::EndChild();

    if (make || fill || edit) {
        const json::Value extra = panel_params(g);
        if (make) app.generate_image(g.prompt, g.width, g.height, true, &extra);
        else if (fill) app.generative_fill(g.prompt, true, &extra);
        else app.generative_edit(g.prompt, true, g.refs, &extra);
    }
    ImGui::End();
}
