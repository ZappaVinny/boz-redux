// Client updates from GitHub releases: which release is newer, which package fits this build,
// unpacking it and swapping in the files it owns (files.txt), never the player's own files.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ReleaseAsset {
    std::string name, url;
    int64_t size = 0;
};

struct Release {
    std::string tag;       // "v0.4.1"
    std::string version;   // "0.4.1"
    std::string title, notes, page;
    bool prerelease = false;
    std::vector<ReleaseAsset> assets;
    const ReleaseAsset *asset(const std::string &name) const;
};

// Where the launcher asks: GitHub's release list. BOZ_UPDATE_API, else the given test address
// (client.ini [launcher] update_api, for phones), replaces it.
std::string update_api_url(const std::string &configured = "");

// GitHub's JSON release list; false when it is not one.
bool parse_releases(const std::string &json, std::vector<Release> *out);

// Semantic versions: <0 when a is older than b. "0.4.0-dev" is older than "0.4.0".
int compare_versions(const std::string &a, const std::string &b);

// The newest release newer than current (pre-releases only with beta), or nullptr.
const Release *newer_release(const std::vector<Release> &releases, const std::string &current,
                             bool beta);

// This build's package in a release: BOZ-Redux-<platform>-<arch>.zip / .tar.gz / .apk.
std::string package_name();

// The hash a SHA256SUMS file gives for name ("" when it lists none).
std::string sha256_for(const std::string &sums, const std::string &name);

// Unpacks a .zip or .tar.gz into dir; false with *error set.
bool extract_package(const std::string &archive, const std::string &dir, std::string *error);

// Whether this game folder is an installed package (files.txt), which updates can replace.
bool updatable_install(const std::string &root);

// Replaces the files of the install in root with the unpacked package (its files.txt) and
// removes those the new one no longer has. Never touches the player's folders (assets, saves,
// mods, import) or client.ini. The old files go to root/update/old; any failure puts them back.
bool apply_update(const std::string &root, const std::string &package, std::string *error);

// After a restart: removes what an update left behind (root/update).
void finish_update(const std::string &root);

// The launcher's update work, on a thread of its own: check, then download, verify and install.
class Updates {
public:
    enum State { IDLE, CHECKING, LATEST, FOUND, DOWNLOADING, READY, FAILED };

    explicit Updates(std::string root);
    ~Updates();

    // Finds the newest release this client can update to; api: a test server's address or "".
    void check(bool beta, const std::string &api = "");
    void install();         // the release found: PC: files swapped, then READY (restart);
                            // Android: the APK handed to the system installer, then READY
    State state() const { return state_; }
    float progress() const { return progress_; }  // 0..1, or < 0 when unknown
    std::string message();
    Release release();
    // Whether this client can update itself (an installed package, or Android); a development
    // checkout is pointed at the download page instead.
    bool can_install() const;

private:
    void run(void (Updates::*job)());
    void job_check();
    void job_install();
    bool fetch(const std::string &url, const std::string &to, int64_t size, bool api);
    void fail(const std::string &text);

    std::string root_;
    bool beta_ = false;
    std::string api_;
    std::thread worker_;
    std::atomic<State> state_{IDLE};
    std::atomic<float> progress_{-1.0f};
    std::mutex mutex_;
    std::string message_;
    Release release_;
};
