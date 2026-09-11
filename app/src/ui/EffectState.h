#pragma once
// State of Adjust.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct EffectState {
    int hsl_h = 0, hsl_s = 0, hsl_l = 0;
    int lv_in_lo = 0, lv_in_hi = 255, lv_out_lo = 0, lv_out_hi = 255;
    float lv_gamma = 1.0f, gamma_value = 1.0f;
    int fade_amount = 45;
    float bwp_src_black[3] = {0, 0, 0}, bwp_src_white[3] = {1, 1, 1}, bwp_dst_black[3] = {0, 0, 0}, bwp_dst_white[3] = {1, 1, 1};
    int edge_smooth_amount = 30;        // Edge Preserving Smooth
    int jpeg_strength = 1, jpeg_crispness = 30;
    float ca_red = 0.0f, ca_blue = 0.0f;
    int nr_strength = 50, nr_blend = 70, nr_sharpen = 0;
    int edge_mode = 1; float edge_color[3] = {0, 0, 0};
    int curl_cols = 4, curl_rows = 4, curl_radius = 60, curl_strength = 50;
    float halo_radius = 60.0f, halo_offset = 20.0f; int halo_spikes = 12, halo_bend = 0;
    float warp_cx = 50.0f, warp_cy = 50.0f, warp_size = 50.0f; int warp_strength_fx = 50;
    float pat_angle = 0.0f, pat_cx = 50.0f, pat_cy = 50.0f, pat_scale = 25.0f; int pat_rotation = 0;
    float mirror_angle = 0.0f, mirror_cx = 50.0f, mirror_cy = 50.0f;
    int offset_x = 0, offset_y = 0;
    int tile_method = 0, tile_direction = 0, tile_transition = 50;
    firn::effects::Light fx_lights[5]; int fx_darkness = 40;
    float sun_x = 0.5f, sun_y = 0.5f, sun_brightness = 0.8f, sun_ray_brightness = 0.6f; int sun_rays = 12; float sun_color[3] = {1, 1, 0.9f};
    int threshold_value = 128, posterize_levels = 6, solarize_threshold = 128;
    int mixer_row = 0;
    int curve_drag = -1;
    float usm_radius = 2.0f; int usm_strength = 100, usm_clipping = 0;
    int median_radius = 1;
    float motion_angle = 0.0f; int motion_strength = 10;
    int sepia_amount = 50;
    firn::adjust::HueMap hue_map_params;
    float wave_ha = 5, wave_hw = 40, wave_va = 0, wave_vw = 40;
    int pinch_strength = 50;
    float twirl_degrees = 90;
    int cutout_x = 5, cutout_y = 5; float cutout_opacity = 0.6f, cutout_blur = 5; float cutout_color[3] = {0, 0, 0};
    float ripple_amp = 5, ripple_wave = 30;
    int spherize_strength = 50, lens_strength = 30;
    int halftone_cell = 6; float halftone_angle = 45; float halftone_ink[3] = {0, 0, 0}, halftone_paper[3] = {1, 1, 1};
    int chrome_bands = 4; float chrome_brightness = 1.0f;
    int obevel_width = 8; float obevel_angle = 315, obevel_depth = 1.0f; float obevel_color[3] = {0.7f, 0.7f, 0.7f};
    int box_radius = 3;
};
