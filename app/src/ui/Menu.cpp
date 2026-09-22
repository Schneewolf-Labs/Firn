#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <fstream>
#include <memory>

#include "App.h"
#include "GenerateBackend.h"
#include "ui/MenuBuilder.h"
#include "ui/MenuState.h"
#include "ui/Shortcut.h"
#include "firn/adjust.h"
#include "firn/icc.h"
#include "firn/io.h"
#include "firn/io_psp.h"
#include "firn/effects.h"
#include "firn/photo.h"
#include "firn/mask.h"
#include "firn/raster16.h"
#include "imgui.h"

using namespace firn;

bool blend_combo(const char* label, BlendMode& mode);  // Palettes.cpp

namespace {

// The Exif tags worth offering when a picture carries none of its own.
struct AddableTag {
    meta::Group group;
    uint16_t tag;
    const char* label;
};
const AddableTag kAddable[] = {
    {meta::Group::Image, 0x010E, "Description"}, {meta::Group::Image, 0x013B, "Artist"},
    {meta::Group::Image, 0x8298, "Copyright"},   {meta::Group::Image, 0x0131, "Software"},
    {meta::Group::Image, 0x0132, "DateTime"},    {meta::Group::Exif, 0x9286, "UserComment"},
    {meta::Group::Exif, 0x9003, "DateTimeOriginal"},
};

void commit_metadata(App& app, const meta::Metadata& md) {
    if (!app.doc) return;
    app.run(std::make_unique<MetadataCommand>("Metadata", md));
    app.status = "Metadata updated";
}

void draw_metadata_tab(App& app) {
    MenuState& ms = *app.menu_state;
    meta::Metadata& md = ms.meta_edit;

    ImGui::TextDisabled("Exif tags, XMP properties and text notes travel with PNG, JPEG and project files.");
    if (ImGui::Button("Remove All")) { md.entries.clear(); md.xmp.clear(); ms.meta_row = -1; }
    ImGui::SameLine();
    if (ImGui::Button("Remove Private")) { md.remove_private(); ms.meta_row = -1; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drops GPS, serial numbers, the owner's name and maker notes, in Exif and in XMP.");
    ImGui::SameLine();
    if (ImGui::BeginCombo("##add", "Add...", ImGuiComboFlags_WidthFitPreview)) {
        for (const AddableTag& a : kAddable)
            if (!md.find(a.group, a.tag) && ImGui::Selectable(a.label)) md.set(a.group, a.tag, "");
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputTextWithHint("##newkey", "New note", ms.meta_new_key, sizeof ms.meta_new_key);
    ImGui::SameLine();
    if (ImGui::Button("Add Note") && ms.meta_new_key[0]) { md.set_text(ms.meta_new_key, ""); ms.meta_new_key[0] = 0; }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("metadata", 4, flags, ImVec2(620, 300))) {
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 28);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        int remove_at = -1;
        for (int i = 0; i < static_cast<int>(md.entries.size()); ++i) {
            meta::Entry& e = md.entries[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", meta::group_name(e.group));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name().c_str());
            ImGui::TableNextColumn();
            if (ms.meta_row == i) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                const bool done = ImGui::InputText("##v", ms.meta_value, sizeof ms.meta_value, ImGuiInputTextFlags_EnterReturnsTrue);
                if (done || ImGui::IsItemDeactivated()) {
                    if (done || ImGui::IsItemDeactivatedAfterEdit()) e.set_text(ms.meta_value);
                    ms.meta_row = -1;
                }
            } else {
                const std::string text = e.text();
                // Editing a value longer than the field would silently truncate it.
                if (e.editable() && text.size() < sizeof(ms.meta_value)) {
                    if (ImGui::Selectable(text.empty() ? "(empty)" : text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        ms.meta_row = i;
                        std::snprintf(ms.meta_value, sizeof ms.meta_value, "%s", text.c_str());
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to edit");
                } else {
                    ImGui::TextDisabled("%s", text.c_str());
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove_at = i;
            ImGui::PopID();
        }
        if (remove_at >= 0) {
            // An XMP property lives in the packet, so dropping the row alone
            // would leave it in the file and bring it back on the next read.
            const meta::Entry& gone = md.entries[static_cast<size_t>(remove_at)];
            if (gone.group == meta::Group::XMP) md.remove_xmp(gone.key);
            else md.entries.erase(md.entries.begin() + remove_at);
            ms.meta_row = -1;
        }
        ImGui::EndTable();
    }
    if (md.empty()) ImGui::TextDisabled("This image carries no metadata.");
}

}  // namespace

// Menu structure follows the original's: File, Edit, View, Image, Effects, Adjust,
// Layers, Objects, Selections, Window, Help. Most entries are placeholders
// until the corresponding commands exist.
void App::draw_menu(MenuBuilder& m) {
    const bool has_doc = doc != nullptr;
    const int layer = active_layer();
    const bool has_any_layer = has_doc && layer >= 0;
    const bool has_layer = has_any_layer && doc->layer(layer).is_raster();  // pixel operations need a raster layer

    if (m.begin_menu("File")) {
        m.item("New...", "Ctrl+N", true, [=, this] { show_new_dialog = true; });
        m.item("Open...", "Ctrl+O", true, [=, this] { request_open(); });
        if (m.begin_menu("Recent Files", !config.recent_files.empty())) {
            for (size_t i = 0; i < config.recent_files.size(); ++i) {
                const std::string r = config.recent_files[i];
                m.push_id(static_cast<int>(i));
                m.item(r.c_str(), nullptr, true, [this, r] { open_document_async(r); });
                m.pop_id();
            }
            m.end_menu();
        }
        m.item("Print...", "Ctrl+P", has_doc, [=, this] { show_print_dialog = true; });
        m.item("Close", "Ctrl+W", has_doc, [=, this] { close_document(current_doc); });
        // Closing every document means every document: the unmodified ones go
        // at once and the rest are asked about one after another, rather than
        // prompting once and leaving the others open while the menu says the
        // work is done.
        m.item("Close All", nullptr, has_doc, [=, this] {
            for (int i = static_cast<int>(docs.size()) - 1; i >= 0; --i) if (!document_modified(i)) close_document(i);
            if (!docs.empty()) { closing_all = true; close_document(0); }
        });
        m.separator();
        m.item("Save", "Ctrl+S", has_doc, [=, this] { save(); });
        m.item("Save As...", "Ctrl+Shift+S", has_doc, [=, this] { request_save_as(); });
        m.item("Revert", nullptr, has_doc && !doc_path.empty(), [=, this] { if (modified()) show_revert_prompt = true; else revert(); });
        if (m.begin_menu("Export", has_doc)) {
            m.item("Picture Tube...", nullptr, true, [=, this] { menu_state->tube_export_ready = false; show_tube_export_dialog = true; });
            m.end_menu();
        }
        m.separator();
        m.item("Preferences...", nullptr, true, [=, this] { menu_state->prefs_edit = config; menu_state->prefs_scale_before = config.ui_scale; show_prefs_dialog = true; });
        m.separator();
        m.item("Exit", nullptr, true, [=, this] { request_quit(); });
        m.end_menu();
    }
    if (m.begin_menu("Edit")) {
        m.item("Undo", "Ctrl+Z", has_doc && history.can_undo(), [=, this] { undo(); });
        m.item("Redo", "Ctrl+Y", has_doc && history.can_redo(), [=, this] { redo(); });
        m.separator();
        m.item("Cut", "Ctrl+X", has_layer, [=, this] { cut(); });
        m.item("Copy", "Ctrl+C", has_layer, [=, this] { copy(); });
        m.item("Copy Merged", "Ctrl+Shift+C", has_doc, [=, this] { copy_merged(); });
        m.item("Paste As New Image", "Ctrl+V", true, [=, this] { paste_as_new_image(); });
        m.item("Paste As New Layer", "Ctrl+L", has_doc, [=, this] { paste_as_new_layer(); });
        m.item("Paste Into Selection", "Ctrl+Shift+L", has_layer && doc->has_selection(), [=, this] { paste_into_selection(); });
        m.item("Clear", "Delete", has_layer, [=, this] { clear_selection(); });
        m.item("Generative Fill...", nullptr, has_layer && doc->has_selection() && generate_configured(), [=, this] { menu_state->generate_prompt[0] = 0; menu_state->generate_whole = false; show_generate_dialog = true; }, false,
               "Hands the selection to an image model and composites what comes back.");
        m.item("Generative Edit...", nullptr, has_layer && generate_configured(), [=, this] { menu_state->generate_prompt[0] = 0; menu_state->generate_whole = true; show_generate_dialog = true; }, false,
               "Asks an instruction model to change the whole layer: \"remove the dog\".");
        m.item("Content-Aware Fill", nullptr, has_layer && doc->has_selection(), [=, this] { content_aware_fill(); }, false,
               "Rebuilds the selection from the rest of the picture, to remove something from it.");
        m.separator();
        {
            const std::string label = last_effect.empty() ? "Repeat" : "Repeat " + last_effect;
            m.item(label.c_str(), "Ctrl+Shift+Y", has_layer && !last_effect.empty(), [=, this] { repeat_last_effect(); });
        }
        m.end_menu();
    }
    if (m.begin_menu("View")) {
        m.item("Zoom In", "+", has_doc, [=, this] { zoom_about(canvas_center, 1.25f); });
        m.item("Zoom Out", "-", has_doc, [=, this] { zoom_about(canvas_center, 0.8f); });
        m.item("Fit to Window", "Ctrl+0", has_doc, [=, this] { fit_requested = true; });
        m.item("Actual Size", "Ctrl+Alt+0", has_doc, [=, this] { zoom = 1.0f; pan_x = pan_y = 0.0f; });
        m.item("Zoom to Selection", nullptr, has_doc && doc->has_selection(), [=, this] { zoom_to_selection(); });
        m.separator();
        m.toggle("Rulers", nullptr, &show_rulers);
        m.toggle("Grid", nullptr, &show_grid);
        m.toggle("Guides", nullptr, &show_guides);
        m.toggle("Mask Overlay", nullptr, &show_mask_overlay);
        m.toggle("Snap to Guides", nullptr, &snap_to_guides);
        m.toggle("Snap to Grid", nullptr, &snap_to_grid);
        m.item("Clear Guides", nullptr, !guides_h().empty() || !guides_v().empty(), [=, this] { guides_h().clear(); guides_v().clear(); });
        m.toggle("Assistants", nullptr, &show_assistants);
        m.toggle("Snap to Assistants", nullptr, &assistant_snap);
        m.item("Clear Assistants", nullptr, !assistants().empty(), [=, this] { assistants().clear(); });
        m.imgui_only([=, this] {
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Grid spacing", &grid_spacing);
            grid_spacing = std::clamp(grid_spacing, 1, 1000);
        });
        m.separator();
        m.toggle("ImGui Demo", nullptr, &show_imgui_demo);
        m.end_menu();
    }
    if (m.begin_menu("Image")) {
        m.item("Flip", nullptr, has_doc, [=, this] { run(std::make_unique<FlipCommand>()); });
        m.item("Mirror", nullptr, has_doc, [=, this] { run(std::make_unique<MirrorCommand>()); });
        if (m.begin_menu("Rotate", has_doc)) {
            m.item("Rotate Clockwise 90", nullptr, true, [=, this] { rotate(90.0f); });
            m.item("Rotate Counter-clockwise 90", nullptr, true, [=, this] { rotate(-90.0f); });
            m.item("Rotate 180", nullptr, true, [=, this] { rotate(180.0f); });
            m.item("Free Rotate...", nullptr, true, [=, this] { show_rotate_dialog = true; });
            m.end_menu();
        }
        m.separator();
        m.item("Crop to Selection", "Ctrl+Shift+R", has_doc && doc->has_selection(), [=, this] { crop_to_selection(); });
        m.item("Resize...", nullptr, has_doc, [=, this] { open_resize_dialog(); });
        // Enlarging with a model rather than a filter. Only offered when the
        // configured server actually has one, since it is the server that
        // decides whether this is possible at all.
        m.item("Upscale with Model", nullptr, has_doc && generate_configured() && !generate_upscaler.empty(),
               [=, this] { upscale_image(generate_upscaler); }, false,
               generate_upscaler.empty() ? "No upscaler model on the image model server."
                                         : generate_upscaler.c_str());
        m.item("Canvas Size...", nullptr, has_doc, [=, this] { open_canvas_dialog(); });
        m.separator();
        m.item("Add Borders...", nullptr, has_doc, [=, this] { show_borders_dialog = true; });
        m.item("Picture Frame...", nullptr, has_doc, [=, this] { show_frame_dialog = true; });
        m.separator();
        m.item("Grayscale", nullptr, has_layer, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Grayscale", raster::grayscale, raster16::grayscale)); });
        if (m.begin_menu("Decrease Color Depth", has_layer)) {
            m.item("2 Colors...", nullptr, true, [=, this] { depth_colors = 2; show_depth_dialog = true; });
            m.item("16 Colors...", nullptr, true, [=, this] { depth_colors = 16; show_depth_dialog = true; });
            m.item("256 Colors...", nullptr, true, [=, this] { depth_colors = 256; show_depth_dialog = true; });
            m.item("32K Colors", nullptr, true, [=, this] { image_decrease_depth(32, false); });
            m.item("64K Colors", nullptr, true, [=, this] { image_decrease_depth(64, false); });
            m.item("8 Bits per Channel", nullptr, doc->bit_depth() == 16, [=, this] { run(std::make_unique<StateEditCommand>("Decrease to 8 Bits per Channel", [](Document& d) { d.set_bit_depth(8); })); });
            m.end_menu();
        }
        if (m.begin_menu("Increase Color Depth", has_doc)) {
            m.text("Images are 16 million colors; 16 bits per channel is optional.");
            m.item("16 Bits per Channel", nullptr, doc->bit_depth() == 8, [=, this] { run(std::make_unique<StateEditCommand>("Increase to 16 Bits per Channel", [](Document& d) { d.set_bit_depth(16); })); });
            m.end_menu();
        }
        if (m.begin_menu("Palette", has_layer)) {
            m.item("Load Palette...", nullptr, true, [=, this] { request_load_palette(); });
            m.item("Save Palette...", nullptr, true, [=, this] { request_save_palette(); });
            m.end_menu();
        }
        if (m.begin_menu("Split Channel", has_doc)) {
            m.item("Split to RGB", nullptr, true, [=, this] { image_split_channels(0); });
            m.item("Split to HSL", nullptr, true, [=, this] { image_split_channels(1); });
            m.item("Split to CMYK", nullptr, true, [=, this] { image_split_channels(2); });
            m.end_menu();
        }
        if (m.begin_menu("Combine Channel", docs.size() >= 3)) {
            m.item("Combine from RGB", nullptr, true, [=, this] { combine_mode = 0; show_combine_dialog = true; });
            m.item("Combine from HSL", nullptr, true, [=, this] { combine_mode = 1; show_combine_dialog = true; });
            m.item("Combine from CMYK", nullptr, true, [=, this] { combine_mode = 2; show_combine_dialog = true; });
            m.end_menu();
        }
        m.item("Arithmetic...", nullptr, docs.size() >= 2, [=, this] { show_arith_dialog = true; });
        m.separator();
        if (m.begin_menu("Color Management", has_doc)) {
            const icc::Profile prof = document_profile();
            const std::string profile_text = std::string("Profile: ") + (doc->icc().empty() ? "(untagged, treated as sRGB)" : prof.description.empty() ? "(unnamed)" : prof.description.c_str());
            m.text(profile_text.c_str());
            m.item("Color Managed Display", nullptr, true, [=, this] {
                color_managed_display = !color_managed_display;
                config.color_managed_display = color_managed_display;
                canvas_tex_revision = ~0ull;
            }, color_managed_display);
            m.separator();
            if (m.begin_menu("Assign Profile")) {
                m.item("sRGB", nullptr, true, [=, this] { assign_profile(icc::encode(icc::srgb(), "sRGB IEC61966-2.1"), "Assign Profile (sRGB)"); });
                m.item("Adobe RGB (1998)", nullptr, true, [=, this] { assign_profile(icc::encode(icc::adobe_rgb(), "Adobe RGB (1998)"), "Assign Profile (Adobe RGB)"); });
                m.item("ProPhoto RGB", nullptr, true, [=, this] { assign_profile(icc::encode(icc::prophoto_rgb(), "ProPhoto RGB"), "Assign Profile (ProPhoto RGB)"); });
                m.item("From File...", nullptr, true, [=, this] { request_load_profile(); });
                m.end_menu();
            }
            if (m.begin_menu("Convert to Profile")) {
                m.item("sRGB", nullptr, true, [=, this] { convert_to_profile(icc::srgb(), icc::encode(icc::srgb(), "sRGB IEC61966-2.1"), "Convert to sRGB"); });
                m.item("Adobe RGB (1998)", nullptr, true, [=, this] { convert_to_profile(icc::adobe_rgb(), icc::encode(icc::adobe_rgb(), "Adobe RGB (1998)"), "Convert to Adobe RGB"); });
                m.item("ProPhoto RGB", nullptr, true, [=, this] { convert_to_profile(icc::prophoto_rgb(), icc::encode(icc::prophoto_rgb(), "ProPhoto RGB"), "Convert to ProPhoto RGB"); });
                m.end_menu();
            }
            m.item("Remove Profile", nullptr, !doc->icc().empty(), [=, this] { assign_profile({}, "Remove Profile"); });
            m.end_menu();
        }
        m.item("Count Colors Used", nullptr, has_doc, [=, this] { image_count_colors(); });
        m.item("Image Information...", "Shift+I", has_doc, [=, this] { show_info_dialog = true; });
        m.end_menu();
    }
    if (m.begin_menu("Adjust")) {
        m.item("Color to Alpha...", nullptr, has_layer, [=, this] { open_adjust = Adj::ColorToAlpha; });
        m.separator();
        if (m.begin_menu("Brightness and Contrast", has_layer)) {
            m.item("Brightness/Contrast...", nullptr, true, [=, this] { open_adjust = Adj::BrightnessContrast; });
            m.item("Curves...", nullptr, true, [=, this] { open_adjust = Adj::Curves; });
            m.item("Gamma Correction...", nullptr, true, [=, this] { open_adjust = Adj::Gamma; });
            m.item("Histogram Equalize", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Histogram Equalize", adjust::histogram_equalize)); });
            m.item("Histogram Stretch", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Histogram Stretch", adjust::histogram_stretch)); });
            m.item("Levels...", nullptr, true, [=, this] { open_adjust = Adj::Levels; });
            m.item("Threshold...", nullptr, true, [=, this] { open_adjust = Adj::Threshold; });
            m.end_menu();
        }
        if (m.begin_menu("Color Balance", has_layer)) {
            m.item("Channel Mixer...", nullptr, true, [=, this] { open_adjust = Adj::ChannelMixer; });
            m.item("Color Balance...", nullptr, true, [=, this] { open_adjust = Adj::ColorBalance; });
            m.end_menu();
        }
        if (m.begin_menu("Hue and Saturation", has_layer)) {
            m.item("Colorize...", nullptr, true, [=, this] { open_adjust = Adj::Colorize; });
            m.item("Hue Map...", nullptr, true, [=, this] { open_adjust = Adj::HueMap; });
            m.item("Hue/Saturation/Lightness...", nullptr, true, [=, this] { open_adjust = Adj::HSL; });
            m.end_menu();
        }
        if (m.begin_menu("Add/Remove Noise", has_layer)) {
            m.item("Add Noise...", nullptr, true, [=, this] { open_adjust = Adj::AddNoise; });
            m.item("Median Filter...", nullptr, true, [=, this] { open_adjust = Adj::Median; });
            m.item("Despeckle", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Despeckle", [](Image& i) { effects::median(i, 1); })); });
            m.item("Edge Preserving Smooth...", nullptr, true, [=, this] { open_adjust = Adj::EdgeSmooth; });
            m.item("Salt and Pepper Filter...", nullptr, true, [=, this] { open_adjust = Adj::SaltPepper; });
            m.item("JPEG Artifact Removal...", nullptr, true, [=, this] { open_adjust = Adj::JpegArtifacts; });
            m.item("Digital Camera Noise Removal...", nullptr, true, [=, this] { open_adjust = Adj::NoiseRemoval; });
            m.item("Erode", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Erode", effects::erode)); });
            m.item("Dilate", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Dilate", effects::dilate)); });
            m.end_menu();
        }
        if (m.begin_menu("Blur", has_layer)) {
            m.item("Average...", nullptr, true, [=, this] { open_adjust = Adj::Average; });
            m.item("Blur More", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Blur More", effects::blur_more)); });
            m.item("Gaussian Blur...", nullptr, true, [=, this] { open_adjust = Adj::Gaussian; });
            m.item("Motion Blur...", nullptr, true, [=, this] { open_adjust = Adj::MotionBlur; });
            m.end_menu();
        }
        if (m.begin_menu("Sharpness", has_layer)) {
            m.item("Sharpen", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Sharpen", effects::sharpen)); });
            m.item("Sharpen More", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Sharpen More", effects::sharpen_more)); });
            m.item("Unsharp Mask...", nullptr, true, [=, this] { open_adjust = Adj::UnsharpMask; });
            m.end_menu();
        }
        if (m.begin_menu("Softness", has_layer)) {
            m.item("Soften", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Soften", effects::soften)); });
            m.item("Soften More", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Soften More", effects::soften_more)); });
            m.end_menu();
        }
        if (m.begin_menu("Photo Fix", has_layer)) {
            m.item("One Step Photo Fix", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "One Step Photo Fix", photo::one_step_photo_fix)); });
            m.item("Automatic Color Balance...", nullptr, true, [=, this] { open_adjust = Adj::AutoColor; });
            m.item("Automatic Contrast Enhancement...", nullptr, true, [=, this] { open_adjust = Adj::AutoContrast; });
            m.item("Automatic Saturation Enhancement...", nullptr, true, [=, this] { open_adjust = Adj::AutoSaturation; });
            m.item("Clarify...", nullptr, true, [=, this] { open_adjust = Adj::Clarify; });
            m.item("Fade Correction...", nullptr, true, [=, this] { open_adjust = Adj::FadeCorrection; });
            m.text("Red-eye: use the Red-eye Removal tool.");
            m.end_menu();
        }
        if (m.begin_menu("Photo Fix (more)", has_layer)) {
            m.item("Black and White Points...", nullptr, true, [=, this] { open_adjust = Adj::BlackWhitePoints; });
            m.item("Histogram Adjustment...", nullptr, true, [=, this] { open_adjust = Adj::HistogramAdjust; });
            m.item("Fill Flash...", nullptr, true, [=, this] { open_adjust = Adj::FillFlash; });
            m.item("Backlighting...", nullptr, true, [=, this] { open_adjust = Adj::Backlighting; });
            m.item("Chromatic Aberration Removal...", nullptr, true, [=, this] { open_adjust = Adj::ChromaticAberration; });
            m.end_menu();
        }
        m.separator();
        m.item("Negative Image", "Ctrl+I", has_layer, [=, this] { run(std::make_unique<InvertCommand>(layer)); });
        m.end_menu();
    }
    if (m.begin_menu("Effects")) {
        m.item("Effect Browser...", nullptr, has_layer, [=, this] { reset_effect_browser(); show_effect_browser = true; });
        m.separator();
        if (m.begin_menu("3D Effects", has_layer)) {
            m.item("Buttonize...", nullptr, true, [=, this] { open_adjust = Adj::Buttonize; });
            m.item("Cutout...", nullptr, true, [=, this] { open_adjust = Adj::Cutout; });
            m.item("Drop Shadow...", nullptr, true, [=, this] { open_adjust = Adj::DropShadow; });
            m.item("Inner Bevel...", nullptr, true, [=, this] { open_adjust = Adj::InnerBevel; });
            m.item("Outer Bevel...", nullptr, true, [=, this] { open_adjust = Adj::OuterBevel; });
            m.end_menu();
        }
        if (m.begin_menu("Distortion Effects", has_layer)) {
            m.item("Lens Distortion...", nullptr, true, [=, this] { open_adjust = Adj::Lens; });
            m.item("Pinch / Punch...", nullptr, true, [=, this] { open_adjust = Adj::Pinch; });
            m.item("Ripple...", nullptr, true, [=, this] { open_adjust = Adj::Ripple; });
            m.item("Spherize...", nullptr, true, [=, this] { open_adjust = Adj::Spherize; });
            m.item("Twirl...", nullptr, true, [=, this] { open_adjust = Adj::Twirl; });
            m.item("Wave...", nullptr, true, [=, this] { open_adjust = Adj::Wave; });
            m.separator();
            m.item("Curlicues...", nullptr, true, [=, this] { open_adjust = Adj::Curlicues; });
            m.item("Displacement Map...", nullptr, true, [=, this] { open_adjust = Adj::DisplacementMap; });
            m.item("Polar Coordinates...", nullptr, true, [=, this] { open_adjust = Adj::PolarCoordinates; });
            m.item("Spiky Halo...", nullptr, true, [=, this] { open_adjust = Adj::SpikyHalo; });
            m.item("Warp...", nullptr, true, [=, this] { open_adjust = Adj::Warp; });
            m.item("Wind...", nullptr, true, [=, this] { open_adjust = Adj::Wind; });
            m.end_menu();
        }
        if (m.begin_menu("Geometric Effects", has_layer)) {
            m.item("Circle...", nullptr, true, [=, this] { open_adjust = Adj::Circle; });
            m.item("Cylinder...", nullptr, true, [=, this] { open_adjust = Adj::Cylinder; });
            m.item("Pentagon...", nullptr, true, [=, this] { open_adjust = Adj::Pentagon; });
            m.item("Perspective...", nullptr, true, [=, this] { open_adjust = Adj::Perspective; });
            m.item("Skew...", nullptr, true, [=, this] { open_adjust = Adj::Skew; });
            m.item("Spherize...", nullptr, true, [=, this] { open_adjust = Adj::Spherize; });
            m.end_menu();
        }
        if (m.begin_menu("Image Effects", has_layer)) {
            m.item("Offset...", nullptr, true, [=, this] { open_adjust = Adj::Offset; });
            m.item("Page Curl...", nullptr, true, [=, this] { open_adjust = Adj::PageCurl; });
            m.item("Seamless Tiling...", nullptr, true, [=, this] { open_adjust = Adj::SeamlessTiling; });
            m.end_menu();
        }
        if (m.begin_menu("Art Media Effects", has_layer)) {
            m.item("Black Pencil...", nullptr, true, [=, this] { open_adjust = Adj::BlackPencil; });
            m.item("Brush Strokes...", nullptr, true, [=, this] { open_adjust = Adj::BrushStrokes; });
            m.item("Charcoal...", nullptr, true, [=, this] { open_adjust = Adj::Charcoal; });
            m.item("Colored Chalk...", nullptr, true, [=, this] { open_adjust = Adj::ColoredChalk; });
            m.item("Colored Pencil...", nullptr, true, [=, this] { open_adjust = Adj::ColoredPencil; });
            m.item("Pencil...", nullptr, true, [=, this] { open_adjust = Adj::Pencil; });
            m.end_menu();
        }
        if (m.begin_menu("Artistic Effects", has_layer)) {
            m.item("Aged Newspaper...", nullptr, true, [=, this] { open_adjust = Adj::AgedNewspaper; });
            m.item("Balls and Bubbles...", nullptr, true, [=, this] { open_adjust = Adj::BallsBubbles; });
            m.item("Chrome...", nullptr, true, [=, this] { open_adjust = Adj::Chrome; });
            m.item("Colored Edges...", nullptr, true, [=, this] { open_adjust = Adj::ColoredEdges; });
            m.item("Colored Foil...", nullptr, true, [=, this] { open_adjust = Adj::ColoredFoil; });
            m.item("Contours...", nullptr, true, [=, this] { open_adjust = Adj::Contours; });
            m.item("Enamel...", nullptr, true, [=, this] { open_adjust = Adj::Enamel; });
            m.item("Glowing Edges...", nullptr, true, [=, this] { open_adjust = Adj::GlowingEdges; });
            m.item("Halftone...", nullptr, true, [=, this] { open_adjust = Adj::Halftone; });
            m.item("Hot Wax Coating...", nullptr, true, [=, this] { open_adjust = Adj::HotWax; });
            m.item("Magnifying Lens...", nullptr, true, [=, this] { open_adjust = Adj::MagnifyingLens; });
            m.item("Neon Glow...", nullptr, true, [=, this] { open_adjust = Adj::NeonGlow; });
            m.item("Posterize...", nullptr, true, [=, this] { open_adjust = Adj::Posterize; });
            m.item("Sepia Toning...", nullptr, true, [=, this] { open_adjust = Adj::Sepia; });
            m.item("Solarize...", nullptr, true, [=, this] { open_adjust = Adj::Solarize; });
            m.item("Topography...", nullptr, true, [=, this] { open_adjust = Adj::Topography; });
            m.end_menu();
        }
        if (m.begin_menu("Edge Effects", has_layer)) {
            m.item("Enhance", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Enhance Edges", effects::enhance_edges)); });
            m.item("Enhance More", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Enhance Edges More", effects::enhance_edges_more)); });
            m.item("Find All", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Find Edges", effects::find_edges)); });
            m.end_menu();
        }
        if (m.begin_menu("Illumination Effects", has_layer)) {
            m.item("Lights...", nullptr, true, [=, this] { open_adjust = Adj::Lights; });
            m.item("Sunburst...", nullptr, true, [=, this] { open_adjust = Adj::Sunburst; });
            m.end_menu();
        }
        if (m.begin_menu("Reflection Effects", has_layer)) {
            m.item("Feedback...", nullptr, true, [=, this] { open_adjust = Adj::Feedback; });
            m.item("Kaleidoscope...", nullptr, true, [=, this] { open_adjust = Adj::Kaleidoscope; });
            m.item("Pattern...", nullptr, true, [=, this] { open_adjust = Adj::Pattern; });
            m.item("Rotating Mirror...", nullptr, true, [=, this] { open_adjust = Adj::RotatingMirror; });
            m.end_menu();
        }
        if (m.begin_menu("Texture Effects", has_layer)) {
            m.item("Blinds...", nullptr, true, [=, this] { open_adjust = Adj::Blinds; });
            m.item("Emboss", nullptr, true, [=, this] { run(std::make_unique<AdjustCommand>(layer, "Emboss", effects::emboss)); });
            m.item("Fine Leather...", nullptr, true, [=, this] { open_adjust = Adj::FineLeather; });
            m.item("Fur...", nullptr, true, [=, this] { open_adjust = Adj::Fur; });
            m.item("Mosaic - Antique...", nullptr, true, [=, this] { open_adjust = Adj::MosaicAntique; });
            m.item("Mosaic - Glass...", nullptr, true, [=, this] { open_adjust = Adj::MosaicGlass; });
            m.item("Pixelate (Mosaic)...", nullptr, true, [=, this] { open_adjust = Adj::Mosaic; });
            m.item("Polished Stone...", nullptr, true, [=, this] { open_adjust = Adj::PolishedStone; });
            m.item("Rough Leather...", nullptr, true, [=, this] { open_adjust = Adj::RoughLeather; });
            m.item("Sandstone...", nullptr, true, [=, this] { open_adjust = Adj::Sandstone; });
            m.item("Sculpture...", nullptr, true, [=, this] { open_adjust = Adj::Sculpture; });
            m.item("Soft Plastic...", nullptr, true, [=, this] { open_adjust = Adj::SoftPlastic; });
            m.item("Straw Wall...", nullptr, true, [=, this] { open_adjust = Adj::StrawWall; });
            m.item("Texture...", nullptr, true, [=, this] { open_adjust = Adj::Texture; });
            m.item("Tiles...", nullptr, true, [=, this] { open_adjust = Adj::Tiles; });
            m.item("Weave...", nullptr, true, [=, this] { open_adjust = Adj::Weave; });
            m.end_menu();
        }
        m.item("User Defined Filter...", nullptr, has_layer, [=, this] { open_adjust = Adj::UserFilter; });
        m.end_menu();
    }
    draw_selections_menu(m);
    if (m.begin_menu("Layers")) {
        draw_layer_menu_items(m);
        m.end_menu();
    }
    if (m.begin_menu("Objects")) {
        const bool on_vector = has_any_layer && doc->layer(layer).is_vector();
        const size_t nsel = on_vector ? selected_objects().size() : 0;
        bool has_text = false;
        if (on_vector) for (const auto& o : doc->layer(layer).objects) if (o.selected && o.is_text) has_text = true;
        if (m.begin_menu("Align", nsel > 0)) {
            static const char* items[] = {"Top", "Bottom", "Left", "Right", "Vertical Center", "Horizontal Center", "Center in Canvas", "Horizontal Center in Canvas", "Vertical Center in Canvas"};
            for (int i = 0; i < 9; ++i) {
                if (i == 6) m.separator();
                m.item(items[i], nullptr, i >= 6 || nsel > 1, [this, i] { object_align(i); });
            }
            m.end_menu();
        }
        if (m.begin_menu("Distribute", nsel > 2)) {
            static const char* items[] = {"Vertical Top", "Vertical Center", "Vertical Bottom", "Horizontal Left", "Horizontal Center", "Horizontal Right", "Space Evenly Vertically", "Space Evenly Horizontally"};
            for (int i = 0; i < 8; ++i) { if (i == 3 || i == 6) m.separator(); m.item(items[i], nullptr, true, [this, i] { object_distribute(i); }); }
            m.end_menu();
        }
        if (m.begin_menu("Make Same Size", nsel > 1)) {
            m.item("Height", nullptr, true, [=, this] { object_same_size(0); });
            m.item("Width", nullptr, true, [=, this] { object_same_size(1); });
            m.item("Both", nullptr, true, [=, this] { object_same_size(2); });
            m.end_menu();
        }
        if (m.begin_menu("Arrange", nsel > 0)) {
            const int n = static_cast<int>(doc->layer(layer).objects.size()) + 1;
            m.item("Bring to Top", nullptr, true, [this, n] { object_arrange(n); });
            m.item("Move Up", nullptr, true, [=, this] { object_arrange(1); });
            m.item("Move Down", nullptr, true, [=, this] { object_arrange(-1); });
            m.item("Send to Bottom", nullptr, true, [this, n] { object_arrange(-n); });
            m.end_menu();
        }
        m.separator();
        m.item("Group", nullptr, nsel > 1, [=, this] { object_group(); });
        m.item("Ungroup", nullptr, nsel > 0, [=, this] { object_ungroup(); });
        m.separator();
        m.item("Edit Text...", nullptr, has_text, [=, this] { open_text_edit(); });
        if (m.begin_menu("Convert Text to Curves", has_text)) {
            m.item("As Single Shape", nullptr, true, [=, this] { object_text_to_curves(false); });
            m.item("As Character Shapes", nullptr, true, [=, this] { object_text_to_curves(true); });
            m.end_menu();
        }
        m.item("Convert to Path", nullptr, has_text, [=, this] { object_convert_to_path(); });
        if (m.begin_menu("Edit Node", node_object >= 0)) {
            m.item("Break", nullptr, true, [=, this] { node_break(); });
            m.item("Join", nullptr, true, [=, this] { node_join(); });
            m.separator();
            m.item("Reverse Path", nullptr, true, [=, this] { path_reverse(); });
            m.item("Close Path", nullptr, true, [=, this] { path_set_closed(true); });
            m.item("Open Path", nullptr, true, [=, this] { path_set_closed(false); });
            m.end_menu();
        }
        m.item("Properties...", nullptr, nsel > 0, [=, this] { open_vector_properties(); });
        m.separator();
        m.item("Select All", nullptr, on_vector, [=, this] { object_select_all(); });
        m.item("Select None", nullptr, nsel > 0, [=, this] { object_select_none(); });
        m.item("Delete", nullptr, nsel > 0, [=, this] { object_delete(); });
        m.end_menu();
    }
    if (m.begin_menu("Window")) {
        for (int i = 0; i < static_cast<int>(docs.size()); ++i) {
            const std::string label = document_title(i) + (document_modified(i) ? "*" : "");
            m.push_id(i);
            m.item(label.c_str(), nullptr, true, [this, i] { activate_document(i); }, i == current_doc);
            m.pop_id();
        }
        if (docs.empty()) m.item("(no images open)", nullptr, false, [] {});
        m.separator();
        m.item("Tabbed Documents", nullptr, true, [=, this] {
            image_windows = !image_windows;
            config.image_windows = image_windows;
            config.save();
            if (image_windows) arrange_request = Arrange::Cascade;
        }, !image_windows);
        const bool can_arrange = image_windows && !docs.empty();
        m.item("Cascade", nullptr, can_arrange, [=, this] { arrange_request = Arrange::Cascade; });
        m.item("Tile Horizontally", nullptr, can_arrange, [=, this] { arrange_request = Arrange::TileHorizontally; });
        m.item("Tile Vertically", nullptr, can_arrange, [=, this] { arrange_request = Arrange::TileVertically; });
        m.end_menu();
    }
    if (m.begin_menu("Help")) {
        m.item("Find a Command...", "Ctrl+K", true, [=, this] { open_command_palette(); });
        m.item("Keyboard Shortcuts...", nullptr, true, [=, this] { show_shortcuts_dialog = true; });
        m.separator();
        m.item("Check for Updates...", nullptr, !update_checking, [=, this] { start_update_check(true); show_about_dialog = true; });
        m.item("About Firn...", nullptr, true, [=, this] { show_about_dialog = true; });
        m.end_menu();
    }
}


// The Layers menu body, shared with the Layers palette's context menu.
void App::draw_layer_menu_items(MenuBuilder& m) {
    const bool has_doc = doc != nullptr;
    const int layer = active_layer();
    const bool has_any_layer = has_doc && layer >= 0;
    const bool has_layer = has_any_layer && doc->layer(layer).is_raster();
    const bool is_group = has_any_layer && doc->layer(layer).type == LayerType::Group;
    const int n = has_doc ? static_cast<int>(doc->layer_count()) : 0;
    const bool is_bg = has_layer && doc->layer(layer).background;
    // ImGui invokes actions while building this menu. Capture predicates
    // before an action can replace/delete layers; later entries must not
    // dereference the original index after Flatten, Delete or Merge.
    const bool is_vector = has_any_layer && doc->layer(layer).is_vector();
    const bool is_adjustment = has_any_layer && doc->layer(layer).is_adjustment();
    const bool has_mask = has_any_layer && doc->layer(layer).has_mask();
    const bool mask_on = has_mask && doc->layer(layer).mask_enabled;
    const bool editing = mask_edit && static_cast<int>(mask_proxy_layer) == layer;
    const bool has_selection = has_doc && doc->has_selection();
    const bool can_merge_down = has_layer && layer > 0 && doc->layer(layer - 1).is_raster() && doc->layer(layer - 1).depth == doc->layer(layer).depth;
    m.item("New Raster Layer", nullptr, has_doc, [=, this] { layer_new(); });
    m.item("New Vector Layer", nullptr, has_doc, [=, this] { layer_new_vector(); });
    if (m.begin_menu("New Adjustment Layer", has_doc)) {
        using K = Adjustment::Kind;
        static const K kinds[] = {K::BrightnessContrast, K::ChannelMixer, K::ColorBalance, K::Curves, K::GradientMap, K::HSL, K::Invert, K::Levels, K::Posterize, K::Threshold};
        for (K k : kinds) m.item(Adjustment::kind_name(k), nullptr, true, [this, k] { layer_new_adjustment(k); });
        m.end_menu();
    }
    if (m.begin_menu("New Filter Layer", has_doc)) {
        using K = Adjustment::Kind;
        static const K kinds[] = {K::GaussianBlur, K::Average, K::UnsharpMask};
        for (K k : kinds) m.item(Adjustment::kind_name(k), nullptr, true, [this, k] { layer_new_adjustment(k); });
        m.end_menu();
    }
    m.item("New Layer Group", nullptr, has_any_layer, [=, this] { layer_new_group(); });
    if (m.begin_menu("New Mask Layer", has_any_layer)) {
        m.item("Show All", nullptr, true, [=, this] { layer_set_mask("New Mask Layer", Mask(doc->width(), doc->height(), 255)); });
        m.item("Hide All", nullptr, true, [=, this] { layer_set_mask("New Mask Layer", Mask(doc->width(), doc->height(), 0)); });
        m.item("From Selection", nullptr, has_selection, [=, this] { layer_mask_from_selection(); });
        m.item("From Image", nullptr, true, [=, this] { layer_mask_from_image(); });
        m.end_menu();
    }
    m.item("Duplicate", nullptr, has_any_layer, [=, this] { layer_duplicate(); });
    m.item("Delete", nullptr, has_any_layer && n > 1, [=, this] { layer_delete(); });
    m.item("Ungroup Layers", nullptr, is_group, [=, this] { layer_ungroup(); });
    m.item("Properties...", nullptr, has_any_layer, [=, this] {
        if (doc->layer(layer).is_adjustment()) open_adjustment_dialog(layer, false);
        else open_layer_properties();
    });
    m.item("Layer Styles...", nullptr, has_any_layer && !is_adjustment, [=, this] { open_layer_styles(layer); });
    {
        const bool in_range = has_doc && layer >= 0 && static_cast<size_t>(layer) < doc->layer_count();
        const bool clipped_now = in_range && doc->layer(layer).clipped;
        m.item(clipped_now ? "Release Clipping Mask" : "Create Clipping Mask", SC("Ctrl+Alt+G"), can_clip_layer(),
               [=, this] { layer_toggle_clipped(); }, clipped_now);
    }
    m.separator();
    if (m.begin_menu("Mask", has_mask)) {
        m.item("Enable Mask", nullptr, true, [=, this] { layer_set_mask(!mask_on ? "Enable Mask" : "Disable Mask", doc->layer(layer).mask, !mask_on); }, mask_on);
        m.item("Edit Mask", nullptr, true, [=, this] { set_mask_edit(!editing); }, editing);
        m.item("Invert Mask", nullptr, true, [=, this] { Mask msk = doc->layer(layer).mask; mask::invert(msk); layer_set_mask("Invert Mask", std::move(msk), doc->layer(layer).mask_enabled); });
        m.item("Delete Mask", nullptr, true, [=, this] { layer_set_mask("Delete Mask", Mask()); });
        m.item("Load Selection From Mask", nullptr, true, [=, this] { set_selection("Load Selection From Mask", doc->layer(layer).mask); });
        m.end_menu();
    }
    if (m.begin_menu("View", has_any_layer)) {
        m.item("Current Only", nullptr, true, [=, this] { layer_view_only(true); });
        m.item("All", nullptr, true, [=, this] { layer_view_only(false); });
        m.end_menu();
    }
    if (m.begin_menu("Arrange", has_any_layer)) {
        m.item("Bring to Top", nullptr, true, [this, n] { layer_arrange(n); });
        m.item("Move Up", nullptr, true, [=, this] { layer_arrange(+1); });
        m.item("Move Down", nullptr, true, [=, this] { layer_arrange(-1); });
        m.item("Send to Bottom", nullptr, true, [this, n] { layer_arrange(-n); });
        m.end_menu();
    }
    if (m.begin_menu("Merge", has_any_layer)) {
        m.item("Merge Down", nullptr, can_merge_down, [=, this] { layer_merge(0); });
        m.item("Merge Visible", nullptr, n > 1, [=, this] { layer_merge(1); });
        m.item("Merge All (Flatten)", nullptr, n > 1, [=, this] { layer_merge(2); });
        m.end_menu();
    }
    m.separator();
    m.item("Promote Background Layer", nullptr, is_bg, [=, this] { layer_promote_background(); });
    m.item("Promote Selection to Layer", nullptr, has_layer && has_selection, [=, this] { promote_selection_to_layer(false); });
    m.item("Convert to Raster Layer", nullptr, is_vector, [=, this] { layer_convert_to_raster(); });
}

void App::draw_dialogs() {
    poll_update_check();
    draw_background_job();
    draw_adjust_dialogs();
    draw_text_dialog();
    draw_vector_dialogs();
    draw_adjustment_layer_dialog();
    draw_layer_styles_dialog();
    draw_image_dialogs();
    draw_material_dialog();
    draw_about_dialog();
    draw_shortcuts_dialog();
    draw_recovery_dialog();
    draw_theme_editor();
    draw_effect_browser();

    if (file_dialog.draw()) {
        if (file_op == PendingFileOp::Open) open_document_async(file_dialog.path());
        else if (file_op == PendingFileOp::SaveAs) save_document_async(file_dialog.path());
        else if (file_op == PendingFileOp::LoadSelection) load_selection(file_dialog.path());
        else if (file_op == PendingFileOp::SaveSelection) save_selection(file_dialog.path());
        else if (file_op == PendingFileOp::LoadPalette) load_palette(file_dialog.path());
        else if (file_op == PendingFileOp::SavePalette) save_palette(file_dialog.path());
        else if (file_op == PendingFileOp::SavePdf) print_to_pdf(file_dialog.path(), false);
        else if (file_op == PendingFileOp::ImportTheme) import_theme(file_dialog.path());
        else if (file_op == PendingFileOp::ExportTheme) export_theme(file_dialog.path());
        else if (file_op == PendingFileOp::ExportTube) export_tube(file_dialog.path(), menu_state->tube_export);
        else if (file_op == PendingFileOp::LoadProfile) {
            std::ifstream pf(file_dialog.path(), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
            const icc::Profile prof = icc::parse(bytes);
            if (!prof.valid) status = "Assign Profile: not an ICC profile";
            else assign_profile(bytes, "Assign Profile (" + (prof.description.empty() ? std::string("file") : prof.description) + ")");
        }
        else if (file_op == PendingFileOp::LoadSwatches) { std::string e; auto pal = io::load_palette(file_dialog.path(), &e); if (pal.empty()) status = "Swatches: " + e; else { swatches = pal; save_swatches(); } }
        else if (file_op == PendingFileOp::SaveSwatches) { std::string e; if (!io::save_palette(swatches, file_dialog.path(), &e)) status = "Swatches: " + e; }
        file_op = PendingFileOp::None;
    }

    draw_tube_export_dialog();
    draw_generate_dialog();
    draw_command_palette();
    draw_delete_alpha_prompt();

    if (show_new_dialog) { ImGui::OpenPopup("New Image"); show_new_dialog = false; }
    if (show_print_dialog) { ImGui::OpenPopup("Print"); show_print_dialog = false; }
    if (ImGui::BeginPopupModal("Print", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(140); ImGui::Combo("Paper", &print_paper, "Letter\0A4\0Legal\0");
        ImGui::Checkbox("Landscape", &print_landscape); ImGui::SameLine(); ImGui::Checkbox("Center on page", &print_center);
        ImGui::SetNextItemWidth(140); ImGui::SliderFloat("Margins (in)", &print_margin, 0.0f, 2.0f, "%.2f");
        ImGui::Checkbox("Fit to page", &print_fit);
        if (!print_fit) { ImGui::SetNextItemWidth(140); ImGui::SliderFloat("Scale %", &print_scale, 5.0f, 400.0f, "%.0f"); ImGui::SetNextItemWidth(140); ImGui::InputInt("Image DPI", &print_dpi); print_dpi = std::clamp(print_dpi, 10, 2400); }
        char printer[128]; std::snprintf(printer, sizeof(printer), "%s", print_printer.c_str());
        ImGui::SetNextItemWidth(200); if (ImGui::InputText("Printer (blank = default)", printer, sizeof(printer))) print_printer = printer;
        ImGui::Separator();
        if (ImGui::Button("Print", ImVec2(100, 0))) { print_to_pdf(Config::directory() + "/print.pdf", true); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Save as PDF...", ImVec2(120, 0))) { request_print_pdf(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (show_jpeg_dialog) { ImGui::OpenPopup("JPEG Options"); show_jpeg_dialog = false; }
    if (show_info_dialog) { ImGui::OpenPopup("Image Information"); show_info_dialog = false; }
    if (show_alpha_save_dialog) { ImGui::OpenPopup("Save Selection To Alpha Channel"); show_alpha_save_dialog = false; }
    if (show_prefs_dialog) { ImGui::OpenPopup("Preferences"); show_prefs_dialog = false; }

    if (ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Config& c = menu_state->prefs_edit;
        ImGui::SeparatorText("General");
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Undo steps per image", &c.undo_limit, 1, 1000);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Undo memory per image (MB)", &c.undo_memory_mb, 64, 16384, "%d", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Autosave every (minutes, 0 = off)", &c.autosave_minutes, 0, 60);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Default JPEG quality", &c.jpeg_quality, 1, 100);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Checkerboard cell (px)", &c.checker_size, 2, 64);
        ImGui::Checkbox("Check for updates on startup", &c.check_updates);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Asks GitHub once a day whether a newer release exists, and says so.\nIt never downloads or installs anything. Off unless you turn it on,\nbecause a check tells a server that someone here is running Firn.");
        {
            // Where generative work goes. An empty list means the feature is
            // off, the same posture as the update check: a request tells a
            // server someone here is running Firn. One server holds one
            // model, so switching model means switching address, and the
            // addresses are a named list rather than a single field.
            ImGui::TextUnformatted("Image model servers");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("stable-diffusion.cpp servers, on this machine or elsewhere.\nOne holds one model, so list the ones you use and pick\nbetween them in the Generate palette. Off while empty.");
            if (c.generate_servers.empty()) ImGui::TextDisabled("None: generation, fill and edit are off.");
            int remove = -1;
            for (size_t i = 0; i < c.generate_servers.size(); ++i) {
                Config::GenServer& g = c.generate_servers[i];
                ImGui::PushID(static_cast<int>(i));
                char name[128], url[256];
                std::snprintf(name, sizeof name, "%s", g.name.c_str());
                std::snprintf(url, sizeof url, "%s", g.url.c_str());
                ImGui::SetNextItemWidth(130);
                if (ImGui::InputTextWithHint("##name", "name", name, sizeof name)) {
                    g.name = name;
                    g.name.erase(std::remove(g.name.begin(), g.name.end(), '|'), g.name.end());
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(260);
                if (ImGui::InputTextWithHint("##url", "http://127.0.0.1:1234", url, sizeof url)) {
                    // The address in use follows the entry being edited, so
                    // correcting a typo does not silently leave the old one
                    // selected.
                    if (c.generate_url == g.url) c.generate_url = url;
                    g.url = url;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(g.url.empty());
                if (ImGui::SmallButton("Test")) {
                    std::string err;
                    const std::string who = firn::genhttp::probe(g.url, &err);
                    if (who.empty()) fail(g.name + ": " + err); else say(g.name + " answered: " + who);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
                ImGui::PopID();
            }
            if (remove >= 0) {
                const std::string gone = c.generate_servers[static_cast<size_t>(remove)].url;
                c.generate_servers.erase(c.generate_servers.begin() + remove);
                if (c.generate_url == gone)
                    c.generate_url = c.generate_servers.empty() ? std::string() : c.generate_servers.front().url;
            }
            if (ImGui::SmallButton("Add server")) c.generate_servers.push_back({"", ""});
        }
        ImGui::SetNextItemWidth(160); ImGui::InputInt("New image width", &c.new_width);
        ImGui::SetNextItemWidth(160); ImGui::InputInt("New image height", &c.new_height);
        c.new_width = std::clamp(c.new_width, 1, 30000); c.new_height = std::clamp(c.new_height, 1, 30000);
        ImGui::SeparatorText("View");
        {
            ensure_themes();
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Theme", c.theme.c_str())) {
                for (const Theme& t : themes)
                    if (ImGui::Selectable((t.name + (t.builtin ? "" : "  (yours)")).c_str(), t.name == c.theme)) { c.theme = t.name; apply_theme(c.theme); }  // previews live; Cancel restores
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Edit Themes...")) { config.theme = c.theme; open_theme_editor(); }
            static const float scales[] = {0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
            int cur = 0;
            for (int i = 1; i < 8; ++i) if (std::abs(c.ui_scale - scales[i]) < 0.01f) cur = i;
            char auto_label[48];
            std::snprintf(auto_label, sizeof(auto_label), "Automatic (%d%%)", static_cast<int>(auto_ui_scale * 100 + 0.5f));
            const char* labels[] = {auto_label, "100%", "125%", "150%", "175%", "200%", "250%", "300%"};
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("UI scale", &cur, labels, 8)) { c.ui_scale = scales[cur]; config.ui_scale = c.ui_scale; apply_theme(c.theme); }  // previews live
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colors, shape, font and text size; save, import and export themes");
        }
        ImGui::Checkbox("Rulers", &c.show_rulers); ImGui::SameLine(); ImGui::Checkbox("Grid", &c.show_grid);
        ImGui::Checkbox("Color managed display (convert tagged images to sRGB for the screen)", &c.color_managed_display);
        ImGui::SetNextItemWidth(160); ImGui::InputInt("Grid spacing", &c.grid_spacing);
        c.grid_spacing = std::clamp(c.grid_spacing, 1, 1000);
        ImGui::SeparatorText(("Extra library folders (besides " + Config::directory() + "/*)").c_str());
        char buf[1024];
        auto path_field = [&](const char* label, std::string& value) {
            std::snprintf(buf, sizeof(buf), "%s", value.c_str());
            ImGui::SetNextItemWidth(360);
            if (ImGui::InputText(label, buf, sizeof(buf))) value = buf;
        };
        path_field("Picture tubes", c.extra_tube_dir);
        path_field("Brush tips", c.extra_brush_dir);
        path_field("Textures", c.extra_texture_dir);
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            const std::string keep_dir = config.last_directory;
            const auto keep_recent = config.recent_files;
            config = c;
            config.last_directory = keep_dir;
            config.recent_files = keep_recent;
            apply_config();
            config.save();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { config.ui_scale = menu_state->prefs_scale_before; apply_theme(config.theme); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Save Selection To Alpha Channel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("Name", alpha_name_buf, sizeof(alpha_name_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Saved with the image in the native format.");
        if ((ImGui::Button("OK") || entered) && doc && doc->has_selection()) {
            {
                std::vector<Document::AlphaChannel> next = doc->alpha_channels();
                next.push_back({alpha_name_buf, doc->selection()});
                run(std::make_unique<AlphaChannelCommand>("Save Selection", std::move(next)));
            }
            status = std::string("Saved selection as alpha channel \"") + alpha_name_buf + "\"";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Image Information", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        MenuState& ms = *menu_state;
        if (doc && !ms.meta_loaded) { ms.meta_edit = doc->metadata(); ms.meta_row = -1; ms.meta_loaded = true; }
        if (doc && ImGui::BeginTabBar("info_tabs")) {
            if (ImGui::BeginTabItem("Image")) {
                size_t rasters = 0, groups = 0, masks = 0;
                for (size_t i = 0; i < doc->layer_count(); ++i) {
                    rasters += doc->layer(i).is_raster();
                    groups += doc->layer(i).type == LayerType::Group;
                    masks += doc->layer(i).has_mask();
                }
                const double mb = static_cast<double>(doc->width()) * doc->height() * 4 * rasters / (1024.0 * 1024.0);
                ImGui::Text("File:        %s", doc_path.empty() ? "(unsaved)" : doc_path.c_str());
                ImGui::Text("Dimensions:  %d x %d pixels", doc->width(), doc->height());
                ImGui::Text("Layers:      %zu raster, %zu group(s), %zu mask(s)", rasters, groups, masks);
                ImGui::Text("Memory:      %.1f MB of layer pixels", mb);
                ImGui::Text("Selection:   %s", doc->has_selection() ? "yes" : "none");
                ImGui::Text("Depth:       %d bits per channel", doc->bit_depth());
                { const icc::Profile prof = document_profile(); ImGui::Text("Profile:     %s", doc->icc().empty() ? "(untagged)" : prof.description.empty() ? "(unnamed)" : prof.description.c_str()); }
                ImGui::Text("Metadata:    %zu entr%s", ms.meta_edit.size(), ms.meta_edit.size() == 1 ? "y" : "ies");
                ImGui::Text("History:     %zu step(s), %s", history.size(), modified() ? "modified" : "saved");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Metadata")) {
                draw_metadata_tab(*this);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        const bool changed = doc && (!(ms.meta_edit.entries == doc->metadata().entries) || ms.meta_edit.xmp != doc->metadata().xmp);
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Apply", ImVec2(90, 0))) { commit_metadata(*this, ms.meta_edit); ms.meta_loaded = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(changed ? "Cancel" : "Close", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { ms.meta_loaded = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    } else if (menu_state->meta_loaded && !ImGui::IsPopupOpen("Image Information")) {
        menu_state->meta_loaded = false;
    }

    if (ImGui::BeginPopupModal("JPEG Options", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool webp = pending_jpeg_path.size() > 5 && pending_jpeg_path.compare(pending_jpeg_path.size() - 5, 5, ".webp") == 0;
        if (webp) {
            bool lossless = jpeg_quality >= 100;
            if (ImGui::Checkbox("Lossless", &lossless)) jpeg_quality = lossless ? 100 : 90;
            if (!lossless) ImGui::SliderInt("Quality", &jpeg_quality, 1, 99);
            ImGui::TextDisabled("Layers are flattened; transparency is kept.");
        } else {
            ImGui::SliderInt("Quality", &jpeg_quality, 1, 100);
            ImGui::TextDisabled("Layers are flattened; transparency becomes white.");
        }
        if (ImGui::Button("Save") || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            const std::string p = pending_jpeg_path;
            ImGui::CloseCurrentPopup();
            save_document_async(p);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { pending_jpeg_path.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (show_revert_prompt) { ImGui::OpenPopup("Revert"); show_revert_prompt = false; }
    if (ImGui::BeginPopupModal("Revert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Discard the changes to \"%s\" and load the saved file again?", doc_title.c_str());
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Revert", ImVec2(90, 0))) { revert(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (pending_close >= 0 && !ImGui::IsPopupOpen("Unsaved Changes")) ImGui::OpenPopup("Unsaved Changes");

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int idx = pending_close;
        if (idx < 0 || idx >= static_cast<int>(docs.size())) { pending_close = -1; ImGui::CloseCurrentPopup(); }
        else {
            // Show the document being asked about: with several open, the
            // canvas was still on a different one and the name in the
            // question meant nothing.
            if (ImGui::IsWindowAppearing() && idx != current_doc) activate_document(idx);
            const int remaining = [this] { int n = 0; for (int i = 0; i < static_cast<int>(docs.size()); ++i) if (document_modified(i)) ++n; return n; }();
            ImGui::Text("Save changes to \"%s\" before closing?", document_title(idx).c_str());
            if (closing_all && remaining > 1) ImGui::TextDisabled("%d of %d still to answer for.", 1, remaining);
            if (ImGui::Button("Save", ImVec2(90, 0))) {
                activate_document(idx);
                pending_close = -1;
                ImGui::CloseCurrentPopup();
                // A format that keeps everything can be written in place; any
                // other needs the Save As dialog, which is asynchronous, so
                // the close waits for it rather than being abandoned.
                const bool in_place = !doc_path.empty() && (io::is_psp_extension(doc_path) || io::is_ora_extension(doc_path));
                if (in_place && save_document(doc_path)) close_document(idx, true);
                else { pending_close_after_save = idx; request_save_as(); }
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(90, 0))) {
                pending_close = -1;
                ImGui::CloseCurrentPopup();
                close_document(idx, true);
                if (pending_quit) request_quit();
                else if (closing_all && !docs.empty()) close_document(0);
                else closing_all = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                pending_close = -1;
                pending_quit = false;
                closing_all = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (show_layer_props_dialog) { ImGui::OpenPopup("Layer Properties"); show_layer_props_dialog = false; }
    if (show_resize_dialog) { ImGui::OpenPopup("Resize"); show_resize_dialog = false; }
    if (show_canvas_dialog) { ImGui::OpenPopup("Canvas Size"); show_canvas_dialog = false; }
    if (show_rotate_dialog) { ImGui::OpenPopup("Free Rotate"); show_rotate_dialog = false; }

    auto escape = [] { if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup(); };
    // Enter accepts unless a multi-line field has the keyboard.
    auto enter = [] { return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false); };

    if (ImGui::BeginPopupModal("New Image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        MenuState& ms = *menu_state;
        // The original's dialog, in the shape Firn can actually back: a
        // preset list, dimensions in real units against a resolution, what
        // the first layer is, and what it is filled with -- and the size in
        // pixels and in memory, which is the number that decides whether an
        // idea is going to work at all.
        struct Preset { const char* name; int w, h; float dpi; };
        static const Preset presets[] = {
            {"Last used", 0, 0, 0},
            {"1280 x 720 (720p)", 1280, 720, 96},
            {"1920 x 1080 (1080p)", 1920, 1080, 96},
            {"3840 x 2160 (4K)", 3840, 2160, 96},
            {"1024 x 1024", 1024, 1024, 96},
            {"512 x 512", 512, 512, 96},
            {"A4 at 300 dpi", 2480, 3508, 300},
            {"A4 at 150 dpi", 1240, 1754, 150},
            {"Letter at 300 dpi", 2550, 3300, 300},
            {"6 x 4 photo at 300 dpi", 1800, 1200, 300},
            {"Business card at 300 dpi", 1050, 600, 300},
        };
        static const char* unit_names[] = {"Pixels", "Inches", "Centimetres"};

        // Pixels are what the document is made of; the other units are a way
        // of saying how many, through the resolution.
        auto per_unit = [&](int units) {
            const float per_inch = ms.new_res_units == 0 ? ms.new_resolution : ms.new_resolution * 2.54f;
            return units == 1 ? per_inch : units == 2 ? per_inch / 2.54f : 1.0f;
        };
        auto to_pixels = [&](float v) { return std::max(1, static_cast<int>(std::lround(v * per_unit(ms.new_units)))); };

        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Presets", &ms.new_preset, [](void*, int i, const char** out) {
                *out = presets[i].name; return true; }, nullptr, IM_ARRAYSIZE(presets))) {
            const Preset& p = presets[ms.new_preset];
            if (p.w > 0) {
                ms.new_units = 0;
                ms.new_res_units = 0;
                ms.new_resolution = p.dpi;
                ms.new_dim_w = static_cast<float>(p.w);
                ms.new_dim_h = static_cast<float>(p.h);
            } else {
                ms.new_units = 0;
                ms.new_dim_w = static_cast<float>(config.new_width);
                ms.new_dim_h = static_cast<float>(config.new_height);
            }
        }

        ImGui::SeparatorText("Image dimensions");
        const char* fmt = ms.new_units == 0 ? "%.0f" : "%.2f";
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputFloat("Width", &ms.new_dim_w, 0, 0, fmt)) ms.new_preset = 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo("Units", &ms.new_units, unit_names, 3)) {
            // Changing units keeps the picture the same size rather than
            // reinterpreting the number, which would silently resize it.
            const float px_w = ms.new_dim_w * per_unit(ms.new_units == 0 ? 1 : 0);
            (void)px_w;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputFloat("Height", &ms.new_dim_h, 0, 0, fmt)) ms.new_preset = 0;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputFloat("Resolution", &ms.new_resolution, 0, 0, "%.0f")) ms.new_preset = 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("##resunits", &ms.new_res_units, "Pixels/inch\0Pixels/cm\0");
        ms.new_resolution = std::clamp(ms.new_resolution, 1.0f, 10000.0f);
        ms.new_dim_w = std::max(ms.new_dim_w, 0.01f);
        ms.new_dim_h = std::max(ms.new_dim_h, 0.01f);

        const int px_w = std::min(to_pixels(ms.new_dim_w), 30000);
        const int px_h = std::min(to_pixels(ms.new_dim_h), 30000);

        ImGui::SeparatorText("Image characteristics");
        ImGui::RadioButton("Raster background", &ms.new_background, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Vector background", &ms.new_background, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A vector layer above the background, ready to draw shapes on.");
        ImGui::SetNextItemWidth(190);
        ImGui::Combo("Depth", &ms.new_depth, "8 bits a channel\0" "16 bits a channel\0");
        ImGui::Checkbox("Transparent", &ms.new_transparent);
        if (!ms.new_transparent) {
            ImGui::SameLine();
            ImGui::ColorEdit3("Colour", ms.new_color, ImGuiColorEditFlags_NoInputs);
        }

        // What it costs. The original shows this and it is the number that
        // decides whether an idea is going to work at all.
        const double bytes = static_cast<double>(px_w) * px_h * 4.0 * (ms.new_depth == 1 ? 2.0 : 1.0) *
                             (ms.new_background == 1 ? 2.0 : 1.0);
        ImGui::Separator();
        ImGui::Text("%d x %d pixels", px_w, px_h);
        ImGui::SameLine();
        ImGui::TextDisabled(bytes >= 1024.0 * 1024.0 ? "   %.1f MB" : "   %.0f kB",
                            bytes >= 1024.0 * 1024.0 ? bytes / (1024.0 * 1024.0) : bytes / 1024.0);
        if (ms.new_units != 0)
            ImGui::TextDisabled("%.2f x %.2f %s at %.0f %s", ms.new_dim_w, ms.new_dim_h,
                                ms.new_units == 1 ? "in" : "cm", ms.new_resolution,
                                ms.new_res_units == 0 ? "per inch" : "per cm");

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(90, 0)) || enter()) {
            auto c = [](float f) { return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
            const Color fill = ms.new_transparent ? Color{0, 0, 0, 0}
                                                  : Color{c(ms.new_color[0]), c(ms.new_color[1]), c(ms.new_color[2]), 255};
            new_document(px_w, px_h, fill, ms.new_background == 1, ms.new_depth == 1 ? 16 : 8);
            config.new_width = px_w;
            config.new_height = px_h;
            new_w = px_w;
            new_h = px_h;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    draw_selection_dialogs();

    if (ImGui::BeginPopupModal("Layer Properties", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        LayerProps& p = layer_props_edit;
        char name[256];
        std::snprintf(name, sizeof(name), "%s", p.name.c_str());
        if (ImGui::InputText("Name", name, sizeof(name))) p.name = name;
        ImGui::Checkbox("Layer is visible", &p.visible);
        ImGui::Checkbox("Lock transparency", &p.lock_alpha);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The clear parts of the layer stay clear: painting and fills\nonly touch pixels that are already there.");
        float op = p.opacity * 100.0f;
        if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) p.opacity = op / 100.0f;
        const bool is_group_layer = doc && active_layer() >= 0 && doc->layer(active_layer()).type == LayerType::Group;
        if (is_group_layer) {
            ImGui::Checkbox("Pass through", &p.pass_through);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Adjustment and filter layers inside the group act on the whole image below it.\nThe group's blend mode and layer style do not apply while this is on.");
        }
        ImGui::BeginDisabled(is_group_layer && p.pass_through);
        ImGui::SetNextItemWidth(160);
        blend_combo("Blend mode", p.blend);
        ImGui::EndDisabled();
        if (ImGui::CollapsingHeader("Blend Ranges")) {
            ImGui::TextDisabled("Limit the layer to a range of tones instead of painting a mask.");
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("Channel", blend_channel_name(p.ranges.channel))) {
                for (int c = 0; c < 4; ++c) {
                    const auto ch = static_cast<BlendRanges::Channel>(c);
                    if (ImGui::Selectable(blend_channel_name(ch), p.ranges.channel == ch)) p.ranges.channel = ch;
                }
                ImGui::EndCombo();
            }
            // Four stops per range: hidden below the first, fully shown from
            // the second to the third, hidden again past the fourth. Stops
            // stay in order so a range never turns inside out.
            auto range_row = [](const char* label, const char* tip, BlendRange& r) {
                int v[4] = {r.low0, r.low1, r.high1, r.high0};
                ImGui::SetNextItemWidth(260);
                const bool changed = ImGui::DragInt4(label, v, 1.0f, 0, 255);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nHidden, fading in, fading out, hidden.", tip);
                if (changed) {
                    for (int& x : v) x = std::clamp(x, 0, 255);
                    for (int i = 1; i < 4; ++i) v[i] = std::max(v[i], v[i - 1]);
                    r.low0 = static_cast<uint8_t>(v[0]); r.low1 = static_cast<uint8_t>(v[1]);
                    r.high1 = static_cast<uint8_t>(v[2]); r.high0 = static_cast<uint8_t>(v[3]);
                }
                return changed;
            };
            range_row("This layer", "Which of this layer's own tones show.", p.ranges.source);
            range_row("Underlying", "Which tones underneath let this layer show.", p.ranges.under);
            ImGui::BeginDisabled(p.ranges.identity());
            if (ImGui::Button("Reset Ranges")) p.ranges = BlendRanges{};
            ImGui::EndDisabled();
        }
        if (ImGui::Button("OK") || enter()) {
            if (doc && active_layer() >= 0) layer_set_props(doc->props(active_layer()), p);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Resize", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        const float aspect = doc ? static_cast<float>(doc->width()) / doc->height() : 1.0f;
        ImGui::RadioButton("Pixels", &menu_state->resize_by_percent, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Percent", &menu_state->resize_by_percent, 1);
        if (menu_state->resize_by_percent) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("%", &resize_pct, 1.0f, 10.0f, "%.1f") && doc) {
                resize_pct = std::max(resize_pct, 0.1f);
                resize_w = std::max(1, static_cast<int>(doc->width() * resize_pct / 100.0f + 0.5f));
                resize_h = std::max(1, static_cast<int>(doc->height() * resize_pct / 100.0f + 0.5f));
            }
        } else {
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("Width", &resize_w)) {
                resize_w = std::max(1, resize_w);
                if (resize_lock) resize_h = std::max(1, static_cast<int>(resize_w / aspect + 0.5f));
            }
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("Height", &resize_h)) {
                resize_h = std::max(1, resize_h);
                if (resize_lock) resize_w = std::max(1, static_cast<int>(resize_h * aspect + 0.5f));
            }
            ImGui::Checkbox("Lock aspect ratio", &resize_lock);
        }
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Resample", &menu_state->resize_filter, "Pixel resize\0Bilinear\0Bicubic\0Edge directed\0Smart size\0Lanczos\0Mitchell\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smart size picks for you: Lanczos when reducing or enlarging a little, edge directed past a doubling. Edge directed follows edges instead of averaging across them, which keeps diagonals and curves clean, and is slower. Lanczos is the sharpest for photographs and can ring on hard edges; Mitchell is soft and never rings.");
        ImGui::Text("%d x %d  ->  %d x %d", doc ? doc->width() : 0, doc ? doc->height() : 0, resize_w, resize_h);
        if (ImGui::Button("OK") || enter()) {
            if (doc && (resize_w != doc->width() || resize_h != doc->height())) {
                tool().cancel(*this);
                run(std::make_unique<ResizeCommand>(resize_w, resize_h, static_cast<raster::Filter>(menu_state->resize_filter)));
                fit_requested = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Canvas Size", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Width", &canvas_w)) canvas_w = std::max(1, canvas_w);
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Height", &canvas_h)) canvas_h = std::max(1, canvas_h);
        ImGui::TextUnformatted("Placement");
        for (int i = 0; i < 9; ++i) {
            if (i % 3) ImGui::SameLine();
            ImGui::PushID(i);
            const bool sel = canvas_anchor == i;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::Button(sel ? "o" : " ", ImVec2(28, 28))) canvas_anchor = i;
            if (sel) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::TextDisabled("Background layers are padded with the background color.");
        if (ImGui::Button("OK") || enter()) {
            if (doc && (canvas_w != doc->width() || canvas_h != doc->height())) {
                const int dx = canvas_w - doc->width(), dy = canvas_h - doc->height();
                const int ox = (canvas_anchor % 3) * dx / 2, oy = (canvas_anchor / 3) * dy / 2;
                tool().cancel(*this);
                run(std::make_unique<CanvasSizeCommand>(canvas_w, canvas_h, ox, oy, background_fill()));
                fit_requested = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Free Rotate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        escape();
        ImGui::RadioButton("Right (clockwise)", &menu_state->rotate_cw, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Left", &menu_state->rotate_cw, 0);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Degrees", &menu_state->rotate_degrees, 0.0f, 359.99f, "%.2f");
        ImGui::TextDisabled("Uncovered corners take the background color on Background layers.");
        if (ImGui::Button("OK") || enter()) { rotate(menu_state->rotate_cw ? menu_state->rotate_degrees : -menu_state->rotate_degrees); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// File > Export > Picture Tube. A tube is the image divided into a grid of
// equal cells, so the grid is offered as divisors of the image rather than
// as free numbers: a cell size that does not tile the image exactly would
// make the tool stamp slivers of its neighbours.
void App::draw_tube_export_dialog() {
    if (show_tube_export_dialog) { ImGui::OpenPopup("Export Picture Tube"); show_tube_export_dialog = false; }
    if (!ImGui::BeginPopupModal("Export Picture Tube", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!doc) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    io::TubeInfo& t = menu_state->tube_export;
    if (!menu_state->tube_export_ready) {
        menu_state->tube_export_ready = true;
        t = io::TubeInfo{};
        t.columns = t.rows = t.total = 1;
        t.step = std::max(doc->width(), doc->height());
        t.placement = 1;
        t.selection = 1;
    }
    ImGui::Text("Image %d x %d", doc->width(), doc->height());
    bool grid_changed = false;
    ImGui::SetNextItemWidth(120);
    grid_changed |= ImGui::InputInt("Cells across", &t.columns);
    ImGui::SetNextItemWidth(120);
    grid_changed |= ImGui::InputInt("Cells down", &t.rows);
    t.columns = std::clamp(t.columns, 1, std::max(1, doc->width()));
    t.rows = std::clamp(t.rows, 1, std::max(1, doc->height()));
    const bool fits = doc->width() % t.columns == 0 && doc->height() % t.rows == 0;
    if (grid_changed) {
        t.total = t.columns * t.rows;
        if (fits) t.step = std::max(doc->width() / t.columns, doc->height() / t.rows);
    }
    if (fits) ImGui::TextDisabled("Cell size %d x %d", doc->width() / t.columns, doc->height() / t.rows);
    else ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%d x %d does not divide into %d x %d cells.", doc->width(), doc->height(), t.columns, t.rows);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Cells used", &t.total);
    t.total = std::clamp(t.total, 1, t.columns * t.rows);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Step (pixels)", &t.step);
    t.step = std::clamp(t.step, 1, 10000);
    int placement = t.placement == 2 ? 1 : 0;
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("Placement", &placement, "Random\0Continuous\0")) t.placement = placement == 1 ? 2 : 1;
    int selection = std::clamp(t.selection - 1, 0, 4);
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("Selection", &selection, "Random\0Incremental\0Angular\0Pressure\0Velocity\0")) t.selection = selection + 1;
    ImGui::Separator();
    ImGui::BeginDisabled(!fits);
    const bool save = ImGui::Button("Save As...", ImVec2(100, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (save) {
        std::string name = doc_path.empty() ? std::string("Tube") : doc_path;
        if (const size_t sl = name.find_last_of("/\\"); sl != std::string::npos) name = name.substr(sl + 1);
        if (const size_t dot = name.rfind('.'); dot != std::string::npos) name = name.substr(0, dot);
        file_op = PendingFileOp::ExportTube;
        file_dialog.open(FileDialog::Mode::Save, "Export Picture Tube", {"psptube"}, name + ".psptube");
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Edit > Generative Fill. The prompt is all Firn asks for; everything else
// about the model is the server's business, which is the point of treating
// it as a service rather than a feature.
void App::draw_generate_dialog() {
    const char* title = menu_state->generate_whole ? "Generative Edit" : "Generative Fill";
    if (show_generate_dialog) { ImGui::OpenPopup(title); show_generate_dialog = false; }
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!doc) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::TextDisabled(menu_state->generate_whole
                            ? "The whole layer is replaced by the model's answer."
                            : "The selection is replaced; the rest of the picture is left alone.");
    ImGui::SetNextItemWidth(420);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool entered = ImGui::InputTextWithHint("##prompt", menu_state->generate_whole ? "What to change" : "What should be there", menu_state->generate_prompt,
                                                  sizeof menu_state->generate_prompt, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("%s", config.generate_url.c_str());
    ImGui::Separator();
    const bool go = ImGui::Button(menu_state->generate_whole ? "Edit" : "Fill", ImVec2(90, 0)) || entered;
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (go) {
        if (menu_state->generate_whole) generative_edit(menu_state->generate_prompt);
        else generative_fill(menu_state->generate_prompt);
        ImGui::CloseCurrentPopup();
    }
    else if (cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
