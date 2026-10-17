#include "os.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sys/stat.h>
#include <direct.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace os {

std::string getenv_str(const char *name) {
    const char *value = std::getenv(name);
    return value ? value : "";
}

std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) {
        return b;
    }
    char last = a.back();
    if (last == '/' || last == '\\') {
        return a + b;
    }
    return a + "/" + b;
}

bool exists(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

int64_t file_size(const std::string &path) {
#if defined(_WIN32)
    struct _stat64 info;
    return _stat64(path.c_str(), &info) == 0 ? (int64_t)info.st_size : -1;
#else
    struct stat info;
    return stat(path.c_str(), &info) == 0 ? (int64_t)info.st_size : -1;
#endif
}

bool make_dir(const std::string &path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || exists(path);
#else
    return mkdir(path.c_str(), 0755) == 0 || exists(path);
#endif
}

bool copy_file(const std::string &from, const std::string &to) {
    FILE *in = std::fopen(from.c_str(), "rb");
    if (!in) {
        return false;
    }
    FILE *out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    std::vector<char> buffer(1 << 20);
    bool ok = true;
    size_t count;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
        if (std::fwrite(buffer.data(), 1, count, out) != count) {
            ok = false;
            break;
        }
    }
    ok = ok && !std::ferror(in);
    std::fclose(in);
    ok = std::fclose(out) == 0 && ok;
    return ok;
}

bool rename_file(const std::string &from, const std::string &to) {
#if defined(_WIN32)
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

std::string executable_name(const std::string &base) {
#if defined(_WIN32)
    return base + ".exe";
#else
    return base;
#endif
}

#if defined(_WIN32)

std::string exe_dir() {
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
    std::string result(path, length);
    size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? "." : result.substr(0, slash);
}

static std::string quote(const std::string &arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

bool spawn(Process &process, const std::string &program, const std::vector<std::string> &args,
           const std::map<std::string, std::string> &env, const std::string &log_path,
           const std::string &working_dir) {
    std::string command = quote(program);
    for (const std::string &arg : args) {
        command += " " + quote(arg);
    }
    for (const auto &entry : env) {
        SetEnvironmentVariableA(entry.first.c_str(), entry.second.c_str());
        _putenv_s(entry.first.c_str(), entry.second.c_str());
    }
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    HANDLE log = INVALID_HANDLE_VALUE;
    if (!log_path.empty()) {
        SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
        log = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = log;
            startup.hStdError = log;
        }
    }
    PROCESS_INFORMATION info = {};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr,
                             working_dir.empty() ? nullptr : working_dir.c_str(), &startup, &info);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (!ok) {
        return false;
    }
    CloseHandle(info.hThread);
    process.handle = (intptr_t)info.hProcess;
    process.finished = false;
    process.exit_code = -1;
    return true;
}

bool Process::running() {
    if (!handle || finished) {
        return false;
    }
    if (WaitForSingleObject((HANDLE)handle, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess((HANDLE)handle, &code);
        CloseHandle((HANDLE)handle);
        handle = 0;
        exit_code = (int)code;
        finished = true;
        return false;
    }
    return true;
}

int wait(Process &process) {
    if (process.handle && !process.finished) {
        WaitForSingleObject((HANDLE)process.handle, INFINITE);
        process.running();
    }
    return process.exit_code;
}

bool has_file_dialogs() {
    return true;
}

std::string pick_file(const char *title, const char *filter_name, const char *pattern) {
    char path[MAX_PATH] = "";
    std::string filter = std::string(filter_name) + '\0' + pattern + '\0' + "All files" + '\0' +
                         "*.*" + '\0' + '\0';
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof(path);
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&dialog) ? path : "";
}

std::string pick_folder(const char *title) {
    char path[MAX_PATH] = "";
    BROWSEINFOA browse = {};
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderA(&browse);
    if (!item) {
        return "";
    }
    bool ok = SHGetPathFromIDListA(item, path);
    CoTaskMemFree(item);
    return ok ? path : "";
}

void open_path(const std::string &path) {
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

#else

std::string exe_dir() {
    char path[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) {
        return ".";
    }
    std::string result(path, (size_t)length);
    size_t slash = result.find_last_of('/');
    return slash == std::string::npos ? "." : result.substr(0, slash);
}

bool spawn(Process &process, const std::string &program, const std::vector<std::string> &args,
           const std::map<std::string, std::string> &env, const std::string &log_path,
           const std::string &working_dir) {
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        for (const auto &entry : env) {
            setenv(entry.first.c_str(), entry.second.c_str(), 1);
        }
        if (!log_path.empty()) {
            int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log >= 0) {
                dup2(log, STDOUT_FILENO);
                dup2(log, STDERR_FILENO);
                close(log);
            }
        }
        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) {
            _exit(127);
        }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const std::string &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    process.handle = pid;
    process.finished = false;
    process.exit_code = -1;
    return true;
}

bool Process::running() {
    if (!handle || finished) {
        return false;
    }
    int status = 0;
    pid_t result = waitpid((pid_t)handle, &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    exit_code = result > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    finished = true;
    handle = 0;
    return false;
}

int wait(Process &process) {
    if (process.handle && !process.finished) {
        int status = 0;
        waitpid((pid_t)process.handle, &status, 0);
        process.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        process.finished = true;
        process.handle = 0;
    }
    return process.exit_code;
}

static bool has_command(const char *name) {
    std::string check = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}

static std::string read_command(const std::string &command) {
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return "";
    }
    char buffer[4096];
    std::string out;
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        out += buffer;
    }
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

bool has_file_dialogs() {
    static int available = -1;
    if (available < 0) {
        available = has_command("zenity") || has_command("kdialog");
    }
    return available;
}

std::string pick_file(const char *title, const char *filter_name, const char *pattern) {
    if (has_command("zenity")) {
        return read_command(std::string("zenity --file-selection --title='") + title +
                            "' --file-filter='" + filter_name + " | " + pattern + "' 2>/dev/null");
    }
    if (has_command("kdialog")) {
        return read_command(std::string("kdialog --title '") + title + "' --getopenfilename . '" +
                            pattern + "' 2>/dev/null");
    }
    return "";
}

std::string pick_folder(const char *title) {
    if (has_command("zenity")) {
        return read_command(std::string("zenity --file-selection --directory --title='") + title +
                            "' 2>/dev/null");
    }
    if (has_command("kdialog")) {
        return read_command(std::string("kdialog --title '") + title +
                            "' --getexistingdirectory . 2>/dev/null");
    }
    return "";
}

void open_path(const std::string &path) {
    // Double fork so xdg-open is reparented to init and never left as a zombie.
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int null = open("/dev/null", O_WRONLY);
            if (null >= 0) {
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
            }
            execlp("xdg-open", "xdg-open", path.c_str(), (char *)nullptr);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

#endif

}  // namespace os
