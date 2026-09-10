#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

#include "TestFramework.h"

namespace {

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [--list] [NAME_SUBSTRING ...]\n"
              << "  --list   Print the registered test names and exit.\n"
              << "  NAME     Run only tests whose name contains this substring.\n";
}

bool matchesAnyFilter(const std::string& name, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    for (const auto& filter : filters) {
        if (name.find(filter) != std::string::npos) return true;
    }
    return false;
}

/// Announces the test about to run, so a test that crashes the process
/// (rather than throwing) still says which one died.
///
/// On a terminal the marker is written to stdout and erased by the result
/// line that follows. Everywhere else -- a CI log, a pipe -- carriage returns
/// do not erase anything, so it goes to stderr instead and leaves stdout as
/// one tidy line per test.
void announceRunning(const std::string& name, bool stdoutIsTerminal) {
    if (stdoutIsTerminal) {
        std::cout << "[RUN ] " << name << std::flush;
    } else {
        std::cerr << "[RUN ] " << name << std::endl;
    }
}

/// Erases the marker announceRunning() left, when it can be erased.
const char* resultPrefix(bool stdoutIsTerminal) { return stdoutIsTerminal ? "\r" : ""; }

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--list") {
            for (const auto& t : abp::test::registry()) std::cout << t.name << "\n";
            return 0;
        }
        filters.push_back(arg);
    }

    const bool isTerminal = isatty(fileno(stdout)) != 0;
    const std::string prefix = resultPrefix(isTerminal);

    int failed = 0;
    int ran = 0;
    for (const auto& t : abp::test::registry()) {
        if (!matchesAnyFilter(t.name, filters)) continue;
        ++ran;

        announceRunning(t.name, isTerminal);

        try {
            t.fn();
            std::cout << prefix << "[PASS] " << t.name << "\n" << std::flush;
        } catch (const abp::test::AssertionFailure& f) {
            std::cout << prefix << "[FAIL] " << t.name << ": " << f.message << "\n" << std::flush;
            ++failed;
        } catch (const std::exception& e) {
            std::cout << prefix << "[FAIL] " << t.name << ": unexpected exception: " << e.what() << "\n" << std::flush;
            ++failed;
        } catch (...) {
            std::cout << prefix << "[FAIL] " << t.name << ": unexpected non-standard exception\n" << std::flush;
            ++failed;
        }
    }

    if (ran == 0) {
        std::cout << "No tests matched the given filter(s).\n";
        return 1;
    }

    std::cout << ran << " test(s), " << failed << " failed.\n";
    return failed == 0 ? 0 : 1;
}
