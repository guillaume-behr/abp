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

- **Contacts, SMS and call log are exported on every backup, with or
  without root**: `personal/contacts.vcf` (the phone's own vCard export,
  or one built from the contacts data when the device cannot stream it),
  `personal/sms.json` and `personal/call_log.json`, read through
  `adb shell content`. A provider the device will not share is skipped
  with the reason. `abp restore` copies `contacts.vcf` to the device's
  Download folder for import; SMS and call log are archival, since only
  the default SMS app may write messages. `--no-personal` turns both off;
  the GUI has matching toggles and shows the exports when exploring.
- **Backup explorer in the GUI.** Opening a backup now gives tabs for
  photos & videos (a gallery with a full-screen viewer), messages (SMS
  and MMS as conversations, with contact names and inline attachments),
  contacts, calls, calendar, Wi-Fi & settings, and files (previews,
  downloads, and `.tar`/`.tar.gz` archives browsable like folders).
  Root-mode photos are served straight out of `shared_storage.tar` by
  offset, via a new native tar reader (ustar, GNU long names, pax), with
  HTTP `Range` support so videos can seek. Files from the phone are never
  served as anything a browser would run: markup comes back as text, and
  every file response carries a `sandbox` Content-Security-Policy.
- The web UI is embedded as several string literals joined at startup;
  the single literal had outgrown the 64 KiB compilers must accept, which
  broke clang builds under `-Werror`.
- **Cancelling**: the GUI has a Cancel button and the CLI stops cleanly
  on Ctrl-C (a second Ctrl-C quits at once). The running adb process is
  terminated, and a cancelled backup still writes a manifest for what it
  captured, with the packages it never reached marked as not captured.
- **Progress**: long steps report `done/total` and the item in hand,
  shown as a progress bar in the GUI and a self-updating line on a
  terminal.
- **`abp verify -i DIR`** (and `/api/backup/verify`, and a *Verify
  checksums* button in the GUI) checks every file a backup's manifest
  lists for presence and, where recorded, its SHA-256 -- without a
  device.
- **GUI refresh**: device cards show the real Android version and root
  status (they used to claim "no root" for every phone, which also made
  the Back up view announce standard mode), unauthorized/offline devices
  say what to do, and the list refreshes itself. The Back up view states
  what the device will give up before starting; a finished job shows
  its summary as tiles with its warnings; backups can be filtered; the
  package table says "APK only" instead of "ok" for apps whose data was
  not captured.
- **More personal data exported**: MMS with their attachments
  (`personal/mms.json`, `mms_parts/`), calendar events as iCalendar
  (`personal/calendar.ics`, local calendars included), and the system,
  secure and global settings (`personal/settings.json`), all without
  root. With root, saved Wi-Fi networks and their passwords
  (`personal/wifi.json`, owner-readable only).
- **Restore re-adds Wi-Fi networks** with `cmd wifi add-network` on
  Android 11+, and copies `calendar.ics` next to `contacts.vcf` in the
  device's Download folder for import.
- **Removable SD cards are backed up**: mounted public volumes (found
  with `sm list-volumes`) are copied with shared storage and recorded as
  path captures; restore prints the command to put them back.
- **Coverage warnings**: a backup now ends with what it could not
  capture -- apps whose secrets are sealed by the phone's hardware
  (authenticators, Signal, Google Wallet) and how many apps' data was out
  of reach without root or likely empty in legacy `adb backup` -- in the
  CLI summary, the GUI, and the API (`warnings`, `packages_without_data`).
- **Root mode captures device-protected app data**
  (`/data/user_de/0/<pkg>`, as `data/<pkg>.de.tar.gz`) and restores it
  with its own owner. That is where Android keeps the SMS database since
  Android 7, so `--system` root backups previously missed messages
  entirely. abp asks for a reboot after restoring system apps' data.
- Manifest format version 4 (`de_data_archive*` per package,
  `personal_data_exports`). Versions 1-3 still load.
- The GUI's **Back up** view can also pull every device partition
  (`--all-files`), and the API accepts `all_files` / `pull_paths`. The
  **Explore** view shows how each package's data was captured and lists
  raw device-path captures with whether each is complete; job summaries
  now include the same per-method and device-path counts the CLI prints.
- Clear device diagnostics: `info`, `list-packages`, `backup`, `restore`
  and the GUI now say *why* a device cannot be used -- unauthorized,
  offline, no permissions, or several devices connected with no serial
  chosen (which adb would otherwise reject command by command). When only
  one listed device is usable, abp pins its serial, because adb's own
  default also counts offline and unauthorized devices and refuses an
  unpinned command next to them.

### Fixed

- Child processes that are given no input now read `/dev/null` instead of
  abp's own stdin. `adb shell` forwards its stdin to the device, so it
  could swallow keystrokes, and under `abp gui &` its first read of the
  terminal stopped it with SIGTTIN, hanging the job.
- The GUI's listening and client sockets are now close-on-exec. The adb
  server that the first adb call starts inherited the listening socket,
  so after abp exited the port stayed bound and the next `abp gui` failed
  with "address already in use".
- Split-APK resolution no longer throws away every package's answer when
  the *last* package's `pm path` fails (for instance, it was uninstalled
  mid-backup), which silently dropped split APKs from the backup.
- The on-device scripts that resolve split APKs and probe `run-as` are
  sent in batches below adb's command-length limits. One script for
  every package overran the 64 KiB host-protocol limit with `--system`
  on a typical phone (and the 4 KiB limit of pre-Android-7 adbd much
  sooner), failing the whole batch.
- `sha256HexFile` fails on a read error instead of returning the digest
  of whatever was read before it, so a truncated read can no longer
  verify an archive (a directory used to hash as the empty string).
- A `manifest.json` that is valid JSON but not an object (`[]`, `42`) is
  rejected as corrupt instead of loading as an empty backup, and
  non-object package entries are skipped instead of becoming nameless
  phantom packages.
- A backup of a device with no matching apps (a freshly reset phone)
  still captures shared storage and `--all-files` paths instead of
  aborting; it only fails when `--only` named packages that all missed,
  or when there is nothing else to capture.
- Standard-mode shared storage keeps a partial `adb pull` of `/sdcard`
  (adb stops at the first unreadable file) with a warning, instead of
  reporting it as not captured while leaving the copied gigabytes on
  disk, unlisted. It is now pulled with timestamps preserved.
- A root-mode restore skips, and reports, a package that is not
  installed on the device instead of unpacking its data into a
  `/data/data/<pkg>` the package manager never created.
- A shared-storage capture that fails is now reported instead of the
  summary just saying "skipped".
- Scanning or browsing backups no longer aborts on an I/O error partway
  through a directory.
- GUI: overlapping job polls no longer print log lines twice, pressing
  Escape in a confirmation dialog counts as "Cancel", IPv6 `Host`
  headers with a port are parsed correctly, and the browser is opened
  with `open` on macOS.
- `abp gui --port` rejects values outside 0-65535, and an unknown
  command prints the usage to stderr.
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
