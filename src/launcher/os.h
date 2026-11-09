// Operating system helpers for the launcher: paths, processes and file dialogs.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os {

// Directory containing the running executable, without a trailing separator.
std::string exe_dir();
std::string join(const std::string &a, const std::string &b);
bool exists(const std::string &path);
int64_t file_size(const std::string &path);  // -1 if missing
bool make_dir(const std::string &path);
bool copy_file(const std::string &from, const std::string &to);
bool rename_file(const std::string &from, const std::string &to);
std::string executable_name(const std::string &base);  // adds .exe on Windows

// A child process. Output (stdout and stderr) goes to log_path when given.
struct Process {
    intptr_t handle = 0;
    bool running();  // polls; sets finished and exit_code once the process has exited
    int exit_code = -1;
    bool finished = false;
};

bool spawn(Process &process, const std::string &program, const std::vector<std::string> &args,
           const std::map<std::string, std::string> &env, const std::string &log_path,
           const std::string &working_dir);
// Why the last spawn failed, in the system's words ("" when it did not).
std::string spawn_error();
// Blocks until the process exits; returns its exit code.
int wait(Process &process);

// Native pickers; empty result when cancelled or unavailable.
bool has_file_dialogs();
std::string pick_file(const char *title, const char *filter_name, const char *pattern);
std::string pick_folder(const char *title);

// Opens a file or folder in the desktop's default application.
void open_path(const std::string &path);

std::string getenv_str(const char *name);

#if defined(__ANDROID__)
// Android (os_android.cpp, calling packaging/android LauncherActivity): there are no child
// processes or desktop file dialogs. Picked files are content:// URIs that only Java can read.
// The calls block; the copies and the download belong on a worker thread.

// The system picker, for any file; empty when cancelled or unavailable (has_file_dialogs()).
std::vector<std::string> pick_documents(bool multiple);
int64_t document_size(const std::string &uri);  // -1 when unknown
bool copy_document(const std::string &uri, const std::string &to);
// A mod's .zip unpacked into mods_dir; returns the folder name, or "" with *error set.
std::string import_mod_zip(const std::string &uri, const std::string &mods_dir, std::string *error);
bool download(const std::string &url, const std::string &to);
// Starts the game's activity (its own process) with these arguments; env sets HOME and BOZ_*.
bool start_game(const std::vector<std::string> &args, const std::map<std::string, std::string> &env);
#endif

}  // namespace os
