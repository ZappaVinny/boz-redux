// Edits client.ini in place: values change on their own lines, so comments and layout survive.
#pragma once

#include <string>
#include <vector>

class IniFile {
public:
    bool load(const std::string &path);
    bool save() const;
    const std::string &path() const { return path_; }

    std::string get(const std::string &section, const std::string &key,
                    const std::string &fallback = "") const;
    // Replaces the value, or adds the key at the end of its section (creating the section).
    void set(const std::string &section, const std::string &key, const std::string &value);

private:
    struct Location {
        int line = -1;           // line holding the key, or -1
        int section_end = -1;    // index after the section's last non-blank line, or -1
    };
    Location find(const std::string &section, const std::string &key) const;

    std::string path_;
    std::vector<std::string> lines_;
};
