# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- `abp gui`: a local web GUI, served from the `abp` binary itself, for
  backing up and restoring devices and for exploring backups already on
  disk (manifest details, per-package archive sizes/checksums/errors, and
  a file browser over the backup directory).
- JSON API behind the GUI (`/api/devices`, `/api/backups`,
  `/api/jobs/backup`, `/api/job`, ...), usable on its own for scripting.
- `BackupStore`: read-only discovery, summarizing and sandboxed browsing
  of backup directories.
- `Logger` sink hook, used to stream a running job's progress to the
  browser while still printing it to the terminal.
- **`--all-files` copies whole device partitions verbatim with
  `adb pull`**, on top of the per-app and shared-storage captures. It
  expands to every persistent partition a device normally has (`/data`,
  `/sdcard`, `/system`, `/system_ext`, `/vendor`, `/product`, `/odm`,
  `/oem`, `/metadata`), skipping any that are absent. `--pull-path PATH`
  (repeatable) captures exactly the paths you name instead.
- Each captured tree lands under `filesystem/` in the backup and is
  recorded in `manifest.json` with its size and whether `adb pull` could
  read all of it. A tree read only in part is kept and marked
  `"complete": false` with the reason, rather than being presented as a
  whole copy.
- `abp` refuses to pull `/`, `/proc`, `/sys`, `/dev`, `/apex`,
  `/mnt/runtime` and similar, with an explanation. They are kernel
  pseudo-filesystems and bind-mount duplicates rather than stored files:
  `/proc/kcore` alone presents all of physical memory as a single file,
  and reading a character device can block indefinitely. Redundant paths
  are collapsed (asking for `/data` and `/data/app` pulls `/data` once),
  and `/sdcard` is skipped when shared storage already covers it.
- These captures are deliberately never restored. Writing a raw
  partition back over a running system is not safe to automate, so
  `abp restore` reports how many are present and leaves them for you to
  copy by hand.
- `abp` warns when `--all-files` cannot reach much: `adb pull` transfers
  as the adb user, and a `su` binary cannot elevate it (unlike shell
  commands), so a complete capture of `/data` needs adbd itself running
  as root via `adb root`.
- Bulk `adb pull` transfers now write adb's own progress display
  straight to the terminal instead of having it captured, so a
  multi-gigabyte pull no longer looks like a hang.
- Manifest format version 3 for the new `filesystem_captures` section.
  Versions 1 and 2 still load, with an empty capture list.

- **Standard (non-root) mode now captures debuggable apps completely.**
  Any package built with `android:debuggable="true"` is read directly
  through `run-as` and stored as a per-package `tar` archive, in the
  same layout root mode produces -- with a SHA-256 checksum, and with no
  on-device confirmation prompt. Only packages `run-as` cannot reach
  fall back to the legacy `adb backup` archive, and if `run-as` covers
  everything selected, the legacy flow (and its prompt) is skipped
  entirely.
- **Selective restore now works in standard mode** for those packages:
  `--only`/`--exclude` filter per package for anything captured as a
  per-package archive. `abp` warns when a legacy-archive restore will
  additionally bring back packages that were deselected, since
  `adb restore` cannot filter.
- `run-as` restores need no UID remapping: `tar` runs as the app's own
  UID, so files land correctly owned by construction, where root mode
  has to snapshot the UID and `chown -R` afterwards.
- A coverage table in README.md comparing exactly what root mode,
  non-root/debuggable and non-root/ordinary each save, plus
  docs/NON_ROOT_BACKUP.md describing the three mechanisms standard mode
  uses and their limits.
- The backup summary reports how many packages were captured by each
  mechanism, so the difference between a complete and a best-effort
  capture is visible rather than implied by the mode name.
- Manifest format version 2: a per-package `data_capture_method`
  (`root_tar` / `run_as_tar` / `legacy_adb_backup` / `none`). Version 1
  manifests are still read, with the method inferred from the entry's
  shape. An unrecognised method from a future abp reads as `none`
  rather than being mistaken for one this build can restore.

### Fixed

- **Restoring shared storage from a standard-mode backup nested every
  folder inside itself.** `adb push` follows `cp`'s rule — when the
  destination already exists as a directory, the source is copied *into*
  it — so pushing `DCIM` to `/sdcard/DCIM` landed the photos in
  `/sdcard/DCIM/DCIM` on any device that already had a `DCIM` folder,
  which is every device. Each top-level entry is now pushed to `/sdcard`
  itself, which merges into the existing directory.
- `fork()`ing no longer allocates in the child. The argv array is built
  before the fork, because the only async-signal-safe thing a forked
  child of a multi-threaded process may do is `exec` — and `abp gui`
  forks from a worker thread while other threads serve HTTP, so a child
  that allocated could deadlock on a malloc lock held at fork time.
- `Sha256::hexDigest()` is idempotent. Finalizing appends padding through
  the same state the padding is computed from, so calling it twice used
  to hash a second round of padding and quietly return a different,
  wrong digest; the digest is now cached and later `update()`s ignored.
- `Logger` invokes its sink with its own mutex released. Holding it fixed
  the lock order as "logger, then whatever the sink locks", which the
  GUI's job-runner sink could have inverted.
- The GUI's HTTP server no longer leaks a connection, or tears itself
  down, when the process cannot start another thread: the request is
  served inline instead and the socket still closes.
- A finished GUI job's thread is joined on destruction rather than
  reaching `std::terminate` if the serve loop ever unwinds.
- Two `--pull-path` arguments that sanitize to the same directory name
  (`/data/app` and `/data_app`) no longer overwrite each other's capture
  while the manifest claims both; the second gets a numbered suffix. The
  same de-duplication now applies to split APKs sharing a basename.
- `abp restore` no longer counts a package as restored when the backup
  held nothing to write back for it. Such packages are reported
  separately as having nothing to restore (`packages_skipped` in the
  GUI's JSON), so the summary cannot overstate what the run achieved.
- Every value interpolated into a device command is now shell-quoted at
  the point of use as well as validated beforehand — previously
  `RootBackend` and `pm path` relied on the package-name validator alone.
- A backup whose shared storage was captured in the other mode now gets
  the explanation it was meant to: the manifest's own
  `shared_storage_is_directory` is checked before the on-disk shape, so
  the message is no longer unreachable behind a failing file test.
- The HTTP server no longer labels an unlisted status code "OK"
  (`503 OK`), and its request-header cap is enforced after each read
  rather than one whole read late.
- Fixed a dangling reference when reading the manifest: `JsonValue::get()`
  returns by value, so iterating `get(key).items()` directly walked a
  destroyed temporary.
- `abp` no longer refuses to run when an offline or unauthorized device
  is listed ahead of a usable one: with no `-s SERIAL`, any ready device
  now satisfies the connection check.
- `adb devices` daemon-start banners ("* daemon not running ...") are no
  longer parsed as devices, so `abp devices` cannot invent phantom
  entries and the header line is recognised wherever it appears.
- Root detection now passes its probe to `su` as a single quoted
  argument (`su -c 'id -u'`). The previous unquoted form silently failed
  on `su` implementations that take only the first word as the command,
  making rooted devices look unrooted.
- APK installation is no longer reported as failed when `adb` prints
  `Success` on stderr rather than stdout, and an install that exits `0`
  while reporting `Failure`/`Error:` is no longer reported as success.
- A deeply nested `manifest.json` crashed the JSON parser with a stack
  overflow; nesting is now bounded and reported as a parse error.
- `\uD83D\uDE00`-style surrogate pairs decode to one code point instead
  of two invalid UTF-8 sequences, and unpaired surrogates become U+FFFD.
- Failing to write `manifest.json` at the end of a backup is now a
  reported error rather than an uncaught exception, and a checksum that
  cannot be computed no longer aborts the whole run.
- A backup is only reported as successful if its manifest was actually
  written; the summary's total size now includes APKs and the legacy
  `.ab` archive rather than counting data archives alone.
- Restore no longer counts backup-time errors recorded in the manifest
  as failures of the current restore.
- Restoring a manifest whose `format_version` is newer than this build
  is refused rather than misread, as `docs/MANIFEST.md` already promised.
- Standard-mode shared-storage restore reports failure when the backup
  directory cannot be read, instead of claiming success.
- A package whose split APKs were only partially pulled is now flagged;
  an incomplete split set cannot be reinstalled.
- `Process` no longer closes the child's stdin descriptor twice, can no
  longer be killed by `SIGPIPE` while writing to a child, no longer
  leaks descriptors when `pipe()`/`fork()` fails, and reports a
  signal-killed child as `128 + signo` instead of `-1`.
- Colour output is decided per stream, so a redirected stdout no longer
  receives escape codes just because stderr is a terminal.
- CI now runs on pushes to `master`; it had been watching a `main`
  branch that does not exist.
- Log output is flushed per line, so warnings no longer all appear ahead
  of the info lines they belong after when output is piped to a file
  (stdout is block-buffered when it is not a terminal; stderr never is).

### Changed

- `abp restore` reports three disjoint package counts — restored, failed,
  and nothing to restore — instead of folding the third into the first.
- The README and docs no longer decorate their headings, tables and
  diagrams with emoji.
- `-s`/`--serial` is only pre-scanned for the subcommands that take it,
  so `abp gui -s` complains about an unknown gui option rather than a
  missing serial value.
- `-v` is now `--verbose` (as in most CLIs); use `-V`/`--version` for
  the version. `-v`, `--verbose`, `--no-color` and `--adb-path` are all
  accepted anywhere on the command line.
- `--no-color` and the `NO_COLOR` environment variable are honoured.
- Split-APK paths are resolved in a single on-device shell loop for the
  packages being backed up, rather than one `adb shell pm path` round
  trip per installed package -- previously tens of seconds of overhead
  on a device with a few hundred apps.
- Root-mode restore runs `am force-stop <pkg>` before replacing an app's
  data directory, so a running app cannot clobber the restored files.
- Unknown options to `devices`, `info` and `list-packages`, and flags
  given without a required value, are now errors instead of being
  silently ignored.
- `--only` names that match no package are reported instead of quietly
  producing an empty backup or restore.
- Backing up into a directory that already contains a backup warns
  before overwriting it.
- Running `abp` with no arguments prints usage to stderr (it is a usage
  error) rather than stdout.

### Added

- Regression tests for subprocess handling, `adb` output parsing (via a
  fake `adb`), filesystem helpers, and the JSON/manifest hardening
  above, bringing the suite from 14 to 61 cases.
- A CI job that builds and runs the test suite under
  AddressSanitizer/UndefinedBehaviorSanitizer.
- The test runner names the test it is about to run (so a crash is
  attributable) and accepts name filters and `--list`.

## [1.0.0] - 2026-09-10

### Added

- Initial release.
- `abp devices`, `abp info`, `abp list-packages` commands.
- `abp backup` / `abp restore` with automatic root/standard backend
  selection (`--root` / `--standard` to force one).
- Root-mode backend: per-app `tar` capture of `/data/data/<pkg>` and of
  `/sdcard`, streamed over `adb exec-out`/`adb shell`; UID remapping and
  `restorecon` on restore.
- Standard-mode backend: legacy `adb backup`/`adb restore` for app data,
  `adb pull`/`adb push` for shared storage.
- Versioned `manifest.json` with per-archive SHA-256 checksums, verified
  before restore.
- Selective backup/restore via `--only`, `--exclude`, `--no-apks`,
  `--no-data`, `--no-shared`, `--system`.
- Dependency-free implementation: custom subprocess wrapper, JSON
  parser/serializer, and SHA-256, all in-tree.
