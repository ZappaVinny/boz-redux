/* The BOZ Redux colour scheme for Dear ImGui, shared by the launcher and the in-game overlay. */
#ifndef BOZ_BRAND_IMGUI_H
#define BOZ_BRAND_IMGUI_H

#include "imgui.h"

ImVec4 brand(unsigned colour, float alpha = 1.0f);
void brand_imgui_colours(ImGuiStyle &style);
/* Adds a built-in brand font (branding.h) at size_pixels; the data stays owned by the program. */
ImFont *brand_imgui_font(const struct brand_blob &blob, float size_pixels);

#endif
