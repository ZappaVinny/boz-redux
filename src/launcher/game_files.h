// Game data setup: extract the player's APK, then get the data packs from Activision's CDN or a
// folder. Long steps run on a worker thread; the UI polls status().
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct PackStatus {
    const char *name;
    bool present = false;  // right size (hash checked when it was downloaded or imported)
};

struct GameStatus {
    bool image = false;           // assets/boz.s3e.unpacked exists
    bool image_verified = false;  // boz.s3e matches the 1.0.11 payload
    bool image_checked = false;
    PackStatus packs[2] = {{"blackops_etc.dz"}, {"blackops_gles1.dz"}};

    bool ready() const { return image && packs[0].present && packs[1].present; }
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

private:
    void start(void (GameFiles::*job)(std::string), std::string argument);
    void set_message(const std::string &text);
    void job_install(std::string apk_path);
    void job_download(std::string unused);
    void job_import(std::string folder);
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
