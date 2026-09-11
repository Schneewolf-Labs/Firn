#pragma once
// State of Palettes.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct PaletteState {
    firn::LayerProps layer_props_before; // props at the start of a live slider drag
    int rename_layer = -1;
    char rename_buf[128] = {};
};
