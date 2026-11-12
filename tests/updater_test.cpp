// Client updates (src/launcher/updater.cpp): versions, the release list, checksums, unpacking a
// .tar.gz, and swapping an install's files without touching the player's.
#include "updater.h"

extern "C" {
#include "test_support.h"
}

#include "miniz.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A ustar header block.
static std::string tar_header(const std::string &name, size_t size, char type, const std::string &link = "") {
    char block[512] = {0};
    std::snprintf(block, 100, "%s", name.c_str());
    std::snprintf(block + 100, 8, "%07o", 0755);
    std::snprintf(block + 108, 8, "%07o", 0);
    std::snprintf(block + 116, 8, "%07o", 0);
    std::snprintf(block + 124, 12, "%011o", (unsigned)size);
    std::snprintf(block + 136, 12, "%011o", 0);
    block[156] = type;
    std::snprintf(block + 157, 100, "%s", link.c_str());
    std::memcpy(block + 257, "ustar", 6);
    std::memset(block + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned char c : block) {
        sum += c;
    }
    std::snprintf(block + 148, 8, "%06o", sum);
    return std::string(block, 512);
}

static std::string tar_file(const std::string &name, const std::string &data) {
    std::string out = tar_header(name, data.size(), '0') + data;
    out.append((512 - data.size() % 512) % 512, '\0');
    return out;
}

static void write_tar_gz(const fs::path &path, const std::string &tar) {
    size_t size = 0;
    void *deflated = tdefl_compress_mem_to_heap(tar.data(), tar.size(), &size, 128);
    assert(deflated);
    std::string gz("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    gz.append((const char *)deflated, size);
    mz_free(deflated);
    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, (const unsigned char *)tar.data(), tar.size());
    for (int i = 0; i < 4; ++i) {
        gz += (char)((crc >> (8 * i)) & 0xff);
    }
    for (int i = 0; i < 4; ++i) {
        gz += (char)((tar.size() >> (8 * i)) & 0xff);
    }
    write(path, gz);
}

int main() {
    // Versions: numbers, then pre-releases before their release.
    assert(compare_versions("0.4.0", "0.4.0") == 0 && compare_versions("v0.4.1", "0.4.0") > 0);
    assert(compare_versions("0.4.0-dev", "0.4.0") < 0 && compare_versions("0.10.0", "0.9.9") > 0);
    assert(compare_versions("0.4.0-beta.2", "0.4.0-beta.10") < 0);
    assert(compare_versions("0.4.0-beta.1", "0.4.0-rc.1") < 0 && compare_versions("1.0", "1.0.0") == 0);

    // GitHub's release list: drafts left out, pre-releases only for beta, the newest wins.
    const char *json = R"([
      {"tag_name": "v0.5.0-beta.1", "name": "Beta", "prerelease": true, "draft": false, "body": "b",
       "html_url": "https://x/b", "assets": []},
      {"tag_name": "v0.4.2", "name": "0.4.2", "prerelease": false, "draft": false,
       "body": "Fixes:\n- \"quoted\" é", "html_url": "https://x/r",
       "assets": [{"name": "SHA256SUMS", "browser_download_url": "https://x/s", "size": 120},
                  {"name": "BOZ-Redux-linux-x64.tar.gz", "browser_download_url": "https://x/l", "size": 5}]},
      {"tag_name": "v9.9.9", "draft": true, "prerelease": false, "assets": []},
      {"tag_name": "v0.4.1", "prerelease": false, "draft": false, "assets": [], "extra": {"a": [1, 2.5e3, null]}}
    ])";
    std::vector<Release> releases;
    assert(parse_releases(json, &releases) && releases.size() == 3);
    assert(releases[1].notes == "Fixes:\n- \"quoted\" \xc3\xa9" && releases[1].assets.size() == 2);
    assert(releases[1].asset("SHA256SUMS")->size == 120);
    const Release *newer = newer_release(releases, "0.4.0-dev", false);
    assert(newer && newer->version == "0.4.2");
    assert(newer_release(releases, "0.4.0", true)->version == "0.5.0-beta.1");
    assert(!newer_release(releases, "0.4.2", false));
    assert(!parse_releases("{\"message\": \"rate limited\"}", &releases));
    assert(!parse_releases("[{\"tag_name\": ", &releases));

    // SHA256SUMS lines ("hash  name" or "hash *name").
    std::string hash(64, 'a');
    std::string sums = hash + "  BOZ-Redux-linux-x64.tar.gz\r\n" + std::string(64, 'B') + " *other.zip\n";
    assert(sha256_for(sums, "BOZ-Redux-linux-x64.tar.gz") == hash);
    assert(sha256_for(sums, "other.zip") == std::string(64, 'b') && sha256_for(sums, "none").empty());

    char dir[1024];
    test_temp_dir(dir, sizeof(dir), "updater");
    fs::path base = dir;

    // An install of version 1: its files, plus the player's.
    fs::path root = base / "root";
    write(root / "files.txt", "boz-redux\nlib/old.so\nREADME.txt\nfiles.txt\n");
    write(root / "boz-redux", "launcher 1");
    write(root / "lib/old.so", "old library");
    write(root / "README.txt", "readme 1");
    write(root / "client.ini", "[player]\nname = Me\n");
    write(root / "saves/save.i3d", "my save");
    write(root / "mods/m/mod.toml", "id = \"m\"\n");
    assert(updatable_install(root.string()) && !updatable_install(base.string()));

    // Version 2 as a .tar.gz (BOZ-Redux/...), with a link and a new file; old.so is gone.
    std::string tar = tar_file("BOZ-Redux/files.txt", "boz-redux\nlib/new.so\nlib/new.so.1\nREADME.txt\n") +
                      tar_file("BOZ-Redux/boz-redux", "launcher 2") +
                      tar_file("BOZ-Redux/lib/new.so", "new library") +
                      tar_file("BOZ-Redux/README.txt", "readme 2");
#if !defined(_WIN32)
    tar += tar_header("BOZ-Redux/lib/new.so.1", 0, '2', "new.so");
#else
    tar += tar_file("BOZ-Redux/lib/new.so.1", "copy");
#endif
    tar.append(1024, '\0');
    write_tar_gz(base / "update.tar.gz", tar);
    std::string error;
    assert(extract_package((base / "update.tar.gz").string(), (base / "unpacked").string(), &error));
    fs::path package = base / "unpacked" / "BOZ-Redux";
    assert(read(package / "boz-redux") == "launcher 2" && read(package / "lib/new.so.1") != "");

    assert(apply_update(root.string(), package.string(), &error));
    assert(read(root / "boz-redux") == "launcher 2" && read(root / "README.txt") == "readme 2");
    assert(read(root / "lib/new.so") == "new library" && !fs::exists(root / "lib/old.so"));
    assert(read(root / "client.ini") == "[player]\nname = Me\n" && read(root / "saves/save.i3d") == "my save");
    assert(fs::exists(root / "mods/m/mod.toml") && read(root / "files.txt").find("new.so") != std::string::npos);
    assert(read(root / "update/old/boz-redux") == "launcher 1");
    finish_update(root.string());
    assert(!fs::exists(root / "update"));

    // A package that would write the player's files, or is missing a file, changes nothing.
    write(package / "files.txt", "boz-redux\nclient.ini\n");
    write(package / "client.ini", "replaced");
    assert(!apply_update(root.string(), package.string(), &error) && read(root / "client.ini") != "replaced");
    write(package / "files.txt", "boz-redux\n../outside\n");
    assert(!apply_update(root.string(), package.string(), &error));
    write(package / "files.txt", "boz-redux\nmissing.dll\n");
    assert(!apply_update(root.string(), package.string(), &error) && read(root / "boz-redux") == "launcher 2");

    // Unsafe names in an archive are refused.
    write_tar_gz(base / "evil.tar.gz", tar_file("../evil.txt", "no") + std::string(1024, '\0'));
    assert(!extract_package((base / "evil.tar.gz").string(), (base / "evil").string(), &error));
    assert(!fs::exists(base / "evil.txt"));

    assert(!package_name().empty());
    test_remove_tree(dir);
    std::puts("updater_test: ok");
    return 0;
}
