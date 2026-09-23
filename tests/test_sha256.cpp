#include "abp/Sha256.h"

#include <filesystem>
#include <string>

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

ABP_TEST(sha256_hex_digest_is_idempotent) {
    // Finalizing appends padding through the same state the padding is
    // computed from, so a second call used to hash a second round of padding
    // and quietly return a different, wrong digest.
    abp::crypto::Sha256 hasher;
    hasher.update("abc", 3);
    const std::string first = hasher.hexDigest();
    ABP_CHECK_EQ(hasher.hexDigest(), first);
    ABP_CHECK_EQ(first, abp::crypto::sha256Hex("abc"));

    // Updates after finalization are ignored rather than corrupting it.
    hasher.update("more", 4);
    ABP_CHECK_EQ(hasher.hexDigest(), first);
}

ABP_TEST(sha256_matches_reference_digests_around_block_boundaries) {
    // The padding path branches on how much of the final 64-byte block is
    // already used: 55 bytes leaves just enough room for the length field,
    // 56 forces a whole extra block. Reference digests from sha256sum.
    struct Case {
        size_t length;
        const char* digest;
    };
    const Case cases[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
    };
    for (const auto& testCase : cases) {
        ABP_CHECK_EQ(sha256Hex(std::string(testCase.length, 'a')), std::string(testCase.digest));
    }
}

ABP_TEST(sha256_file_refuses_what_it_cannot_read_to_the_end) {
    // Opening a directory "succeeds" on Linux, and every read then fails. That
    // used to hash as the empty string, which checksumMatches() would accept
    // for any archive whose recorded digest happened to be that of nothing.
    bool threw = false;
    try {
        sha256HexFile(std::filesystem::temp_directory_path().string());
    } catch (const std::exception&) {
        threw = true;
    }
    ABP_CHECK(threw);
}
