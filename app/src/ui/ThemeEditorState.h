#pragma once
// State of ThemeEditor.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/raster.h"
#include "firn/vector.h"
#include "imgui.h"

struct ThemeEditorState {
    Theme theme_edit, theme_editor_before;
    std::string theme_edit_from;        // name of the theme the edit started from
    char theme_name_buf[64] = {};
};
