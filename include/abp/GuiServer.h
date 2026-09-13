#pragma once

#include <filesystem>
#include <string>

namespace abp {

struct GuiOptions {
    /// Address to bind. Defaults to loopback: the GUI drives adb on this
    /// machine, so it must not be reachable from the network unless the
    /// user explicitly asks for that.
    std::string host = "127.0.0.1";

    /// TCP port; 0 lets the operating system pick a free one.
    int port = 8787;

    /// Directory the "Explore backups" view starts browsing from.
    std::filesystem::path backupRoot;

    /// How many directory levels below backupRoot to search for backups.
    int scanDepth = 2;

    /// Open the GUI in the user's default browser once the server is up.
    bool openBrowser = true;

    /// Shared secret every API request must present. Generated at startup
    /// when empty.
    std::string token;
};

/// Serves the abp web GUI: a single-page app plus the JSON API it drives,
/// backed by the same BackupManager/BackupStore the CLI uses. Blocks until
/// the server is stopped (Ctrl-C, or the GUI's own "Stop server" button)
/// and returns a process exit code.
class GuiServer {
public:
    static int run(const GuiOptions& options);
};

} // namespace abp
