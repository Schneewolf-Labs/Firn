#pragma once
// State of MaterialDialog.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct MaterialDialogState {
    float color_backup[4] = {0, 0, 0, 1};
    int material_tab = 0;               // 0 color, 1 gradient, 2 pattern
    int material_tab_request = -1;      // tab to select on the next frame
    float frame_hue = 0.0f;             // hue chosen on the frame picker's ring
    char html_color[10] = "#000000";   // the material dialog's HTML field
    int gradient_sel_color = -1, gradient_sel_opacity = -1;   // selected stops in the gradient editor
    char gradient_save_name[64] = {};
};
