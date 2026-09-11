#pragma once
// State of Tools.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct ToolState {
    int retouch_amount = 20;            // % per stroke for lighten/darken, saturation, hue
    int replacer_tolerance = 30;
    int fill_tolerance = 20;
    float fill_opacity = 1.0f;
    int wand_tolerance = 20;
    float redeye_strength = 1.0f;
    float tube_scale = 1.0f;
    int tube_step_override = 0;          // 0 = use the tube's own step
    int tube_placement = 0, tube_selection = 0;  // 0 = as in the file
};
