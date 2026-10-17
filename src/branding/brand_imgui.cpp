#include "brand_imgui.h"

#include "branding.h"

ImVec4 brand(unsigned colour, float alpha) {
    return ImVec4(((colour >> 16) & 0xff) / 255.0f, ((colour >> 8) & 0xff) / 255.0f,
                  (colour & 0xff) / 255.0f, alpha);
}

void brand_imgui_colours(ImGuiStyle &style) {
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = brand(BRAND_WHITE);
    c[ImGuiCol_TextDisabled] = brand(BRAND_GREY);
    c[ImGuiCol_WindowBg] = brand(BRAND_BLACK);
    c[ImGuiCol_ChildBg] = brand(BRAND_PANEL);
    c[ImGuiCol_PopupBg] = brand(BRAND_PANEL);
    c[ImGuiCol_Border] = brand(BRAND_LINE);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = brand(BRAND_PANEL);
    c[ImGuiCol_FrameBgHovered] = brand(BRAND_RAISED);
    c[ImGuiCol_FrameBgActive] = brand(BRAND_LINE);
    c[ImGuiCol_TitleBg] = brand(BRAND_BLACK);
    c[ImGuiCol_TitleBgActive] = brand(BRAND_PANEL);
    c[ImGuiCol_ScrollbarBg] = brand(BRAND_BLACK);
    c[ImGuiCol_ScrollbarGrab] = brand(BRAND_LINE);
    c[ImGuiCol_ScrollbarGrabHovered] = brand(BRAND_GREY);
    c[ImGuiCol_ScrollbarGrabActive] = brand(BRAND_RED);
    c[ImGuiCol_CheckMark] = brand(BRAND_WHITE);
    c[ImGuiCol_CheckboxSelectedBg] = brand(BRAND_RED);
    c[ImGuiCol_TextLink] = brand(BRAND_RED);
    c[ImGuiCol_TreeLines] = brand(BRAND_LINE);
    c[ImGuiCol_DragDropTarget] = brand(BRAND_RED);
    c[ImGuiCol_ResizeGripHovered] = brand(BRAND_RED, 0.6f);
    c[ImGuiCol_ResizeGripActive] = brand(BRAND_RED);
    c[ImGuiCol_PlotLines] = brand(BRAND_GREY);
    c[ImGuiCol_PlotLinesHovered] = brand(BRAND_RED);
    c[ImGuiCol_PlotHistogramHovered] = brand(BRAND_RED_DARK);
    c[ImGuiCol_MenuBarBg] = brand(BRAND_PANEL);
    c[ImGuiCol_TitleBgCollapsed] = brand(BRAND_BLACK);
    c[ImGuiCol_InputTextCursor] = brand(BRAND_WHITE);
    c[ImGuiCol_SliderGrab] = brand(BRAND_RED);
    c[ImGuiCol_SliderGrabActive] = brand(BRAND_RED_DARK);
    c[ImGuiCol_Button] = brand(BRAND_RAISED);
    c[ImGuiCol_ButtonHovered] = brand(BRAND_LINE);
    c[ImGuiCol_ButtonActive] = brand(BRAND_RED_DARK);
    c[ImGuiCol_Header] = brand(BRAND_RED, 0.22f);
    c[ImGuiCol_HeaderHovered] = brand(BRAND_RED, 0.32f);
    c[ImGuiCol_HeaderActive] = brand(BRAND_RED, 0.45f);
    c[ImGuiCol_Separator] = brand(BRAND_LINE);
    c[ImGuiCol_SeparatorHovered] = brand(BRAND_RED);
    c[ImGuiCol_SeparatorActive] = brand(BRAND_RED);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = brand(BRAND_RAISED);
    c[ImGuiCol_TabSelected] = brand(BRAND_PANEL);
    c[ImGuiCol_TabSelectedOverline] = brand(BRAND_RED);
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = brand(BRAND_PANEL);
    c[ImGuiCol_TabDimmedSelectedOverline] = brand(BRAND_RED);
    c[ImGuiCol_PlotHistogram] = brand(BRAND_RED);
    c[ImGuiCol_TableHeaderBg] = brand(BRAND_RAISED);
    c[ImGuiCol_TableBorderStrong] = brand(BRAND_LINE);
    c[ImGuiCol_TableBorderLight] = brand(BRAND_LINE);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = brand(BRAND_WHITE, 0.03f);
    c[ImGuiCol_TextSelectedBg] = brand(BRAND_RED, 0.35f);
    c[ImGuiCol_NavCursor] = brand(BRAND_RED);
    c[ImGuiCol_ModalWindowDimBg] = brand(BRAND_BLACK, 0.7f);
}

ImFont *brand_imgui_font(const struct brand_blob &blob, float size_pixels) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void *)blob.data, (int)blob.size,
                                                     size_pixels, &config);
}
