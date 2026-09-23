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

    /// Appends `length` bytes to the hash. Ignored once hexDigest() has been
    /// called: the state is finalized at that point and cannot absorb more.
    void update(const void* data, size_t length);

    /// Finalizes the hash and returns it as a 64-character lowercase hex
    /// string. Further update() calls are ignored, and calling this again
    /// returns the same digest rather than hashing the padding a second time.
    std::string hexDigest();

private:
    void processBlock(const uint8_t block[64]);
    void appendByte(uint8_t byte);

    uint32_t state_[8];
    uint64_t totalLength_ = 0;
    uint8_t buffer_[64];
    size_t bufferLength_ = 0;
    std::string digest_; ///< Non-empty once finalized.
};

/// Convenience: hashes an in-memory buffer.
std::string sha256Hex(const std::string& data);

/// Convenience: hashes the contents of a file on disk. Throws
/// std::runtime_error if the file cannot be opened or read to the end.
std::string sha256HexFile(const std::string& path);

} // namespace abp::crypto
