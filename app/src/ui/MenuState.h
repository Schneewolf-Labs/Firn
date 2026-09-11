#pragma once
// State of Menu.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct MenuState {
    Config prefs_edit;                  // working copy while the dialog is open
    float prefs_scale_before = 0.0f;    // UI scale to restore when the dialog is cancelled
    int resize_by_percent = 0;
    int resize_filter = 4;              // raster::Filter; Smart by default, as in the original
    float rotate_degrees = 15.0f;
    int rotate_cw = 1;
};
