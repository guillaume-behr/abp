#include "abp/Sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace abp::crypto {
namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256() {
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::processBlock(const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

/// Absorbs one byte without the finalized-state guard update() carries, so
/// the padding hexDigest() appends is not rejected by its own bookkeeping.
void Sha256::appendByte(uint8_t byte) {
    buffer_[bufferLength_++] = byte;
    if (bufferLength_ == sizeof(buffer_)) {
        processBlock(buffer_);
        bufferLength_ = 0;
    }
}

void Sha256::update(const void* data, size_t length) {
    if (!digest_.empty()) return; // Already finalized; see hexDigest().
    const auto* bytes = static_cast<const uint8_t*>(data);
    totalLength_ += length;

    while (length > 0) {
        size_t take = std::min(length, sizeof(buffer_) - bufferLength_);
        std::memcpy(buffer_ + bufferLength_, bytes, take);
        bufferLength_ += take;
        bytes += take;
        length -= take;

        if (bufferLength_ == sizeof(buffer_)) {
            processBlock(buffer_);
            bufferLength_ = 0;
        }
    }
}

std::string Sha256::hexDigest() {
    // Padding runs through update(), which mutates the very state that
    // decides what the padding should be. Returning the cached digest keeps a
    // second call from hashing a second round of padding into the first --
    // which silently produced a wrong checksum rather than an error.
    if (!digest_.empty()) return digest_;

    uint64_t bitLength = totalLength_ * 8;

    uint8_t pad = 0x80;
    appendByte(pad);

    while (bufferLength_ != 56) {
        appendByte(0x00);
    }

    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) {
        lengthBytes[i] = static_cast<uint8_t>(bitLength >> (56 - i * 8));
    }
    // Bypass update() here to avoid re-triggering the padding branch logic;
    // directly append into buffer_ and process the final block.
    std::memcpy(buffer_ + 56, lengthBytes, 8);
    processBlock(buffer_);

    char hex[65];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(hex + i * 8, 9, "%08x", state_[i]);
    }
    digest_.assign(hex, 64);
    return digest_;
}

std::string sha256Hex(const std::string& data) {
    Sha256 hasher;
    hasher.update(data.data(), data.size());
    return hasher.hexDigest();
}

std::string sha256HexFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("sha256HexFile: cannot open file: " + path);
    }

    Sha256 hasher;
    std::vector<char> buffer(1 << 16);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = file.gcount();
        if (got > 0) {
            hasher.update(buffer.data(), static_cast<size_t>(got));
        }
    }
    // The loop above also ends on a read *error* (badbit), not just at end of
    // file. Returning a digest then would fingerprint only the part that could
    // be read -- a directory opened by mistake hashes as the empty string --
    // and a restore would accept a truncated archive as verified.
    if (file.bad() || !file.eof()) {
        throw std::runtime_error("sha256HexFile: read error: " + path);
    }
    return hasher.hexDigest();
}

} // namespace abp::crypto
