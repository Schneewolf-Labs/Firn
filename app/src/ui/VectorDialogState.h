#pragma once
// State of VectorDialog.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct VectorDialogState {
    firn::vec::Object vector_props_edit; // dialog working copy (first selected object)
    int vector_props_layer = -1;
    int vector_props_index = -1;
};
