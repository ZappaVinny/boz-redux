// Mod .zip installs (src/launcher/mod_install.cpp): staging, updates, unsafe names, removal.
#include "mod_install.h"
extern "C" {
#include "test_support.h"
}

#include "miniz.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

static std::string g_dir;

static void add(const std::string &zip, const char *name, const char *text) {
    bool ok = mz_zip_add_mem_to_archive_file_in_place(zip.c_str(), name, text, std::strlen(text),
                                                      nullptr, 0, MZ_BEST_SPEED);
    assert(ok);
}

static bool exists(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

static std::string read(const std::string &path) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return "";
    }
    char buffer[256] = "";
    size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    return std::string(buffer, n);
}

int main() {
    char dir[1024];
    test_temp_dir(dir, sizeof(dir), "mod_install");
    g_dir = dir;
    std::string mods = g_dir + "/mods";

    // Names: relative and inside the zip only.
    assert(mod_zip_safe_name("assets/a.bin") && mod_zip_safe_name("my-mod/mod.toml"));
    assert(!mod_zip_safe_name("../evil") && !mod_zip_safe_name("a/../../b") &&
           !mod_zip_safe_name("/etc/passwd") && !mod_zip_safe_name("C:/x") &&
           !mod_zip_safe_name("a\\..\\b"));

    // A zip of the mod's folder: mod.toml one folder down; the folder becomes the mod's id.
    std::string zip = g_dir + "/cool.zip";
    add(zip, "Cool Mod/mod.toml", "id = \"cool\"\nname = \"Cool\"\nversion = \"1.0\"\ngame = \"1.0.11\"\n"
                                  "[sdk]\nclient_api = 99\n");
    add(zip, "Cool Mod/assets/a.txt", "one");
    add(zip, "stray.txt", "left out");
    std::string id, name, version;
    assert(mod_zip_peek(zip, &id, &name, &version) && id == "cool" && name == "Cool" && version == "1.0");
    StagedMod staged = mod_zip_stage(zip, mods, "cool");
    assert(staged.ok && !staged.update && staged.folder == "cool");
    assert(!std::strcmp(staged.mod.name, "Cool") && staged.warnings.size() == 1);  // client_api 99
    assert(exists(staged.staging + "/assets/a.txt") && !exists(staged.staging + "/stray.txt"));
    std::string error;
    assert(mod_zip_commit(staged, mods, &error));
    assert(read(mods + "/cool/assets/a.txt") == "one" && !exists(staged.staging));

    // The same id again is an update into the folder it already has, replacing its files.
    std::string zip2 = g_dir + "/cool2.zip";
    add(zip2, "mod.toml", "id = \"cool\"\nname = \"Cool\"\nversion = \"1.1\"\n");
    add(zip2, "assets/b.txt", "two");
    staged = mod_zip_stage(zip2, mods, "cool2");
    assert(staged.ok && staged.update && staged.folder == "cool" && staged.old_version == "1.0");
    assert(mod_zip_commit(staged, mods, &error));
    assert(read(mods + "/cool/assets/b.txt") == "two" && !exists(mods + "/cool/assets/a.txt"));
    assert(!exists(mods + "/.cool.old"));

    // Cancelled: nothing changes.
    staged = mod_zip_stage(zip, mods, "cool");
    assert(staged.ok);
    mod_zip_discard(staged);
    assert(!exists(staged.staging) && read(mods + "/cool/assets/b.txt") == "two");

    // Not a mod, not a zip, unsafe names.
    std::string plain = g_dir + "/plain.zip";
    add(plain, "readme.txt", "hi");
    assert(!mod_zip_stage(plain, mods, "plain").ok);
    assert(!mod_zip_peek(plain, &id, &name, &version));
    FILE *junk = std::fopen((g_dir + "/junk.zip").c_str(), "wb");
    std::fputs("not a zip", junk);
    std::fclose(junk);
    assert(!mod_zip_stage(g_dir + "/junk.zip", mods, "junk").ok);
    std::string evil = g_dir + "/evil.zip";
    add(evil, "mod.toml", "id = \"evil\"\n");
    add(evil, "../outside.txt", "no");
    StagedMod bad = mod_zip_stage(evil, mods, "evil");
    assert(!bad.ok && !exists(g_dir + "/outside.txt") && !exists(mods + "/.incoming.part"));

    // No id: named after the zip.
    std::string anon = g_dir + "/My Mod!.zip";
    add(anon, "mod.toml", "name = \"Anon\"\n");
    staged = mod_zip_stage(anon, mods, "My Mod!");
    assert(staged.ok && staged.folder == "My-Mod-");
    mod_zip_discard(staged);

    // Leftovers of an interrupted install are cleaned; removal deletes the folder.
    std::filesystem::create_directories(mods + "/.x.part");
    mod_clean_leftovers(mods);
    assert(!exists(mods + "/.x.part") && exists(mods + "/cool"));
    assert(!mod_remove(mods, "../cool", &error));
    assert(mod_remove(mods, "cool", &error) && !exists(mods + "/cool"));

    test_remove_tree(dir);
    std::puts("mod_install_test: ok");
    return 0;
}
