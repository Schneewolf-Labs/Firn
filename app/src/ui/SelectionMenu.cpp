// The Selections menu and its dialogs: matting, the Modify submenu,
// selection edit mode, promote/float/defloat, and selections from masks
// and vector objects.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "App.h"
#include "ui/MenuBuilder.h"
#include "ui/SelectionMenuState.h"
#include "firn/commands.h"
#include "firn/mask.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

using namespace firn;

namespace {

// Ids for show_sel_dialog.
enum SelDialog { None = 0, Expand, Contract, Feather, InsideOutsideFeather, SpecksHoles, ColorRange, Similar, ShapeAntialias, Smooth, Defringe, Count };
const char* const kTitles[Count] = {nullptr, "Expand Selection", "Contract Selection", "Feather Selection", "Inside/Outside Feather", "Remove Specks and Holes",
                                    "Select Color Range", "Select Similar", "Shape-based Anti-alias", "Smooth Selection", "Defringe"};

const Image& active_pixels(const App& app) { return app.doc->layer(app.active_layer()).pixels; }

}  // namespace

// --- Actions ------------------------------------------------------------

void App::select_from_mask() {
    if (!doc || active_layer() < 0) return;
    const Layer& L = doc->layer(active_layer());
    if (L.has_mask()) { set_selection("Selection From Mask", L.mask); return; }
    // No mask: the layer's transparency plays the part, as the original does.
    Mask m(doc->width(), doc->height(), 0);
    for (size_t i = 0; i < m.size(); ++i) m.data()[i] = L.pixels.data()[i * 4 + 3];
    set_selection("Selection From Mask", std::move(m));
}

void App::select_from_vector() {
    if (!doc || active_layer() < 0 || !doc->layer(active_layer()).is_vector()) return;
    std::vector<vec::Object> chosen;
    for (const vec::Object& o : doc->layer(active_layer()).objects) if (o.selected && !o.is_group) chosen.push_back(o);
    if (chosen.empty()) { status = "Select one or more vector objects first."; return; }
    Image cov(doc->width(), doc->height(), Color{0, 0, 0, 0});
    for (vec::Object& o : chosen) {
        // Coverage only: paint the object solid white with its own outline.
        o.fill.kind = vec::PaintStyle::Kind::Solid; o.fill.color = {255, 255, 255, 255}; o.fill.texture.reset();
        if (o.stroke.enabled()) { o.stroke.kind = vec::PaintStyle::Kind::Solid; o.stroke.color = {255, 255, 255, 255}; o.stroke.texture.reset(); }
        o.visible = true;
    }
    vec::rasterize(chosen, cov);
    Mask m(doc->width(), doc->height(), 0);
    for (size_t i = 0; i < m.size(); ++i) m.data()[i] = cov.data()[i * 4 + 3];
    set_selection("Selection From Vector Object", std::move(m));
}

void App::promote_selection_to_layer(bool floating) {
    if (!doc || active_layer() < 0 || !doc->layer(active_layer()).is_raster() || !doc->has_selection()) return;
    const Layer& L = doc->layer(active_layer());
    Image px(doc->width(), doc->height(), Color{0, 0, 0, 0});
    const Mask& sel = doc->selection();
    for (size_t i = 0; i < sel.size(); ++i) {
        const int a = L.pixels.data()[i * 4 + 3] * sel.data()[i] / 255;
        if (!a) continue;
        for (int c = 0; c < 3; ++c) px.data()[i * 4 + c] = L.pixels.data()[i * 4 + c];
        px.data()[i * 4 + 3] = static_cast<uint8_t>(a);
    }
    run(std::make_unique<PasteLayerCommand>(floating ? "Floating Selection" : "Promoted Selection", std::move(px), floating));
}

bool App::has_floating_layer() const {
    if (!doc) return false;
    for (size_t i = 0; i < doc->layer_count(); ++i) if (doc->layer(i).floating && doc->layer(i).is_raster()) return true;
    return false;
}

// Defloat merges the floating selection back into the layer under it.
void App::defloat() {
    if (!doc) return;
    for (size_t i = doc->layer_count(); i-- > 0;) {
        if (!doc->layer(i).floating || !doc->layer(i).is_raster() || i == 0) continue;
        if (!doc->layer(i - 1).is_raster()) { status = "Defloat: the layer under the floating selection is not a raster layer."; return; }
        doc->set_active_layer(static_cast<int>(i));
        run(std::make_unique<MergeLayersCommand>(MergeLayersCommand::Kind::Down, static_cast<int>(i)));
        return;
    }
}

// --- Menu ------------------------------------------------------------

void App::draw_selections_menu(MenuBuilder& m) {
    if (!m.begin_menu("Selections")) return;
    const bool has_doc = doc != nullptr;
    const bool has_sel = has_doc && doc->has_selection();
    const int layer = active_layer();
    const bool raster = has_doc && layer >= 0 && doc->layer(layer).is_raster();
    m.item("Select All", "Ctrl+A", has_doc, [&] { select_all(); });
    m.item("Select None", "Ctrl+D", has_sel, [&] { select_none(); });
    m.item("From Mask", nullptr, has_doc && layer >= 0 && (doc->layer(layer).has_mask() || raster), [&] { select_from_mask(); });
    m.item("From Vector Object", nullptr, has_doc && layer >= 0 && doc->layer(layer).is_vector(), [&] { select_from_vector(); });
    m.item("Invert", "Ctrl+Shift+I", has_doc, [&] { select_invert(); });
    m.separator();
    if (m.begin_menu("Matting", raster)) {
        auto matte = [&](const char* name, Color c) {
            run(std::make_unique<AdjustCommand>(static_cast<size_t>(layer), name, [c](Image& img) { raster::remove_matte(img, c); }));
        };
        m.item("Remove Black Matte", nullptr, true, [&] { matte("Remove Black Matte", {0, 0, 0, 255}); });
        m.item("Remove White Matte", nullptr, true, [&] { matte("Remove White Matte", {255, 255, 255, 255}); });
        m.item("Defringe...", nullptr, true, [&] { show_sel_dialog = Defringe; });
        m.end_menu();
    }
    if (m.begin_menu("Modify", has_sel)) {
        m.item("Expand...", nullptr, true, [&] { show_sel_dialog = Expand; });
        m.item("Contract...", nullptr, true, [&] { show_sel_dialog = Contract; });
        m.item("Feather...", nullptr, true, [&] { show_sel_dialog = Feather; });
        m.item("Inside/Outside Feather...", nullptr, true, [&] { show_sel_dialog = InsideOutsideFeather; });
        m.item("Unfeather", nullptr, true, [&] { Mask msk = doc->selection(); mask::unfeather(msk); set_selection("Unfeather", std::move(msk)); });
        m.separator();
        m.item("Remove Specks and Holes...", nullptr, true, [&] { show_sel_dialog = SpecksHoles; });
        m.item("Select Color Range...", nullptr, raster, [&] { show_sel_dialog = ColorRange; });
        m.item("Select Similar...", nullptr, raster, [&] { show_sel_dialog = Similar; });
        m.item("Shape-based Anti-alias...", nullptr, true, [&] { show_sel_dialog = ShapeAntialias; });
        m.item("Smooth...", nullptr, true, [&] { show_sel_dialog = Smooth; });
        m.end_menu();
    }
    m.separator();
    m.item("Hide Marquee", "Ctrl+Shift+M", has_doc, [&] { show_marquee = !show_marquee; }, !show_marquee);
    m.item("Edit Selection", nullptr, has_doc, [&] { set_selection_edit(!selection_edit); }, selection_edit);
    m.separator();
    m.item("Promote Selection to Layer", nullptr, raster && has_sel, [&] { promote_selection_to_layer(false); });
    m.item("Float", "Ctrl+F", raster && has_sel && !has_floating_layer(), [&] { promote_selection_to_layer(true); });
    m.item("Defloat", "Ctrl+Shift+F", has_floating_layer(), [&] { defloat(); });
    m.separator();
    if (m.begin_menu("Load/Save Selection", has_doc)) {
        if (m.begin_menu("Load Selection From Alpha Channel", !doc->alpha_channels().empty())) {
            for (size_t i = 0; i < doc->alpha_channels().size(); ++i) {
                m.push_id(static_cast<int>(i));
                m.item(doc->alpha_channels()[i].name.c_str(), nullptr, true, [this, i] { set_selection("Load Selection From Alpha Channel", doc->alpha_channels()[i].mask); });
                m.pop_id();
            }
            m.separator();
            m.item("Delete All Alpha Channels", nullptr, true, [&] { doc->alpha_channels().clear(); });
            m.end_menu();
        }
        m.item("Save Selection To Alpha Channel...", nullptr, doc->has_selection(), [&] {
            std::snprintf(alpha_name_buf, sizeof(alpha_name_buf), "Selection #%zu", doc->alpha_channels().size() + 1);
            show_alpha_save_dialog = true;
        });
        m.separator();
        m.item("Load Selection From Disk...", nullptr, true, [&] { request_load_selection(); });
        m.item("Save Selection To Disk...", nullptr, has_doc && doc->has_selection(), [&] { request_save_selection(); });
        m.end_menu();
    }
    m.end_menu();
}

// --- Dialogs ------------------------------------------------------------

void App::draw_selection_dialogs() {
    if (show_sel_dialog > 0 && show_sel_dialog < Count) { ImGui::OpenPopup(kTitles[show_sel_dialog]); show_sel_dialog = 0; }
    auto escape = [] { if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup(); };
    auto enter = [] { return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false); };
    auto buttons = [&](const char* name, auto&& apply) {
        if (ImGui::Button("OK", ImVec2(90, 0)) || enter()) {
            if (doc) apply();
            (void)name;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
    };
    auto modify = [&](const char* name, auto&& fn) {
        if (!doc->has_selection()) return;
        Mask m = doc->selection();
        fn(m);
        set_selection(name, std::move(m));
    };

    for (int which = Expand; which <= Feather; ++which) {
        if (!ImGui::BeginPopupModal(kTitles[which], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        escape();
        ImGui::SliderInt("Pixels", &selection_menu_state->sel_modify_px, 1, 100);
        buttons(kTitles[which], [&] {
            modify(kTitles[which], [&](Mask& m) {
                if (which == Expand) mask::expand(m, selection_menu_state->sel_modify_px);
                else if (which == Contract) mask::contract(m, selection_menu_state->sel_modify_px);
                else mask::feather(m, static_cast<float>(selection_menu_state->sel_modify_px));
            });
        });
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kTitles[InsideOutsideFeather], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        static int side = 2;  // 0 inside, 1 outside, 2 both
        ImGui::RadioButton("Inside", &side, 0); ImGui::SameLine();
        ImGui::RadioButton("Outside", &side, 1); ImGui::SameLine();
        ImGui::RadioButton("Both", &side, 2);
        ImGui::SliderInt("Feather amount", &selection_menu_state->sel_modify_px, 1, 100);
        buttons("Inside/Outside Feather", [&] {
            modify("Inside/Outside Feather", [&](Mask& m) {
                if (side == 0) mask::feather_inside(m, static_cast<float>(selection_menu_state->sel_modify_px));
                else if (side == 1) mask::feather_outside(m, static_cast<float>(selection_menu_state->sel_modify_px));
                else mask::feather(m, static_cast<float>(selection_menu_state->sel_modify_px));
            });
        });
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kTitles[SpecksHoles], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        static int what = 2;  // 0 specks, 1 holes, 2 both
        ImGui::RadioButton("Remove specks", &what, 0); ImGui::SameLine();
        ImGui::RadioButton("Remove holes", &what, 1); ImGui::SameLine();
        ImGui::RadioButton("Both", &what, 2);
        ImGui::SliderInt("Speck size (pixels)", &selection_menu_state->sel_speck, 1, 1000, "%d", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderInt("Hole size (pixels)", &selection_menu_state->sel_hole, 1, 1000, "%d", ImGuiSliderFlags_Logarithmic);
        buttons("Remove Specks and Holes", [&] {
            modify("Remove Specks and Holes", [&](Mask& m) { mask::remove_specks_and_holes(m, what == 1 ? 0 : selection_menu_state->sel_speck, what == 0 ? 0 : selection_menu_state->sel_hole); });
        });
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kTitles[ColorRange], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        static int op = 0;  // 0 add, 1 subtract
        ImGui::RadioButton("Add color range", &op, 0); ImGui::SameLine();
        ImGui::RadioButton("Subtract color range", &op, 1);
        ImGui::ColorEdit3("Reference color", selection_menu_state->sel_color);
        ImGui::SameLine();
        if (ImGui::SmallButton("Foreground")) { for (int i = 0; i < 3; ++i) selection_menu_state->sel_color[i] = fg_color[i]; }
        ImGui::SliderInt("Tolerance", &selection_menu_state->sel_tolerance, 0, 200);
        ImGui::SliderInt("Softness", &selection_menu_state->sel_softness, 0, 200);
        buttons("Select Color Range", [&] {
            if (active_layer() < 0 || !doc->layer(active_layer()).is_raster()) return;
            const Color c{static_cast<uint8_t>(selection_menu_state->sel_color[0] * 255 + 0.5f), static_cast<uint8_t>(selection_menu_state->sel_color[1] * 255 + 0.5f), static_cast<uint8_t>(selection_menu_state->sel_color[2] * 255 + 0.5f), 255};
            Mask range = mask::select_color_range(active_pixels(*this), c, selection_menu_state->sel_tolerance, selection_menu_state->sel_softness);
            Mask result = doc->has_selection() ? doc->selection() : Mask(doc->width(), doc->height(), 0);
            mask::combine(result, range, op == 0 ? mask::Combine::Add : mask::Combine::Subtract);
            set_selection("Select Color Range", std::move(result));
        });
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kTitles[Similar], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::SliderInt("Tolerance", &selection_menu_state->sel_tolerance, 0, 200);
        ImGui::TextDisabled("Selects every pixel of the layer within the tolerance of a color inside the selection.");
        buttons("Select Similar", [&] {
            if (active_layer() < 0 || !doc->layer(active_layer()).is_raster() || !doc->has_selection()) return;
            set_selection("Select Similar", mask::select_similar(active_pixels(*this), doc->selection(), selection_menu_state->sel_tolerance));
        });
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kTitles[ShapeAntialias], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::Checkbox("Inside", &sel_aa_inside); ImGui::SameLine();
        ImGui::Checkbox("Outside", &sel_aa_outside);
        buttons("Shape-based Anti-alias", [&] { modify("Shape-based Anti-alias", [&](Mask& m) { mask::shape_antialias(m, sel_aa_inside, sel_aa_outside); }); });
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kTitles[Smooth], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::SliderInt("Smoothing amount", &selection_menu_state->sel_smooth_amount, 1, 100, "%d", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Preserve corners", &sel_preserve_corners);
        buttons("Smooth Selection", [&] { modify("Smooth Selection", [&](Mask& m) { mask::smooth(m, selection_menu_state->sel_smooth_amount, sel_preserve_corners); }); });
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kTitles[Defringe], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::SliderInt("Width (pixels)", &selection_menu_state->sel_defringe, 1, 20);
        buttons("Defringe", [&] {
            if (active_layer() < 0 || !doc->layer(active_layer()).is_raster()) return;
            const int w = selection_menu_state->sel_defringe;
            run(std::make_unique<AdjustCommand>(static_cast<size_t>(active_layer()), "Defringe", [w](Image& img) { raster::defringe(img, w); }));
        });
        ImGui::EndPopup();
    }
}
