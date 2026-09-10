# 📜 Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

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
