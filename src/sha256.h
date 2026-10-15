// sha256.h - SHA-256 (FIPS 180-4), for payload/live-file checks and the D2RLoader pins.
// Portable and dependency-free on purpose: the same code runs in the tests and on the player's PC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vrml {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string hex();   // finishes the hash; lowercase, 64 characters

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buflen_ = 0;
    uint64_t total_ = 0;
};

std::string Sha256Hex(const std::string& data);
// false when the file cannot be read (then *hex is empty)
bool Sha256File(const std::filesystem::path& file, std::string* hex);
bool IsSha256Hex(const std::string& s);   // exactly 64 hex digits (either case)

}  // namespace vrml
