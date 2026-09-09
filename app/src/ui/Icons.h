#pragma once
#include "imgui.h"

// Draws the icon for a tool (by its name()) into a size x size box at p.
void draw_tool_icon(ImDrawList* dl, const char* name, ImVec2 p, float size, ImU32 col);
