#pragma once
// State of Palettes.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct PaletteState {
    ImGuiTextFilter tool_filter;
    float options_content_height = 0;
    firn::LayerProps layer_props_before; // props at the start of a live slider drag
    int rename_layer = -1;
    size_t history_shown = 0;   // cursor the History palette last scrolled to
    char rename_buf[128] = {};
};
