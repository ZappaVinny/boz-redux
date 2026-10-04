#include "ini_file.h"

#include <cctype>
#include <cstdio>
#include <fstream>

namespace {

std::string trim(const std::string &text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace((unsigned char)text[begin])) {
        ++begin;
    }
    while (end > begin && std::isspace((unsigned char)text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool is_section(const std::string &line, std::string *name) {
    std::string text = trim(line);
    if (text.size() < 2 || text.front() != '[') {
        return false;
    }
    size_t close = text.find(']');
    if (close == std::string::npos) {
        return false;
    }
    *name = trim(text.substr(1, close - 1));
    return true;
}

// Splits "key = value # comment" into key and value (the comment is dropped from value).
bool split_entry(const std::string &line, std::string *key, std::string *value) {
    std::string text = trim(line);
    if (text.empty() || text[0] == '#' || text[0] == ';') {
        return false;
    }
    size_t equals = text.find('=');
    if (equals == std::string::npos) {
        return false;
    }
    *key = trim(text.substr(0, equals));
    std::string rest = text.substr(equals + 1);
    size_t comment = rest.find_first_of("#;");
    *value = trim(comment == std::string::npos ? rest : rest.substr(0, comment));
    return true;
}

}  // namespace

bool IniFile::load(const std::string &path) {
    path_ = path;
    lines_.clear();
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines_.push_back(line);
    }
    return true;
}

bool IniFile::save() const {
    if (path_.empty()) {
        return false;  // never loaded: nothing to write to
    }
    std::string temp = path_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        for (const std::string &line : lines_) {
            out << line << '\n';
        }
        if (!out) {
            return false;
        }
    }
    std::remove(path_.c_str());
    return std::rename(temp.c_str(), path_.c_str()) == 0;
}

IniFile::Location IniFile::find(const std::string &section, const std::string &key) const {
    Location location;
    bool in_section = false;
    for (int i = 0; i < (int)lines_.size(); ++i) {
        std::string name;
        if (is_section(lines_[i], &name)) {
            in_section = name == section;
            continue;
        }
        if (!in_section) {
            continue;
        }
        if (!trim(lines_[i]).empty()) {
            location.section_end = i + 1;
        }
        std::string entry_key, entry_value;
        if (split_entry(lines_[i], &entry_key, &entry_value) && entry_key == key) {
            location.line = i;
        }
    }
    if (location.section_end < 0) {
        // Section header with nothing under it yet.
        for (int i = 0; i < (int)lines_.size(); ++i) {
            std::string name;
            if (is_section(lines_[i], &name) && name == section) {
                location.section_end = i + 1;
            }
        }
    }
    return location;
}

std::string IniFile::get(const std::string &section, const std::string &key,
                         const std::string &fallback) const {
    Location location = find(section, key);
    if (location.line < 0) {
        return fallback;
    }
    std::string entry_key, value;
    split_entry(lines_[location.line], &entry_key, &value);
    return value;
}

void IniFile::set(const std::string &section, const std::string &key, const std::string &value) {
    Location location = find(section, key);
    std::string line = key + " = " + value;
    if (location.line >= 0) {
        lines_[location.line] = line;
    } else if (location.section_end >= 0) {
        lines_.insert(lines_.begin() + location.section_end, line);
    } else {
        if (!lines_.empty() && !trim(lines_.back()).empty()) {
            lines_.push_back("");
        }
        lines_.push_back("[" + section + "]");
        lines_.push_back(line);
    }
}
