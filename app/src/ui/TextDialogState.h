#pragma once
// State of TextDialog.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct TextDialogState {
    int text_vec_layer = -1;             // vector layer the text dialog previews on (create_as_vector)
    int text_vec_index = -1;             // object being previewed there
    char text_buf[2048] = "Text";
    int text_align = 0;
    float text_angle = 0.0f;            // degrees clockwise
    int text_x_offset = 0, text_y_offset = 0;  // placement shift from stroke padding / rotation
    int text_temp_layer = -1;           // preview layer while the dialog is open
    int text_prev_active = -1;
};
