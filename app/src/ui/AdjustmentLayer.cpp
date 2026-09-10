// Adjustment layers: creation from the Layers menu and the properties
// dialog that edits one live. The layer's parameters are changed in place
// (the composite follows); OK records one SetAdjustmentCommand.
#include <algorithm>
#include <cstdio>
#include <memory>

#include "App.h"
#include "firn/commands.h"
#include "imgui.h"

using namespace firn;

bool curve_editor(App& app, ImVec2 size);   // Adjust.cpp

void App::layer_new_adjustment(Adjustment::Kind kind) {
    if (!doc) return;
    Adjustment a;
    a.kind = kind;
    const std::string name = Adjustment::kind_name(kind);
    int n = 1;
    for (size_t i = 0; i < doc->layer_count(); ++i) if (doc->layer(i).is_adjustment() && doc->layer(i).adjustment.kind == kind) ++n;
    auto cmd = std::make_unique<AddAdjustmentLayerCommand>(name + (n > 1 ? " " + std::to_string(n) : ""), a);
    AddAdjustmentLayerCommand* raw = cmd.get();
    run(std::move(cmd));
    open_adjustment_dialog(static_cast<int>(raw->index()), true);
}

void App::open_adjustment_dialog(int layer, bool created) {
    if (!doc || layer < 0 || layer >= static_cast<int>(doc->layer_count()) || !doc->layer(layer).is_adjustment()) return;
    adj_layer_index = layer;
    adj_layer_created = created;
    adj_before = doc->layer(layer).adjustment;
    adj_name_before = doc->layer(layer).name;
    curve_points = doc->layer(layer).adjustment.curves[0];
    show_adjust_layer_dialog = true;
}

namespace {

bool slider_int(const char* label, int& v, int lo, int hi, float width = 220) {
    ImGui::SetNextItemWidth(width);
    return ImGui::SliderInt(label, &v, lo, hi);
}

bool adjustment_body(App& app, Adjustment& a) {
    bool changed = false;
    switch (a.kind) {
        case Adjustment::Kind::BrightnessContrast:
            changed |= slider_int("Brightness", a.brightness, -255, 255);
            changed |= slider_int("Contrast", a.contrast, -100, 100);
            break;
        case Adjustment::Kind::Levels: {
            static int channel = 0;
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("Channel", &channel, "RGB\0Red\0Green\0Blue\0");
            Adjustment::Levels& l = a.levels[std::clamp(channel, 0, 3)];
            changed |= slider_int("Input low", l.in_low, 0, 254);
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Gamma", &l.gamma, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            changed |= slider_int("Input high", l.in_high, 1, 255);
            changed |= slider_int("Output low", l.out_low, 0, 255);
            changed |= slider_int("Output high", l.out_high, 0, 255);
            if (l.in_high <= l.in_low) l.in_high = l.in_low + 1;
            break;
        }
        case Adjustment::Kind::Curves: {
            static int channel = 0;
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("Channel", &channel, "RGB\0Red\0Green\0Blue\0")) app.curve_points = a.curves[std::clamp(channel, 0, 3)];
            if (curve_editor(app, ImVec2(256, 256))) { a.curves[std::clamp(channel, 0, 3)] = app.curve_points; changed = true; }
            if (ImGui::SmallButton("Reset channel")) { a.curves[std::clamp(channel, 0, 3)] = {{0, 0}, {255, 255}}; app.curve_points = a.curves[std::clamp(channel, 0, 3)]; changed = true; }
            break;
        }
        case Adjustment::Kind::ColorBalance: {
            static int range = 1;
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("Tonal range", &range, "Shadows\0Midtones\0Highlights\0");
            int* v = range == 0 ? a.color_balance.shadows : range == 2 ? a.color_balance.highlights : a.color_balance.midtones;
            changed |= slider_int("Cyan / Red", v[0], -100, 100);
            changed |= slider_int("Magenta / Green", v[1], -100, 100);
            changed |= slider_int("Yellow / Blue", v[2], -100, 100);
            changed |= ImGui::Checkbox("Preserve luminosity", &a.color_balance.preserve_luminosity);
            break;
        }
        case Adjustment::Kind::HSL:
            changed |= ImGui::Checkbox("Colorize", &a.colorize);
            if (a.colorize) {
                changed |= slider_int("Hue", a.colorize_hue, 0, 359);
                changed |= slider_int("Saturation", a.colorize_saturation, 0, 100);
            } else {
                changed |= slider_int("Hue", a.hue, -180, 180);
                changed |= slider_int("Saturation", a.saturation, -100, 100);
                changed |= slider_int("Lightness", a.lightness, -100, 100);
            }
            break;
        case Adjustment::Kind::ChannelMixer: {
            static int out = 0;
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("Output channel", &out, "Red\0Green\0Blue\0");
            static const char* names[] = {"Red %", "Green %", "Blue %"};
            for (int i = 0; i < 3; ++i) { ImGui::SetNextItemWidth(220); changed |= ImGui::SliderFloat(names[i], &a.mixer.mix[out][i], -200.0f, 200.0f, "%.0f"); }
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Constant", &a.mixer.constant[out], -200.0f, 200.0f, "%.0f");
            changed |= ImGui::Checkbox("Monochrome", &a.mixer.monochrome);
            break;
        }
        case Adjustment::Kind::Invert: ImGui::TextDisabled("Inverts the colors of everything below this layer."); break;
        case Adjustment::Kind::Threshold: changed |= slider_int("Threshold", a.threshold, 1, 255); break;
        case Adjustment::Kind::Posterize: changed |= slider_int("Levels", a.posterize, 2, 255); break;
        case Adjustment::Kind::GaussianBlur:
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Radius", &a.blur_radius, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            ImGui::TextDisabled("Blurs everything below this layer, live.");
            break;
        case Adjustment::Kind::Average:
            changed |= slider_int("Radius", a.average_radius, 1, 30);
            break;
        case Adjustment::Kind::UnsharpMask:
            ImGui::SetNextItemWidth(220);
            changed |= ImGui::SliderFloat("Radius", &a.unsharp_radius, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            changed |= slider_int("Strength", a.unsharp_strength, 1, 500);
            changed |= slider_int("Clipping", a.unsharp_clipping, 0, 100);
            break;
        default: break;
    }
    return changed;
}

}  // namespace

void App::draw_adjustment_layer_dialog() {
    if (show_adjust_layer_dialog) { ImGui::OpenPopup("Adjustment Layer"); show_adjust_layer_dialog = false; }
    if (!ImGui::BeginPopupModal("Adjustment Layer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!doc || adj_layer_index < 0 || adj_layer_index >= static_cast<int>(doc->layer_count()) || !doc->layer(adj_layer_index).is_adjustment()) {
        ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return;
    }
    Layer& L = doc->layer(adj_layer_index);
    ImGui::TextUnformatted(Adjustment::kind_name(L.adjustment.kind));
    char name[256];
    std::snprintf(name, sizeof(name), "%s", L.name.c_str());
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("Name", name, sizeof(name))) L.name = name;
    float op = L.opacity * 100.0f;
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) { L.opacity = op / 100.0f; doc->touch(); }
    ImGui::Separator();
    if (adjustment_body(*this, L.adjustment)) doc->touch();
    ImGui::Separator();
    const bool ok = ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok) {
        const Adjustment after = L.adjustment;
        const std::string new_name = L.name;
        if (new_name != adj_name_before) {
            LayerProps before = doc->props(adj_layer_index), props = before;
            L.name = adj_name_before;
            before.name = adj_name_before; props.name = new_name;
            run(std::make_unique<LayerPropertiesCommand>(adj_layer_index, before, props));
        }
        if (!(after == adj_before)) {
            L.adjustment = adj_before;
            run(std::make_unique<SetAdjustmentCommand>(adj_layer_index, adj_before, after));
        }
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        L.adjustment = adj_before;
        L.name = adj_name_before;
        doc->touch();
        if (adj_layer_created) undo();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
