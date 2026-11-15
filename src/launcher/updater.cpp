// Client updates (updater.h).
#include "updater.h"

#include "miniz.h"
#include "os.h"
#include "sha256.h"

#include <chrono>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
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
    auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// --- A small JSON reader (GitHub's release list) -------------------------------------------

struct Json {
    enum Kind { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } kind = NUL;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json *get(const char *key) const {
        for (const auto &field : fields) {
            if (field.first == key) {
                return &field.second;
            }
        }
        return nullptr;
    }
    std::string str(const char *key) const {
        const Json *value = get(key);
        return value && value->kind == STRING ? value->text : "";
    }
};

struct Parser {
    const char *at, *end;
    int depth = 0;

    void space() {
        while (at < end && std::isspace((unsigned char)*at)) {
            ++at;
        }
    }
    bool literal(const char *word) {
        size_t n = std::strlen(word);
        if ((size_t)(end - at) >= n && !std::strncmp(at, word, n)) {
            at += n;
            return true;
        }
        return false;
    }
    static void utf8(std::string &out, unsigned code) {
        if (code < 0x80) {
            out += (char)code;
        } else if (code < 0x800) {
            out += (char)(0xc0 | (code >> 6));
            out += (char)(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            out += (char)(0xe0 | (code >> 12));
            out += (char)(0x80 | ((code >> 6) & 0x3f));
            out += (char)(0x80 | (code & 0x3f));
        } else {
            out += (char)(0xf0 | (code >> 18));
            out += (char)(0x80 | ((code >> 12) & 0x3f));
            out += (char)(0x80 | ((code >> 6) & 0x3f));
            out += (char)(0x80 | (code & 0x3f));
        }
    }
    bool hex4(unsigned *code) {
        if (end - at < 4) {
            return false;
        }
        *code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *at++;
            *code <<= 4;
            if (c >= '0' && c <= '9') {
                *code |= (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                *code |= (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                *code |= (unsigned)(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }
    bool string(std::string *out) {
        if (at >= end || *at != '"') {
            return false;
        }
        ++at;
        while (at < end && *at != '"') {
            char c = *at++;
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (at >= end) {
                return false;
            }
            char e = *at++;
            switch (e) {
            case 'n': *out += '\n'; break;
            case 't': *out += '\t'; break;
            case 'r': *out += '\r'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(&code)) {
                    return false;
                }
                if (code >= 0xd800 && code < 0xdc00 && end - at >= 6 && at[0] == '\\' && at[1] == 'u') {
                    at += 2;
                    unsigned low = 0;
                    if (!hex4(&low)) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(*out, code);
                break;
            }
            default: *out += e; break;
            }
        }
        if (at >= end) {
            return false;
        }
        ++at;
        return true;
    }
    bool value(Json *out) {
        if (++depth > 64) {
            return false;
        }
        space();
        bool ok = false;
        if (at >= end) {
        } else if (*at == '{') {
            ++at;
            out->kind = Json::OBJECT;
            space();
            if (at < end && *at == '}') {
                ++at;
                ok = true;
            } else {
                while (true) {
                    space();
                    std::string key;
                    Json item;
                    if (!string(&key)) {
                        break;
                    }
                    space();
                    if (at >= end || *at++ != ':' || !value(&item)) {
                        break;
                    }
                    out->fields.emplace_back(std::move(key), std::move(item));
                    space();
                    if (at < end && *at == ',') {
                        ++at;
                        continue;
                    }
                    ok = at < end && *at++ == '}';
                    break;
                }
            }
        } else if (*at == '[') {
            ++at;
            out->kind = Json::ARRAY;
            space();
            if (at < end && *at == ']') {
                ++at;
                ok = true;
            } else {
                while (true) {
                    Json item;
                    if (!value(&item)) {
                        break;
                    }
                    out->items.push_back(std::move(item));
                    space();
                    if (at < end && *at == ',') {
                        ++at;
                        continue;
                    }
                    ok = at < end && *at++ == ']';
                    break;
                }
            }
        } else if (*at == '"') {
            out->kind = Json::STRING;
            ok = string(&out->text);
        } else if (literal("true")) {
            out->kind = Json::BOOL;
            out->boolean = true;
            ok = true;
        } else if (literal("false")) {
            out->kind = Json::BOOL;
            ok = true;
        } else if (literal("null")) {
            ok = true;
        } else {
            char *stop = nullptr;
            std::string number(at, (size_t)std::min<ptrdiff_t>(end - at, 64));
            out->number = std::strtod(number.c_str(), &stop);
            ok = stop != number.c_str();
            out->kind = Json::NUMBER;
            at += stop - number.c_str();
        }
        --depth;
        return ok;
    }
};

// A version's parts: numbers, then the pre-release text ("" for a release).
struct Version {
    std::vector<long> numbers;
    std::string pre;
};

Version split_version(std::string text) {
    Version v;
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) {
        text.erase(0, 1);
    }
    size_t plus = text.find('+');
    if (plus != std::string::npos) {
        text.resize(plus);
    }
    size_t dash = text.find('-');
    if (dash != std::string::npos) {
        v.pre = text.substr(dash + 1);
        text.resize(dash);
    }
    std::stringstream parts(text);
    std::string part;
    while (std::getline(parts, part, '.')) {
        v.numbers.push_back(std::atol(part.c_str()));
    }
    while (v.numbers.size() < 3) {
        v.numbers.push_back(0);
    }
    return v;
}

// Pre-release identifiers, dot by dot: numbers compare as numbers and sort before words.
int compare_pre(const std::string &a, const std::string &b) {
    std::stringstream left(a), right(b);
    std::string x, y;
    while (true) {
        bool has_x = (bool)std::getline(left, x, '.'), has_y = (bool)std::getline(right, y, '.');
        if (!has_x || !has_y) {
            return has_x - has_y;
        }
        bool nx = !x.empty() && std::all_of(x.begin(), x.end(), ::isdigit);
        bool ny = !y.empty() && std::all_of(y.begin(), y.end(), ::isdigit);
        if (nx && ny) {
            long d = std::atol(x.c_str()) - std::atol(y.c_str());
            if (d) {
                return d < 0 ? -1 : 1;
            }
        } else if (nx != ny) {
            return nx ? -1 : 1;
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
}

// Paths an update may never write: the player's data.
bool protected_path(const std::string &path) {
    static const char *const folders[] = {"assets/", "saves/", "mods/", "import/", "update/"};
    for (const char *folder : folders) {
        if (!path.compare(0, std::strlen(folder), folder)) {
            return true;
        }
    }
    return path == "client.ini" || path == "boz-log.txt";
}

bool safe_relative(const std::string &path) {
    if (path.empty() || path[0] == '/' || path.find(':') != std::string::npos ||
        path.find('\\') != std::string::npos) {
        return false;
    }
    std::stringstream parts(path);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part == ".." || part == ".") {
            return false;
        }
    }
    return true;
}

std::vector<std::string> read_list(const fs::path &file) {
    std::vector<std::string> list;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            list.push_back(line);
        }
    }
    return list;
}

// --- .tar.gz ------------------------------------------------------------------------------

bool gunzip(const std::string &path, std::string *out, std::string *error) {
    std::ifstream in(to_path(path), std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 18 || (unsigned char)data[0] != 0x1f || (unsigned char)data[1] != 0x8b || data[2] != 8) {
        *error = "the update is not a gzip file";
        return false;
    }
    unsigned char flags = (unsigned char)data[3];
    size_t at = 10;
    if (flags & 4) {
        at += 2 + ((unsigned char)data[at] | ((unsigned char)data[at + 1] << 8));
    }
    if (flags & 8) {
        at = data.find('\0', at) + 1;
    }
    if (flags & 16) {
        at = data.find('\0', at) + 1;
    }
    if (flags & 2) {
        at += 2;
    }
    if (at == 0 || at >= data.size()) {
        *error = "the update's gzip header is damaged";
        return false;
    }
    size_t size = 0;
    void *raw = tinfl_decompress_mem_to_heap(data.data() + at, data.size() - at, &size, 0);
    if (!raw) {
        *error = "the update could not be unpacked";
        return false;
    }
    out->assign((const char *)raw, size);
    mz_free(raw);
    return true;
}

bool extract_tar_gz(const std::string &archive, const fs::path &dir, std::string *error) {
    std::string tar;
    if (!gunzip(archive, &tar, error)) {
        return false;
    }
    std::string long_name;
    for (size_t at = 0; at + 512 <= tar.size();) {
        const char *header = tar.data() + at;
        if (!header[0]) {
            break;
        }
        std::string name(header, strnlen(header, 100));
        std::string prefix(header + 345, strnlen(header + 345, 155));
        if (!prefix.empty() && std::string(header + 257, 5) == "ustar") {
            name = prefix + "/" + name;
        }
        if (!long_name.empty()) {
            name = long_name;
            long_name.clear();
        }
        size_t size = (size_t)std::strtoull(std::string(header + 124, 12).c_str(), nullptr, 8);
        unsigned mode = (unsigned)std::strtoul(std::string(header + 100, 8).c_str(), nullptr, 8);
        char type = header[156];
        std::string link(header + 157, strnlen(header + 157, 100));
        size_t data = at + 512;
        at = data + (size + 511) / 512 * 512;
        if (data + size > tar.size()) {
            *error = "the update archive is cut short";
            return false;
        }
        if (type == 'L') {  // GNU long name for the next entry
            long_name.assign(tar.data() + data, strnlen(tar.data() + data, size));
            continue;
        }
        while (!name.empty() && name.back() == '/') {
            name.pop_back();
        }
        if (name.empty() || type == 'x' || type == 'g') {
            continue;
        }
        if (!safe_relative(name)) {
            *error = "the update holds an unsafe file name: " + name;
            return false;
        }
        fs::path target = dir / to_path(name);
        std::error_code code;
        if (type == '5') {
            fs::create_directories(target, code);
            continue;
        }
        fs::create_directories(target.parent_path(), code);
        if (type == '2') {
            if (!safe_relative(link)) {
                *error = "the update holds an unsafe link: " + name;
                return false;
            }
            fs::remove(target, code);
            fs::create_symlink(to_path(link), target, code);
            continue;
        }
        if (type != '0' && type != '\0') {
            continue;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(tar.data() + data, (std::streamsize)size);
        if (!out) {
            *error = "could not write " + name;
            return false;
        }
        out.close();
        fs::permissions(target, (fs::perms)(mode & 0777), fs::perm_options::replace, code);
    }
    return true;
}

bool extract_zip(const std::string &archive, const fs::path &dir, std::string *error) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, archive.c_str(), 0)) {
        *error = "the update is not a zip file";
        return false;
    }
    bool ok = true;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            *error = "the update zip is damaged";
            ok = false;
            break;
        }
        std::string name = stat.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        while (!name.empty() && name.back() == '/') {
            name.pop_back();
        }
        if (name.empty()) {
            continue;
        }
        if (!safe_relative(name)) {
            *error = "the update holds an unsafe file name: " + name;
            ok = false;
            break;
        }
        fs::path target = dir / to_path(name);
        std::error_code code;
        if (stat.m_is_directory) {
            fs::create_directories(target, code);
            continue;
        }
        fs::create_directories(target.parent_path(), code);
        if (!mz_zip_reader_extract_to_file(&zip, i, from_path(target).c_str(), 0)) {
            *error = "could not unpack " + name;
            ok = false;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

}  // namespace

const ReleaseAsset *Release::asset(const std::string &name) const {
    for (const ReleaseAsset &item : assets) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

std::string update_api_url(const std::string &configured) {
    std::string custom = os::getenv_str("BOZ_UPDATE_API");
    if (custom.empty()) {
        custom = configured;
    }
    return custom.empty() ? "https://api.github.com/repos/ZappaVinny/boz-redux/releases?per_page=20"
                          : custom;
}

bool parse_releases(const std::string &json, std::vector<Release> *out) {
    Json root;
    Parser parser{json.data(), json.data() + json.size()};
    if (!parser.value(&root) || root.kind != Json::ARRAY) {
        return false;
    }
    out->clear();
    for (const Json &item : root.items) {
        if (item.kind != Json::OBJECT) {
            continue;
        }
        const Json *draft = item.get("draft");
        if (draft && draft->boolean) {
            continue;
        }
        Release release;
        release.tag = item.str("tag_name");
        release.version = release.tag;
        if (!release.version.empty() && (release.version[0] == 'v' || release.version[0] == 'V')) {
            release.version.erase(0, 1);
        }
        release.title = item.str("name");
        release.notes = item.str("body");
        release.page = item.str("html_url");
        const Json *pre = item.get("prerelease");
        release.prerelease = pre && pre->boolean;
        if (const Json *assets = item.get("assets")) {
            for (const Json &asset : assets->items) {
                ReleaseAsset entry;
                entry.name = asset.str("name");
                entry.url = asset.str("browser_download_url");
                const Json *size = asset.get("size");
                entry.size = size ? (int64_t)size->number : 0;
                if (!entry.name.empty() && !entry.url.empty()) {
                    release.assets.push_back(entry);
                }
            }
        }
        if (!release.version.empty()) {
            out->push_back(release);
        }
    }
    return true;
}

int compare_versions(const std::string &a, const std::string &b) {
    Version x = split_version(a), y = split_version(b);
    size_t n = std::max(x.numbers.size(), y.numbers.size());
    x.numbers.resize(n, 0);
    y.numbers.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (x.numbers[i] != y.numbers[i]) {
            return x.numbers[i] < y.numbers[i] ? -1 : 1;
        }
    }
    if (x.pre.empty() || y.pre.empty()) {
        return x.pre.empty() == y.pre.empty() ? 0 : x.pre.empty() ? 1 : -1;
    }
    return compare_pre(x.pre, y.pre);
}

const Release *newer_release(const std::vector<Release> &releases, const std::string &current,
                             bool beta) {
    const Release *best = nullptr;
    for (const Release &release : releases) {
        if ((release.prerelease && !beta) || compare_versions(release.version, current) <= 0) {
            continue;
        }
        if (!best || compare_versions(release.version, best->version) > 0) {
            best = &release;
        }
    }
    return best;
}

std::string package_name() {
#if defined(__ANDROID__)
    return "BOZ-Redux-android-arm64.apk";
#elif defined(_WIN32)
#if defined(_WIN64)
    return "BOZ-Redux-windows-x64.zip";
#else
    return "BOZ-Redux-windows-x86.zip";
#endif
#elif defined(__x86_64__)
    return "BOZ-Redux-linux-x64.tar.gz";
#else
    return "BOZ-Redux-linux-x86.tar.gz";
#endif
}

std::string sha256_for(const std::string &sums, const std::string &name) {
    std::stringstream lines(sums);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t space = line.find(' ');
        if (space == std::string::npos || space != 64) {
            continue;
        }
        std::string file = line.substr(space);
        file.erase(0, file.find_first_not_of(" *"));
        if (file == name) {
            std::string hash = line.substr(0, 64);
            for (char &c : hash) {
                c = (char)std::tolower((unsigned char)c);
            }
            return hash;
        }
    }
    return "";
}

bool extract_package(const std::string &archive, const std::string &dir, std::string *error) {
    std::error_code code;
    fs::remove_all(to_path(dir), code);
    fs::create_directories(to_path(dir), code);
    bool zip = archive.size() > 4 && archive.compare(archive.size() - 4, 4, ".zip") == 0;
    return zip ? extract_zip(archive, to_path(dir), error) : extract_tar_gz(archive, to_path(dir), error);
}

bool updatable_install(const std::string &root) {
    std::error_code code;
    return fs::is_regular_file(to_path(root) / "files.txt", code);
}

bool apply_update(const std::string &root_text, const std::string &package_text, std::string *error) {
    fs::path root = to_path(root_text), package = to_path(package_text);
    std::vector<std::string> next = read_list(package / "files.txt");
    std::vector<std::string> now = read_list(root / "files.txt");
    if (next.empty()) {
        *error = "the update has no file list";
        return false;
    }
    if (std::find(next.begin(), next.end(), "files.txt") == next.end()) {
        next.push_back("files.txt");
    }
    std::error_code code;
    for (const std::string &file : next) {
        if (!safe_relative(file) || protected_path(file)) {
            *error = "the update's file list names " + file;
            return false;
        }
        if (!fs::exists(fs::symlink_status(package / to_path(file), code))) {
            *error = "the update is missing " + file;
            return false;
        }
    }
    fs::path old = root / "update" / "old";
    fs::remove_all(old, code);
    fs::create_directories(old, code);
    std::set<std::string> keep(next.begin(), next.end());
    std::vector<std::string> moved, written;  // for the way back
    auto move_aside = [&](const std::string &file) {
        fs::path from = root / to_path(file);
        if (!fs::exists(fs::symlink_status(from, code))) {
            return true;
        }
        fs::path to = old / to_path(file);
        fs::create_directories(to.parent_path(), code);
        // A running program or library cannot be overwritten on Windows, but it can be renamed.
        fs::rename(from, to, code);
        if (code) {
            *error = "could not move " + file + " aside: " + code.message();
            return false;
        }
        moved.push_back(file);
        return true;
    };
    bool ok = true;
    for (const std::string &file : next) {
        if (!(ok = move_aside(file))) {
            break;
        }
        fs::path target = root / to_path(file);
        fs::create_directories(target.parent_path(), code);
        fs::copy(package / to_path(file), target, fs::copy_options::copy_symlinks, code);
        if (code) {
            *error = "could not write " + file + ": " + code.message();
            ok = false;
            break;
        }
        written.push_back(file);
    }
    for (size_t i = 0; ok && i < now.size(); ++i) {
        const std::string &file = now[i];
        if (!keep.count(file) && safe_relative(file) && !protected_path(file)) {
            ok = move_aside(file);
        }
    }
    if (ok) {
        return true;
    }
    // The way back: the new files out, the old ones in place again.
    for (const std::string &file : written) {
        fs::remove(root / to_path(file), code);
    }
    for (const std::string &file : moved) {
        fs::rename(old / to_path(file), root / to_path(file), code);
    }
    return false;
}

void finish_update(const std::string &root) {
    std::error_code code;
    fs::remove_all(to_path(root) / "update", code);
}

// --- The launcher's update work ---------------------------------------------------------------

Updates::Updates(std::string root) : root_(std::move(root)) {}

Updates::~Updates() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::string Updates::message() {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

Release Updates::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    return release_;
}

bool Updates::can_install() const {
#if defined(__ANDROID__)
    return true;
#else
    return updatable_install(root_);
#endif
}

void Updates::run(void (Updates::*job)()) {
    State now = state_;
    if (now == CHECKING || now == DOWNLOADING) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    worker_ = std::thread([this, job]() { (this->*job)(); });
}

void Updates::check(bool beta, const std::string &api) {
    beta_ = beta;
    api_ = api;
    run(&Updates::job_check);
}

void Updates::install() {
    if (state_ == FOUND) {
        run(&Updates::job_install);
    }
}

void Updates::fail(const std::string &text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = text;
    }
    state_ = FAILED;
}

// Downloads url into to, showing progress from the file's size against size (when known).
bool Updates::fetch(const std::string &url, const std::string &to, int64_t size, bool api) {
    std::remove(to.c_str());
    std::atomic<bool> done{false};
    bool ok = false;
#if defined(__ANDROID__)
    (void)api;
    std::thread thread([&]() {
        ok = os::download(url, to);
        done = true;
    });
#else
    os::Process process;
    std::vector<std::string> args = {"-fsSL", "--retry", "2", "--connect-timeout", "15"};
    if (api) {
        args.insert(args.end(), {"--max-time", "30", "-H", "Accept: application/vnd.github+json"});
    }
    args.insert(args.end(), {"-o", to, url});
    if (!os::spawn(process, "curl", args, {}, "", "")) {
        return false;
    }
    std::thread thread([&]() {
        ok = os::wait(process) == 0;
        done = true;
    });
#endif
    while (!done) {
        int64_t written = os::file_size(to);
        progress_ = size > 0 && written > 0 ? (float)((double)written / (double)size) : -1.0f;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    thread.join();
    return ok && os::file_size(to) > 0;
}

void Updates::job_check() {
    state_ = CHECKING;
    std::string dir = os::join(root_, "update");
    os::make_dir(dir);
    std::string list = os::join(dir, "releases.json");
    if (!fetch(update_api_url(api_), list, 0, true)) {
        fail("Could not check for updates (no connection?).");
        return;
    }
    std::ifstream in(to_path(list), std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::remove(list.c_str());
    std::vector<Release> releases;
    if (!parse_releases(json, &releases)) {
        fail("The update check got an answer it could not read.");
        return;
    }
    const Release *newer = newer_release(releases, BOZ_VERSION, beta_);
    if (newer && !newer->asset(package_name())) {
        newer = nullptr;  // a release without this platform's package
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (newer) {
        release_ = *newer;
        message_ = "";
        state_ = FOUND;
    } else {
        message_ = "BOZ Redux is up to date.";
        state_ = LATEST;
    }
}

void Updates::job_install() {
    Release release = this->release();
    state_ = DOWNLOADING;
    const ReleaseAsset *asset = release.asset(package_name());
    const ReleaseAsset *sums = release.asset("SHA256SUMS");
    if (!asset || !sums) {
        fail("That release has no package for this device.");
        return;
    }
    std::string dir = os::join(root_, "update");
    os::make_dir(dir);
    std::string sums_file = os::join(dir, "SHA256SUMS");
    std::string download = os::join(dir, asset->name);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Downloading BOZ Redux " + release.version + "...";
    }
    if (!fetch(sums->url, sums_file, sums->size, false) || !fetch(asset->url, download, asset->size, false)) {
        fail("The download failed. Check your connection and try again.");
        return;
    }
    std::ifstream in(to_path(sums_file), std::ios::binary);
    std::string sums_text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::string expected = sha256_for(sums_text, asset->name);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Checking the download...";
    }
    progress_ = -1.0f;
    if (expected.empty() || sha256_file(download) != expected) {
        std::remove(download.c_str());
        fail("The download did not match the release's checksum; nothing was changed.");
        return;
    }
#if defined(__ANDROID__)
    // Android installs the new app itself, after the player agrees.
    if (!os::install_apk(download)) {
        fail("Android could not open its installer for the update.");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = "Android is asking to install the update.";
    state_ = READY;
#else
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Installing...";
    }
    std::string package_root = os::join(dir, "package");
    std::string error;
    if (!extract_package(download, package_root, &error)) {
        fail("Could not unpack the update: " + error + ".");
        return;
    }
    std::string package = os::join(package_root, "BOZ-Redux");
    if (!apply_update(root_, package, &error)) {
        fail("Could not install the update (" + error + "); the old version is still in place.");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = "Updated to " + release.version + ".";
    state_ = READY;
#endif
}
