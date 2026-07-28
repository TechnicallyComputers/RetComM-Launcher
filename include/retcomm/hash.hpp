#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// Lowercase hex without 0x prefix. Empty string on I/O error.
std::string file_crc32_hex(const fs::path& path);
std::string file_sha1_hex(const fs::path& path);

std::string crc32_hex(uint32_t crc);
std::string to_lower_hex(const std::string& s);

} // namespace retcomm
