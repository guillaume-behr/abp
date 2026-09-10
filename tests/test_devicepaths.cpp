#include "abp/DevicePaths.h"

#include <algorithm>
#include <string>

#include "TestFramework.h"

using namespace abp::devicepaths;

namespace {

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

} // namespace

ABP_TEST(devicepaths_accepts_real_storage_paths) {
    for (const char* path : {"/data", "/sdcard", "/system", "/vendor", "/data/local/tmp",
                              "/storage/emulated/0", "/data/app"}) {
        ABP_CHECK(isPullable(path));
    }
}

ABP_TEST(devicepaths_refuses_the_filesystem_root) {
    // `adb pull /` would descend into /proc, /sys and /dev.
    ABP_CHECK(classify("/") == PathVerdict::Root);
    ABP_CHECK(classify("//") == PathVerdict::Root);
    ABP_CHECK(!explainVerdict(PathVerdict::Root, "/").empty());
}

ABP_TEST(devicepaths_refuses_pseudo_filesystems_and_their_children) {
    for (const char* path : {"/proc", "/proc/kcore", "/proc/1/mem", "/sys", "/sys/fs/cgroup",
                              "/dev", "/dev/block/sda", "/acct", "/config", "/apex",
                              "/apex/com.android.runtime", "/mnt/runtime/write", "/storage/self"}) {
        ABP_CHECK(classify(path) == PathVerdict::PseudoFilesystem);
    }
}

ABP_TEST(devicepaths_does_not_confuse_a_prefix_for_a_parent_directory) {
    // "/procedures" merely starts with "/proc"; it is an ordinary path.
    ABP_CHECK(isPullable("/procedures"));
    ABP_CHECK(isPullable("/systemfoo"));
    ABP_CHECK(isPullable("/devices"));
    ABP_CHECK(!isUnder("/datafoo", "/data"));
    ABP_CHECK(isUnder("/data/app", "/data"));
    ABP_CHECK(isUnder("/data", "/data"));
}

ABP_TEST(devicepaths_refuses_relative_paths_and_traversal) {
    ABP_CHECK(classify("data/app") == PathVerdict::NotAbsolute);
    ABP_CHECK(classify("") == PathVerdict::Empty);
    ABP_CHECK(classify("   ") == PathVerdict::Empty);
    ABP_CHECK(classify("/data/../proc") == PathVerdict::Traversal);
    ABP_CHECK(classify("/data/..") == PathVerdict::Traversal);
}

ABP_TEST(devicepaths_normalizes_redundant_slashes) {
    ABP_CHECK_EQ(normalize("/data//app/"), "/data/app");
    ABP_CHECK_EQ(normalize("/data/"), "/data");
    ABP_CHECK_EQ(normalize("///"), "/");
    ABP_CHECK_EQ(normalize("/"), "/");
    ABP_CHECK_EQ(normalize(""), "");
}

ABP_TEST(devicepaths_every_default_root_is_pullable) {
    const auto roots = defaultCaptureRoots();
    ABP_CHECK(!roots.empty());
    for (const auto& root : roots) {
        ABP_CHECK(isPullable(root));
    }
    // The two that actually hold user data must be there.
    ABP_CHECK(contains(roots, "/data"));
    ABP_CHECK(contains(roots, "/sdcard"));
    // And the pseudo-filesystems must not.
    ABP_CHECK(!contains(roots, "/proc"));
    ABP_CHECK(!contains(roots, "/sys"));
    ABP_CHECK(!contains(roots, "/dev"));
}

ABP_TEST(devicepaths_collapse_drops_paths_covered_by_an_ancestor) {
    // Pulling /data already brings /data/app with it.
    auto collapsed = collapseRedundant({"/data", "/data/app", "/data/local/tmp", "/system"});
    ABP_CHECK_EQ(collapsed.size(), 2u);
    ABP_CHECK_EQ(collapsed[0], "/data");
    ABP_CHECK_EQ(collapsed[1], "/system");
}

ABP_TEST(devicepaths_collapse_handles_an_ancestor_listed_last) {
    // Order given must not change which paths survive.
    auto collapsed = collapseRedundant({"/data/app", "/data/local", "/data"});
    ABP_CHECK_EQ(collapsed.size(), 1u);
    ABP_CHECK_EQ(collapsed[0], "/data");
}

ABP_TEST(devicepaths_collapse_dedupes_and_normalizes) {
    auto collapsed = collapseRedundant({"/data/", "/data", "//data//", "/system"});
    ABP_CHECK_EQ(collapsed.size(), 2u);
    ABP_CHECK_EQ(collapsed[0], "/data");
    ABP_CHECK_EQ(collapsed[1], "/system");
}

ABP_TEST(devicepaths_collapse_keeps_siblings_apart) {
    auto collapsed = collapseRedundant({"/data", "/datafoo", "/sdcard"});
    ABP_CHECK_EQ(collapsed.size(), 3u);
}

ABP_TEST(devicepaths_collapse_handles_empty_input) {
    ABP_CHECK_EQ(collapseRedundant({}).size(), 0u);
    ABP_CHECK_EQ(collapseRedundant({"", "   "}).size(), 0u);
}

ABP_TEST(devicepaths_explain_gives_a_reason_for_every_refusal) {
    for (PathVerdict verdict : {PathVerdict::Empty, PathVerdict::NotAbsolute, PathVerdict::Root,
                                 PathVerdict::PseudoFilesystem, PathVerdict::Traversal}) {
        ABP_CHECK(!explainVerdict(verdict, "/some/path").empty());
    }
    // Ok has nothing to explain.
    ABP_CHECK(explainVerdict(PathVerdict::Ok, "/data").empty());
}
