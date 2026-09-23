# Usage reference

## Global options

These may appear anywhere on the command line, before or after the
subcommand:

| Option              | Description                                                        |
|----------------------|---------------------------------------------------------------------|
| `--adb-path PATH`    | Use this `adb` executable instead of the one on `PATH`.            |
| `-v, --verbose`      | Print debug-level log messages.                                    |
| `--no-color`         | Disable coloured output.                                           |

`--adb-path` can also be set via the `ABP_ADB_PATH` environment variable,
which `--adb-path` overrides if both are given.

Colour is enabled automatically when the relevant stream is a terminal
and disabled when it is redirected. Setting the `NO_COLOR` environment
variable to any non-empty value turns it off, as does `--no-color`.

`abp -V` / `abp --version` prints the version. (`-v` is *verbose*, as it
is in most CLIs; use the capital `-V` for the version.)

## `abp devices`

Lists devices visible to `adb`, including unauthorized/offline ones:

```
$ abp devices
ABC123          device       Pixel_8
DEF456          unauthorized
```

If nothing is listed, check that USB debugging is enabled and that
you've accepted the "Allow USB debugging?" prompt on the device.

## `abp info [-s SERIAL]`

```
$ abp info
Serial:        ABC123
Manufacturer:  Google
Model:         Pixel 8
Android:       15 (SDK 35)
Root access:   yes (su binary available)
```

`-s SERIAL` targets a specific device when more than one is connected;
otherwise `abp` uses the one ready device. With several connected and no
serial given, every command (`info`, `list-packages`, `backup`,
`restore`) stops up front and lists their serials, and a device that is
unauthorized or offline is reported as such rather than as missing.

## `abp list-packages [-s SERIAL] [--system] [--json]`

Lists installed packages. By default only third-party (user-installed)
apps are shown; `--system` includes system apps too. `--json` prints a
JSON array (`name`, `system_app`, `apk_count`) instead of plain text —
useful for scripting `--only`/`--exclude` lists.

## `abp backup -o DIR [options]`

| Option                | Description |
|------------------------|-------------|
| `-s, --serial SERIAL`  | Target a specific device. |
| `-o, --output DIR`     | **Required.** Directory to write the backup into (created if missing). |
| `--system`             | Include system apps (default: third-party only). |
| `--no-apks`            | Skip extracting APK files. |
| `--no-data`            | Skip app data. |
| `--no-shared`          | Skip `/sdcard`. |
| `--only PKGS`          | Comma-separated package names; only these are backed up. |
| `--exclude PKGS`       | Comma-separated package names to skip. |
| `--root`               | Require root; fail immediately if unavailable. |
| `--standard`           | Force standard (non-root) mode even if root is available. |
| `--all-files`          | Also copy every persistent device partition verbatim with `adb pull`. |
| `--pull-path PATH`     | Also copy one device path verbatim with `adb pull`. Repeatable. |
| `-y, --yes`            | Skip the confirmation prompt. |

Examples:

```sh
# Everything abp can reach, auto-detecting root:
abp backup -o ~/backups/full

# Apps + their data, no photos/videos, forcing root mode:
abp backup -o ~/backups/apps-only --no-shared --root

# Just two apps' data (not their APKs):
abp backup -o ~/backups/two-apps --no-apks --only com.example.one,com.example.two

# Everything except a noisy app:
abp backup -o ~/backups/most --exclude com.chatty.app
```

### Pulling whole device paths

`--all-files` and `--pull-path` copy device trees verbatim with
`adb pull -a`, on top of everything else `abp` captures. Each tree lands
under `filesystem/<name>` in the backup directory and is listed in
`manifest.json` under `filesystem_captures`.

```sh
# Every persistent partition adb can read:
abp backup -o ~/backups/full --all-files

# Just two specific trees, and nothing else:
abp backup -o ~/backups/misc --no-apks --no-data --no-shared \
    --pull-path /data/misc --pull-path /data/system
```

`--all-files` expands to these roots, skipping any that a given device
does not have:

```
/data  /sdcard  /system  /system_ext  /vendor  /product  /odm  /oem  /metadata
```

Notes:

- **Coverage depends on root.** `adb pull` reads as whatever user adbd
  runs as. With `adb root` (or a userdebug build) that is root and the
  capture is complete. Otherwise it is the shell user, which can read
  `/sdcard` and the read-only system partitions but almost nothing under
  `/data`. A tree that could only be read in part is recorded with
  `"complete": false` and a note saying why.
- **A `su` binary does not help here.** `su` elevates commands run
  *through the shell*; `adb pull` is a separate file-transfer service
  that `abp` cannot route through `su`. For a complete `--all-files`
  capture you need adbd itself running as root.
- **Redundant paths are collapsed.** Asking for `/data` and `/data/app`
  pulls `/data` once. `/sdcard` is skipped when shared storage was
  already captured, unless you passed `--no-shared`.
- **`/`, `/proc`, `/sys`, `/dev`, `/apex` and friends are refused**, with
  an explanation. They are kernel pseudo-filesystems and bind-mount
  duplicates, not stored files — `/proc/kcore` alone presents all of
  physical memory as one file, and reading a character device under
  `/dev` can block indefinitely.
- **These captures are not restored.** See the restore section below.

### What gets captured

Standard (non-root) mode captures debuggable apps completely via `run-as`
and falls back to legacy `adb backup` only for the rest, so coverage is
per app rather than all-or-nothing. See the
[coverage table](../README.md#-what-each-mode-can-save) and
[NON_ROOT_BACKUP.md](NON_ROOT_BACKUP.md).

The backup summary reports the split, for example:

```
Backup complete (standard mode).
  Packages:        48
  With app data:   41
    via run-as:     12 (complete per-app archives)
    via adb backup: 29 (partial; apps may have opted out)
  Errors:          0
```

## `abp restore -i DIR [options]`

| Option                | Description |
|------------------------|-------------|
| `-s, --serial SERIAL`  | Target a specific device. |
| `-i, --input DIR`      | **Required.** Backup directory produced by `abp backup`. |
| `--no-apks`            | Don't reinstall APKs. |
| `--no-data`            | Don't restore app data. |
| `--no-shared`          | Don't restore shared storage. |
| `--only PKGS`          | Comma-separated package names to restore (root mode only — see below). |
| `--exclude PKGS`       | Comma-separated package names to skip. |
| `-y, --yes`            | Skip the confirmation prompt. |

There is no `--root`/`--standard` flag for restore: the backup's own
`manifest.json` records which mode produced it, and that dictates how
its app data must be restored.

**Whole-partition captures (`--all-files`/`--pull-path`) are never pushed
back.** Restoring a raw partition over a running system is not safe to
automate: writing `/system` needs a writable system partition and can
leave a device unbootable, and dropping a `/data` tree over a live system
would break app UIDs and SELinux labels far more thoroughly than the
per-package restore does. `abp restore` reports how many such captures a
backup contains and leaves them in `filesystem/` for you to copy from by
hand.

**Per-package filtering (`--only`/`--exclude`) works for app data that was
captured into a per-package archive** — that is, everything in a root-mode
backup, and every debuggable app in a standard-mode backup (captured via
`run-as`). Check `data_capture_method` in `manifest.json`: `root_tar` and
`run_as_tar` filter per package; `legacy_adb_backup` does not.

Packages captured into `legacy_backup.ab` by `adb backup` share one opaque
archive that `adb restore` can only write back as a whole. `abp` only
invokes that restore if at least one selected package needs it, and warns
when doing so will also restore packages you deselected.

APK installation always respects the filters. Shared storage has no
per-package concept, so `--only`/`--exclude` don't apply to it at all —
use `--no-shared` to skip it entirely.

Restoring shared storage from a standard-mode backup merges the captured
tree into `/sdcard`: existing files at the same paths are overwritten, and
files the device has that the backup does not are left alone. `abp` never
deletes anything from `/sdcard`.

### Reading the restore summary

```
Restore complete.
  Packages restored: 14
  Packages failed:   1
  Nothing to restore: 3 (no APK and no data in the backup)
  Shared storage:    restored
```

The three package counts are disjoint:

- **restored** — an APK was reinstalled, app data was written back, or
  both.
- **failed** — something was attempted and did not work; the reason is in
  the log and in that package's `error` field.
- **nothing to restore** — the package is listed in the manifest, but the
  backup holds neither an APK nor captured data for it. Most often this
  is an app that `adb backup` declined to capture and whose APK was
  skipped with `--no-apks`. It is not a failure, and counting it as a
  success would overstate what the run achieved.

Examples:

```sh
# Restore everything:
abp restore -i ~/backups/full

# Just reinstall APKs, skip data and media:
abp restore -i ~/backups/full --no-data --no-shared

# Restore one app's data only (root-mode backup):
abp restore -i ~/backups/full --only com.example.one --no-shared
```

## `abp gui [options]`

Starts the local web GUI and (unless told not to) opens it in a browser:

```
$ abp gui
abp 1.0.0 web GUI
  Serving:      http://127.0.0.1:8787/?token=6f1c...
  Backup root:  /home/you/abp-backups
  Press Ctrl-C to stop.
```

| Option | Description |
|---|---|
| `--port PORT` | Port to listen on (default `8787`; `0` picks a free one and prints it). |
| `--host ADDR` | Address to bind (default `127.0.0.1`). |
| `-d, --backup-dir DIR` | Folder the "Explore backups" view starts from (default: the current directory). |
| `--scan-depth N` | How many directory levels below that to search (default `2`). |
| `--no-browser` | Print the URL instead of opening a browser. |
| `-v, --verbose` | Include debug-level messages in the live job log. |

The URL carries a random API token; open exactly the URL `abp` prints, or
set `ABP_GUI_TOKEN` to choose the token yourself. The GUI can back up,
restore and browse existing backups, and runs one backup/restore job at a
time. See [GUI.md](GUI.md) for the full picture, including the JSON API.

## Exit codes

| Code | Meaning |
|------|---------|
| 0    | Success. |
| 1    | Operation ran but failed (no device, bad manifest, per-package restore failures, user declined confirmation). |
| 2    | Usage error (bad flags, missing required option, unknown subcommand). |

For `abp gui`, exit code 1 also covers "could not bind the port" — most
often another `abp gui` already running on it.

## Troubleshooting

- **`Could not run 'adb'`** — install Android Platform Tools and ensure
  `adb` is on `PATH`, or pass `--adb-path`.
- **`No connected and authorized device found`** — run `adb devices`
  yourself; if the device shows `unauthorized`, accept the RSA key
  prompt on the device screen and try again.
- **Root mode falls back to standard unexpectedly** — run `abp info` to
  see the detected root method; if it says `none` but you believe the
  device is rooted, check that your root manager grants access to the
  `adb shell` (not just `su` from a terminal app), and watch the device
  screen for a superuser grant prompt during the first backup attempt.
- **Standard-mode backup produces no data for an app** — that app almost
  certainly has `android:allowBackup="false"`, which is the default for
  apps targeting recent Android SDKs. This is a hard limitation of
  `adb backup`, not a bug in `abp`; root mode has no such restriction.
