#pragma once
// Parameters of the Adjust and Effects dialogs. They belong to the dialogs
// in app/src/ui/Adjust.cpp and nothing else reads them, so they live here
// rather than on App, which 25 translation units include.
struct AdjustState {
    float cta_color[3] = {1.0f, 1.0f, 1.0f};
    float cta_transparency = 0.0f, cta_opacity = 1.0f;
    int acb_strength = 30, acb_temperature = 6500; bool acb_remove_cast = false;   // the original's factory preset has RemoveColorCast 0
    int ace_bias = 1, ace_strength = 0, ace_appearance = 1;
    int ase_bias = 1, ase_strength = 1; bool ase_skin = false;   // Skintones 0 in the original's factory preset
    int clarify_strength = 2;
    float ha_low = 0.5f, ha_high = 0.5f, ha_gamma = 1.0f; int ha_midtones = 0, ha_channel = 0;
    int sp_size = 3, sp_sensitivity = 15; bool sp_smaller = true, sp_aggressive = false;
    int flash_strength = 40, backlight_strength = 40;
    int fx_amount = 50, fx_detail = 40, fx_blur = 2, fx_density = 50, fx_opacity = 70, fx_luminance = 30, fx_width = 8, fx_depth = 30, fx_smooth = 20, fx_size = 24, fx_border = 20;
    float fx_angle = 315.0f, fx_cx = 50.0f, fx_cy = 50.0f, fx_size_pct = 40.0f;
    float fx_color[3] = {1, 1, 1}, fx_color2[3] = {0, 0, 0};
    int fx_count = 20, fx_min = 10, fx_max = 60; bool fx_bubbles = true;
    int fx_refraction = 60, fx_shading = 50, fx_intensity = 50, fx_sharpness = 50;
    int fx_shape = 0, fx_columns = 12, fx_rows = 12, fx_grout = 20, fx_grout_alpha = 30, fx_curvature = 50, fx_diffusion = 30;
    int fx_length = 12, fx_gap = 3, fx_stroke_width = 4;
    bool fx_horizontal = false, fx_from_left = true, fx_fill_gaps = true;
    float fx_kernel[25] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; float fx_divisor = 1.0f, fx_bias = 0.0f;
    int kal_petals = 6; float kal_angle = 0, kal_radius = 50;
    int bc_brightness = 0, bc_contrast = 0;
};
