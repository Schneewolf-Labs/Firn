#include <algorithm>
#include <fstream>
#include <memory>

#include "App.h"
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

// Menu structure follows the original's: File, Edit, View, Image, Effects, Adjust,
// Layers, Objects, Selections, Window, Help. Most entries are placeholders
// until the corresponding commands exist.
void App::draw_menu() {
    const bool has_doc = doc != nullptr;
    const int layer = active_layer();
    const bool has_any_layer = has_doc && layer >= 0;
    const bool has_layer = has_any_layer && doc->layer(layer).is_raster();  // pixel operations need a raster layer

    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New...", "Ctrl+N")) show_new_dialog = true;
        if (ImGui::MenuItem("Open...", "Ctrl+O")) request_open();
        if (ImGui::BeginMenu("Recent Files", !config.recent_files.empty())) {
            for (size_t i = 0; i < config.recent_files.size(); ++i) {
                const std::string& r = config.recent_files[i];
                if (ImGui::MenuItem(r.c_str())) { open_document(r); break; }
            }
            ImGui::Separator();
        if (ImGui::MenuItem("Print...", "Ctrl+P", false, has_doc)) show_print_dialog = true;
        ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Close", "Ctrl+W", false, has_doc)) close_document(current_doc);
        if (ImGui::MenuItem("Close All", nullptr, false, has_doc)) { for (int i = static_cast<int>(docs.size()) - 1; i >= 0; --i) if (!document_modified(i)) close_document(i); if (!docs.empty()) close_document(0); }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, has_doc)) save();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, has_doc)) request_save_as();
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences...")) { prefs_edit = config; prefs_scale_before = config.ui_scale; show_prefs_dialog = true; }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) request_quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, has_doc && history.can_undo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, has_doc && history.can_redo())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, has_layer)) cut();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_layer)) copy();
        if (ImGui::MenuItem("Paste As New Image", "Ctrl+V")) paste_as_new_image();
        if (ImGui::MenuItem("Paste As New Layer", "Ctrl+L", false, has_doc)) paste_as_new_layer();
        if (ImGui::MenuItem("Clear", "Delete", false, has_layer)) clear_selection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom In", "+", false, has_doc)) zoom_about(canvas_center, 1.25f);
        if (ImGui::MenuItem("Zoom Out", "-", false, has_doc)) zoom_about(canvas_center, 0.8f);
        if (ImGui::MenuItem("Fit to Window", "Ctrl+0", false, has_doc)) fit_requested = true;
        if (ImGui::MenuItem("Actual Size", "Ctrl+Alt+0", false, has_doc)) { zoom = 1.0f; pan_x = pan_y = 0.0f; }
        ImGui::Separator();
        ImGui::MenuItem("Rulers", nullptr, &show_rulers);
        ImGui::MenuItem("Grid", nullptr, &show_grid);
        ImGui::MenuItem("Guides", nullptr, &show_guides);
        ImGui::MenuItem("Mask Overlay", nullptr, &show_mask_overlay);
        ImGui::MenuItem("Snap to Guides", nullptr, &snap_to_guides);
        ImGui::MenuItem("Snap to Grid", nullptr, &snap_to_grid);
        if (ImGui::MenuItem("Clear Guides", nullptr, false, !guides_h.empty() || !guides_v.empty())) { guides_h.clear(); guides_v.clear(); }
        ImGui::MenuItem("Assistants", nullptr, &show_assistants);
        ImGui::MenuItem("Snap to Assistants", nullptr, &assistant_snap);
        if (ImGui::MenuItem("Clear Assistants", nullptr, false, !assistants.empty())) assistants.clear();
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Grid spacing", &grid_spacing);
        grid_spacing = std::clamp(grid_spacing, 1, 1000);
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &show_imgui_demo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Image")) {
        if (ImGui::MenuItem("Flip", nullptr, false, has_doc)) run(std::make_unique<FlipCommand>());
        if (ImGui::MenuItem("Mirror", nullptr, false, has_doc)) run(std::make_unique<MirrorCommand>());
        if (ImGui::BeginMenu("Rotate", has_doc)) {
            if (ImGui::MenuItem("Rotate Clockwise 90")) rotate(90.0f);
            if (ImGui::MenuItem("Rotate Counter-clockwise 90")) rotate(-90.0f);
            if (ImGui::MenuItem("Rotate 180")) rotate(180.0f);
            if (ImGui::MenuItem("Free Rotate...")) show_rotate_dialog = true;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Crop to Selection", "Ctrl+Shift+R", false, has_doc && doc->has_selection())) crop_to_selection();
        if (ImGui::MenuItem("Resize...", nullptr, false, has_doc)) open_resize_dialog();
        if (ImGui::MenuItem("Canvas Size...", nullptr, false, has_doc)) open_canvas_dialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Add Borders...", nullptr, false, has_doc)) show_borders_dialog = true;
        if (ImGui::MenuItem("Picture Frame...", nullptr, false, has_doc)) show_frame_dialog = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Grayscale", nullptr, false, has_layer)) run(std::make_unique<AdjustCommand>(layer, "Grayscale", raster::grayscale, raster16::grayscale));
        if (ImGui::BeginMenu("Decrease Color Depth", has_layer)) {
            if (ImGui::MenuItem("2 Colors...")) { depth_colors = 2; show_depth_dialog = true; }
            if (ImGui::MenuItem("16 Colors...")) { depth_colors = 16; show_depth_dialog = true; }
            if (ImGui::MenuItem("256 Colors...")) { depth_colors = 256; show_depth_dialog = true; }
            if (ImGui::MenuItem("32K Colors")) image_decrease_depth(32, false);
            if (ImGui::MenuItem("64K Colors")) image_decrease_depth(64, false);
            if (ImGui::MenuItem("8 Bits per Channel", nullptr, false, doc->bit_depth() == 16)) run(std::make_unique<StateEditCommand>("Decrease to 8 Bits per Channel", [](Document& d) { d.set_bit_depth(8); }));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Increase Color Depth", has_doc)) {
            ImGui::TextDisabled("Images are 16 million colors; 16 bits per channel is optional.");
            if (ImGui::MenuItem("16 Bits per Channel", nullptr, false, doc->bit_depth() == 8)) run(std::make_unique<StateEditCommand>("Increase to 16 Bits per Channel", [](Document& d) { d.set_bit_depth(16); }));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Palette", has_layer)) {
            if (ImGui::MenuItem("Load Palette...")) request_load_palette();
            if (ImGui::MenuItem("Save Palette...")) request_save_palette();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Split Channel", has_doc)) {
            if (ImGui::MenuItem("Split to RGB")) image_split_channels(0);
            if (ImGui::MenuItem("Split to HSL")) image_split_channels(1);
            if (ImGui::MenuItem("Split to CMYK")) image_split_channels(2);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Combine Channel", docs.size() >= 3)) {
            if (ImGui::MenuItem("Combine from RGB")) { combine_mode = 0; show_combine_dialog = true; }
            if (ImGui::MenuItem("Combine from HSL")) { combine_mode = 1; show_combine_dialog = true; }
            if (ImGui::MenuItem("Combine from CMYK")) { combine_mode = 2; show_combine_dialog = true; }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Arithmetic...", nullptr, false, docs.size() >= 2)) show_arith_dialog = true;
        ImGui::Separator();
        if (ImGui::BeginMenu("Color Management", has_doc)) {
            const icc::Profile prof = document_profile();
            ImGui::TextDisabled("Profile: %s", doc->icc().empty() ? "(untagged, treated as sRGB)" : prof.description.empty() ? "(unnamed)" : prof.description.c_str());
            if (ImGui::MenuItem("Color Managed Display", nullptr, &color_managed_display)) { config.color_managed_display = color_managed_display; canvas_tex_revision = ~0ull; }
            ImGui::Separator();
            if (ImGui::BeginMenu("Assign Profile")) {
                if (ImGui::MenuItem("sRGB")) assign_profile(icc::encode(icc::srgb(), "sRGB IEC61966-2.1"), "Assign Profile (sRGB)");
                if (ImGui::MenuItem("Adobe RGB (1998)")) assign_profile(icc::encode(icc::adobe_rgb(), "Adobe RGB (1998)"), "Assign Profile (Adobe RGB)");
                if (ImGui::MenuItem("ProPhoto RGB")) assign_profile(icc::encode(icc::prophoto_rgb(), "ProPhoto RGB"), "Assign Profile (ProPhoto RGB)");
                if (ImGui::MenuItem("From File...")) request_load_profile();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Convert to Profile")) {
                if (ImGui::MenuItem("sRGB")) convert_to_profile(icc::srgb(), icc::encode(icc::srgb(), "sRGB IEC61966-2.1"), "Convert to sRGB");
                if (ImGui::MenuItem("Adobe RGB (1998)")) convert_to_profile(icc::adobe_rgb(), icc::encode(icc::adobe_rgb(), "Adobe RGB (1998)"), "Convert to Adobe RGB");
                if (ImGui::MenuItem("ProPhoto RGB")) convert_to_profile(icc::prophoto_rgb(), icc::encode(icc::prophoto_rgb(), "ProPhoto RGB"), "Convert to ProPhoto RGB");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Remove Profile", nullptr, false, !doc->icc().empty())) assign_profile({}, "Remove Profile");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Count Colors Used", nullptr, false, has_doc)) image_count_colors();
        if (ImGui::MenuItem("Image Information...", "Shift+I", false, has_doc)) show_info_dialog = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Adjust")) {
        if (ImGui::MenuItem("Color to Alpha...", nullptr, false, has_layer)) open_adjust = Adj::ColorToAlpha;
        ImGui::Separator();
        if (ImGui::BeginMenu("Brightness and Contrast", has_layer)) {
            if (ImGui::MenuItem("Brightness/Contrast...")) open_adjust = Adj::BrightnessContrast;
            if (ImGui::MenuItem("Curves...")) open_adjust = Adj::Curves;
            if (ImGui::MenuItem("Gamma Correction...")) open_adjust = Adj::Gamma;
            if (ImGui::MenuItem("Histogram Equalize")) run(std::make_unique<AdjustCommand>(layer, "Histogram Equalize", adjust::histogram_equalize));
            if (ImGui::MenuItem("Histogram Stretch")) run(std::make_unique<AdjustCommand>(layer, "Histogram Stretch", adjust::histogram_stretch));
            if (ImGui::MenuItem("Levels...")) open_adjust = Adj::Levels;
            if (ImGui::MenuItem("Threshold...")) open_adjust = Adj::Threshold;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Color Balance", has_layer)) {
            if (ImGui::MenuItem("Channel Mixer...")) open_adjust = Adj::ChannelMixer;
            if (ImGui::MenuItem("Color Balance...")) open_adjust = Adj::ColorBalance;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Hue and Saturation", has_layer)) {
            if (ImGui::MenuItem("Colorize...")) open_adjust = Adj::Colorize;
            if (ImGui::MenuItem("Hue Map...")) open_adjust = Adj::HueMap;
            if (ImGui::MenuItem("Hue/Saturation/Lightness...")) open_adjust = Adj::HSL;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Add/Remove Noise", has_layer)) {
            if (ImGui::MenuItem("Add Noise...")) open_adjust = Adj::AddNoise;
            if (ImGui::MenuItem("Median Filter...")) open_adjust = Adj::Median;
            if (ImGui::MenuItem("Despeckle")) run(std::make_unique<AdjustCommand>(layer, "Despeckle", [](Image& i) { effects::median(i, 1); }));
            if (ImGui::MenuItem("Salt and Pepper Filter...")) open_adjust = Adj::SaltPepper;
            if (ImGui::MenuItem("JPEG Artifact Removal...")) open_adjust = Adj::JpegArtifacts;
            if (ImGui::MenuItem("Digital Camera Noise Removal...")) open_adjust = Adj::NoiseRemoval;
            if (ImGui::MenuItem("Erode")) run(std::make_unique<AdjustCommand>(layer, "Erode", effects::erode));
            if (ImGui::MenuItem("Dilate")) run(std::make_unique<AdjustCommand>(layer, "Dilate", effects::dilate));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Blur", has_layer)) {
            if (ImGui::MenuItem("Average...")) open_adjust = Adj::Average;
            if (ImGui::MenuItem("Blur More")) run(std::make_unique<AdjustCommand>(layer, "Blur More", effects::blur_more));
            if (ImGui::MenuItem("Gaussian Blur...")) open_adjust = Adj::Gaussian;
            if (ImGui::MenuItem("Motion Blur...")) open_adjust = Adj::MotionBlur;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Sharpness", has_layer)) {
            if (ImGui::MenuItem("Sharpen")) run(std::make_unique<AdjustCommand>(layer, "Sharpen", effects::sharpen));
            if (ImGui::MenuItem("Sharpen More")) run(std::make_unique<AdjustCommand>(layer, "Sharpen More", effects::sharpen_more));
            if (ImGui::MenuItem("Unsharp Mask...")) open_adjust = Adj::UnsharpMask;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Softness", has_layer)) {
            if (ImGui::MenuItem("Soften")) run(std::make_unique<AdjustCommand>(layer, "Soften", effects::soften));
            if (ImGui::MenuItem("Soften More")) run(std::make_unique<AdjustCommand>(layer, "Soften More", effects::soften_more));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Photo Fix", has_layer)) {
            if (ImGui::MenuItem("One Step Photo Fix")) run(std::make_unique<AdjustCommand>(layer, "One Step Photo Fix", photo::one_step_photo_fix));
            if (ImGui::MenuItem("Automatic Color Balance...")) open_adjust = Adj::AutoColor;
            if (ImGui::MenuItem("Automatic Contrast Enhancement...")) open_adjust = Adj::AutoContrast;
            if (ImGui::MenuItem("Automatic Saturation Enhancement...")) open_adjust = Adj::AutoSaturation;
            if (ImGui::MenuItem("Clarify...")) open_adjust = Adj::Clarify;
            if (ImGui::MenuItem("Fade Correction...")) open_adjust = Adj::FadeCorrection;
            ImGui::TextDisabled("Red-eye: use the Red-eye Removal tool.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Photo Fix (more)", has_layer)) {
            if (ImGui::MenuItem("Black and White Points...")) open_adjust = Adj::BlackWhitePoints;
            if (ImGui::MenuItem("Histogram Adjustment...")) open_adjust = Adj::HistogramAdjust;
            if (ImGui::MenuItem("Fill Flash...")) open_adjust = Adj::FillFlash;
            if (ImGui::MenuItem("Backlighting...")) open_adjust = Adj::Backlighting;
            if (ImGui::MenuItem("Chromatic Aberration Removal...")) open_adjust = Adj::ChromaticAberration;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Negative Image", "Ctrl+I", false, has_layer))
            run(std::make_unique<InvertCommand>(layer));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Effects")) {
        if (ImGui::MenuItem("Effect Browser...", nullptr, false, has_layer)) { reset_effect_browser(); show_effect_browser = true; }
        ImGui::Separator();
        if (ImGui::BeginMenu("3D Effects", has_layer)) {
            if (ImGui::MenuItem("Buttonize...")) open_adjust = Adj::Buttonize;
            if (ImGui::MenuItem("Cutout...")) open_adjust = Adj::Cutout;
            if (ImGui::MenuItem("Drop Shadow...")) open_adjust = Adj::DropShadow;
            if (ImGui::MenuItem("Inner Bevel...")) open_adjust = Adj::InnerBevel;
            if (ImGui::MenuItem("Outer Bevel...")) open_adjust = Adj::OuterBevel;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Distortion Effects", has_layer)) {
            if (ImGui::MenuItem("Lens Distortion...")) open_adjust = Adj::Lens;
            if (ImGui::MenuItem("Pinch / Punch...")) open_adjust = Adj::Pinch;
            if (ImGui::MenuItem("Ripple...")) open_adjust = Adj::Ripple;
            if (ImGui::MenuItem("Spherize...")) open_adjust = Adj::Spherize;
            if (ImGui::MenuItem("Twirl...")) open_adjust = Adj::Twirl;
            if (ImGui::MenuItem("Wave...")) open_adjust = Adj::Wave;
            ImGui::Separator();
            if (ImGui::MenuItem("Curlicues...")) open_adjust = Adj::Curlicues;
            if (ImGui::MenuItem("Displacement Map...")) open_adjust = Adj::DisplacementMap;
            if (ImGui::MenuItem("Polar Coordinates...")) open_adjust = Adj::PolarCoordinates;
            if (ImGui::MenuItem("Spiky Halo...")) open_adjust = Adj::SpikyHalo;
            if (ImGui::MenuItem("Warp...")) open_adjust = Adj::Warp;
            if (ImGui::MenuItem("Wind...")) open_adjust = Adj::Wind;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Geometric Effects", has_layer)) {
            if (ImGui::MenuItem("Circle...")) open_adjust = Adj::Circle;
            if (ImGui::MenuItem("Cylinder...")) open_adjust = Adj::Cylinder;
            if (ImGui::MenuItem("Pentagon...")) open_adjust = Adj::Pentagon;
            if (ImGui::MenuItem("Perspective...")) open_adjust = Adj::Perspective;
            if (ImGui::MenuItem("Skew...")) open_adjust = Adj::Skew;
            if (ImGui::MenuItem("Spherize...")) open_adjust = Adj::Spherize;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Image Effects", has_layer)) {
            if (ImGui::MenuItem("Offset...")) open_adjust = Adj::Offset;
            if (ImGui::MenuItem("Page Curl...")) open_adjust = Adj::PageCurl;
            if (ImGui::MenuItem("Seamless Tiling...")) open_adjust = Adj::SeamlessTiling;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Art Media Effects", has_layer)) {
            if (ImGui::MenuItem("Black Pencil...")) open_adjust = Adj::BlackPencil;
            if (ImGui::MenuItem("Brush Strokes...")) open_adjust = Adj::BrushStrokes;
            if (ImGui::MenuItem("Charcoal...")) open_adjust = Adj::Charcoal;
            if (ImGui::MenuItem("Colored Chalk...")) open_adjust = Adj::ColoredChalk;
            if (ImGui::MenuItem("Colored Pencil...")) open_adjust = Adj::ColoredPencil;
            if (ImGui::MenuItem("Pencil...")) open_adjust = Adj::Pencil;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Artistic Effects", has_layer)) {
            if (ImGui::MenuItem("Aged Newspaper...")) open_adjust = Adj::AgedNewspaper;
            if (ImGui::MenuItem("Balls and Bubbles...")) open_adjust = Adj::BallsBubbles;
            if (ImGui::MenuItem("Chrome...")) open_adjust = Adj::Chrome;
            if (ImGui::MenuItem("Colored Edges...")) open_adjust = Adj::ColoredEdges;
            if (ImGui::MenuItem("Colored Foil...")) open_adjust = Adj::ColoredFoil;
            if (ImGui::MenuItem("Contours...")) open_adjust = Adj::Contours;
            if (ImGui::MenuItem("Enamel...")) open_adjust = Adj::Enamel;
            if (ImGui::MenuItem("Glowing Edges...")) open_adjust = Adj::GlowingEdges;
            if (ImGui::MenuItem("Halftone...")) open_adjust = Adj::Halftone;
            if (ImGui::MenuItem("Hot Wax Coating...")) open_adjust = Adj::HotWax;
            if (ImGui::MenuItem("Magnifying Lens...")) open_adjust = Adj::MagnifyingLens;
            if (ImGui::MenuItem("Neon Glow...")) open_adjust = Adj::NeonGlow;
            if (ImGui::MenuItem("Posterize...")) open_adjust = Adj::Posterize;
            if (ImGui::MenuItem("Sepia Toning...")) open_adjust = Adj::Sepia;
            if (ImGui::MenuItem("Solarize...")) open_adjust = Adj::Solarize;
            if (ImGui::MenuItem("Topography...")) open_adjust = Adj::Topography;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edge Effects", has_layer)) {
            if (ImGui::MenuItem("Enhance")) run(std::make_unique<AdjustCommand>(layer, "Enhance Edges", effects::enhance_edges));
            if (ImGui::MenuItem("Enhance More")) run(std::make_unique<AdjustCommand>(layer, "Enhance Edges More", effects::enhance_edges_more));
            if (ImGui::MenuItem("Find All")) run(std::make_unique<AdjustCommand>(layer, "Find Edges", effects::find_edges));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Illumination Effects", has_layer)) {
            if (ImGui::MenuItem("Lights...")) open_adjust = Adj::Lights;
            if (ImGui::MenuItem("Sunburst...")) open_adjust = Adj::Sunburst;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Reflection Effects", has_layer)) {
            if (ImGui::MenuItem("Feedback...")) open_adjust = Adj::Feedback;
            if (ImGui::MenuItem("Kaleidoscope...")) open_adjust = Adj::Kaleidoscope;
            if (ImGui::MenuItem("Pattern...")) open_adjust = Adj::Pattern;
            if (ImGui::MenuItem("Rotating Mirror...")) open_adjust = Adj::RotatingMirror;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Texture Effects", has_layer)) {
            if (ImGui::MenuItem("Blinds...")) open_adjust = Adj::Blinds;
            if (ImGui::MenuItem("Emboss")) run(std::make_unique<AdjustCommand>(layer, "Emboss", effects::emboss));
            if (ImGui::MenuItem("Fine Leather...")) open_adjust = Adj::FineLeather;
            if (ImGui::MenuItem("Fur...")) open_adjust = Adj::Fur;
            if (ImGui::MenuItem("Mosaic - Antique...")) open_adjust = Adj::MosaicAntique;
            if (ImGui::MenuItem("Mosaic - Glass...")) open_adjust = Adj::MosaicGlass;
            if (ImGui::MenuItem("Pixelate (Mosaic)...")) open_adjust = Adj::Mosaic;
            if (ImGui::MenuItem("Polished Stone...")) open_adjust = Adj::PolishedStone;
            if (ImGui::MenuItem("Rough Leather...")) open_adjust = Adj::RoughLeather;
            if (ImGui::MenuItem("Sandstone...")) open_adjust = Adj::Sandstone;
            if (ImGui::MenuItem("Sculpture...")) open_adjust = Adj::Sculpture;
            if (ImGui::MenuItem("Soft Plastic...")) open_adjust = Adj::SoftPlastic;
            if (ImGui::MenuItem("Straw Wall...")) open_adjust = Adj::StrawWall;
            if (ImGui::MenuItem("Texture...")) open_adjust = Adj::Texture;
            if (ImGui::MenuItem("Tiles...")) open_adjust = Adj::Tiles;
            if (ImGui::MenuItem("Weave...")) open_adjust = Adj::Weave;
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("User Defined Filter...", nullptr, false, has_layer)) open_adjust = Adj::UserFilter;
        ImGui::EndMenu();
    }
    draw_selections_menu();
    if (ImGui::BeginMenu("Layers")) {
        draw_layer_menu_items();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Objects")) {
        const bool on_vector = has_any_layer && doc->layer(layer).is_vector();
        const size_t nsel = on_vector ? selected_objects().size() : 0;
        bool has_text = false;
        if (on_vector) for (const auto& o : doc->layer(layer).objects) if (o.selected && o.is_text) has_text = true;
        if (ImGui::BeginMenu("Align", nsel > 0)) {
            static const char* items[] = {"Top", "Bottom", "Left", "Right", "Vertical Center", "Horizontal Center", "Center in Canvas", "Horizontal Center in Canvas", "Vertical Center in Canvas"};
            for (int i = 0; i < 9; ++i) {
                if (i == 6) ImGui::Separator();
                if (ImGui::MenuItem(items[i], nullptr, false, i >= 6 || nsel > 1)) object_align(i);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Distribute", nsel > 2)) {
            static const char* items[] = {"Vertical Top", "Vertical Center", "Vertical Bottom", "Horizontal Left", "Horizontal Center", "Horizontal Right", "Space Evenly Vertically", "Space Evenly Horizontally"};
            for (int i = 0; i < 8; ++i) { if (i == 3 || i == 6) ImGui::Separator(); if (ImGui::MenuItem(items[i])) object_distribute(i); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Make Same Size", nsel > 1)) {
            if (ImGui::MenuItem("Height")) object_same_size(0);
            if (ImGui::MenuItem("Width")) object_same_size(1);
            if (ImGui::MenuItem("Both")) object_same_size(2);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Arrange", nsel > 0)) {
            const int n = static_cast<int>(doc->layer(layer).objects.size()) + 1;
            if (ImGui::MenuItem("Bring to Top")) object_arrange(n);
            if (ImGui::MenuItem("Move Up")) object_arrange(1);
            if (ImGui::MenuItem("Move Down")) object_arrange(-1);
            if (ImGui::MenuItem("Send to Bottom")) object_arrange(-n);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Group", nullptr, false, nsel > 1)) object_group();
        if (ImGui::MenuItem("Ungroup", nullptr, false, nsel > 0)) object_ungroup();
        ImGui::Separator();
        if (ImGui::MenuItem("Edit Text...", nullptr, false, has_text)) open_text_edit();
        if (ImGui::BeginMenu("Convert Text to Curves", has_text)) {
            if (ImGui::MenuItem("As Single Shape")) object_text_to_curves(false);
            if (ImGui::MenuItem("As Character Shapes")) object_text_to_curves(true);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Properties...", nullptr, false, nsel > 0)) open_vector_properties();
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", nullptr, false, on_vector)) object_select_all();
        if (ImGui::MenuItem("Select None", nullptr, false, nsel > 0)) object_select_none();
        if (ImGui::MenuItem("Delete", nullptr, false, nsel > 0)) object_delete();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        for (int i = 0; i < static_cast<int>(docs.size()); ++i) {
            const std::string label = document_title(i) + (document_modified(i) ? "*" : "");
            if (ImGui::MenuItem(label.c_str(), nullptr, i == current_doc)) activate_document(i);
        }
        if (docs.empty()) ImGui::MenuItem("(no images open)", nullptr, false, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Tabbed Documents", nullptr, !image_windows)) {
            image_windows = !image_windows;
            config.image_windows = image_windows;
            config.save();
            if (image_windows) arrange_request = Arrange::Cascade;
        }
        const bool can_arrange = image_windows && !docs.empty();
        if (ImGui::MenuItem("Cascade", nullptr, false, can_arrange)) arrange_request = Arrange::Cascade;
        if (ImGui::MenuItem("Tile Horizontally", nullptr, false, can_arrange)) arrange_request = Arrange::TileHorizontally;
        if (ImGui::MenuItem("Tile Vertically", nullptr, false, can_arrange)) arrange_request = Arrange::TileVertically;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts...")) show_shortcuts_dialog = true;
        ImGui::Separator();
        if (ImGui::MenuItem("About Firn...")) show_about_dialog = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}


// The Layers menu body, shared with the Layers palette's context menu.
void App::draw_layer_menu_items() {
    const bool has_doc = doc != nullptr;
    const int layer = active_layer();
    const bool has_any_layer = has_doc && layer >= 0;
    const bool has_layer = has_any_layer && doc->layer(layer).is_raster();
    const bool is_group = has_any_layer && doc->layer(layer).type == LayerType::Group;
    const int n = has_doc ? static_cast<int>(doc->layer_count()) : 0;
    const bool is_bg = has_layer && doc->layer(layer).background;
    if (ImGui::MenuItem("New Raster Layer", nullptr, false, has_doc)) layer_new();
    if (ImGui::MenuItem("New Vector Layer", nullptr, false, has_doc)) layer_new_vector();
    if (ImGui::BeginMenu("New Adjustment Layer", has_doc)) {
        using K = Adjustment::Kind;
        static const K kinds[] = {K::BrightnessContrast, K::ChannelMixer, K::ColorBalance, K::Curves, K::HSL, K::Invert, K::Levels, K::Posterize, K::Threshold};
        for (K k : kinds) if (ImGui::MenuItem(Adjustment::kind_name(k))) layer_new_adjustment(k);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("New Filter Layer", has_doc)) {
        using K = Adjustment::Kind;
        static const K kinds[] = {K::GaussianBlur, K::Average, K::UnsharpMask};
        for (K k : kinds) if (ImGui::MenuItem(Adjustment::kind_name(k))) layer_new_adjustment(k);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("New Layer Group", nullptr, false, has_any_layer)) layer_new_group();
    if (ImGui::BeginMenu("New Mask Layer", has_any_layer)) {
        if (ImGui::MenuItem("Show All")) layer_set_mask("New Mask Layer", Mask(doc->width(), doc->height(), 255));
        if (ImGui::MenuItem("Hide All")) layer_set_mask("New Mask Layer", Mask(doc->width(), doc->height(), 0));
        if (ImGui::MenuItem("From Selection", nullptr, false, doc->has_selection())) layer_mask_from_selection();
        if (ImGui::MenuItem("From Image")) layer_mask_from_image();
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Duplicate", nullptr, false, has_any_layer)) layer_duplicate();
    if (ImGui::MenuItem("Delete", nullptr, false, has_any_layer && n > 1)) layer_delete();
    if (ImGui::MenuItem("Ungroup Layers", nullptr, false, is_group)) layer_ungroup();
    if (ImGui::MenuItem("Properties...", nullptr, false, has_any_layer)) {
        if (doc->layer(layer).is_adjustment()) open_adjustment_dialog(layer, false);
        else open_layer_properties();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Mask", has_any_layer && doc->layer(layer).has_mask())) {
        bool on = doc->layer(layer).mask_enabled;
        if (ImGui::MenuItem("Enable Mask", nullptr, &on)) layer_set_mask(on ? "Enable Mask" : "Disable Mask", doc->layer(layer).mask, on);
        bool editing = mask_edit && static_cast<int>(mask_proxy_layer) == layer;
        if (ImGui::MenuItem("Edit Mask", nullptr, &editing)) set_mask_edit(editing);
        if (ImGui::MenuItem("Invert Mask")) { Mask m = doc->layer(layer).mask; mask::invert(m); layer_set_mask("Invert Mask", std::move(m), doc->layer(layer).mask_enabled); }
        if (ImGui::MenuItem("Delete Mask")) layer_set_mask("Delete Mask", Mask());
        if (ImGui::MenuItem("Load Selection From Mask")) set_selection("Load Selection From Mask", doc->layer(layer).mask);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View", has_any_layer)) {
        if (ImGui::MenuItem("Current Only")) layer_view_only(true);
        if (ImGui::MenuItem("All")) layer_view_only(false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Arrange", has_any_layer)) {
        if (ImGui::MenuItem("Bring to Top")) layer_arrange(n);
        if (ImGui::MenuItem("Move Up")) layer_arrange(+1);
        if (ImGui::MenuItem("Move Down")) layer_arrange(-1);
        if (ImGui::MenuItem("Send to Bottom")) layer_arrange(-n);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Merge", has_any_layer)) {
        if (ImGui::MenuItem("Merge Down", nullptr, false, has_layer && layer > 0 && doc->layer(layer - 1).is_raster() && doc->layer(layer - 1).depth == doc->layer(layer).depth)) layer_merge(0);
        if (ImGui::MenuItem("Merge Visible", nullptr, false, n > 1)) layer_merge(1);
        if (ImGui::MenuItem("Merge All (Flatten)", nullptr, false, n > 1)) layer_merge(2);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Promote Background Layer", nullptr, false, is_bg)) layer_promote_background();
    if (ImGui::MenuItem("Promote Selection to Layer", nullptr, false, has_layer && doc->has_selection())) promote_selection_to_layer(false);
    if (ImGui::MenuItem("Convert to Raster Layer", nullptr, false, has_any_layer && doc->layer(layer).is_vector())) layer_convert_to_raster();
}

void App::draw_dialogs() {
    draw_adjust_dialogs();
    draw_text_dialog();
    draw_vector_dialogs();
    draw_adjustment_layer_dialog();
    draw_image_dialogs();
    draw_material_dialog();
    draw_about_dialog();
    draw_shortcuts_dialog();
    draw_recovery_dialog();
    draw_theme_editor();
    draw_effect_browser();

    if (file_dialog.draw()) {
        if (file_op == PendingFileOp::Open) open_document(file_dialog.path());
        else if (file_op == PendingFileOp::SaveAs) save_document(file_dialog.path());
        else if (file_op == PendingFileOp::LoadSelection) load_selection(file_dialog.path());
        else if (file_op == PendingFileOp::SaveSelection) save_selection(file_dialog.path());
        else if (file_op == PendingFileOp::LoadPalette) load_palette(file_dialog.path());
        else if (file_op == PendingFileOp::SavePalette) save_palette(file_dialog.path());
        else if (file_op == PendingFileOp::SavePdf) print_to_pdf(file_dialog.path(), false);
        else if (file_op == PendingFileOp::ImportTheme) import_theme(file_dialog.path());
        else if (file_op == PendingFileOp::ExportTheme) export_theme(file_dialog.path());
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
        Config& c = prefs_edit;
        ImGui::SeparatorText("General");
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Undo steps per image", &c.undo_limit, 1, 1000);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Undo memory per image (MB)", &c.undo_memory_mb, 64, 16384, "%d", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Autosave every (minutes, 0 = off)", &c.autosave_minutes, 0, 60);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Default JPEG quality", &c.jpeg_quality, 1, 100);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Checkerboard cell (px)", &c.checker_size, 2, 64);
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
        ImGui::SeparatorText("Extra library folders (besides ~/.config/firn/*)");
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
        if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { config.ui_scale = prefs_scale_before; apply_theme(config.theme); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Save Selection To Alpha Channel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("Name", alpha_name_buf, sizeof(alpha_name_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Saved with the image in the native format.");
        if ((ImGui::Button("OK") || entered) && doc && doc->has_selection()) {
            doc->alpha_channels().push_back({alpha_name_buf, doc->selection()});
            status = std::string("Saved selection as alpha channel \"") + alpha_name_buf + "\"";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Image Information", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (doc) {
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
            ImGui::Text("History:     %zu step(s), %s", history.size(), modified() ? "modified" : "saved");
        }
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
            save_document(p);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { pending_jpeg_path.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (pending_close >= 0 && !ImGui::IsPopupOpen("Unsaved Changes")) ImGui::OpenPopup("Unsaved Changes");

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int idx = pending_close;
        if (idx < 0 || idx >= static_cast<int>(docs.size())) { pending_close = -1; ImGui::CloseCurrentPopup(); }
        else {
            ImGui::Text("Save changes to \"%s\" before closing?", document_title(idx).c_str());
            if (ImGui::Button("Save", ImVec2(90, 0))) {
                activate_document(idx);
                pending_close = -1;
                ImGui::CloseCurrentPopup();
                // Saving may need a dialog; close afterwards only if it succeeded in place.
                if (!doc_path.empty() && io::is_psp_extension(doc_path) ? save_document(doc_path) : false) close_document(idx, true);
                else { request_save_as(); pending_quit = false; }
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(90, 0))) {
                pending_close = -1;
                ImGui::CloseCurrentPopup();
                close_document(idx, true);
                if (pending_quit) request_quit();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                pending_close = -1;
                pending_quit = false;
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
        ImGui::InputInt("Width", &new_w);
        ImGui::InputInt("Height", &new_h);
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;
        if (ImGui::Button("OK") || enter()) { new_document(new_w, new_h); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
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
        float op = p.opacity * 100.0f;
        if (ImGui::SliderFloat("Opacity", &op, 0.0f, 100.0f, "%.0f%%")) p.opacity = op / 100.0f;
        ImGui::SetNextItemWidth(160);
        blend_combo("Blend mode", p.blend);
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
        ImGui::RadioButton("Pixels", &resize_by_percent, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Percent", &resize_by_percent, 1);
        if (resize_by_percent) {
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
        ImGui::Combo("Resample", &resize_filter, "Pixel resize\0Bilinear\0Bicubic\0");
        ImGui::Text("%d x %d  ->  %d x %d", doc ? doc->width() : 0, doc ? doc->height() : 0, resize_w, resize_h);
        if (ImGui::Button("OK") || enter()) {
            if (doc && (resize_w != doc->width() || resize_h != doc->height())) {
                tool().cancel(*this);
                run(std::make_unique<ResizeCommand>(resize_w, resize_h, static_cast<raster::Filter>(resize_filter)));
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
        ImGui::RadioButton("Right (clockwise)", &rotate_cw, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Left", &rotate_cw, 0);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Degrees", &rotate_degrees, 0.0f, 359.99f, "%.2f");
        ImGui::TextDisabled("Uncovered corners take the background color on Background layers.");
        if (ImGui::Button("OK") || enter()) { rotate(rotate_cw ? rotate_degrees : -rotate_degrees); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
