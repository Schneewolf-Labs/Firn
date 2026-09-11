#pragma once
// State of SelectionMenu.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct SelectionMenuState {
    int sel_tolerance = 30, sel_softness = 20;
    float sel_color[3] = {1.0f, 1.0f, 1.0f};
    int sel_speck = 10, sel_hole = 10;
    int sel_smooth_amount = 5;
    int sel_defringe = 1;
    int sel_modify_px = 1;
};
