#include "game_files.h"

#include "os.h"
#include "sha256.h"

#include <chrono>
#include <cstdio>

namespace {

const char *const CDN = "http://cdn-boz-android.callofduty.com/PROD/CODBOZ/1_0_9/";
// boz.s3e from the 1.0.11 APK (versionCode 1045111).
const char *const IMAGE_SHA256 = "f458c15a7111779ad320af377d0bb751294119788ba06d430bee0cc977539fee";

struct PackInfo {
    const char *name;
    int64_t size;
    const char *sha256;
};

const PackInfo PACKS[2] = {
    {"blackops_etc.dz", 487603915,
     "670cefce1951f1fb710d9c9d00d6103f4b81379c414885057af450a897baaedc"},
    {"blackops_gles1.dz", 337594086,
     "60846cf7a121ebaa200be82bd6811adcec44c607414b41086a6ffa1bd339b12c"},
};

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

GameFiles::GameFiles(std::string root, std::string bin_dir)
    : root_(std::move(root)), assets_(os::join(root_, "assets")), bin_dir_(std::move(bin_dir)) {
    if (os::exists(os::join(assets_, "boz.s3e"))) {
        start(&GameFiles::job_install, "");  // empty path: only re-check the installed version
    }
}

GameFiles::~GameFiles() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

GameStatus GameFiles::status() {
    GameStatus status;
    status.image = os::exists(os::join(assets_, "boz.s3e.unpacked"));
    int image_state = image_state_;
    status.image_checked = image_state != 0;
    status.image_verified = image_state == 1;
    for (int i = 0; i < 2; ++i) {
        status.packs[i].present = os::file_size(os::join(assets_, PACKS[i].name)) == PACKS[i].size;
    }
    return status;
}

std::string GameFiles::message() {
    std::lock_guard<std::mutex> lock(message_mutex_);
    return message_;
}

void GameFiles::set_message(const std::string &text) {
    std::lock_guard<std::mutex> lock(message_mutex_);
    message_ = text;
}

void GameFiles::start(void (GameFiles::*job)(std::string), std::string argument) {
    if (busy_) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    busy_ = true;
    progress_ = -1.0f;
    worker_ = std::thread([this, job, argument]() {
        (this->*job)(argument);
        busy_ = false;
    });
}

void GameFiles::install_apk(const std::string &apk_path) {
    start(&GameFiles::job_install, apk_path);
}

void GameFiles::download_packs() {
    start(&GameFiles::job_download, "");
}

void GameFiles::import_packs(const std::string &folder) {
    start(&GameFiles::job_import, folder);
}

void GameFiles::check_image() {
    std::string hash = sha256_file(os::join(assets_, "boz.s3e"));
    image_state_ = hash == IMAGE_SHA256 ? 1 : 2;
}

void GameFiles::job_install(std::string apk_path) {
    if (apk_path.empty()) {
        check_image();
        return;
    }
    if (!os::exists(apk_path)) {
        set_message("APK not found: " + apk_path);
        return;
    }
    set_message("Extracting the APK...");
    os::make_dir(assets_);
    os::Process process;
    std::string extractor = os::join(bin_dir_, os::executable_name("codboz_apk_extract"));
    if (!os::spawn(process, extractor, {"extract", apk_path, assets_}, {},
                   os::join(root_, "setup-log.txt"), "")) {
        set_message("Could not start the APK extractor (" + extractor + ").");
        return;
    }
    int code = os::wait(process);
    if (code != 0 || !os::exists(os::join(assets_, "boz.s3e.unpacked"))) {
        set_message("That APK could not be extracted. Is it the Black Ops Zombies APK? "
                    "Details are in setup-log.txt.");
        return;
    }
    set_message("Checking the game version...");
    check_image();
    set_message(image_state_ == 1
                    ? "APK installed: version 1.0.11."
                    : "APK installed, but it is not the 1.0.11 release this client was built for. "
                      "It may not work.");
}

bool GameFiles::verify_pack(int index, const std::string &path) {
    if (os::file_size(path) != PACKS[index].size) {
        return false;
    }
    progress_ = 0.0f;
    return sha256_file(path, &progress_) == PACKS[index].sha256;
}

void GameFiles::job_download(std::string) {
    os::make_dir(assets_);
    for (int i = 0; i < 2; ++i) {
        std::string target = os::join(assets_, PACKS[i].name);
        if (os::file_size(target) == PACKS[i].size) {
            continue;
        }
        std::string part = target + ".part";
        std::remove(part.c_str());
        set_message(std::string("Downloading ") + PACKS[i].name + " from Activision's server...");
        os::Process process;
        if (!os::spawn(process, "curl", {"-fsSL", "-o", part, std::string(CDN) + PACKS[i].name}, {},
                       "", "")) {
            set_message("Could not start curl to download the data packs.");
            return;
        }
        while (process.running()) {
            int64_t size = os::file_size(part);
            progress_ = size > 0 ? (float)((double)size / (double)PACKS[i].size) : 0.0f;
            sleep_ms(100);
        }
        if (process.exit_code != 0) {
            std::remove(part.c_str());
            set_message(std::string("Download of ") + PACKS[i].name +
                        " failed. Check your connection, or import the packs from a folder.");
            return;
        }
        set_message(std::string("Checking ") + PACKS[i].name + "...");
        if (!verify_pack(i, part)) {
            std::remove(part.c_str());
            set_message(std::string(PACKS[i].name) + " did not match the expected file.");
            return;
        }
        if (!os::rename_file(part, target)) {
            set_message(std::string("Could not save ") + PACKS[i].name + ".");
            return;
        }
    }
    set_message("Data packs ready.");
}

void GameFiles::job_import(std::string folder) {
    os::make_dir(assets_);
    int imported = 0;
    for (int i = 0; i < 2; ++i) {
        std::string target = os::join(assets_, PACKS[i].name);
        if (os::file_size(target) == PACKS[i].size) {
            continue;
        }
        std::string source = os::join(folder, PACKS[i].name);
        if (!os::exists(source)) {
            source = os::join(os::join(folder, "obb"), PACKS[i].name);
        }
        if (!os::exists(source)) {
            continue;
        }
        set_message(std::string("Checking ") + PACKS[i].name + "...");
        if (!verify_pack(i, source)) {
            set_message(std::string(PACKS[i].name) + " in that folder is not the right file.");
            return;
        }
        set_message(std::string("Copying ") + PACKS[i].name + "...");
        progress_ = -1.0f;
        std::string part = target + ".part";
        if (!os::copy_file(source, part) || !os::rename_file(part, target)) {
            std::remove(part.c_str());
            set_message(std::string("Could not copy ") + PACKS[i].name + ".");
            return;
        }
        ++imported;
    }
    GameStatus now = status();
    if (now.packs[0].present && now.packs[1].present) {
        set_message(imported ? "Data packs imported." : "Data packs were already in place.");
    } else {
        set_message("That folder does not contain blackops_etc.dz and blackops_gles1.dz.");
    }
}
