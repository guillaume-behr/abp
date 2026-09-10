#include "abp/StringUtil.h"
#include "TestFramework.h"

using namespace abp::strutil;

ABP_TEST(split_basic) {
    auto parts = split("a,b,,c", ',');
    ABP_CHECK_EQ(parts.size(), 4u);
    ABP_CHECK_EQ(parts[0], "a");
    ABP_CHECK_EQ(parts[1], "b");
    ABP_CHECK_EQ(parts[2], "");
    ABP_CHECK_EQ(parts[3], "c");
}

ABP_TEST(join_basic) {
    ABP_CHECK_EQ(join({"a", "b", "c"}, ", "), "a, b, c");
    ABP_CHECK_EQ(join({}, ", "), "");
}

ABP_TEST(trim_basic) {
    ABP_CHECK_EQ(trim("  hello \t\n"), "hello");
    ABP_CHECK_EQ(trim("none"), "none");
    ABP_CHECK_EQ(trim("   "), "");
}

ABP_TEST(starts_ends_with) {
    ABP_CHECK(startsWith("package:com.foo", "package:"));
    ABP_CHECK(!startsWith("pack", "package:"));
    ABP_CHECK(endsWith("base.apk", ".apk"));
    ABP_CHECK(!endsWith("base.apk", ".ab"));
}

ABP_TEST(shell_quote_escapes_single_quotes) {
    ABP_CHECK_EQ(shellQuote("simple"), "'simple'");
    ABP_CHECK_EQ(shellQuote("it's"), "'it'\\''s'");
}

ABP_TEST(package_name_validation) {
    ABP_CHECK(isValidPackageName("com.example.app"));
    ABP_CHECK(isValidPackageName("com.example.app_2"));
    ABP_CHECK(!isValidPackageName(""));
    ABP_CHECK(!isValidPackageName("com"));
    ABP_CHECK(!isValidPackageName("com.example; rm -rf /"));
    ABP_CHECK(!isValidPackageName("com.example.app$(whoami)"));
}

ABP_TEST(format_bytes) {
    ABP_CHECK_EQ(formatBytes(0), "0 B");
    ABP_CHECK_EQ(formatBytes(1023), "1023 B");
    ABP_CHECK_EQ(formatBytes(1024), "1.00 KB");
}
