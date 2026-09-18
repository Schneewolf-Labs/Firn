#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
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
    // Layer thumbnails: one small texture per layer, rebuilt when that layer
    // changes. Keyed by the layer's identity rather than its index, so
    // restacking does not invalidate every one of them.
    struct LayerThumb { unsigned int tex = 0; uint64_t revision = 0; int w = 0, h = 0; };
    std::map<const void*, LayerThumb> layer_thumbs;
    // Ctrl+K command palette: the menu tree as it was when it opened.
    struct CommandEntry { std::string path, shortcut; bool enabled = true; std::function<void()> action; };
    std::vector<CommandEntry> cmd_entries;
    char cmd_query[128] = {0};
    int cmd_selected = 0;
    char rename_buf[128] = {};
};
