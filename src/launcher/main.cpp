// BOZ Redux launcher: sets up game files, edits client.ini, installs mods and starts the game.
#include "boz_api.h"
#include "brand_imgui.h"
#include "branding.h"
#include "client_config.h"
#include "game_files.h"
#include "ini_file.h"
#include "mod_install.h"
#include "mods.h"
#include "os.h"
#include "server_check.h"
#include "updater.h"

#include "imgui.h"
#include "imgui_internal.h"  // touch_scroll: the active id and the window's move id
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#if !defined(__ANDROID__)
#define SDL_MAIN_HANDLED  // on Android, SDL's activity calls this main as SDL_main
#endif
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <dirent.h>
#elif !defined(_WIN32)
#include <dlfcn.h>
#include <unistd.h>
#endif
#if !defined(__ANDROID__)
#include <dirent.h>
#endif

namespace {

enum Tab { TAB_GAME_FILES, TAB_SETTINGS, TAB_MODS, TAB_COUNT };

// A .zip in the mods folder that is not installed (or is another version than the one that is).
struct ZipOffer {
    std::string path, file, id, name, version, installed_version;
};

struct Launcher {
    std::string root;
    std::string bin_dir;
    GameFiles *files = nullptr;
    IniFile ini;
    bool ini_loaded = false;
    std::string settings_note;

    char apk_path[1024] = "";
    char pack_folder[1024] = "";

    int capturing = -1;  // binding index waiting for a key or mouse press

    int tab = TAB_GAME_FILES;
    int settings_page = 0;

    std::vector<mod_info> mods;
    bool mods_scanned = false;
    int mod_selected = -1;
    std::vector<ZipOffer> zip_offers;
    std::set<std::string> dismissed_zips;
    StagedMod staged;            // a mod zip unpacked, waiting for Install/Cancel
    bool staged_pending = false;
    int remove_index = -1;       // mod waiting for the Remove question
    std::string mods_note;
    bool mods_note_error = false;

    Updates *updates = nullptr;
    bool update_hidden = false;   // "Later" pressed
    bool update_asked = false;    // the player started the check or the update: show failures
    bool restart = false;         // the update is in place: start the new launcher

    char server_input[128] = "";
    std::future<ServerCheck> server_check;  // a server being checked (added or chosen)
    std::string server_checking;            // its name
    bool server_adding = false;             // Add (true) or choosing one in the list (false)
    std::string server_note;                // the last check's result
    bool server_note_ok = false;

    os::Process game;
    bool game_running = false;
    std::string game_note;
    std::vector<std::string> game_problems;  // lines from boz-log.txt after a failed run
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
SDL_Renderer *g_renderer = nullptr;

// Layout scale: 1 on the desktop; phones and TVs draw everything larger (set_layout).
float g_scale = 1.0f;
float px(float v) {
    return v * g_scale;
}

// Touch layout (phones and TVs; BOZ_LAUNCHER_TOUCH=1 previews it on a PC): bigger targets, finger
// scrolling, no mouse or keyboard settings.
bool g_touch = false;
// Portrait (the window taller than wide, a phone held upright): one column, tabs at the bottom.
bool g_portrait = false;
// Short landscape windows (a phone held sideways): the tabs sit in the header row.
bool g_compact = false;

const char *this_device() {
    return g_touch ? "this device" : "this PC";
}

// Touch: dragging a list with a finger scrolls it (ImGui scrolls with a wheel or the bar).
// Call inside the scrolling window, before its End.
void touch_scroll() {
    if (!g_touch) {
        return;
    }
    // A press on a window's empty space makes the window's move handle active (even when it
    // cannot move): that counts as no item.
    ImGuiID active = ImGui::GetCurrentContext()->ActiveId;
    bool idle = active == 0 || active == ImGui::GetCurrentWindow()->MoveId;
    if (idle && ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, px(6))) {
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
    }
}

void apply_style() {
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
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
    // Touch: buttons and fields about 48 dp tall, the size Android asks for finger targets.
    style.FramePadding = g_touch ? ImVec2(14, 14) : ImVec2(12, 7);
    style.ItemSpacing = g_touch ? ImVec2(10, 12) : ImVec2(10, 9);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.CellPadding = g_touch ? ImVec2(8, 10) : ImVec2(8, 6);
    style.ScrollbarSize = g_touch ? 18.0f : 12.0f;
    style.GrabMinSize = g_touch ? 24.0f : 12.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DisabledAlpha = 0.38f;
    if (g_portrait) {
        style.WindowPadding = ImVec2(16, 14);
    }
    brand_imgui_colours(style);
    style.ScaleAllSizes(g_scale);
    style.FontScaleDpi = g_scale;
}

// The scale and orientation for a window size; true when they changed (the style is rebuilt).
bool set_layout(SDL_Renderer *renderer) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    bool portrait = g_touch && h > w;  // the PC keeps its own layout at any window size
    float scale = 1.0f;
    int short_side = portrait ? w : h;
#if defined(__ANDROID__)
    // Sized by the screen's density (one desktop pixel per Android dp, the size Android's own text
    // has), as far as the layout still fits: about 950 by 450 desktop pixels in landscape, 430 by
    // 780 upright.
    float dpi = 0.0f;
    float by_density = SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) == 0 && dpi > 0.0f
                           ? dpi / 160.0f : (float)std::min(w, h) / 400.0f;
    float fits = portrait ? std::min((float)w / 430.0f, (float)h / 780.0f)
                          : std::min((float)h / 450.0f, (float)w / 950.0f);
    scale = std::max(1.0f, std::min(3.0f, std::min(by_density, fits)));
#endif
    bool compact = g_touch && !portrait && (float)short_side / scale < 560.0f;
    static bool applied = false;
    if (applied && scale == g_scale && portrait == g_portrait && compact == g_compact) {
        return false;
    }
    applied = true;
    g_scale = scale;
    g_portrait = portrait;
    g_compact = compact;
    SDL_Log("launcher layout: %dx%d, scale %.2f, %s", w, h, scale, portrait ? "portrait" : "landscape");
    apply_style();
    return true;
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

// Pictures from mods (map cards), loaded once per path.
struct Picture {
    SDL_Texture *texture = nullptr;
    int width = 0, height = 0;
};
std::map<std::string, Picture> g_pictures;

const Picture &picture(const std::string &path) {
    auto found = g_pictures.find(path);
    if (found != g_pictures.end()) {
        return found->second;
    }
    Picture &entry = g_pictures[path];
    int w = 0, h = 0;
    unsigned char *pixels = brand_image_load_file(path.c_str(), &w, &h);
    if (pixels) {
        if (SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, 32, w * 4,
                                                                      SDL_PIXELFORMAT_RGBA32)) {
            entry.texture = SDL_CreateTextureFromSurface(g_renderer, surface);
            if (entry.texture) {
                SDL_SetTextureScaleMode(entry.texture, SDL_ScaleModeBest);
                entry.width = w;
                entry.height = h;
            }
            SDL_FreeSurface(surface);
        }
        brand_image_free(pixels);
    }
    return entry;
}

void forget_pictures() {
    for (auto &entry : g_pictures) {
        if (entry.second.texture) {
            SDL_DestroyTexture(entry.second.texture);
        }
    }
    g_pictures.clear();
}

// A section heading: condensed capitals with a red bar in front, then a thin rule.
void section(const char *title) {
    ImGui::Spacing();
    ImGui::PushFont(g_brand.heading, 20.0f);
    ImVec2 at = ImGui::GetCursorScreenPos();
    float height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + px(3)),
                                              ImVec2(at.x + px(4), at.y + height - px(3)),
                                              ImGui::GetColorU32(brand(BRAND_RED)));
    ImGui::SetCursorScreenPos(ImVec2(at.x + px(14), at.y));
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

// Buttons in a row that wrap to the next line when the row is full (narrow windows, portrait).
void flow_next(const char *next_label) {
    float next = ImGui::CalcTextSize(next_label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
    float right = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next;
    if (right < ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x) {
        ImGui::SameLine();
    }
}

void status_line(bool ok, const char *good, const char *bad) {
    ImVec2 at = ImGui::GetCursorScreenPos();
    float height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + px(6), at.y + height * 0.5f), px(4.5f),
                                                ImGui::GetColorU32(ok ? OK_COLOUR : ERROR_COLOUR));
    ImGui::SetCursorScreenPos(ImVec2(at.x + px(20), at.y));
    ImGui::TextWrapped("%s", ok ? good : bad);
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
[[maybe_unused]] std::string format_float(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

void save_setting(Launcher &l, const char *section, const char *key, const std::string &value) {
    l.ini.set(section, key, value);
    l.settings_note = l.ini.save() ? "Saved to client.ini." : "Could not write client.ini.";
}

// The lines of boz-log.txt that say what went wrong (faults, errors), the last few.
std::vector<std::string> log_problems(const std::string &root) {
    std::vector<std::string> found;
    FILE *file = std::fopen(os::join(root, "boz-log.txt").c_str(), "rb");
    if (!file) {
        return found;
    }
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, std::max(0L, size - 65536), SEEK_SET);
    char line[512];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        std::string lower = text;
        for (char &c : lower) {
            c = (char)std::tolower((unsigned char)c);
        }
        bool noise = lower.find("recovered") != std::string::npos ||
                     lower.find("dynarmic a32 jit") != std::string::npos;
        bool problem = lower.find("error") != std::string::npos || lower.find("fault") != std::string::npos ||
                       lower.find("failed") != std::string::npos || lower.find("cannot") != std::string::npos ||
                       lower.find("unable") != std::string::npos;
        if (problem && !noise) {
            found.push_back(text.size() > 200 ? text.substr(0, 200) + "..." : text);
        }
    }
    std::fclose(file);
    if (found.size() > 4) {
        found.erase(found.begin(), found.end() - 4);
    }
    return found;
}

// --- Game files tab -------------------------------------------------------------------------

void job_message(Launcher &l) {
    bool busy = l.files->busy();
    std::string message = l.files->message();
    if (busy) {
        float progress = l.files->progress();
        ImGui::ProgressBar(progress < 0 ? -1.0f * (float)ImGui::GetTime() : progress,
                           ImVec2(-1, 0), message.empty() ? "Working..." : message.c_str());
    } else if (!message.empty()) {
        ImGui::TextWrapped("%s", message.c_str());
    }
}

void image_status(const GameStatus &status) {
    if (!status.image) {
        status_line(false, "", "Not installed. Use your own Black Ops Zombies APK, version 1.0.11.");
    } else if (!status.image_checked) {
        status_line(true, "Installed (checking version...)", "");
    } else if (status.image_verified) {
        status_line(true, "Installed: version 1.0.11", "");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ERROR_COLOUR);
        ImGui::TextWrapped("This APK is not the 1.0.11 release, so BOZ Redux can't run it. Choose "
                           "your original Black Ops Zombies APK, version 1.0.11.");
        ImGui::PopStyleColor();
    }
}

// Under the APK section: where to ask when the APK or the import does not work.
const char *const DISCORD_URL = "https://discord.gg/kVPQSMmsPF";

void apk_help() {
    if (g_touch) {   // phones: the line wraps, and a full-size button a finger can hit
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Trouble with your APK version or importing? Ask on the Discord.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Open the Discord")) {
            SDL_OpenURL(DISCORD_URL);
        }
        return;
    }
    ImGui::TextDisabled("Trouble with your APK version or importing?");
    ImGui::SameLine();
    if (ImGui::SmallButton("Ask on the Discord")) {
        SDL_OpenURL(DISCORD_URL);   // a browser on every platform (os::open_path is a no-op on Android)
    }
}

#if defined(__ANDROID__)

// First run: what is left before PLAY works.
void setup_checklist(const GameStatus &status) {
    if (status.ready()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, brand(BRAND_RAISED));
    ImGui::BeginChild("checklist", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_Borders);
    ImGui::PushFont(g_brand.strong, 0.0f);
    ImGui::TextUnformatted("Two steps, once, then press PLAY:");
    ImGui::PopFont();
    status_line(status.image && status.image_verified, "1. The game APK is installed.",
                status.image && status.image_checked ? "1. That APK is not version 1.0.11: choose the right one (below)."
                                                     : "1. Install your game APK (below).");
    bool packs = status.packs[0].present && status.packs[1].present;
    status_line(packs, "2. The data packs are in place.", "2. Get the data packs (below).");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The first file in a folder whose name ends with suffix (no case), or "".
std::string find_file(const std::string &folder, const char *suffix) {
    std::string found;
    if (DIR *dir = opendir(folder.c_str())) {
        size_t length = std::strlen(suffix);
        while (dirent *entry = readdir(dir)) {
            std::string name = entry->d_name;
            if (name.size() > length &&
                strcasecmp(name.c_str() + name.size() - length, suffix) == 0) {
                found = os::join(folder, name);
                break;
            }
        }
        closedir(dir);
    }
    return found;
}

// Phones and TVs: files come from the system picker (content:// URIs) or, on a TV without
// one, from the game folder's import/ folder (adb push), never from a typed path.
void game_files_tab(Launcher &l, const GameStatus &status) {
    bool busy = l.files->busy();
    bool picker = os::has_file_dialogs();
    std::string import_dir = os::join(l.root, "import");
    os::make_dir(import_dir);
    setup_checklist(status);

    section("1. Game APK");
    image_status(status);
    apk_help();
    ImGui::BeginDisabled(busy);
    if (picker && primary_button(status.image ? "Reinstall from an APK file..." : "Choose the APK file...")) {
        std::string picked = os::pick_file("", "", "");
        if (!picked.empty()) {
            l.files->install_apk(picked);
        }
    }
    std::string local_apk = find_file(import_dir, ".apk");
    if (!local_apk.empty()) {
        std::string label = "Install " + local_apk.substr(local_apk.rfind('/') + 1) + " (import folder)";
        if (picker) {
            flow_next(label.c_str());
        }
        if (ImGui::Button(label.c_str())) {
            l.files->install_apk(local_apk);
        }
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
    if (picker) {
        flow_next("Import files...");
        if (ImGui::Button("Import files...")) {
            std::vector<std::string> picked = os::pick_documents(true);
            if (!picked.empty()) {
                l.files->import_pack_documents(picked);
            }
        }
    }
    if (!find_file(import_dir, ".dz").empty()) {
        flow_next("Import from the import folder");
        if (ImGui::Button("Import from the import folder")) {
            l.files->import_packs(import_dir);
        }
    }
    ImGui::EndDisabled();
    if (picker) {
        ImGui::TextDisabled("Import: choose blackops_etc.dz and blackops_gles1.dz, or the game's .obb file.");
    } else {
        ImGui::TextWrapped("No file picker on this device. To use files from a PC instead, copy them "
                           "into %s (for example with adb push); buttons for them appear here.",
                           import_dir.c_str());
    }
    ImGui::Spacing();
    job_message(l);
    ImGui::Spacing();
    ImGui::TextDisabled("BOZ Redux %s. Not affiliated with Activision. Use your own copy of the game.",
                        BOZ_VERSION);
}

#else

void game_files_tab(Launcher &l, const GameStatus &status) {
    bool busy = l.files->busy();

    section("1. Game APK");
    image_status(status);
    apk_help();
    ImGui::SetNextItemWidth(px(-260));
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
    ImGui::SetNextItemWidth(px(-260));
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
    job_message(l);

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

#endif

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

// Characters the game accepts in a player name (s3e_config.c valid_player_name).
int player_name_filter(ImGuiInputTextCallbackData *data) {
    ImWchar c = data->EventChar;
    bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == ' ' || c == '-' || c == '_' || c == '.';
    return allowed ? 0 : 1;
}

// The name this device plays under without one set: "Player-" and the first four hex digits of
// saves/device-id.bin (s3e_config.c); the game makes the file on its first start.
std::string default_player_name(const Launcher &l) {
    std::string path = os::join(os::join(l.root, "saves"), "device-id.bin");
    unsigned char bytes[2];
    FILE *file = std::fopen(path.c_str(), "rb");
    bool read = file && std::fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    if (file) {
        std::fclose(file);
    }
    if (!read) {
        return "Player-xxxx";
    }
    char name[16];
    std::snprintf(name, sizeof(name), "Player-%02x%02x", bytes[0], bytes[1]);
    return name;
}

// A hint shown on hover with a mouse; on touch screens (no hover) as a line under the item.
void hint(const char *text) {
    if (g_touch) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

// Field widths: a fixed column on wide windows, most of the row when upright.
float field_width() {
    return g_portrait ? -px(120) : px(260);
}

void settings_player_display(Launcher &l) {
    std::string chosen;
    section("Player");
    {
        char name[14];  // at most 13 characters, as the game allows
        std::snprintf(name, sizeof(name), "%s", l.ini.get("player", "name").c_str());
        std::string placeholder = default_player_name(l) + " (" + this_device() + "'s default)";
        if (ImGui::InputTextWithHint("Name", placeholder.c_str(), name, sizeof(name),
                                     ImGuiInputTextFlags_CallbackCharFilter |
                                         ImGuiInputTextFlags_EnterReturnsTrue,
                                     player_name_filter) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            save_setting(l, "player", "name", name);
        }
        hint("Your name in the game and in co-op: letters, digits, space, - _ . Two players with "
             "the same name cannot share a co-op room.");
    }

    section("Display");
#if !defined(__ANDROID__)
    bool fullscreen = l.ini.get("display", "fullscreen", "true") != "false";
    if (ImGui::Checkbox("Fullscreen (F11 toggles in game)", &fullscreen)) {
        save_setting(l, "display", "fullscreen", fullscreen ? "true" : "false");
    }
#endif
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
#if defined(_WIN32)
        const char *renderers[] = {"auto", "d3d12", "vulkan", "software"};
        const char *renderer_names[] = {"Auto (ANGLE, Direct3D 11)", "Mesa Direct3D 12",
                                        "Mesa Vulkan", "Software (slow, always works)"};
        if (combo("Renderer", l.ini.get("display", "renderer", "auto"), renderers, renderer_names,
                  4, &chosen)) {
            save_setting(l, "display", "renderer", chosen);
        }
#endif
        const char *values[] = {"fit", "stretch", "none"};
        const char *names[] = {"Fit (keep 16:9)", "Stretch to window", "None"};
        if (combo("Scaling", l.ini.get("display", "scaling", "fit"), values, names, 3, &chosen)) {
            save_setting(l, "display", "scaling", chosen);
        }
    }
    {
        float saved = 0.0f;
        bool custom = fov_parse(l.ini.get("display", "fov").c_str(), &saved);
        int fov = custom ? (int)(saved + 0.5f) : FOV_GAME_DEFAULT;
        ImGui::SetNextItemWidth(g_portrait ? -px(120) : px(260));
        ImGui::SliderInt("Field of view", &fov, FOV_MIN, FOV_MAX, custom ? "%d" : "%d (map's own)",
                         ImGuiSliderFlags_AlwaysClamp);
        // Keep the dragged value in memory each frame; write the file once on release.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_setting(l, "display", "fov", std::to_string(fov));
        } else if (ImGui::IsItemActive() && (custom || fov != FOV_GAME_DEFAULT)) {
            l.ini.set("display", "fov", std::to_string(fov));
        }
        hint("The game's own number: most maps use 50. Higher shows more around you. Aiming down "
             "sights still zooms.");
        if (custom) {
            if (!g_touch) {
                ImGui::SameLine();
            }
            if (ImGui::Button("Map's own##fov")) {
                save_setting(l, "display", "fov", "");
            }
        }
    }
#if !defined(__ANDROID__)
    bool software_cursor = l.ini.get("display", "software_cursor", "false") == "true";
    if (ImGui::Checkbox("Crosshair pointer in menus", &software_cursor)) {
        save_setting(l, "display", "software_cursor", software_cursor ? "true" : "false");
    }
    hint("Draw the port's crosshair instead of the system mouse pointer. Controllers always show it.");
#endif
}

#if !defined(__ANDROID__)
void settings_mouse(Launcher &l) {
    std::string chosen;
    section("Mouse");
    float look = (float)std::atof(l.ini.get("input", "look_sensitivity", "3.0").c_str());
    ImGui::SliderFloat("Look sensitivity", &look, 0.2f, 15.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    hint("0.022 degrees per mouse count at 1.0, the same scale as Source and Quake games.");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        save_setting(l, "input", "look_sensitivity", format_float(look));
    } else if (ImGui::IsItemActive()) {
        l.ini.set("input", "look_sensitivity", format_float(look));
    }
    float aim = (float)std::atof(l.ini.get("input", "aim_sensitivity", "1.0").c_str());
    ImGui::SliderFloat("Aiming sensitivity", &aim, 0.1f, 3.0f, "%.2f");
    hint("Multiplier while aiming down sights. 1.0 feels the same as without aiming (the zoom is "
         "already allowed for).");
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
    const char *values[] = {"stick", "swipe"};
    const char *names[] = {"Stick (recommended)", "Swipe"};
    if (combo("Look mode", l.ini.get("input", "look_mode", "stick"), values, names, 2, &chosen)) {
        save_setting(l, "input", "look_mode", chosen);
    }
}

void settings_controls(Launcher &l) {
    section("Controls (during a match)");
    ImGui::TextDisabled("Click Set, then press a key or mouse button. Separate several with commas.");
    // Touch layouts: only the table scrolls (the PC's settings page scrolls as a whole).
    if (g_touch) {
        ImGui::BeginChild("keys_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
    }
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(180));
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, px(150));
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
    if (g_touch) {
        touch_scroll();
        ImGui::EndChild();
    }
}
#endif

// Online: where Play Online goes. Off, or BOZ servers the player adds (never a preset one).
// A server is checked before it is added or chosen (server_check.cpp), so a typo or another
// kind of server is refused with what is wrong.
std::vector<std::string> saved_servers(Launcher &l) {
    std::vector<std::string> servers;
    std::string list = l.ini.get("network", "servers");
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string host = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        host.erase(0, host.find_first_not_of(" \t"));
        host.erase(host.find_last_not_of(" \t") + 1);
        if (!host.empty() && std::find(servers.begin(), servers.end(), host) == servers.end()) {
            servers.push_back(host);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    std::string chosen = l.ini.get("network", "server");   // set before the list existed
    if (!chosen.empty() && std::find(servers.begin(), servers.end(), chosen) == servers.end()) {
        servers.push_back(chosen);
    }
    return servers;
}

void save_servers(Launcher &l, const std::vector<std::string> &servers) {
    std::string list;
    for (const std::string &host : servers) {
        list += (list.empty() ? "" : ",") + host;
    }
    save_setting(l, "network", "servers", list);
}

void start_server_check(Launcher &l, const std::string &host, bool adding) {
    l.server_checking = host;
    l.server_adding = adding;
    l.server_note = "Checking " + host + "...";
    l.server_note_ok = true;
    l.server_check = std::async(std::launch::async, check_boz_server, host);
}

// A finished check: add or choose the server when it is a BOZ server.
void finish_server_check(Launcher &l) {
    if (!l.server_check.valid() ||
        l.server_check.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    ServerCheck result = l.server_check.get();
    std::string host = l.server_checking;
    l.server_checking.clear();
    l.server_note_ok = result.ok;
    if (!result.ok) {
        l.server_note = host + ": " + result.message;
        return;
    }
    std::vector<std::string> servers = saved_servers(l);
    if (std::find(servers.begin(), servers.end(), host) == servers.end()) {
        servers.push_back(host);
        save_servers(l, servers);
    }
    save_setting(l, "network", "server", host);
    if (l.server_adding) {
        l.server_input[0] = 0;
    }
    l.server_note = host + ": BOZ server found. Play Online goes there from the next game start.";
}

void settings_online(Launcher &l) {
    finish_server_check(l);
    section("Online");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Play Online, in the game's Multiplayer menu, goes to the BOZ server chosen "
                           "here. Everyone who plays together chooses the same one. Wi-Fi co-op "
                           "needs none.");
    ImGui::PopTextWrapPos();
    bool busy = l.server_check.valid();
    std::string chosen = l.ini.get("network", "server");
    std::vector<std::string> servers = saved_servers(l);
    ImGui::BeginDisabled(busy);
    if (ImGui::RadioButton("Off (Wi-Fi co-op only)", chosen.empty()) && !chosen.empty()) {
        save_setting(l, "network", "server", "");
        l.server_note = "Online play is off from the next game start.";
        l.server_note_ok = true;
    }
    int remove = -1;
    for (size_t i = 0; i < servers.size(); ++i) {
        ImGui::PushID((int)i);
        if (ImGui::RadioButton(servers[i].c_str(), servers[i] == chosen) && servers[i] != chosen) {
            start_server_check(l, servers[i], false);   // still a BOZ server? then it is chosen
        }
        ImGui::SameLine();
        if (g_touch ? ImGui::Button("Remove") : ImGui::SmallButton("Remove")) {
            remove = (int)i;
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        if (servers[remove] == chosen) {
            save_setting(l, "network", "server", "");
        }
        servers.erase(servers.begin() + remove);
        save_servers(l, servers);
        l.server_note.clear();
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(g_portrait ? -1.0f : px(320));
    bool entered = ImGui::InputTextWithHint("##server", "A server's name or IP address", l.server_input,
                                            sizeof(l.server_input), ImGuiInputTextFlags_EnterReturnsTrue);
    if (!g_portrait) {
        ImGui::SameLine();
    }
    std::string host = l.server_input;
    host.erase(0, host.find_first_not_of(" \t"));
    host.erase(host.find_last_not_of(" \t") + 1);
    ImGui::BeginDisabled(host.empty());
    if ((ImGui::Button("Add server") || entered) && !host.empty()) {
        start_server_check(l, host, true);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!l.server_note.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, busy ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                  : l.server_note_ok ? OK_COLOUR : ERROR_COLOUR);
        ImGui::TextUnformatted(l.server_note.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    hint("Only BOZ servers are accepted (they run the game's online service, like cod-boz-online). "
         "The launcher checks a server before adding or choosing it.");
}

void settings_troubleshooting(Launcher &l) {
    section("Troubleshooting");
    bool status = l.ini.get("debug", "status", "false") == "true";
    if (ImGui::Checkbox("Log frame rate and emulator statistics", &status)) {
        save_setting(l, "debug", "status", status ? "true" : "false");
    }
    bool log_files = l.ini.get("debug", "log_files", "false") == "true";
    if (ImGui::Checkbox("Log every file the game opens (for mod makers)", &log_files)) {
        save_setting(l, "debug", "log_files", log_files ? "true" : "false");
    }
    bool log_network = l.ini.get("debug", "log_network", "false") == "true";
    if (ImGui::Checkbox("Log network activity (for co-op problems)", &log_network)) {
        save_setting(l, "debug", "log_network", log_network ? "true" : "false");
    }
}

void settings_updates(Launcher &l) {
    section("Updates");
    bool check = l.ini.get("launcher", "check_updates", "true") != "false";
    if (ImGui::Checkbox("Check for updates when the launcher starts", &check)) {
        save_setting(l, "launcher", "check_updates", check ? "true" : "false");
    }
    bool beta = l.ini.get("launcher", "beta", "false") == "true";
    if (ImGui::Checkbox("Include test versions (beta)", &beta)) {
        save_setting(l, "launcher", "beta", beta ? "true" : "false");
    }
    hint("Test versions come before a release and may have problems.");
    Updates::State state = l.updates->state();
    ImGui::BeginDisabled(state == Updates::CHECKING || state == Updates::DOWNLOADING);
    if (ImGui::Button("Check now")) {
        l.update_hidden = false;
        l.update_asked = true;
        l.updates->check(beta, l.ini.get("launcher", "update_api"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (state == Updates::CHECKING) {
        ImGui::TextDisabled("Checking...");
    } else if (state == Updates::FOUND) {
        ImGui::TextDisabled("BOZ Redux %s is available (see the top of the window).",
                            l.updates->release().version.c_str());
    } else {
        ImGui::TextDisabled("%s", l.updates->message().empty() ? (std::string("This is version ") + BOZ_VERSION + ".").c_str()
                                                               : l.updates->message().c_str());
    }
}

struct SettingsPage {
    const char *name;
    void (*draw)(Launcher &);
};

const SettingsPage SETTINGS_PAGES[] = {
    {"Player & display", settings_player_display},
#if !defined(__ANDROID__)
    {"Mouse", settings_mouse},
    {"Controls", settings_controls},
#endif
    {"Online", settings_online},
    {"Updates", settings_updates},
    {"Troubleshooting", settings_troubleshooting},
};
const int SETTINGS_PAGE_COUNT = (int)(sizeof(SETTINGS_PAGES) / sizeof(SETTINGS_PAGES[0]));

// Settings in short pages: a list on the left (upright: a row of buttons on top), so no page
// needs scrolling at normal sizes.
void settings_tab(Launcher &l) {
    ensure_ini(l);
    if (!l.ini_loaded) {
        ImGui::TextColored(ERROR_COLOUR, "Could not open client.ini in %s.", l.root.c_str());
        return;
    }
    if (!g_touch) {
        // The PC: one page, as it always was.
        ImGui::BeginChild("settings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.2f));
        ImGui::PushItemWidth(px(260));
        for (const SettingsPage &page : SETTINGS_PAGES) {
            page.draw(l);
        }
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::TextDisabled("%s", l.settings_note.empty() ? "Changes apply the next time the game starts."
                                                          : l.settings_note.c_str());
        return;
    }
    l.settings_page = std::max(0, std::min(l.settings_page, SETTINGS_PAGE_COUNT - 1));
    float note_height = ImGui::GetTextLineHeightWithSpacing();
    if (g_portrait) {
        for (int i = 0; i < SETTINGS_PAGE_COUNT; ++i) {
            if (i) {
                flow_next(SETTINGS_PAGES[i].name);
            }
            bool selected = i == l.settings_page;
            if (selected ? primary_button(SETTINGS_PAGES[i].name) : ImGui::Button(SETTINGS_PAGES[i].name)) {
                l.settings_page = i;
            }
        }
    } else {
        ImGui::BeginChild("settings_pages", ImVec2(px(200), -note_height), ImGuiChildFlags_Borders);
        for (int i = 0; i < SETTINGS_PAGE_COUNT; ++i) {
            if (ImGui::Selectable(SETTINGS_PAGES[i].name, i == l.settings_page, 0,
                                  ImVec2(0, ImGui::GetFrameHeight()))) {
                l.settings_page = i;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("settings_page", ImVec2(0, -note_height));
    ImGui::PushItemWidth(field_width());
    SETTINGS_PAGES[l.settings_page].draw(l);
    ImGui::PopItemWidth();
    touch_scroll();
    ImGui::EndChild();
    ImGui::TextDisabled("%s", l.settings_note.empty() ? "Changes apply the next time the game starts."
                                                      : l.settings_note.c_str());
}

// --- Mods tab ------------------------------------------------------------------------------

// .zip files in the mods folder that are not installed as they are: offered for install.
void find_zip_offers(Launcher &l) {
    l.zip_offers.clear();
    std::string folder = os::join(l.root, "mods");
    DIR *dir = opendir(folder.c_str());
    if (!dir) {
        return;
    }
    while (dirent *entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() < 5 || name[0] == '.') {
            continue;
        }
        std::string extension = name.substr(name.size() - 4);
        for (char &c : extension) {
            c = (char)std::tolower((unsigned char)c);
        }
        if (extension != ".zip" || l.dismissed_zips.count(name)) {
            continue;
        }
        ZipOffer offer;
        offer.path = os::join(folder, name);
        offer.file = name;
        if (!mod_zip_peek(offer.path, &offer.id, &offer.name, &offer.version)) {
            continue;
        }
        if (offer.id.empty()) {
            offer.id = name.substr(0, name.size() - 4);
        }
        bool same = false;
        for (const mod_info &mod : l.mods) {
            if (offer.id == mod.id) {
                offer.installed_version = mod.version;
                same = offer.version == mod.version;
            }
        }
        if (!same) {
            l.zip_offers.push_back(offer);
        }
    }
    closedir(dir);
}

bool is_map_mod(const mod_info &mod) {
    return mod.map_count > 0;
}

// The mods by name (no load order to manage: mods are switched on or off, maps chosen on PLAY).
void scan_mods(Launcher &l) {
    std::vector<mod_info> found(MODS_MAX);
    std::string disabled = l.ini.get("mods", "disabled");
    int count = mods_scan(l.root.c_str(), "", disabled.c_str(), found.data(), MODS_MAX);
    found.resize(count);
    std::stable_sort(found.begin(), found.end(), [](const mod_info &a, const mod_info &b) {
        for (size_t i = 0;; ++i) {
            int x = std::tolower((unsigned char)a.name[i]), y = std::tolower((unsigned char)b.name[i]);
            if (x != y || !x) {
                return x < y;
            }
        }
    });
    l.mods = found;
    l.mods_scanned = true;
    if (l.mod_selected >= count) {
        l.mod_selected = count - 1;
    }
    find_zip_offers(l);
}

// client.ini [mods]: the switched-off mods; map mods are on only while their map is played
// (start_game), the chosen map loading last so its files win.
void save_mods(Launcher &l, const std::string &playing_mod = "") {
    std::string order, disabled;
    for (const mod_info &mod : l.mods) {
        bool on = is_map_mod(mod) ? playing_mod == mod.id : mod.enabled;
        if (!on) {
            disabled += (disabled.empty() ? "" : ", ") + std::string(mod.id);
        } else if (playing_mod != mod.id) {
            order += (order.empty() ? "" : ", ") + std::string(mod.id);
        }
    }
    if (!playing_mod.empty()) {
        order += (order.empty() ? "" : ", ") + playing_mod;
    }
    l.ini.set("mods", "order", order);
    save_setting(l, "mods", "disabled", disabled);
}

void mods_note(Launcher &l, const std::string &text, bool error) {
    l.mods_note = text;
    l.mods_note_error = error;
}

// Starts installing a mod .zip (a path, or a picked content:// URI on Android).
void install_zip(Launcher &l, const std::string &zip) {
    if (zip.empty() || l.files->busy()) {
        return;
    }
    mods_note(l, "", false);
    l.tab = TAB_MODS;
    l.files->stage_mod(zip);
}

void pick_and_install_zip(Launcher &l) {
    std::string picked = os::pick_file("Choose a mod (.zip)", "BOZ Redux mod", "*.zip");
    install_zip(l, picked);
}

const char *const INSTALL_POPUP = "Install mod";
const char *const REMOVE_POPUP = "Remove mod";

// A modal question in the middle of the window, as wide as fits.
bool begin_question(const char *name) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(px(560), viewport->Size.x - px(32)), 0));
    return ImGui::BeginPopupModal(name, nullptr,
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_AlwaysAutoResize);
}

// Two answer buttons sharing the row.
int answer_buttons(const char *yes, const char *no) {
    float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    int answer = 0;
    if (primary_button(yes, ImVec2(width, 0))) {
        answer = 1;
    }
    ImGui::SameLine();
    if (ImGui::Button(no, ImVec2(width, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        answer = -1;
    }
    return answer;
}

void install_question(Launcher &l) {
    StagedMod staged;
    if (l.files->take_staged_mod(&staged)) {
        if (staged.ok) {
            l.staged = staged;
            l.staged_pending = true;
            ImGui::OpenPopup(INSTALL_POPUP);
        } else {
            mods_note(l, staged.error, true);
        }
    }
    if (!begin_question(INSTALL_POPUP)) {
        return;
    }
    const mod_info &mod = l.staged.mod;
    ImGui::PushFont(g_brand.strong, 0.0f);
    if (l.staged.update) {
        ImGui::TextWrapped("Update %s from %s to %s?", mod.name,
                           l.staged.old_version.empty() ? "?" : l.staged.old_version.c_str(),
                           mod.version[0] ? mod.version : "?");
    } else {
        ImGui::TextWrapped("Install %s%s%s?", mod.name, mod.version[0] ? " " : "", mod.version);
    }
    ImGui::PopFont();
    if (mod.author[0]) {
        ImGui::TextDisabled("by %s", mod.author);
    }
    if (mod.description[0]) {
        ImGui::TextWrapped("%s", mod.description);
    }
    for (const std::string &warning : l.staged.warnings) {
        ImGui::PushStyleColor(ImGuiCol_Text, WARN_COLOUR);
        ImGui::TextWrapped("%s", warning.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    int answer = answer_buttons(l.staged.update ? "UPDATE" : "INSTALL", "Cancel");
    if (answer != 0) {
        if (answer > 0) {
            std::string error;
            std::string mods_dir = os::join(l.root, "mods");
            if (mod_zip_commit(l.staged, mods_dir, &error)) {
                mods_note(l, std::string(mod.name) + (l.staged.update ? " updated." : " installed."), false);
                scan_mods(l);
                for (int i = 0; i < (int)l.mods.size(); ++i) {
                    if (l.staged.folder == l.mods[i].folder) {
                        l.mod_selected = i;
                    }
                }
                save_mods(l);  // a new mod goes to the end of the load order
            } else {
                mod_zip_discard(l.staged);
                mods_note(l, error, true);
            }
        } else {
            mod_zip_discard(l.staged);
        }
        l.staged_pending = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void remove_question(Launcher &l) {
    if (!begin_question(REMOVE_POPUP)) {
        return;
    }
    if (l.remove_index < 0 || l.remove_index >= (int)l.mods.size()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const mod_info &mod = l.mods[l.remove_index];
    ImGui::PushFont(g_brand.strong, 0.0f);
    ImGui::TextWrapped("Remove %s?", mod.name);
    ImGui::PopFont();
    ImGui::TextWrapped("Its folder (mods/%s) is deleted from %s. Its saved settings stay.", mod.folder,
                       this_device());
    ImGui::Spacing();
    int answer = answer_buttons("REMOVE", "Cancel");
    if (answer != 0) {
        if (answer > 0) {
            std::string error, name = mod.name;
            if (mod_remove(os::join(l.root, "mods"), mod.folder, &error)) {
                mods_note(l, name + " removed.", false);
            } else {
                mods_note(l, error, true);
            }
            scan_mods(l);
            save_mods(l);
        }
        l.remove_index = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void mod_details(Launcher &l) {
    if (l.mod_selected < 0 || l.mod_selected >= (int)l.mods.size()) {
        ImGui::TextDisabled("Select a mod or map to see its details.");
        return;
    }
    const mod_info &mod = l.mods[l.mod_selected];
    ImGui::PushFont(g_brand.strong, 0.0f);
    ImGui::TextWrapped("%s", mod.name);
    ImGui::PopFont();
    ImGui::TextDisabled("%s%s%s%s", mod.version[0] ? "v" : "", mod.version, mod.author[0] ? "  ·  by " : "",
                        mod.author);
    if (!mod.has_manifest) {
        ImGui::TextColored(WARN_COLOUR, "No mod.toml: named after its folder.");
    } else if (mod.game[0] && std::strcmp(mod.game, "1.0.11") != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, WARN_COLOUR);
        ImGui::TextWrapped("Made for game version %s; this client runs 1.0.11.", mod.game);
        ImGui::PopStyleColor();
    }
    if (mods_api_differs(&mod)) {
        ImGui::PushStyleColor(ImGuiCol_Text, WARN_COLOUR);
        ImGui::TextWrapped("Built for BOZ Redux API %d (this one has %d); it may not work.",
                           mod.client_api, BOZ_LUA_API);
        ImGui::PopStyleColor();
    }
    if (mod.description[0]) {
        ImGui::TextWrapped("%s", mod.description);
    }
    std::string connects = mod_connects_text(mod);
    if (!connects.empty()) {
        ImGui::TextWrapped("%s", connects.c_str());
    }
    if (is_map_mod(mod)) {
        ImGui::TextDisabled("Played from PLAY > Where to play.");
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(l.files->busy() || !os::has_file_dialogs());
    if (ImGui::Button("Update from a .zip...")) {
        pick_and_install_zip(l);
    }
    ImGui::EndDisabled();
#if !defined(__ANDROID__)
    flow_next("Open folder");
    if (ImGui::Button("Open folder")) {
        os::open_path(mod.dir);
    }
#endif
    flow_next("Remove...");
    if (ImGui::Button("Remove...")) {
        l.remove_index = l.mod_selected;
        ImGui::OpenPopup(REMOVE_POPUP);
    }
    remove_question(l);
}

// One row: [checkbox] name ... version, every part on the same text line; the name selects.
void mod_row(Launcher &l, int index, bool checkbox) {
    mod_info &mod = l.mods[index];
    ImGui::PushID(index);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (checkbox) {
        if (ImGui::Checkbox("##on", &mod.enabled)) {
            save_mods(l);
        }
        ImGui::TableNextColumn();
    }
    std::string label = mod.name;
    if (is_map_mod(mod) && mod.map_count > 1) {
        label += "  (" + std::to_string(mod.map_count) + " maps)";
    }
    if (mods_api_differs(&mod) || (mod.game[0] && std::strcmp(mod.game, "1.0.11") != 0)) {
        label += "  (!)";
    }
    // With a checkbox: the name on the checkbox's text line. A map row: the whole row height,
    // its name in the middle.
    bool selected = l.mod_selected == index;
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
    if (checkbox) {
        ImGui::AlignTextToFramePadding();
        if (ImGui::Selectable(label.c_str(), selected, flags)) {
            l.mod_selected = index;
        }
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Selectable(label.c_str(), selected, flags, ImVec2(0, ImGui::GetFrameHeight()))) {
            l.mod_selected = index;
        }
        ImGui::PopStyleVar();
    }
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", mod.version);
    ImGui::PopID();
}

// A list of mods or of maps: checkbox column for mods only.
void mod_table(Launcher &l, const char *id, bool maps) {
    int columns = maps ? 2 : 3;
    ImGui::PushStyleColor(ImGuiCol_Header, brand(BRAND_RAISED));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, brand(BRAND_LINE));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, brand(BRAND_LINE));
    if (ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        if (!maps) {
            ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        }
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("version", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < (int)l.mods.size(); ++i) {
            if (is_map_mod(l.mods[i]) == maps) {
                mod_row(l, i, !maps);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(3);
}

// The two lists: mods (switched on and off) and the loaded maps (chosen on PLAY).
void mod_lists(Launcher &l) {
    int mods = 0, maps = 0, on = 0;
    for (const mod_info &mod : l.mods) {
        if (is_map_mod(mod)) {
            ++maps;
        } else {
            ++mods;
            on += mod.enabled;
        }
    }
    section("Mods");
    if (mods) {
        mod_table(l, "mods", false);
    } else {
        ImGui::TextDisabled("No mods installed.");
    }
    if (on > 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, WARN_COLOUR);
        ImGui::TextWrapped("%d mods are on: mods can interact and make the game unstable.", on);
        ImGui::PopStyleColor();
    }
    section("Loaded maps");
    if (maps) {
        mod_table(l, "maps", true);
    } else {
        ImGui::TextDisabled("No maps installed.");
    }
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
    ImGui::BeginDisabled(l.files->busy() || !os::has_file_dialogs());
    if (primary_button("Install a mod (.zip)...")) {
        pick_and_install_zip(l);
    }
    ImGui::EndDisabled();
#if !defined(__ANDROID__)
    flow_next("Open mods folder");
    if (ImGui::Button("Open mods folder")) {
        os::make_dir(folder);
        os::open_path(folder);
    }
#endif
    flow_next("Refresh");
    if (ImGui::Button("Refresh")) {
        scan_mods(l);
    }
    if (!g_touch) {
        flow_next("Or drag a mod's .zip onto this window.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Or drag a mod's .zip onto this window.");
    }
    install_question(l);

    for (size_t i = 0; i < l.zip_offers.size(); ++i) {
        const ZipOffer &offer = l.zip_offers[i];
        ImGui::PushID((int)i);
        ImGui::AlignTextToFramePadding();
        std::string name = offer.name.empty() ? offer.id : offer.name;
        if (offer.installed_version.empty()) {
            ImGui::TextColored(WARN_COLOUR, "%s is in the mods folder but not installed.", offer.file.c_str());
        } else {
            ImGui::TextColored(WARN_COLOUR, "%s holds %s %s (installed: %s).", offer.file.c_str(),
                               name.c_str(), offer.version.c_str(), offer.installed_version.c_str());
        }
        flow_next("Install");
        ImGui::BeginDisabled(l.files->busy());
        if (ImGui::SmallButton("Install")) {
            install_zip(l, offer.path);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Not now")) {
            l.dismissed_zips.insert(offer.file);
        }
        ImGui::PopID();
    }
    for (const std::string &file : l.dismissed_zips) {
        l.zip_offers.erase(std::remove_if(l.zip_offers.begin(), l.zip_offers.end(),
                                          [&](const ZipOffer &o) { return o.file == file; }),
                           l.zip_offers.end());
    }
    if (l.files->busy()) {
        job_message(l);
    }
    if (!l.mods_note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, l.mods_note_error ? ERROR_COLOUR : OK_COLOUR);
        ImGui::TextWrapped("%s", l.mods_note.c_str());
        ImGui::PopStyleColor();
    }

    if (l.mods.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Nothing installed yet. Install a mod or map from its .zip%s.",
                           g_touch ? "" : ", or put its folder (with its mod.toml) in the mods folder and press Refresh");
        return;
    }
    if (l.mod_selected < 0) {
        l.mod_selected = 0;
    }
    if (g_portrait) {
        // Upright: the lists, then the details, all in the page (which a finger scrolls).
        mod_lists(l);
        section("Details");
        mod_details(l);
        return;
    }
    // Wide: the lists on the left (they scroll on their own), the details on the right.
    ImVec2 room = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("mod_lists", ImVec2(room.x * 0.55f, room.y), ImGuiChildFlags_Borders |
                                                                    ImGuiChildFlags_AlwaysUseWindowPadding);
    mod_lists(l);
    touch_scroll();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("mod_details", ImVec2(0, room.y), ImGuiChildFlags_AlwaysUseWindowPadding);
    mod_details(l);
    touch_scroll();
    ImGui::EndChild();
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

// One map card: a mod's map ("mod id/map name"), or the game's own menu (empty entry).
struct MapCard {
    std::string entry;
    std::string title, subtitle, description, picture;
};

std::vector<MapCard> map_cards(const Launcher &l) {
    std::vector<MapCard> cards;
    for (const mod_info &mod : l.mods) {
        for (int i = 0; i < mod.map_count; ++i) {
            const mod_map_info &info = mod.map_info[i];
            MapCard card;
            card.entry = std::string(mod.id) + "/" + mod.maps[i];
            card.title = info.title[0] ? info.title : mod.map_count > 1 ? mod.maps[i] : mod.name;
            card.subtitle = std::string(mod.name) + (mod.author[0] ? std::string("  ·  by ") + mod.author : "");
            card.description = info.description[0] ? info.description : mod.description;
            std::string picture = info.picture[0] ? info.picture : mod.preview;
            if (!picture.empty() && mod_zip_safe_name(picture)) {
                card.picture = os::join(mod.dir, picture);
            }
            cards.push_back(card);
        }
    }
    return cards;
}

bool has_mod_maps(const Launcher &l) {
    for (const mod_info &mod : l.mods) {
        if (mod.map_count) {
            return true;
        }
    }
    return false;
}

// Starts the game: on the main menu (empty map), or in a mod's map ("mod id/map name"), alone
// or in the Wi-Fi co-op room for that map.
void start_game(Launcher &l, SDL_Window *window, const std::string &map, bool coop) {
    std::string saves = os::join(l.root, "saves");
    os::make_dir(saves);
    // Only the chosen map's mod is on (two maps would fight over the same game files).
    save_mods(l, map.empty() ? "" : map.substr(0, map.find('/')));
    if (!map.empty()) {
        size_t slash = map.find('/');
        std::string mod = map.substr(0, slash);
        // The map's mod starts it once the menu is up (boz.levels reads these one-shot settings).
        mods_set_setting(saves.c_str(), mod.c_str(), "autostart_level",
                         map.substr(slash + 1).c_str());
        mods_set_setting(saves.c_str(), mod.c_str(), "autostart_mode", coop ? "coop" : NULL);
    }
    save_setting(l, "launcher", "last_played", map.empty() ? "menu" : map);
    l.game_problems.clear();
    std::map<std::string, std::string> env;
    env["HOME"] = saves;
    std::string image = os::join(os::join(l.root, "assets"), "boz.s3e.unpacked");
#if defined(__ANDROID__)
    // The game's activity runs in its own process; this one waits behind it.
    (void)window;
    if (!os::start_game({"--root", l.root, "--log", os::join(l.root, "boz-log.txt"), "--run", image},
                        env)) {
        l.game_note = "Could not start the game.";
    }
    return;
#endif
#if !defined(_WIN32)
    if (os::getenv_str("XDG_CACHE_HOME").empty() && !os::getenv_str("HOME").empty()) {
        env["XDG_CACHE_HOME"] = os::join(os::getenv_str("HOME"), ".cache");
    }
#endif
    std::string loader = os::join(l.bin_dir, os::executable_name("codboz_s3e_loader"));
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
                      : "The game stopped with an error (code " + std::to_string(l.game.exit_code) + ").";
    if (l.game.exit_code != 0) {
        l.game_problems = log_problems(l.root);
    }
#if defined(_WIN32)
    // 0x80070057 (E_INVALIDARG): the GPU renderer gave up after opening the window (seen with
    // Mesa's Direct3D 12 driver on AMD). Software rendering works everywhere.
    if ((unsigned)l.game.exit_code == 0x80070057u &&
        l.ini.get("display", "renderer", "auto") == "auto") {
        save_setting(l, "display", "renderer", "software");
        l.game_note = "The GPU renderer failed on this PC, so the renderer is now set to Software "
                      "(Settings tab). Press PLAY to try again.";
        l.game_problems.clear();
    }
#endif
}

const char *const PLAY_CHOOSER = "Where to play";

// The picture area of a card: the map's picture (cropped to 16:9), else its name on dark grey.
void card_picture(const MapCard &card, float width, bool menu) {
    float height = width * 9.0f / 16.0f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const Picture *pic = card.picture.empty() ? nullptr : &picture(card.picture);
    if (pic && pic->texture) {
        // Crop the picture to the card's 16:9 window, keeping its middle.
        float aspect = (float)pic->width / (float)pic->height, want = 16.0f / 9.0f;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (aspect > want) {
            float keep = want / aspect;
            uv0.x = (1 - keep) * 0.5f, uv1.x = 1 - uv0.x;
        } else {
            float keep = aspect / want;
            uv0.y = (1 - keep) * 0.5f, uv1.y = 1 - uv0.y;
        }
        draw->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)pic->texture), at,
                              ImVec2(at.x + width, at.y + height), uv0, uv1, IM_COL32_WHITE, px(4));
    } else {
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height),
                            ImGui::GetColorU32(brand(BRAND_RAISED)), px(4));
        if (menu && g_brand.logo) {
            float logo_h = height * 0.55f;
            float logo_w = logo_h * (float)g_brand.logo_width / (float)g_brand.logo_height;
            ImVec2 corner(at.x + (width - logo_w) * 0.5f, at.y + (height - logo_h) * 0.5f);
            draw->AddImage(ImTextureRef((ImTextureID)(intptr_t)g_brand.logo), corner,
                           ImVec2(corner.x + logo_w, corner.y + logo_h));
        } else {
            ImGui::PushFont(g_brand.heading, 26.0f);
            std::string upper = card.title;
            for (char &c : upper) {
                c = (char)std::toupper((unsigned char)c);
            }
            ImVec2 size = ImGui::CalcTextSize(upper.c_str(), nullptr, false, width - px(16));
            draw->AddText(nullptr, 0.0f,
                          ImVec2(at.x + (width - size.x) * 0.5f, at.y + (height - size.y) * 0.5f),
                          ImGui::GetColorU32(brand(BRAND_WHITE)), upper.c_str(), nullptr, width - px(16));
            ImGui::PopFont();
        }
    }
    ImGui::Dummy(ImVec2(width, height));
}

// A card; returns 1 for SOLO (or MAIN MENU), 2 for CO-OP.
int map_card(const MapCard &card, float width, bool last) {
    bool menu = card.entry.empty();
    int chosen = 0;
    ImGui::PushID(card.entry.c_str());
    ImGui::BeginGroup();
    card_picture(card, width, menu);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::PushFont(g_brand.heading, 22.0f);
    ImGui::TextUnformatted(card.title.c_str());
    ImGui::PopFont();
    if (last) {
        ImGui::SameLine();
        ImGui::TextColored(brand(BRAND_RED), "LAST PLAYED");
    }
    if (!card.subtitle.empty()) {
        ImGui::TextDisabled("%s", card.subtitle.c_str());
    }
    if (!card.description.empty()) {
        ImGui::TextWrapped("%s", card.description.c_str());
    }
    ImGui::PopTextWrapPos();
    float gap = ImGui::GetStyle().ItemSpacing.x;
    if (menu) {
        if (primary_button("MAIN MENU", ImVec2(width, 0))) {
            chosen = 1;
        }
    } else {
        if (primary_button("SOLO", ImVec2((width - gap) * 0.5f, 0))) {
            chosen = 1;
        }
        if (!g_touch && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Straight into this map, on your own.");
        }
        ImGui::SameLine();
        if (ImGui::Button("CO-OP", ImVec2((width - gap) * 0.5f, 0))) {
            chosen = 2;
        }
        if (!g_touch && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Straight into the Wi-Fi co-op room for this map: joins a friend's room "
                              "on the same map, or hosts one.\nEveryone needs this map's mod.");
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return chosen;
}

// PLAY with mod maps: a card for each map (alone or as a Wi-Fi co-op room) and one for the game's
// main menu. The last choice comes first.
void play_chooser(Launcher &l, SDL_Window *window) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float margin = g_portrait ? 0.0f : px(24);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x - 2 * margin, viewport->Size.y - 2 * margin));
    if (!ImGui::BeginPopupModal(PLAY_CHOOSER, nullptr,
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    ImGui::PushFont(g_brand.heading, 28.0f);
    ImGui::TextUnformatted("WHERE TO PLAY");
    ImGui::PopFont();
    const char *close = "Close";
    float close_width = ImGui::CalcTextSize(close).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - close_width);
    bool cancel = ImGui::Button(close) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    std::vector<MapCard> cards = map_cards(l);
    MapCard menu;
    menu.title = "The game's own maps";
    menu.description = "The main menu: Kino, Five, Ascension, Call of the Dead, Dead Ops and the rest.";
    cards.push_back(menu);
    std::string last = l.ini.get("launcher", "last_played");
    int last_index = -1;
    for (int i = 0; i < (int)cards.size(); ++i) {
        if ((last == "menu" && cards[i].entry.empty()) || (!last.empty() && last == cards[i].entry)) {
            last_index = i;
        }
    }
    if (last_index > 0) {
        std::rotate(cards.begin(), cards.begin() + last_index, cards.begin() + last_index + 1);
        last_index = 0;
    }

    ImGui::BeginChild("cards", ImVec2(0, 0));
    float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    float gap = px(18);
    int columns = std::max(1, (int)((avail + gap) / (px(300) + gap)));
    float width = (avail - gap * (float)(columns - 1)) / (float)columns;
    std::string chosen;
    bool coop = false, started = false;
    for (int i = 0; i < (int)cards.size(); ++i) {
        if (i % columns) {
            ImGui::SameLine(0, gap);
        } else if (i) {
            ImGui::Dummy(ImVec2(0, px(8)));
        }
        int pick = map_card(cards[i], width, i == last_index);
        if (pick) {
            chosen = cards[i].entry;
            coop = pick == 2;
            started = true;
        }
    }
    touch_scroll();
    ImGui::EndChild();
    if (started || cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (started) {
        start_game(l, window, chosen, coop);
    }
}

void play_button(Launcher &l, SDL_Window *window, const GameStatus &status, const ImVec2 &size) {
    ImGui::BeginDisabled(!status.ready() || l.files->busy() || l.game_running);
    ImGui::PushStyleColor(ImGuiCol_Button, brand(BRAND_RED));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(240, 52, 64));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, brand(BRAND_RED_DARK));
    ImGui::PushStyleColor(ImGuiCol_Border, brand(BRAND_RED));
    ImGui::PushFont(g_brand.heading, 30.0f);
    if (ImGui::Button(l.game_running ? "RUNNING" : "PLAY", size)) {
        if (!has_mod_maps(l)) {
            start_game(l, window, "", false);
        } else {
            ImGui::OpenPopup(PLAY_CHOOSER);
        }
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    // BOZ_LAUNCHER_PLAY=1 opens the chooser at once (for the launcher screenshot).
    static bool chooser_requested = os::getenv_str("BOZ_LAUNCHER_PLAY") == "1";
    if (chooser_requested && has_mod_maps(l)) {
        chooser_requested = false;
        ImGui::OpenPopup(PLAY_CHOOSER);
    }
    play_chooser(l, window);
}

void red_rule() {
    // The red rule under the header, as under the logo's wordmark.
    ImVec2 at = ImGui::GetCursorScreenPos();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(viewport->Pos.x, at.y),
                                              ImVec2(viewport->Pos.x + viewport->Size.x, at.y + px(2)),
                                              ImGui::GetColorU32(brand(BRAND_RED)));
    ImGui::Dummy(ImVec2(0, px(8)));
}

void nav_bar(Launcher &l);

void header(Launcher &l, SDL_Window *window, const GameStatus &status) {
    if (g_compact) {
        // Sideways phone: logo, tabs and PLAY in one row.
        float row = px(48);
        ImVec2 top = ImGui::GetCursorPos();
        if (g_brand.logo) {
            float logo_width = row * (float)g_brand.logo_width / (float)g_brand.logo_height;
            ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)g_brand.logo), ImVec2(logo_width, row));
            ImGui::SameLine(0, px(16));
        }
        nav_bar(l);
        float play_width = px(160);
        ImGui::SameLine(ImGui::GetWindowWidth() - play_width - ImGui::GetStyle().WindowPadding.x);
        ImGui::SetCursorPosY(top.y);
        play_button(l, window, status, ImVec2(play_width, row));
        ImGui::SetCursorPosY(top.y + row + px(8));
        red_rule();
        return;
    }
    if (g_portrait) {
        // Upright: the logo centred, PLAY across the width under it.
        float logo_height = px(64);
        if (g_brand.logo) {
            float logo_width = logo_height * (float)g_brand.logo_width / (float)g_brand.logo_height;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - logo_width) * 0.5f);
            ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)g_brand.logo), ImVec2(logo_width, logo_height));
        }
        play_button(l, window, status, ImVec2(-1, px(56)));
        ImGui::Dummy(ImVec2(0, px(2)));
        red_rule();
        return;
    }
    const float logo_height = px(78);
    const float play_width = px(200), play_height = px(54), gap = px(20);
    const char *title = "CLASSIC BLACK OPS ZOMBIES";
    char subtitle[96];
    std::snprintf(subtitle, sizeof(subtitle), "The original mobile game, %s   v%s",
                  g_touch ? "reborn" : "on PC", BOZ_VERSION);
    ImVec2 top = ImGui::GetCursorPos();
    if (g_brand.logo) {
        float logo_width = logo_height * (float)g_brand.logo_width / (float)g_brand.logo_height;
        ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)g_brand.logo), ImVec2(logo_width, logo_height));
        ImGui::SameLine(0, px(22));
    }

    // On a narrow window the title gives way.
    ImGui::PushFont(g_brand.heading, 24.0f);
    float text_width = ImGui::CalcTextSize(title).x;
    ImGui::PopFont();
    text_width = std::max(text_width, ImGui::CalcTextSize(subtitle).x);
    float text_x = ImGui::GetCursorPosX();
    float right = ImGui::GetWindowWidth() - play_width - ImGui::GetStyle().WindowPadding.x;
    if (text_x + text_width <= right - gap) {
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(top.y + px(16));
        ImGui::PushFont(g_brand.heading, 24.0f);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::TextDisabled("%s", subtitle);
        ImGui::EndGroup();
    } else {
        ImGui::Dummy(ImVec2(0, logo_height));
    }

    ImGui::SameLine(right);
    ImGui::SetCursorPosY(top.y + (logo_height - play_height) * 0.5f);
    play_button(l, window, status, ImVec2(play_width, play_height));
    ImGui::SetCursorPosY(top.y + logo_height + px(14));
    red_rule();
}

// The page tabs: across the top on wide windows, a bar at the bottom when upright.
void nav_bar(Launcher &l) {
    static const char *const labels[TAB_COUNT] = {"GAME FILES", "SETTINGS", "MODS"};
    float gap = px(4);
    float width = g_portrait ? (ImGui::GetContentRegionAvail().x - gap * (float)(TAB_COUNT - 1)) / (float)TAB_COUNT
                             : g_compact ? px(130) : px(150);
    float height = g_portrait ? px(56) : g_compact ? px(48) : ImGui::GetFrameHeight();
    for (int i = 0; i < TAB_COUNT; ++i) {
        if (i) {
            ImGui::SameLine(0, gap);
        }
        bool selected = l.tab == i;
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? brand(BRAND_RAISED) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? brand(BRAND_WHITE) : brand(BRAND_GREY));
        ImGui::PushFont(g_brand.heading, 19.0f);
        if (ImGui::Button(labels[i], ImVec2(width, height))) {
            l.tab = i;
        }
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        if (selected) {
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            float y = g_portrait ? min.y : max.y - px(3);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, y + px(3)),
                                                      ImGui::GetColorU32(brand(BRAND_RED)));
        }
    }
    if (!g_portrait && !g_compact) {
        ImGui::Separator();
    }
}

void footer() {
    ImGui::Separator();
    ImGui::TextDisabled("BOZ Redux %s  ·  Not affiliated with Activision. Use your own copy of the game.",
                        BOZ_VERSION);
}

// Tab labels in the heading face, as the brand's condensed capitals.
bool begin_tab(const char *label, bool select) {
    ImGui::PushFont(g_brand.heading, 19.0f);
    bool open = ImGui::BeginTabItem(label, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
    ImGui::PopFont();
    return open;
}

void draw_page(Launcher &l, const GameStatus &status) {
    if (l.tab == TAB_GAME_FILES) {
        game_files_tab(l, status);
    } else if (l.tab == TAB_SETTINGS) {
        settings_tab(l);
    } else {
        mods_tab(l);
    }
}

// The PC: the tab bar under the header and the footer line, as the launcher always looked.
void draw_desktop(Launcher &l, const GameStatus &status) {
    static int shown_tab = -1;  // a tab chosen by the code (a dropped .zip) is selected here
    float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    if (ImGui::BeginTabBar("tabs")) {
        static const char *const labels[TAB_COUNT] = {"GAME FILES", "SETTINGS", "MODS"};
        int request = l.tab != shown_tab ? l.tab : -1;
        for (int tab = 0; tab < TAB_COUNT; ++tab) {
            if (!begin_tab(labels[tab], tab == request)) {
                continue;
            }
            if (request < 0 || tab == request) {
                l.tab = shown_tab = tab;
            }
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(22), px(16)));
            ImGui::BeginChild("page", ImVec2(0, -footer_height), ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            draw_page(l, status);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    footer();
}

const char *const NOTES_POPUP = "What's new";

void notes_popup(Launcher &l) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(px(640), viewport->Size.x - px(32)),
                                    std::min(px(520), viewport->Size.y - px(32))));
    if (!ImGui::BeginPopupModal(NOTES_POPUP, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        return;
    }
    Release release = l.updates->release();
    ImGui::PushFont(g_brand.strong, 0.0f);
    ImGui::TextWrapped("BOZ Redux %s", release.version.c_str());
    ImGui::PopFont();
    ImGui::BeginChild("notes", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.3f), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", release.notes.empty() ? "No notes for this release." : release.notes.c_str());
    touch_scroll();
    ImGui::EndChild();
    if (ImGui::Button("Close", ImVec2(-1, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// A line under the header while an update is available, downloading or done.
void update_banner(Launcher &l) {
    Updates::State state = l.updates->state();
    Release release;
    if (state == Updates::FOUND && !l.update_hidden) {
        release = l.updates->release();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(OK_COLOUR, "BOZ Redux %s is available (you have %s).", release.version.c_str(),
                           BOZ_VERSION);
        flow_next("What's new");
        if (ImGui::Button("What's new")) {
            ImGui::OpenPopup(NOTES_POPUP);
        }
        const char *action = l.updates->can_install() ? "Update now" : "Open the download page";
        flow_next(action);
        ImGui::BeginDisabled(l.game_running || l.files->busy());
        if (primary_button(action)) {
            l.update_asked = true;
            if (l.updates->can_install()) {
                l.updates->install();
            } else {
                os::open_path(release.page);
            }
        }
        ImGui::EndDisabled();
        flow_next("Later");
        if (ImGui::Button("Later")) {
            l.update_hidden = true;
        }
        // BOZ_LAUNCHER_AUTO_UPDATE=1 presses Update now (tests of the update path).
        static bool automatic = os::getenv_str("BOZ_LAUNCHER_AUTO_UPDATE") == "1";
        if (automatic && l.updates->can_install()) {
            automatic = false;
            l.update_asked = true;
            l.updates->install();
        }
    } else if (state == Updates::DOWNLOADING) {
        float progress = l.updates->progress();
        ImGui::ProgressBar(progress < 0 ? -1.0f * (float)ImGui::GetTime() : progress, ImVec2(-1, 0),
                           l.updates->message().c_str());
    } else if (state == Updates::READY) {
        ImGui::TextColored(OK_COLOUR, "%s", l.updates->message().c_str());
#if !defined(__ANDROID__)
        l.restart = true;
#endif
    } else if (state == Updates::FAILED && l.update_asked) {
        ImGui::PushStyleColor(ImGuiCol_Text, ERROR_COLOUR);
        ImGui::TextWrapped("%s", l.updates->message().c_str());
        ImGui::PopStyleColor();
    }
    notes_popup(l);
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
    update_banner(l);
    if (!l.game_note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ERROR_COLOUR);
        ImGui::TextWrapped("%s", l.game_note.c_str());
        for (const std::string &line : l.game_problems) {
            ImGui::TextWrapped("  %s", line.c_str());
        }
        ImGui::PopStyleColor();
    }
    if (!g_touch) {
        if (!status.ready()) {
            ImGui::TextColored(WARN_COLOUR, "Set up the game files below to play.");
        }
        draw_desktop(l, status);
        if (l.tab != TAB_MODS) {
            install_question(l);  // a .zip dropped on another page asks here too
        }
        ImGui::End();
        return;
    }
    if (!g_portrait && !g_compact) {
        nav_bar(l);
    }
    // Touch layouts: the page, and the tabs at the bottom when upright.
    float nav_height = g_portrait ? px(56) + ImGui::GetStyle().ItemSpacing.y * 2 : 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        g_portrait ? ImVec2(px(4), px(8)) : ImVec2(px(22), px(16)));
    ImGui::BeginChild("page", ImVec2(0, -nav_height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    draw_page(l, status);
    touch_scroll();
    ImGui::EndChild();
    if (l.tab != TAB_MODS) {
        install_question(l);
    }
    if (g_portrait) {
        ImGui::Separator();
        nav_bar(l);
    }
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

#if !defined(_WIN32) && !defined(__ANDROID__)
bool has_library(const char *name) {
    void *handle = dlopen(name, RTLD_LAZY);
    if (handle) {
        dlclose(handle);
    }
    return handle != nullptr;
}

// Prefer native Wayland when the 32-bit keyboard library SDL needs for it is installed. NVIDIA's
// driver has no 32-bit Wayland EGL platform on most distributions, so EGL falls back to Mesa's
// software renderer there; X11 (XWayland) reaches the GPU through GLX instead.
void choose_video_driver() {
    if (!os::getenv_str("SDL_VIDEODRIVER").empty() || os::getenv_str("WAYLAND_DISPLAY").empty()) {
        return;
    }
    bool nvidia = access("/proc/driver/nvidia/version", F_OK) == 0;
    if (nvidia && !has_library("libnvidia-egl-wayland.so.1")) {
        return;
    }
    if (has_library("libxkbcommon.so.0")) {
        setenv("SDL_VIDEODRIVER", "wayland", 0);
    }
}
#endif

bool ends_with_no_case(const std::string &text, const char *suffix) {
    size_t length = std::strlen(suffix);
    if (text.size() < length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (std::tolower((unsigned char)text[text.size() - length + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

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
#if defined(__ANDROID__)
    g_touch = true;
#else
    g_touch = os::getenv_str("BOZ_LAUNCHER_TOUCH") == "1";
#endif
#if !defined(_WIN32) && !defined(__ANDROID__)
    choose_video_driver();
    // The window class the desktop shortcut names (StartupWMClass), so the taskbar shows the
    // BOZ Redux icon; the game inherits it.
    setenv("SDL_VIDEO_X11_WMCLASS", "boz-redux", 0);
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "boz-redux", 0);
#endif

    SDL_SetMainReady();
#if defined(__ANDROID__)
    const Uint32 subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;  // TV remotes, controllers
    // The launcher turns with the phone (the game itself stays landscape).
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
#else
    const Uint32 subsystems = SDL_INIT_VIDEO;
#endif
    if (SDL_Init(subsystems) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    // 1080x760, or 90% of the screen's free area when that is smaller (a laptop, display scaling).
    // BOZ_LAUNCHER_SIZE=WxH sets it (previews of the phone layouts with BOZ_LAUNCHER_TOUCH=1).
    int width = 1080, height = 760;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        width = std::min(width, usable.w * 9 / 10);
        height = std::min(height, usable.h * 9 / 10);
    }
    std::string size_setting = os::getenv_str("BOZ_LAUNCHER_SIZE");
    [[maybe_unused]] bool sized = std::sscanf(size_setting.c_str(), "%dx%d", &width, &height) == 2;
#if defined(__ANDROID__)
    const Uint32 window_flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
#else
    const Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    SDL_Window *window = SDL_CreateWindow("BOZ Redux", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, width, height, window_flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
#if !defined(__ANDROID__)
    if (!sized) {
        SDL_SetWindowMinimumSize(window, std::min(900, width), std::min(620, height));
    }
#endif
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
    g_renderer = renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    if (g_touch) {
        // A controller or TV remote can move between the buttons.
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    }
    set_layout(renderer);
    load_fonts();
    load_logo(renderer);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    finish_update(l.root);  // what the last update left behind
    GameFiles files(l.root, l.bin_dir);
    l.files = &files;
    Updates updates(l.root);
    l.updates = &updates;
    ensure_ini(l);
    // Screenshots check only against a test server (BOZ_UPDATE_API).
    if (l.ini.get("launcher", "check_updates", "true") != "false" &&
        (os::getenv_str("BOZ_LAUNCHER_SCREENSHOT").empty() || !os::getenv_str("BOZ_UPDATE_API").empty())) {
        updates.check(l.ini.get("launcher", "beta", "false") == "true", l.ini.get("launcher", "update_api"));
    }
    mod_clean_leftovers(os::join(l.root, "mods"));
    // The first start goes to the game files until the game is installed; later ones to the mods.
    l.tab = files.status().ready() ? TAB_MODS : TAB_GAME_FILES;
    // BOZ_LAUNCHER_TAB=0, 1 or 2 opens Game files, Settings or Mods first (screenshots).
    std::string first_tab = os::getenv_str("BOZ_LAUNCHER_TAB");
    if (!first_tab.empty()) {
        l.tab = std::max(0, std::min(TAB_COUNT - 1, std::atoi(first_tab.c_str())));
    }
    l.settings_page = std::atoi(os::getenv_str("BOZ_LAUNCHER_SETTINGS_PAGE").c_str());

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
                if (event.type == SDL_RENDER_DEVICE_RESET) {
                    // The GPU context was lost (Android, after the game ran): rebuild textures.
                    ImGui_ImplSDLRenderer2_DestroyDeviceObjects();
                    if (g_brand.logo) {
                        SDL_DestroyTexture(g_brand.logo);
                        g_brand.logo = nullptr;
                    }
                    forget_pictures();
                    load_logo(renderer);
                }
                if (event.type == SDL_QUIT) {
                    done = !l.game_running;
                } else if (event.type == SDL_DROPFILE) {
                    std::string dropped = event.drop.file;
                    SDL_free(event.drop.file);
                    if (ends_with_no_case(dropped, ".apk")) {
                        std::snprintf(l.apk_path, sizeof(l.apk_path), "%s", dropped.c_str());
                        l.tab = TAB_GAME_FILES;
                    } else if (ends_with_no_case(dropped, ".zip")) {
                        install_zip(l, dropped);
                    } else {
                        std::snprintf(l.pack_folder, sizeof(l.pack_folder), "%s", dropped.c_str());
                        l.tab = TAB_GAME_FILES;
                    }
                }
            } while (SDL_PollEvent(&event));
        }
        poll_game(l, window);
        if (l.game_running) {
            continue;
        }
        set_layout(renderer);  // the phone turned, or the window was resized
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw(l, window);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, (BRAND_BLACK >> 16) & 0xff, (BRAND_BLACK >> 8) & 0xff, BRAND_BLACK & 0xff, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!screenshot.empty() && updates.state() != Updates::CHECKING && ++frames == 4) {
            save_screenshot(renderer, screenshot);
            done = true;
        }
        SDL_RenderPresent(renderer);
        if (l.restart) {
            // The update is in place: the new launcher takes over (without the test switch).
#if defined(_WIN32)
            _putenv("BOZ_LAUNCHER_AUTO_UPDATE=");
#else
            unsetenv("BOZ_LAUNCHER_AUTO_UPDATE");
#endif
            os::Process next;
            std::string launcher = os::join(l.bin_dir, os::executable_name("boz-redux"));
            if (!os::spawn(next, launcher, {"--root", l.root}, {}, "", l.root)) {
                std::fprintf(stderr, "could not start %s: %s\n", launcher.c_str(), os::spawn_error().c_str());
            }
            done = true;
        }
    }

    if (l.staged_pending) {
        mod_zip_discard(l.staged);
    }
    forget_pictures();
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
