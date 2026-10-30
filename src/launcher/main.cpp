// BOZ Redux launcher: sets up game files, edits client.ini and starts the game client.
#include "brand_imgui.h"
#include "branding.h"
#include "client_config.h"
#include "game_files.h"
#include "ini_file.h"
#include "mods.h"
#include "os.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace {


struct Launcher {
    std::string root;
    std::string bin_dir;
    GameFiles *files = nullptr;
    IniFile ini;
    bool ini_loaded = false;
    std::string settings_note;

    char apk_path[1024] = "";
    char pack_folder[1024] = "";
    char custom_resolution[32] = "";

    int capturing = -1;  // binding index waiting for a key or mouse press

    std::vector<mod_info> mods;
    std::map<std::string, std::vector<std::string>> mod_conflicts;  // file -> enabled mod ids
    bool mods_scanned = false;
    int mod_selected = -1;

    os::Process game;
    bool game_running = false;
    std::string game_note;
};

ImVec4 rgb(int r, int g, int b) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

const ImVec4 OK_COLOUR = rgb(62, 196, 109);
const ImVec4 WARN_COLOUR = rgb(232, 168, 56);
const ImVec4 ERROR_COLOUR = rgb(255, 92, 98);

// The brand fonts (built in from branding/fonts) and the logo texture.
struct Brand {
    ImFont *body = nullptr;
    ImFont *strong = nullptr;
    ImFont *heading = nullptr;
    SDL_Texture *logo = nullptr;
    int logo_width = 0, logo_height = 0;
};
Brand g_brand;

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(28, 22);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.FrameBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 3.0f;
    style.FramePadding = ImVec2(12, 7);
    style.ItemSpacing = ImVec2(10, 9);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.CellPadding = ImVec2(8, 6);
    style.ScrollbarSize = 12.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DisabledAlpha = 0.38f;
    brand_imgui_colours(style);
}

void load_fonts() {
    g_brand.body = brand_imgui_font(BRAND_FONT_REGULAR, 18.0f);
    g_brand.strong = brand_imgui_font(BRAND_FONT_SEMIBOLD, 18.0f);
    g_brand.heading = brand_imgui_font(BRAND_FONT_HEADING, 18.0f);
    if (!g_brand.body) {
        g_brand.body = ImGui::GetIO().Fonts->AddFontDefault();
    }
    ImGui::GetIO().FontDefault = g_brand.body;
}

SDL_Surface *brand_surface(const struct brand_blob &blob, unsigned char **pixels, int *w, int *h) {
    *pixels = brand_image_decode(&blob, w, h);
    if (!*pixels) {
        return nullptr;
    }
    return SDL_CreateRGBSurfaceWithFormatFrom(*pixels, *w, *h, 32, *w * 4, SDL_PIXELFORMAT_RGBA32);
}

void set_window_icon(SDL_Window *window) {
    unsigned char *pixels = nullptr;
    int w = 0, h = 0;
    if (SDL_Surface *icon = brand_surface(BRAND_ICON, &pixels, &w, &h)) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }
    brand_image_free(pixels);
}

void load_logo(SDL_Renderer *renderer) {
    unsigned char *pixels = nullptr;
    if (SDL_Surface *logo = brand_surface(BRAND_LOGO, &pixels, &g_brand.logo_width,
                                          &g_brand.logo_height)) {
        g_brand.logo = SDL_CreateTextureFromSurface(renderer, logo);
        if (g_brand.logo) {
            SDL_SetTextureBlendMode(g_brand.logo, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_brand.logo, SDL_ScaleModeBest);
        }
        SDL_FreeSurface(logo);
    }
    brand_image_free(pixels);
}

// A section heading: condensed capitals with a red bar in front, then a thin rule.
void section(const char *title) {
    ImGui::Spacing();
    ImGui::PushFont(g_brand.heading, 20.0f);
    ImVec2 at = ImGui::GetCursorScreenPos();
    float height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + 3), ImVec2(at.x + 4, at.y + height - 3),
                                              ImGui::GetColorU32(brand(BRAND_RED)));
    ImGui::SetCursorScreenPos(ImVec2(at.x + 14, at.y));
    std::string upper = title;
    for (char &ch : upper) {
        ch = (char)std::toupper((unsigned char)ch);
    }
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopFont();
    ImGui::Separator();
}

// A button in the brand red, for the main action of a section.
bool primary_button(const char *label, const ImVec2 &size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, brand(BRAND_RED));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(240, 52, 64));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, brand(BRAND_RED_DARK));
    ImGui::PushStyleColor(ImGuiCol_Border, brand(BRAND_RED));
    ImGui::PushFont(g_brand.strong, 0.0f);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return pressed;
}

void status_line(bool ok, const char *good, const char *bad) {
    ImVec2 at = ImGui::GetCursorScreenPos();
    float height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + 6, at.y + height * 0.5f), 4.5f,
                                                ImGui::GetColorU32(ok ? OK_COLOUR : ERROR_COLOUR));
    ImGui::SetCursorScreenPos(ImVec2(at.x + 20, at.y));
    ImGui::TextUnformatted(ok ? good : bad);
}

void ensure_ini(Launcher &l) {
    if (l.ini_loaded) {
        return;
    }
    std::string path = os::join(l.root, "client.ini");
    if (!os::exists(path)) {
        client_config_write_default(path.c_str());
    }
    l.ini_loaded = l.ini.load(path);
}

// A setting value with two decimals ("3.25"), the precision the sliders show.
std::string format_float(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

void save_setting(Launcher &l, const char *section, const char *key, const std::string &value) {
    l.ini.set(section, key, value);
    l.settings_note = l.ini.save() ? "Saved to client.ini." : "Could not write client.ini.";
}

// --- Game files tab -------------------------------------------------------------------------

void game_files_tab(Launcher &l, const GameStatus &status) {
    bool busy = l.files->busy();

    section("1. Game APK");
    if (!status.image) {
        status_line(false, "", "Not installed. Use your own Black Ops Zombies APK, version 1.0.11.");
    } else if (!status.image_checked) {
        status_line(true, "Installed (checking version...)", "");
    } else if (status.image_verified) {
        status_line(true, "Installed: version 1.0.11", "");
    } else {
        ImGui::TextColored(WARN_COLOUR, "Installed, but not the 1.0.11 release. It may not work.");
    }
    ImGui::SetNextItemWidth(-260);
    ImGui::InputTextWithHint("##apk", "Path to com.activision.boz.apk (or drag it onto this window)",
                             l.apk_path, sizeof(l.apk_path));
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !os::has_file_dialogs());
    if (ImGui::Button("Browse...##apk")) {
        std::string picked = os::pick_file("Choose the Black Ops Zombies APK", "Android app", "*.apk");
        if (!picked.empty()) {
            std::snprintf(l.apk_path, sizeof(l.apk_path), "%s", picked.c_str());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !l.apk_path[0]);
    if (primary_button(status.image ? "Reinstall" : "Install")) {
        l.files->install_apk(l.apk_path);
    }
    ImGui::EndDisabled();

    section("2. Data packs");
    for (const PackStatus &pack : status.packs) {
        status_line(pack.present, pack.name, pack.name);
    }
    bool packs_ready = status.packs[0].present && status.packs[1].present;
    ImGui::BeginDisabled(busy || packs_ready);
    if (primary_button("Download from Activision (825 MB)")) {
        l.files->download_packs();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Or import them from a folder (an Android obb folder works):");
    ImGui::SetNextItemWidth(-260);
    ImGui::InputTextWithHint("##packs", "Folder with blackops_etc.dz and blackops_gles1.dz",
                             l.pack_folder, sizeof(l.pack_folder));
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || packs_ready || !os::has_file_dialogs());
    if (ImGui::Button("Browse...##packs")) {
        std::string picked = os::pick_folder("Folder with the data packs");
        if (!picked.empty()) {
            std::snprintf(l.pack_folder, sizeof(l.pack_folder), "%s", picked.c_str());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || packs_ready || !l.pack_folder[0]);
    if (primary_button("Import")) {
        l.files->import_packs(l.pack_folder);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    std::string message = l.files->message();
    if (busy) {
        float progress = l.files->progress();
        ImGui::ProgressBar(progress < 0 ? -1.0f * (float)ImGui::GetTime() : progress,
                           ImVec2(-1, 0), message.empty() ? "Working..." : message.c_str());
    } else if (!message.empty()) {
        ImGui::TextWrapped("%s", message.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button("Open game folder")) {
        os::open_path(l.root);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open saves folder")) {
        os::make_dir(os::join(l.root, "saves"));
        os::open_path(os::join(l.root, "saves"));
    }
}

// --- Settings tab ---------------------------------------------------------------------------

bool combo(const char *label, const std::string &current, const char *const *values,
           const char *const *names, int count, std::string *chosen) {
    int selected = -1;
    for (int i = 0; i < count; ++i) {
        if (current == values[i]) {
            selected = i;
        }
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, selected >= 0 ? names[selected] : current.c_str())) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(names[i], i == selected)) {
                *chosen = values[i];
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void settings_tab(Launcher &l) {
    ensure_ini(l);
    if (!l.ini_loaded) {
        ImGui::TextColored(ERROR_COLOUR, "Could not open client.ini in %s.", l.root.c_str());
        return;
    }
    std::string chosen;
    ImGui::BeginChild("settings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.2f));
    ImGui::PushItemWidth(260);

    section("Display");
    bool fullscreen = l.ini.get("display", "fullscreen", "true") != "false";
    if (ImGui::Checkbox("Fullscreen (F11 toggles in game)", &fullscreen)) {
        save_setting(l, "display", "fullscreen", fullscreen ? "true" : "false");
    }
    {
        const char *values[] = {"on", "adaptive", "off"};
        const char *names[] = {"On (smoothest)", "Adaptive (tear when late)", "Off"};
        if (combo("V-sync", l.ini.get("display", "vsync", "on"), values, names, 3, &chosen)) {
            save_setting(l, "display", "vsync", chosen);
        }
    }
    {
        const char *values[] = {"auto", "30", "60", "120", "144", "165", "240", "0"};
        const char *names[] = {"Match display", "30", "60", "120", "144", "165", "240", "Unlimited"};
        if (combo("Frame rate limit", l.ini.get("display", "fps_limit", "auto"), values, names, 8,
                  &chosen)) {
            save_setting(l, "display", "fps_limit", chosen);
        }
    }
    {
        const char *values[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"};
        if (combo("Render resolution", l.ini.get("display", "resolution", "1280x720"), values,
                  values, 5, &chosen)) {
            save_setting(l, "display", "resolution", chosen);
        }
    }
    {
        const char *values[] = {"fit", "stretch", "none"};
        const char *names[] = {"Fit (keep 16:9)", "Stretch to window", "None"};
#if defined(_WIN32)
        {
            const char *renderers[] = {"auto", "d3d12", "vulkan", "software"};
            const char *renderer_names[] = {"Auto (ANGLE, Direct3D 11)", "Mesa Direct3D 12",
                                            "Mesa Vulkan", "Software (slow, always works)"};
            if (combo("Renderer", l.ini.get("display", "renderer", "auto"), renderers, renderer_names,
                      4, &chosen)) {
                save_setting(l, "display", "renderer", chosen);
            }
        }
#endif
        if (combo("Scaling", l.ini.get("display", "scaling", "fit"), values, names, 3, &chosen)) {
            save_setting(l, "display", "scaling", chosen);
        }
    }
    bool software_cursor = l.ini.get("display", "software_cursor", "false") == "true";
    if (ImGui::Checkbox("Crosshair pointer in menus", &software_cursor)) {
        save_setting(l, "display", "software_cursor", software_cursor ? "true" : "false");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw the port's crosshair instead of the system mouse pointer.\n"
                          "Controllers always show it.");
    }

    section("Mouse");
    float look = (float)std::atof(l.ini.get("input", "look_sensitivity", "3.0").c_str());
    ImGui::SliderFloat("Look sensitivity", &look, 0.2f, 15.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("0.022 degrees per mouse count at 1.0, the same scale as Source and Quake games.");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        save_setting(l, "input", "look_sensitivity", format_float(look));
    } else if (ImGui::IsItemActive()) {
        l.ini.set("input", "look_sensitivity", format_float(look));
    }
    float aim = (float)std::atof(l.ini.get("input", "aim_sensitivity", "1.0").c_str());
    ImGui::SliderFloat("Aiming sensitivity", &aim, 0.1f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Multiplier while aiming down sights. 1.0 feels the same as without aiming\n"
                          "(the zoom is already allowed for).");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        save_setting(l, "input", "aim_sensitivity", format_float(aim));
    } else if (ImGui::IsItemActive()) {
        l.ini.set("input", "aim_sensitivity", format_float(aim));
    }
    bool invert = l.ini.get("input", "invert_y", "false") == "true";
    if (ImGui::Checkbox("Invert vertical look", &invert)) {
        save_setting(l, "input", "invert_y", invert ? "true" : "false");
    }

    section("Mouse in Dead Ops Arcade");
    ImGui::TextDisabled("Dead Ops still steers the game's touch stick with the mouse.");
    int sensitivity = std::atoi(l.ini.get("input", "mouse_sensitivity", "12000").c_str());
    ImGui::SliderInt("Stick sensitivity", &sensitivity, 2000, 60000);
    // Keep the dragged value in memory each frame; write the file once on release.
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        save_setting(l, "input", "mouse_sensitivity", std::to_string(sensitivity));
    } else if (ImGui::IsItemActive()) {
        l.ini.set("input", "mouse_sensitivity", std::to_string(sensitivity));
    }
    {
        const char *values[] = {"stick", "swipe"};
        const char *names[] = {"Stick (recommended)", "Swipe"};
        if (combo("Look mode", l.ini.get("input", "look_mode", "stick"), values, names, 2,
                  &chosen)) {
            save_setting(l, "input", "look_mode", chosen);
        }
    }

    section("Controls (during a match)");
    ImGui::TextDisabled("Click Set, then press a key or mouse button. Separate several with commas.");
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150);
        for (int i = 0; i < CLIENT_BINDING_COUNT; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(CLIENT_BINDINGS[i].label);
            ImGui::TableNextColumn();
            char value[96];
            std::snprintf(value, sizeof(value), "%s",
                          l.ini.get("keys", CLIENT_BINDINGS[i].action, CLIENT_BINDINGS[i].defaults).c_str());
            ImGui::SetNextItemWidth(-1);
            if (l.capturing == i) {
                ImGui::TextColored(brand(BRAND_RED), "Press a key or mouse button...");
            } else if (ImGui::InputText("##value", value, sizeof(value),
                                        ImGuiInputTextFlags_EnterReturnsTrue) ||
                       ImGui::IsItemDeactivatedAfterEdit()) {
                save_setting(l, "keys", CLIENT_BINDINGS[i].action, value);
            }
            ImGui::TableNextColumn();
            if (l.capturing == i) {
                if (ImGui::Button("Cancel")) {
                    l.capturing = -1;
                }
            } else {
                if (ImGui::Button("Set")) {
                    l.capturing = i;
                }
                ImGui::SameLine();
                if (ImGui::Button("Default")) {
                    save_setting(l, "keys", CLIENT_BINDINGS[i].action, CLIENT_BINDINGS[i].defaults);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    section("Troubleshooting");
    bool status = l.ini.get("debug", "status", "false") == "true";
    if (ImGui::Checkbox("Log frame rate and emulator statistics", &status)) {
        save_setting(l, "debug", "status", status ? "true" : "false");
    }
    bool log_files = l.ini.get("debug", "log_files", "false") == "true";
    if (ImGui::Checkbox("Log every file the game opens (for mod makers)", &log_files)) {
        save_setting(l, "debug", "log_files", log_files ? "true" : "false");
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::TextDisabled("%s", l.settings_note.empty() ? "Changes apply the next time the game starts."
                                                      : l.settings_note.c_str());
}

// --- Mods tab ------------------------------------------------------------------------------

void collect_conflict(const char *relative, const char *, void *user) {
    auto *files = static_cast<std::vector<std::string> *>(user);
    std::string key = relative;
    for (char &c : key) {
        c = (char)std::tolower((unsigned char)c);
    }
    size_t slash = key.rfind('/');
    files->push_back(slash == std::string::npos ? key : key.substr(slash + 1));
}

// Files that more than one enabled mod replaces, matched by file name like the game's lookup.
void find_mod_conflicts(Launcher &l) {
    std::map<std::string, std::vector<std::string>> owners;
    for (const mod_info &mod : l.mods) {
        if (!mod.enabled) {
            continue;
        }
        std::vector<std::string> files;
        mods_list_assets(&mod, collect_conflict, &files);
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        for (const std::string &file : files) {
            owners[file].push_back(mod.id);
        }
    }
    l.mod_conflicts.clear();
    for (auto &entry : owners) {
        if (entry.second.size() > 1) {
            l.mod_conflicts[entry.first] = entry.second;
        }
    }
}

void scan_mods(Launcher &l) {
    std::vector<mod_info> found(MODS_MAX);
    std::string order = l.ini.get("mods", "order"), disabled = l.ini.get("mods", "disabled");
    int count = mods_scan(l.root.c_str(), order.c_str(), disabled.c_str(), found.data(), MODS_MAX);
    found.resize(count);
    l.mods = found;
    l.mods_scanned = true;
    if (l.mod_selected >= count) {
        l.mod_selected = count - 1;
    }
    find_mod_conflicts(l);
}

void save_mods(Launcher &l) {
    std::string order, disabled;
    for (const mod_info &mod : l.mods) {
        order += (order.empty() ? "" : ", ") + std::string(mod.id);
        if (!mod.enabled) {
            disabled += (disabled.empty() ? "" : ", ") + std::string(mod.id);
        }
    }
    l.ini.set("mods", "order", order);
    save_setting(l, "mods", "disabled", disabled);
    find_mod_conflicts(l);
}

void mods_tab(Launcher &l) {
    ensure_ini(l);
    if (!l.ini_loaded) {
        ImGui::TextColored(ERROR_COLOUR, "Could not open client.ini in %s.", l.root.c_str());
        return;
    }
    if (!l.mods_scanned) {
        scan_mods(l);
    }
    std::string folder = os::join(l.root, "mods");
    if (ImGui::Button("Open mods folder")) {
        os::make_dir(folder);
        os::open_path(folder);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        scan_mods(l);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Later mods win when two replace the same file.");

    if (l.mods.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("No mods installed. Put each mod in its own folder in %s, with a "
                           "mod.toml and an assets folder, then press Refresh.",
                           folder.c_str());
        return;
    }

    float details_height = ImGui::GetTextLineHeightWithSpacing() * 6;
    ImGui::BeginChild("mod_list", ImVec2(0, -details_height), ImGuiChildFlags_Borders);
    int move_from = -1, move_to = -1;
    if (ImGui::BeginTable("mods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)l.mods.size(); ++i) {
            mod_info &mod = l.mods[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &mod.enabled)) {
                save_mods(l);
            }
            ImGui::TableNextColumn();
            if (ImGui::Selectable(mod.name, l.mod_selected == i,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                l.mod_selected = i;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(mod.version[0] ? mod.version : "-");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                move_from = i, move_to = i - 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == (int)l.mods.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                move_from = i, move_to = i + 1;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move_from >= 0) {
        std::swap(l.mods[move_from], l.mods[move_to]);
        if (l.mod_selected == move_from) {
            l.mod_selected = move_to;
        } else if (l.mod_selected == move_to) {
            l.mod_selected = move_from;
        }
        save_mods(l);
    }
    ImGui::EndChild();

    if (l.mod_selected >= 0 && l.mod_selected < (int)l.mods.size()) {
        const mod_info &mod = l.mods[l.mod_selected];
        ImGui::Text("%s", mod.name);
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s  folder: mods/%s", mod.id, mod.author[0] ? "  by " : "",
                            mod.author, mod.folder);
        if (!mod.has_manifest) {
            ImGui::TextColored(WARN_COLOUR, "No mod.toml: named after its folder.");
        } else if (mod.game[0] && std::strcmp(mod.game, "1.0.11") != 0) {
            ImGui::TextColored(WARN_COLOUR, "Made for game version %s; this client runs 1.0.11.",
                               mod.game);
        }
        if (mod.description[0]) {
            ImGui::TextWrapped("%s", mod.description);
        }
    } else {
        ImGui::TextDisabled("Select a mod to see its details.");
    }
    if (!l.mod_conflicts.empty()) {
        std::string list;
        for (auto &entry : l.mod_conflicts) {
            list += (list.empty() ? "" : ", ") + entry.first + " (";
            for (size_t i = 0; i < entry.second.size(); ++i) {
                list += (i ? ", " : "") + entry.second[i];
            }
            list += ")";
        }
        ImGui::TextColored(WARN_COLOUR, "%d file%s replaced by more than one mod: %s",
                           (int)l.mod_conflicts.size(), l.mod_conflicts.size() == 1 ? "" : "s",
                           list.c_str());
    }
}

// Turns a key or mouse press into a binding while a "Set" button is waiting.
bool capture_binding(Launcher &l, const SDL_Event &event) {
    if (l.capturing < 0) {
        return false;
    }
    std::string name;
    if (event.type == SDL_KEYDOWN) {
        name = SDL_GetScancodeName(event.key.keysym.scancode);
    } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button >= 1 &&
               event.button.button <= 5) {
        // SDL numbers buttons left, middle, right; client.ini uses Mouse1 left, Mouse2 middle,
        // Mouse3 right, which is the same order.
        name = "Mouse" + std::to_string(event.button.button);
        if (event.button.button == SDL_BUTTON_LEFT && ImGui::GetIO().WantCaptureMouse &&
            ImGui::IsAnyItemHovered()) {
            return false;  // the click on Set/Cancel itself
        }
    } else {
        return false;
    }
    if (!name.empty()) {
        save_setting(l, "keys", CLIENT_BINDINGS[l.capturing].action, name);
    }
    l.capturing = -1;
    return true;
}

// --- Play -----------------------------------------------------------------------------------

// Maps that enabled mods add, as "mod id/map name" (the client.ini [mods] start_map value).
std::vector<std::string> mod_maps(const Launcher &l) {
    std::vector<std::string> maps;
    for (const mod_info &mod : l.mods) {
        for (int i = 0; mod.enabled && i < mod.map_count; ++i) {
            maps.push_back(std::string(mod.id) + "/" + mod.maps[i]);
        }
    }
    return maps;
}

std::string map_label(const Launcher &l, const std::string &entry) {
    if (entry.empty()) {
        return "Main menu";
    }
    size_t slash = entry.find('/');
    std::string id = entry.substr(0, slash), map = entry.substr(slash + 1);
    for (const mod_info &mod : l.mods) {
        if (id == mod.id) {
            return mod.map_count > 1 ? std::string(mod.name) + ": " + map : std::string(mod.name);
        }
    }
    return map;
}

// The start_map setting when it still names a map of an enabled mod, else "" (the main menu).
std::string chosen_map(const Launcher &l) {
    std::string entry = l.ini.get("mods", "start_map");
    std::vector<std::string> maps = mod_maps(l);
    return std::find(maps.begin(), maps.end(), entry) != maps.end() ? entry : "";
}

void start_map_picker(Launcher &l, float width) {
    std::string current = chosen_map(l);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##start_map", map_label(l, current).c_str())) {
        std::vector<std::string> entries = mod_maps(l);
        entries.insert(entries.begin(), "");
        for (const std::string &entry : entries) {
            if (ImGui::Selectable(map_label(l, entry).c_str(), entry == current)) {
                save_setting(l, "mods", "start_map", entry);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Where PLAY takes you: the main menu, or straight into a map from a mod.");
    }
}

void start_game(Launcher &l, SDL_Window *window) {
    std::string saves = os::join(l.root, "saves");
    os::make_dir(saves);
    std::string map = chosen_map(l);
    if (!map.empty()) {
        size_t slash = map.find('/');
        // The map's mod starts it once the menu is up (boz.world reads this one-shot setting).
        mods_set_setting(saves.c_str(), map.substr(0, slash).c_str(), "autostart_level",
                         map.substr(slash + 1).c_str());
    }
    std::map<std::string, std::string> env;
    env["HOME"] = saves;
#if !defined(_WIN32)
    if (os::getenv_str("XDG_CACHE_HOME").empty() && !os::getenv_str("HOME").empty()) {
        env["XDG_CACHE_HOME"] = os::join(os::getenv_str("HOME"), ".cache");
    }
#endif
    std::string loader = os::join(l.bin_dir, os::executable_name("codboz_s3e_loader"));
    std::string image = os::join(os::join(l.root, "assets"), "boz.s3e.unpacked");
    if (!os::spawn(l.game, loader, {"--root", l.root, "--run", image}, env,
                   os::join(l.root, "boz-log.txt"), l.root)) {
        l.game_note = "Could not start " + loader + ": " + os::spawn_error();
        return;
    }
    l.game_running = true;
    l.game_note.clear();
    SDL_HideWindow(window);
}

void poll_game(Launcher &l, SDL_Window *window) {
    if (!l.game_running || l.game.running()) {
        return;
    }
    l.game_running = false;
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    l.game_note = l.game.exit_code == 0
                      ? ""
                      : "The game stopped with an error (code " + std::to_string(l.game.exit_code) +
                            "). Details are in boz-log.txt.";
#if defined(_WIN32)
    // 0x80070057 (E_INVALIDARG): the GPU renderer gave up after opening the window (seen with
    // Mesa's Direct3D 12 driver on AMD). Software rendering works everywhere.
    if ((unsigned)l.game.exit_code == 0x80070057u &&
        l.ini.get("display", "renderer", "auto") == "auto") {
        save_setting(l, "display", "renderer", "software");
        l.game_note = "The GPU renderer failed on this PC, so the renderer is now set to Software "
                      "(Settings tab). Press PLAY to try again.";
    }
#endif
}

// Tab labels in the heading face, as the brand's condensed capitals.
bool begin_tab(const char *label, bool select) {
    ImGui::PushFont(g_brand.heading, 19.0f);
    bool open = ImGui::BeginTabItem(label, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
    ImGui::PopFont();
    return open;
}

void header(Launcher &l, SDL_Window *window, const GameStatus &status) {
    const float logo_height = 78.0f;
    const float play_width = 200.0f, play_height = 54.0f, picker_width = 230.0f;
    ImVec2 top = ImGui::GetCursorPos();
    if (g_brand.logo) {
        float logo_width = logo_height * (float)g_brand.logo_width / (float)g_brand.logo_height;
        ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)g_brand.logo), ImVec2(logo_width, logo_height));
        ImGui::SameLine(0, 22);
    }
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(top.y + 16);
    ImGui::PushFont(g_brand.heading, 24.0f);
    ImGui::TextUnformatted("CLASSIC BLACK OPS ZOMBIES");
    ImGui::PopFont();
    ImGui::TextDisabled("The original mobile game, on PC   v%s", BOZ_VERSION);
    ImGui::EndGroup();

    float right = ImGui::GetWindowWidth() - play_width - ImGui::GetStyle().WindowPadding.x;
    float play_y = top.y + (logo_height - play_height) * 0.5f;
    if (!mod_maps(l).empty()) {
        ImGui::SameLine(right - picker_width - 12);
        ImGui::SetCursorPosY(play_y + (play_height - ImGui::GetFrameHeight()) * 0.5f);
        start_map_picker(l, picker_width);
    }
    ImGui::SameLine(right);
    ImGui::SetCursorPosY(play_y);
    ImGui::BeginDisabled(!status.ready() || l.files->busy() || l.game_running);
    ImGui::PushStyleColor(ImGuiCol_Button, brand(BRAND_RED));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(240, 52, 64));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, brand(BRAND_RED_DARK));
    ImGui::PushStyleColor(ImGuiCol_Border, brand(BRAND_RED));
    ImGui::PushFont(g_brand.heading, 30.0f);
    if (ImGui::Button(l.game_running ? "RUNNING" : "PLAY", ImVec2(play_width, play_height))) {
        start_game(l, window);
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(top.y + logo_height + 14);

    // The red rule under the header, as under the logo's wordmark.
    ImVec2 at = ImGui::GetCursorScreenPos();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(viewport->Pos.x, at.y),
                                              ImVec2(viewport->Pos.x + viewport->Size.x, at.y + 2),
                                              ImGui::GetColorU32(brand(BRAND_RED)));
    ImGui::Dummy(ImVec2(0, 8));
}

void footer(Launcher &l) {
    ImGui::Separator();
    ImGui::TextDisabled("BOZ Redux %s  ·  Not affiliated with Activision. Use your own copy of the game.",
                        BOZ_VERSION);
    const char *label = "Open log";
    float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::SmallButton(label)) {
        os::open_path(os::join(l.root, "boz-log.txt"));
    }
}

void draw(Launcher &l, SDL_Window *window) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("BOZ Redux", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    GameStatus status = l.files->status();
    ensure_ini(l);
    if (!l.mods_scanned) {
        scan_mods(l);
    }
    header(l, window, status);
    if (!status.ready()) {
        ImGui::TextColored(WARN_COLOUR, "Set up the game files below to play.");
    }
    if (!l.game_note.empty()) {
        ImGui::TextColored(ERROR_COLOUR, "%s", l.game_note.c_str());
    }

    float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    if (ImGui::BeginTabBar("tabs")) {
        struct {
            const char *label;
            int tab;
        } tabs[] = {{"GAME FILES", 0}, {"SETTINGS", 1}, {"MODS", 2}};
        // BOZ_LAUNCHER_TAB=1 or 2 opens Settings or Mods first (with the screenshot option).
        static int first_tab = std::atoi(os::getenv_str("BOZ_LAUNCHER_TAB").c_str());
        for (auto &tab : tabs) {
            bool select = first_tab > 0 && tab.tab == first_tab;
            if (!begin_tab(tab.label, select)) {
                continue;
            }
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 16));
            ImGui::BeginChild("page", ImVec2(0, -footer_height), ImGuiChildFlags_AlwaysUseWindowPadding);
            if (tab.tab == 0) {
                game_files_tab(l, status);
            } else if (tab.tab == 1) {
                settings_tab(l);
            } else {
                mods_tab(l);
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::EndTabItem();
        }
        first_tab = 0;
        ImGui::EndTabBar();
    }
    footer(l);
    ImGui::End();
}

void save_screenshot(SDL_Renderer *renderer, const std::string &path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (shot && SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels,
                                     shot->pitch) == 0) {
        SDL_SaveBMP(shot, path.c_str());
    }
    SDL_FreeSurface(shot);
}

#if !defined(_WIN32)
// Prefer native Wayland when the 32-bit keyboard library SDL needs for it is installed.
void choose_video_driver() {
    if (!os::getenv_str("SDL_VIDEODRIVER").empty() || os::getenv_str("WAYLAND_DISPLAY").empty()) {
        return;
    }
    void *xkb = dlopen("libxkbcommon.so.0", RTLD_LAZY);
    if (xkb) {
        dlclose(xkb);
        setenv("SDL_VIDEODRIVER", "wayland", 0);
    }
}
#endif

}  // namespace

int main(int argc, char **argv) {
    Launcher l;
    l.bin_dir = os::exe_dir();
    l.root = l.bin_dir;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--root") && i + 1 < argc) {
            l.root = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--root GAME_DIR]\n", argv[0]);
            return 2;
        }
    }
#if !defined(_WIN32)
    choose_video_driver();
    // The window class the desktop shortcut names (StartupWMClass), so the taskbar shows the
    // BOZ Redux icon; the game inherits it.
    setenv("SDL_VIDEO_X11_WMCLASS", "boz-redux", 0);
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "boz-redux", 0);
#endif

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    SDL_Window *window = SDL_CreateWindow(
        "BOZ Redux", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1080, 760,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 900, 620);
    set_window_icon(window);
    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    apply_style();
    load_fonts();
    load_logo(renderer);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    GameFiles files(l.root, l.bin_dir);
    l.files = &files;

    // BOZ_LAUNCHER_SCREENSHOT=file.bmp: save the fourth frame and quit (documentation, checks).
    std::string screenshot = os::getenv_str("BOZ_LAUNCHER_SCREENSHOT");
    int frames = 0;
    bool done = false;
    while (!done) {
        SDL_Event event;
        // Idle politely: wake for input, or every 50 ms to animate progress.
        if (SDL_WaitEventTimeout(&event, l.game_running ? 250 : 50)) {
            do {
                if (capture_binding(l, event)) {
                    continue;
                }
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT) {
                    done = !l.game_running;
                } else if (event.type == SDL_DROPFILE) {
                    std::string dropped = event.drop.file;
                    SDL_free(event.drop.file);
                    if (dropped.size() > 4 && dropped.compare(dropped.size() - 4, 4, ".apk") == 0) {
                        std::snprintf(l.apk_path, sizeof(l.apk_path), "%s", dropped.c_str());
                    } else {
                        std::snprintf(l.pack_folder, sizeof(l.pack_folder), "%s", dropped.c_str());
                    }
                }
            } while (SDL_PollEvent(&event));
        }
        poll_game(l, window);
        if (l.game_running) {
            continue;
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw(l, window);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, (BRAND_BLACK >> 16) & 0xff, (BRAND_BLACK >> 8) & 0xff, BRAND_BLACK & 0xff, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!screenshot.empty() && ++frames == 4) {
            save_screenshot(renderer, screenshot);
            done = true;
        }
        SDL_RenderPresent(renderer);
    }

    if (g_brand.logo) {
        SDL_DestroyTexture(g_brand.logo);
    }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
