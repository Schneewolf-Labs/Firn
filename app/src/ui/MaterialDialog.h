#pragma once
#include "imgui.h"

struct App;

// A material swatch that opens the Material Properties dialog on click and
// swaps the materials on right-click. Returns true when the dialog was opened.
bool material_box(App& app, bool foreground, ImVec2 size);
// The top of the Materials palette: both boxes, swap, transparency, and the
// Frame / Rainbow / Swatches picker tabs (the swatch list itself is drawn by the caller).
void draw_materials_header(App& app);
