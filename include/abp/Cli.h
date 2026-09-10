#pragma once

#include <vector>
#include <string>

namespace abp {

/// Entry point for the abp command-line interface. Parses argv into a
/// subcommand (devices/info/list-packages/backup/restore/help/version) and
/// runs it. Returns a process exit code.
class Cli {
public:
    static int run(int argc, char** argv);
};

} // namespace abp
