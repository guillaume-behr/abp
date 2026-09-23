# Architecture

`abp` is organized in layers, each of which only talks to the layer
directly below it:

```mermaid
flowchart TB
    CLI["Cli<br/><sub>src/cli</sub>"]
    GUI["GuiServer + HttpServer<br/><sub>src/gui</sub>"]
    STORE["BackupStore<br/><sub>src/backup/BackupStore.cpp</sub>"]
    BM["BackupManager<br/><sub>src/backup/BackupManager.cpp</sub>"]
    IF{{"IBackupBackend"}}
    RB["RootBackend"]
    SB["StandardBackend"]
    ADB["AdbClient<br/><sub>src/adb</sub>"]
    PROC["Process<br/><sub>src/util — fork/exec, no host shell involved</sub>"]

    CLI --> BM
    CLI --> GUI
    GUI --> BM
    GUI --> STORE
    BM --> IF
    IF --> RB
    IF --> SB
    RB --> ADB
    SB --> ADB
    ADB --> PROC
```

## Cli

Parses `argv` into a subcommand and its options (hand-rolled, no argument
parsing library — the option set is small and stable enough that a
generic parser would add a dependency for little benefit). Prints
usage/errors, asks for interactive confirmation before a backup/restore
unless `-y`/`--yes` is given, and turns a `BackupSummary`/`RestoreSummary`
into human-readable output. `abp gui` is parsed here too, but hands off
immediately to `GuiServer`.

## GuiServer

`abp gui` serves a single-page app (compiled into the binary from
`src/gui/web/index.html`) plus the JSON API it drives, on top of
`HttpServer` — a ~350-line HTTP/1.1 server that speaks just enough of the
protocol for a local, single-origin app: one thread per connection, no
keep-alive, no TLS.

The API layer holds no backup logic of its own. It parses a request into
the same `BackupOptions`/`RestoreOptions` the CLI builds, hands them to
`BackupManager` on a worker thread, and streams progress back by
installing a `Logger` sink that tees every log line into the running
job's buffer. One job runs at a time, which is also why a process-global
logger sink is enough.

Requests are authenticated with a token generated at startup and
validated on every `/api/...` call; see [GUI.md](GUI.md) for the rest of
the security model.

## BackupStore

The read-only counterpart to `BackupManager`: it discovers backup
directories under a root, summarizes their manifests, and lists files
inside one. It never touches a device, which is what lets the GUI's
"Explore backups" view work with nothing plugged in. Every browsing path
is resolved through `resolveInside()`, which canonicalizes the path
(following symlinks) and refuses anything that leaves the backup
directory.

## BackupManager

The only place that knows the end-to-end backup/restore *workflow*:
connect, detect the device and root access, pick a backend, enumerate and
filter packages, extract/install APKs (identical in both modes, so it's
not backend-specific), delegate app data and shared storage to the
chosen backend, and read/write `manifest.json`.

`BackupManager` never shells out to `adb` directly except through
`AdbClient`, and never contains backend-specific commands — that's the
backend's job.

## PersonalData

Exports contacts (vCard), SMS/MMS and call log (JSON), calendar events
(iCalendar) and settings through Android's content providers and
`settings`, independently of the backend: it needs no root and runs in
both modes. Given root it also exports saved Wi-Fi networks, which
restore re-adds with `cmd wifi add-network`. `parseContentQuery()` turns the
tool's unescaped `Row: n col=value, ...` output into rows by relying on
the known projection, with the one free-text column placed last. The
exports are recorded in the manifest's `personal_data_exports`; on
restore contacts and calendar are copied to the device for the user to
import, and the rest is archival.

## IBackupBackend

A small strategy interface (`backupAppData`, `restoreAppData`,
`backupSharedStorage`, `restoreSharedStorage`) implemented by:

- **RootBackend** — tar-over-adb, described in detail in
  [ROOT_BACKUP.md](ROOT_BACKUP.md).
- **StandardBackend** — the legacy `adb backup`/`adb restore` flow plus
  `adb pull`/`adb push` for shared storage.

`StandardBackend` is itself a hybrid: it captures each debuggable package
individually through `run-as` (same archive layout as root mode) and only
falls back to the whole-device legacy `adb backup` for packages `run-as`
cannot reach. Which one was used is recorded per package as
`data_capture_method`, because restore has to dispatch on it — per-package
archives restore selectively, the legacy archive does not. See
[NON_ROOT_BACKUP.md](NON_ROOT_BACKUP.md).

Both backends mutate a shared `Manifest`, which is what eventually gets
serialized to `manifest.json`. Keeping backends manifest-aware (rather
than returning some backend-specific result type) means `BackupManager`
doesn't need to know anything about *how* a backend records what it did.

## AdbClient

Thin, typed wrapper over the `adb` command-line tool: `shell`,
`push`/`pull`, `installApks`, `execOutToFile` (stream a device command's
stdout straight to a local file), `shellFromFile` (stream a local file
into a device command's stdin), plus the legacy `backupToFile`/
`restoreFromFile`. It also does device/package enumeration (`adb devices
-l`, `pm list packages -f`, `pm path`) and root detection.

Package enumeration is deliberately split in two. `listPackages()` makes
a single `pm list packages -f` call, which yields every package name plus
its *base* APK. Split APKs need `pm path`, so `resolveApkPaths()` batches
those lookups into one on-device shell loop and is called only for the
packages actually being backed up — asking per package would cost one
adb round trip each, which is tens of seconds on a device with a few
hundred apps.

`AdbClient::shell()` takes a single, already-quoted command string (see
`StringUtil::shellQuote`), rather than an argv array, because that's what
adb itself expects: everything after `adb shell` is forwarded verbatim
to the device's shell. Every value interpolated into that string —
package names, device paths, the UID/GID a restore chowns to — goes
through two independent defences: it is validated (see
`StringUtil::isValidPackageName` and `DevicePaths::classify`) *and* it is
wrapped in `shellQuote()` at the point of use. Either one alone would
do; both together mean a future caller that forgets the validator still
cannot smuggle shell metacharacters into a command abp constructs.

## Process

A dependency-free `fork`/`exec` wrapper (`src/util/Process.cpp`). No
command ever goes through `/bin/sh` on the *host* side — `adb` is always
invoked with an explicit argv array. Three modes:

- `run()` — capture stdout/stderr in memory (for short, textual output:
  `pm list packages`, `getprop`, ...).
- `runToFile()` — redirect the child's stdout straight to a file, so
  streaming a multi-gigabyte tar archive off a device never passes
  through this process's heap.
- `runFromFile()` — the mirror image, for pushing an archive back in via
  a command's stdin.

A child given no input reads `/dev/null`, never abp's own stdin. And every
mode can be cancelled: `Process::requestCancel()` (the GUI's Cancel
button, the CLI's Ctrl-C) sends the running child SIGTERM, then SIGKILL
after a grace period, and makes further `run*()` calls return at once
until `clearCancel()`. The backup and restore loops check the same flag
between packages, so a cancel stops at the next step boundary rather
than halfway through writing the manifest.

Two details matter because `abp gui` forks from a worker thread while
other threads are serving HTTP:

- **Everything the child needs is built before the `fork()`.** The only
  async-signal-safe thing a forked child of a multi-threaded process may
  do is `exec`; a child that allocated could block forever on a malloc
  lock another thread happened to hold at the instant of the fork. The
  argv array is therefore assembled in the parent, and the child does
  nothing but `dup2`/`close` and `execvp`.
- **Every pipe is created close-on-exec.** Without that, a fork happening
  concurrently on another thread would inherit an unrelated pipe, hold
  its write end open, and stall that pipe's reader until the second child
  also exited. `dup2()` clears the flag on whatever the child installs as
  its standard streams, so those still survive the exec.

## Util

`Json` (a small, order-preserving JSON value/parser/serializer),
`Sha256` (FIPS 180-4, used only for backup integrity checking, not for
anything security-sensitive), `StringUtil`, `FsUtil`, and `Logger`.
`Logger` is process-global and takes an optional sink, which is how the
GUI tees a running job's output into the browser; the sink is copied out
and invoked with the logger's own mutex released, so a sink that takes
another lock cannot invert the lock order. None
of these have any dependency on the rest of the codebase, and none of
them know what a "backup" is — that keeps them easy to unit-test in
isolation (see `tests/`).
