// BOZ Redux launcher: sets up game files, edits client.ini and starts the game client.
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

struct Binding {
    const char *action;
    const char *label;
    const char *defaults;
};

// Must match BINDING_DEFAULTS in s3e_input.c.
const Binding BINDINGS[] = {
    {"move_forward", "Move forward", "W"},
    {"move_back", "Move back", "S"},
    {"move_left", "Move left", "A"},
    {"move_right", "Move right", "D"},
    {"shoot", "Shoot", "Mouse1"},
    {"aim", "Aim", "Mouse3"},
    {"reload", "Reload", "R"},
    {"action", "Use", "E, F"},
    {"sprint", "Sprint", "Left Shift"},
    {"melee", "Knife", "V"},
    {"grenade", "Grenade", "G"},
    {"tactical", "Tactical", "Q"},
    {"crouch", "Crouch", "C, Space"},
    {"alt_fire", "Alternate fire", "X"},
    {"switch_weapon", "Switch weapon", "1"},
    {"pause", "Pause", "Escape"},
    {"toggle_mode", "Free the mouse", "Tab"},
    {"fullscreen", "Toggle fullscreen", "F11"},
};
const int BINDING_COUNT = (int)(sizeof(BINDINGS) / sizeof(BINDINGS[0]));

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

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = rgb(22, 22, 20);
    c[ImGuiCol_ChildBg] = rgb(30, 30, 27);
    c[ImGuiCol_FrameBg] = rgb(44, 43, 38);
    c[ImGuiCol_FrameBgHovered] = rgb(62, 60, 52);
    c[ImGuiCol_FrameBgActive] = rgb(75, 72, 60);
    c[ImGuiCol_Button] = rgb(92, 26, 22);
    c[ImGuiCol_ButtonHovered] = rgb(128, 36, 30);
    c[ImGuiCol_ButtonActive] = rgb(150, 44, 36);
    c[ImGuiCol_Header] = rgb(92, 26, 22);
    c[ImGuiCol_HeaderHovered] = rgb(128, 36, 30);
    c[ImGuiCol_HeaderActive] = rgb(150, 44, 36);
    c[ImGuiCol_Tab] = rgb(44, 43, 38);
    c[ImGuiCol_TabHovered] = rgb(128, 36, 30);
    c[ImGuiCol_TabSelected] = rgb(92, 26, 22);
    c[ImGuiCol_CheckMark] = rgb(214, 170, 80);
    c[ImGuiCol_SliderGrab] = rgb(214, 170, 80);
    c[ImGuiCol_SliderGrabActive] = rgb(240, 196, 100);
    c[ImGuiCol_PlotHistogram] = rgb(214, 170, 80);
    c[ImGuiCol_Separator] = rgb(60, 58, 50);
}

void load_font() {
    ImGuiIO &io = ImGui::GetIO();
    const char *candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    for (const char *path : candidates) {
        if (os::exists(path) && io.Fonts->AddFontFromFileTTF(path, 18.0f)) {
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

void status_line(bool ok, const char *good, const char *bad) {
    ImGui::TextColored(ok ? rgb(120, 200, 110) : rgb(220, 120, 90), "%s %s", ok ? "[ok]" : "[--]",
                       ok ? good : bad);
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

    ImGui::SeparatorText("1. Game APK");
    if (!status.image) {
        status_line(false, "", "Not installed. Use your own Black Ops Zombies APK, version 1.0.11.");
    } else if (!status.image_checked) {
        status_line(true, "Installed (checking version...)", "");
    } else if (status.image_verified) {
        status_line(true, "Installed: version 1.0.11", "");
    } else {
        ImGui::TextColored(rgb(230, 190, 90),
                           "[!!] Installed, but not the 1.0.11 release. It may not work.");
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
    if (ImGui::Button(status.image ? "Reinstall" : "Install")) {
        l.files->install_apk(l.apk_path);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("2. Data packs");
    for (const PackStatus &pack : status.packs) {
        status_line(pack.present, pack.name, pack.name);
    }
    bool packs_ready = status.packs[0].present && status.packs[1].present;
    ImGui::BeginDisabled(busy || packs_ready);
    if (ImGui::Button("Download from Activision (825 MB)")) {
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
    if (ImGui::Button("Import")) {
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
        ImGui::TextColored(rgb(220, 120, 90), "Could not open client.ini in %s.", l.root.c_str());
        return;
    }
    std::string chosen;
    ImGui::BeginChild("settings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.2f));
    ImGui::PushItemWidth(260);

    ImGui::SeparatorText("Display");
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

    ImGui::SeparatorText("Mouse");
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

    ImGui::SeparatorText("Mouse in Dead Ops Arcade");
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

    ImGui::SeparatorText("Controls (during a match)");
    ImGui::TextDisabled("Click Set, then press a key or mouse button. Separate several with commas.");
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150);
        for (int i = 0; i < BINDING_COUNT; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(BINDINGS[i].label);
            ImGui::TableNextColumn();
            char value[96];
            std::snprintf(value, sizeof(value), "%s",
                          l.ini.get("keys", BINDINGS[i].action, BINDINGS[i].defaults).c_str());
            ImGui::SetNextItemWidth(-1);
            if (l.capturing == i) {
                ImGui::TextColored(rgb(214, 170, 80), "Press a key or mouse button...");
            } else if (ImGui::InputText("##value", value, sizeof(value),
                                        ImGuiInputTextFlags_EnterReturnsTrue) ||
                       ImGui::IsItemDeactivatedAfterEdit()) {
                save_setting(l, "keys", BINDINGS[i].action, value);
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
                    save_setting(l, "keys", BINDINGS[i].action, BINDINGS[i].defaults);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Troubleshooting");
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
        ImGui::TextColored(rgb(220, 120, 90), "Could not open client.ini in %s.", l.root.c_str());
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
            ImGui::TextColored(rgb(230, 180, 80), "No mod.toml: named after its folder.");
        } else if (mod.game[0] && std::strcmp(mod.game, "1.0.11") != 0) {
            ImGui::TextColored(rgb(230, 180, 80), "Made for game version %s; this client runs 1.0.11.",
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
        ImGui::TextColored(rgb(230, 180, 80), "%d file%s replaced by more than one mod: %s",
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
        save_setting(l, "keys", BINDINGS[l.capturing].action, name);
    }
    l.capturing = -1;
    return true;
}

// --- Play -----------------------------------------------------------------------------------

void start_game(Launcher &l, SDL_Window *window) {
    std::string saves = os::join(l.root, "saves");
    os::make_dir(saves);
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
        l.game_note = "Could not start " + loader + ".";
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
}

void draw(Launcher &l, SDL_Window *window) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("BOZ Redux", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    GameStatus status = l.files->status();
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextColored(rgb(214, 170, 80), "BOZ Redux");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("Call of Duty: Black Ops Zombies for desktop");

    float play_width = 160;
    ImGui::SameLine(ImGui::GetWindowWidth() - play_width - ImGui::GetStyle().WindowPadding.x);
    ImGui::BeginDisabled(!status.ready() || l.files->busy() || l.game_running);
    if (ImGui::Button(l.game_running ? "Running..." : "PLAY", ImVec2(play_width, 40))) {
        start_game(l, window);
    }
    ImGui::EndDisabled();
    if (!status.ready()) {
        ImGui::TextColored(rgb(230, 190, 90), "Set up the game files below to play.");
    }
    if (!l.game_note.empty()) {
        ImGui::TextColored(rgb(220, 120, 90), "%s", l.game_note.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Open log")) {
            os::open_path(os::join(l.root, "boz-log.txt"));
        }
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Game files")) {
            game_files_tab(l, status);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            settings_tab(l);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Mods")) {
            mods_tab(l);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
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
#endif

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    SDL_Window *window = SDL_CreateWindow(
        "BOZ Redux", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 680,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
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
    load_font();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    GameFiles files(l.root, l.bin_dir);
    l.files = &files;

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
        SDL_SetRenderDrawColor(renderer, 22, 22, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
