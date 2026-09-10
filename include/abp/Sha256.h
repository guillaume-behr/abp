#pragma once

#include <cstdint>
#include <string>

namespace abp::crypto {

/// Streaming SHA-256 (FIPS 180-4) implementation used to fingerprint backup
/// archives so a restore can verify a file was not corrupted or truncated.
/// Not intended for anything security-sensitive beyond integrity checking.
class Sha256 {
public:
    Sha256();

    void update(const void* data, size_t length);

    /// Finalizes the hash and returns it as a 64-character lowercase hex
    /// string. The object must not be updated again afterwards.
    std::string hexDigest();

private:
    void processBlock(const uint8_t block[64]);

    uint32_t state_[8];
    uint64_t totalLength_ = 0;
    uint8_t buffer_[64];
    size_t bufferLength_ = 0;
};

/// Convenience: hashes an in-memory buffer.
std::string sha256Hex(const std::string& data);

/// Convenience: hashes the contents of a file on disk. Throws
/// std::runtime_error if the file cannot be opened.
std::string sha256HexFile(const std::string& path);

} // namespace abp::crypto
