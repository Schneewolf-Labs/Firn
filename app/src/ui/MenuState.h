#pragma once
// State of Menu.cpp: nothing else reads it, so it lives
// here rather than on App, which 32 translation units include.
#include "firn/adjust.h"
#include "firn/io_psp.h"
#include "firn/metadata.h"
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
    // Image Information > Metadata: the working copy, and the row being typed into.
    firn::meta::Metadata meta_edit;
    int meta_row = -1;                  // index into meta_edit.entries, -1 for none
    char meta_value[1024] = {0};
    char meta_new_key[80] = {0};
    bool meta_loaded = false;           // meta_edit matches the document
    // File > Export > Picture Tube: the cell grid and how the tool places them.
    char generate_prompt[512] = {0};    // Edit > Generative Fill / Edit
    bool generate_whole = false;        // instruction editing rather than filling a selection
    firn::io::TubeInfo tube_export{};
    bool tube_export_ready = false;     // the grid has been sized to this image
    // File > New. The original's dialog in the shape Firn can actually back:
    // a preset list, dimensions in real units against a resolution, what the
    // first layer is, and what it is filled with.
    int new_preset = 0;
    int new_units = 0;          // 0 pixels, 1 inches, 2 centimetres
    float new_dim_w = 800, new_dim_h = 600;   // in `new_units`
    float new_resolution = 96.0f;
    int new_res_units = 0;      // 0 per inch, 1 per centimetre
    int new_background = 0;     // 0 raster, 1 vector
    int new_depth = 0;          // 0 eight bits a channel, 1 sixteen
    bool new_transparent = false;
    float new_color[3] = {1.0f, 1.0f, 1.0f};
};
