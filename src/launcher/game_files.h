// Game data setup: extract the player's APK, then get the data packs from Activision's CDN or a
// folder. Long steps run on a worker thread; the UI polls status().
#pragma once

#include "mod_install.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PackStatus {
    const char *name;
    bool present = false;  // right size (hash checked when it was downloaded or imported)
};

struct GameStatus {
    bool image = false;           // assets/boz.s3e.unpacked exists
    bool image_verified = false;  // boz.s3e matches the 1.0.11 payload
    bool image_checked = false;
    PackStatus packs[2] = {{"blackops_etc.dz"}, {"blackops_gles1.dz"}};

    // PLAY needs the exact 1.0.11 game code: mods and the client call it at fixed addresses, so
    // another version (1.0.8.7, a repack) starts but breaks them.
    bool ready() const { return image && image_verified && packs[0].present && packs[1].present; }
};

class GameFiles {
public:
    GameFiles(std::string root, std::string bin_dir);
    ~GameFiles();

    const std::string &root() const { return root_; }
    const std::string &assets() const { return assets_; }

    GameStatus status();  // cheap; call every frame
    bool busy() const { return busy_; }
    float progress() const { return progress_; }  // 0..1, or < 0 when unknown
    std::string message();                        // what the current or last job did

    void install_apk(const std::string &apk_path);
    void download_packs();
    void import_packs(const std::string &folder);
#if defined(__ANDROID__)
    // Picked files (content:// URIs): each one with the size of a pack becomes that pack, so an
    // Android OBB (main.*.obb is blackops_etc.dz) works under any name.
    void import_pack_documents(const std::vector<std::string> &uris);
#endif
    // A mod's .zip (a path, or a content:// URI on Android) unpacked into mods/ staging; the UI
    // then takes it (take_staged_mod) and asks before putting it in place.
    void stage_mod(const std::string &zip);
    bool take_staged_mod(StagedMod *staged);  // once per staged zip

private:
    void start(void (GameFiles::*job)(std::string), std::string argument);
    void set_message(const std::string &text);
    void job_install(std::string apk_path);
    void job_download(std::string unused);
    void job_import(std::string folder);
#if defined(__ANDROID__)
    void job_import_documents(std::string uris);
    bool fetch(bool (*copy)(const std::string &, const std::string &), const std::string &from,
               const std::string &part, int64_t size);
#endif
    void job_stage_mod(std::string zip);
    std::atomic<bool> staged_ready_{false};
    StagedMod staged_;
    void check_image();
    bool verify_pack(int index, const std::string &path);

    std::string root_;
    std::string assets_;
    std::string bin_dir_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<float> progress_{-1.0f};
    std::atomic<int> image_state_{0};  // 0 unknown, 1 verified, 2 unknown version
    std::mutex message_mutex_;
    std::string message_;
};
