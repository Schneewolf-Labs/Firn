#pragma once
// State of AdjustmentLayer.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct AdjustLayerState {
    int adj_layer_index = -1;
    firn::Adjustment adj_before;
    std::string adj_name_before;
};
