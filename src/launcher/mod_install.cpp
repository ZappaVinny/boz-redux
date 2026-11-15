// Mods from .zip files (mod_install.h), with miniz.
#include "mod_install.h"

#include "boz_api.h"
#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string &utf8) {
#if defined(_WIN32) && defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));   // C++20: u8path is deprecated
#elif defined(_WIN32)
    return fs::u8path(utf8);
#else
    return fs::path(utf8);
#endif
}

std::string from_path(const fs::path &path) {
#if defined(_WIN32)
    auto text = path.generic_u8string();  // std::string in C++17, std::u8string after
    return std::string(text.begin(), text.end());
#else
    return path.generic_string();
#endif
}

// A usable folder name: letters, digits, . _ - (others become -), not starting with a dot.
std::string folder_name(const std::string &text) {
    std::string out;
    for (char c : text) {
        unsigned char u = (unsigned char)c;
        out += std::isalnum(u) || c == '_' || c == '-' || c == '.' ? c : '-';
    }
    while (!out.empty() && out[0] == '.') {
        out.erase(0, 1);
    }
    return out.size() > 100 ? out.substr(0, 100) : out;
}

bool remove_tree(const fs::path &path, std::string *error) {
    std::error_code code;
    fs::remove_all(path, code);
    if (code && error) {
        *error = "Could not delete " + from_path(path) + ": " + code.message();
    }
    return !code;
}

// The folder holding mod.toml inside the zip ("" for the root, "name/" one level down).
bool find_prefix(mz_zip_archive *zip, std::string *prefix) {
    bool found = false;
    mz_uint count = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < count; ++i) {
        char name[1024];
        mz_zip_reader_get_filename(zip, i, name, sizeof(name));
        std::string entry = name;
        std::replace(entry.begin(), entry.end(), '\\', '/');
        if (entry == "mod.toml") {
            *prefix = "";
            return true;
        }
        size_t slash = entry.find('/');
        if (!found && slash != std::string::npos && slash > 0 && entry.substr(slash + 1) == "mod.toml") {
            *prefix = entry.substr(0, slash + 1);
            found = true;
        }
    }
    return found;
}

// An installed mod with this id (another folder may hold it), or nullptr.
bool installed_with_id(const std::string &mods_dir, const char *id, mod_info *out) {
    std::error_code code;
    for (const fs::directory_entry &entry : fs::directory_iterator(to_path(mods_dir), code)) {
        std::string name = from_path(entry.path().filename());
        if (name.empty() || name[0] == '.' || !entry.is_directory(code)) {
            continue;
        }
        if (mods_read_folder(from_path(entry.path()).c_str(), out) && !std::strcmp(out->id, id)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool mod_zip_safe_name(const std::string &raw) {
    std::string name = raw;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool mod_zip_peek(const std::string &zip_path, std::string *id, std::string *name,
                  std::string *version) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        return false;
    }
    std::string prefix;
    size_t size = 0;
    char *text = nullptr;
    if (find_prefix(&zip, &prefix)) {
        text = (char *)mz_zip_reader_extract_file_to_heap(&zip, (prefix + "mod.toml").c_str(), &size, 0);
    }
    mz_zip_reader_end(&zip);
    if (!text) {
        return false;
    }
    std::string manifest(text, size);
    mz_free(text);
    *id = *name = *version = "";
    size_t at = 0;
    while (at < manifest.size()) {
        size_t end = manifest.find('\n', at);
        std::string line = manifest.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? manifest.size() : end + 1;
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        if (line[first] == '[') {
            break;  // top-level keys only
        }
        size_t equals = line.find('='), open = line.find('"'), close = line.rfind('"');
        if (equals == std::string::npos || open == std::string::npos || close <= open) {
            continue;
        }
        std::string key = line.substr(first, equals - first);
        key.erase(key.find_last_not_of(" \t") + 1);
        std::string value = line.substr(open + 1, close - open - 1);
        if (key == "id") {
            *id = value;
        } else if (key == "name") {
            *name = value;
        } else if (key == "version") {
            *version = value;
        }
    }
    return true;
}

StagedMod mod_zip_stage(const std::string &zip_path, const std::string &mods_dir,
                        const std::string &fallback_name) {
    StagedMod staged;
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    std::error_code code;
    fs::create_directories(to_path(mods_dir), code);
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        staged.error = "That file is not a .zip, or it is damaged.";
        return staged;
    }
    std::string prefix;
    if (!find_prefix(&zip, &prefix)) {
        mz_zip_reader_end(&zip);
        staged.error = "That .zip has no mod.toml, so it is not a BOZ Redux mod. (Map projects are "
                       "built into mods with the SDK.)";
        return staged;
    }
    fs::path staging = to_path(mods_dir) / ".incoming.part";
    remove_tree(staging, nullptr);
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && staged.error.empty(); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            staged.error = "That .zip is damaged.";
            break;
        }
        std::string name = stat.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        if (!mod_zip_safe_name(name)) {
            staged.error = "That .zip has unsafe file names (" + name + "); it was not installed.";
            break;
        }
        std::string relative = name.substr(prefix.size());
        if (relative.empty()) {
            continue;
        }
        fs::path out = staging / to_path(relative);
        if (stat.m_is_directory) {
            fs::create_directories(out, code);
            continue;
        }
        fs::create_directories(out.parent_path(), code);
        if (!mz_zip_reader_extract_to_file(&zip, i, from_path(out).c_str(), 0)) {
            staged.error = "Could not unpack " + relative + " (is the disk full?).";
        }
    }
    mz_zip_reader_end(&zip);
    if (!staged.error.empty()) {
        remove_tree(staging, nullptr);
        return staged;
    }
    mods_read_folder(from_path(staging).c_str(), &staged.mod);
    // The folder: where this mod already is, else its id, else the zip's folder or file name.
    mod_info old{};
    if (staged.mod.id[0] && std::strcmp(staged.mod.id, ".incoming.part") != 0 &&
        installed_with_id(mods_dir, staged.mod.id, &old)) {
        staged.update = true;
        staged.folder = old.folder;
        staged.old_version = old.version;
    } else {
        staged.folder = folder_name(std::strcmp(staged.mod.id, ".incoming.part") ? staged.mod.id : "");
        if (staged.folder.empty()) {
            staged.folder = folder_name(prefix.empty() ? fallback_name : prefix.substr(0, prefix.size() - 1));
        }
        if (staged.folder.empty()) {
            staged.folder = "mod";
        }
    }
    if (!std::strcmp(staged.mod.id, ".incoming.part")) {
        std::snprintf(staged.mod.id, sizeof(staged.mod.id), "%s", staged.folder.c_str());
        std::snprintf(staged.mod.name, sizeof(staged.mod.name), "%s", staged.folder.c_str());
    }
    std::snprintf(staged.mod.folder, sizeof(staged.mod.folder), "%s", staged.folder.c_str());
    fs::path final_staging = to_path(mods_dir) / ("." + staged.folder + ".part");
    remove_tree(final_staging, nullptr);
    fs::rename(staging, final_staging, code);
    if (code) {
        remove_tree(staging, nullptr);
        staged.error = "Could not prepare the mod: " + code.message();
        return staged;
    }
    staged.staging = from_path(final_staging);
    if (staged.mod.game[0] && std::strcmp(staged.mod.game, "1.0.11") != 0) {
        staged.warnings.push_back(std::string("Made for game version ") + staged.mod.game +
                                  "; this client runs 1.0.11.");
    }
    if (mods_api_differs(&staged.mod)) {
        staged.warnings.push_back("Built for BOZ Redux API " + std::to_string(staged.mod.client_api) +
                                  " (this one has " + std::to_string(BOZ_LUA_API) +
                                  "); it may not work.");
    }
    std::string connects = mod_connects_text(staged.mod);
    if (!connects.empty()) {
        staged.warnings.push_back(connects);
    }
    staged.ok = true;
    return staged;
}

std::string mod_connects_text(const mod_info &mod) {
    std::string named;
    bool any = false;
    std::string list = mod.connects;
    size_t at = 0;
    while (at <= list.size() && !list.empty()) {
        size_t end = list.find(',', at);
        std::string host = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (host == "*") {
            any = true;
        } else if (!host.empty()) {
            named += (named.empty() ? "" : ", ") + host;
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    if (named.empty() && !any) {
        return "";
    }
    std::string text = "Can send Play Online to ";
    if (!named.empty()) {
        text += named + (any ? ", or to any server you enter." : ".");
    } else {
        text += "any server you enter.";
    }
    return text;
}

bool mod_zip_commit(const StagedMod &staged, const std::string &mods_dir, std::string *error) {
    fs::path target = to_path(mods_dir) / to_path(staged.folder);
    fs::path backup = to_path(mods_dir) / ("." + staged.folder + ".old");
    std::error_code code;
    remove_tree(backup, nullptr);
    bool had_old = fs::exists(target, code);
    if (had_old) {
        // A link (the Developer mod in a checkout) is replaced, not followed.
        fs::rename(target, backup, code);
        if (code) {
            *error = "Could not move the old " + staged.folder + " aside (is the game running?): " +
                     code.message();
            return false;
        }
    }
    fs::rename(to_path(staged.staging), target, code);
    if (code) {
        if (had_old) {
            fs::rename(backup, target, code);
        }
        *error = "Could not install " + staged.folder + ".";
        return false;
    }
    if (had_old) {
        remove_tree(backup, nullptr);
    }
    return true;
}

void mod_zip_discard(const StagedMod &staged) {
    if (!staged.staging.empty()) {
        remove_tree(to_path(staged.staging), nullptr);
    }
}

bool mod_remove(const std::string &mods_dir, const std::string &folder, std::string *error) {
    if (folder.empty() || folder[0] == '.' || !mod_zip_safe_name(folder) ||
        folder.find('/') != std::string::npos) {
        *error = "Not a mod folder.";
        return false;
    }
    fs::path target = to_path(mods_dir) / to_path(folder);
    std::error_code code;
    if (fs::is_symlink(target, code)) {
        fs::remove(target, code);  // a linked mod: the link only
        if (code) {
            *error = "Could not remove " + folder + ": " + code.message();
        }
        return !code;
    }
    return remove_tree(target, error);
}

void mod_clean_leftovers(const std::string &mods_dir) {
    std::error_code code;
    for (const fs::directory_entry &entry : fs::directory_iterator(to_path(mods_dir), code)) {
        std::string name = from_path(entry.path().filename());
        bool part = name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0;
        bool old = name.size() > 4 && name.compare(name.size() - 4, 4, ".old") == 0;
        if (!name.empty() && name[0] == '.' && (part || old)) {
            remove_tree(entry.path(), nullptr);
        }
    }
}
