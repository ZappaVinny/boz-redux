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
// Blocks until the process exits; returns its exit code.
int wait(Process &process);

// Native pickers; empty result when cancelled or unavailable.
bool has_file_dialogs();
std::string pick_file(const char *title, const char *filter_name, const char *pattern);
std::string pick_folder(const char *title);

// Opens a file or folder in the desktop's default application.
void open_path(const std::string &path);

std::string getenv_str(const char *name);

}  // namespace os
