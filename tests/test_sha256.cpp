#include "abp/Sha256.h"
#include "TestFramework.h"

using namespace abp::crypto;

ABP_TEST(sha256_empty_string) {
    ABP_CHECK_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ABP_CHECK_EQ(sha256Hex("").size(), 64u);
}

ABP_TEST(sha256_abc) {
    ABP_CHECK_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

ABP_TEST(sha256_longer_message) {
    ABP_CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

ABP_TEST(sha256_handles_multi_block_updates) {
    Sha256 hasher;
    std::string chunk(100, 'a');
    for (int i = 0; i < 10; ++i) {
        hasher.update(chunk.data(), chunk.size());
    }
    // Equivalent to hashing 1000 'a' characters in one shot.
    ABP_CHECK_EQ(hasher.hexDigest(), sha256Hex(std::string(1000, 'a')));
}
