#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// Lowercase hex SHA-256 of a file, or "" if it cannot be read. progress (0..1) is updated while
// hashing when given.
std::string sha256_file(const std::string &path, std::atomic<float> *progress = nullptr);
std::string sha256_hex(const void *data, size_t size);
