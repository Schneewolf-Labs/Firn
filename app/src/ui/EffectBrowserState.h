#pragma once
// State of EffectBrowser.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct EffectBrowserState {
    firn::Image browser_source;
    uint64_t browser_revision = ~0ull;
    int browser_layer = -1;
};
