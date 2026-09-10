#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Minimal, dependency-free self-registering test framework. abp only needs
// enough here to unit-test its pure-logic components (Json, Sha256,
// StringUtil, Manifest); nothing about it should be mistaken for a
// general-purpose testing library.
namespace abp::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const std::string& name, const std::function<void()>& fn) { registry().push_back({name, fn}); }
};

struct AssertionFailure {
    std::string message;
};

} // namespace abp::test

#define ABP_TEST(name)                                                                     \
    static void abp_test_##name();                                                         \
    static ::abp::test::Registrar abp_registrar_##name(#name, abp_test_##name); \
    static void abp_test_##name()

#define ABP_CHECK(cond)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            throw ::abp::test::AssertionFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) +      \
                                                 ": CHECK failed: " #cond};                                     \
        }                                                                                                       \
    } while (0)

#define ABP_CHECK_EQ(a, b)                                                                          \
    do {                                                                                             \
        if (!((a) == (b))) {                                                                         \
            std::ostringstream abp_oss_;                                                             \
            abp_oss_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " != " #b " (got '"  \
                     << (a) << "' expected '" << (b) << "')";                                        \
            throw ::abp::test::AssertionFailure{abp_oss_.str()};                                     \
        }                                                                                             \
    } while (0)
