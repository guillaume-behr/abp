# Backup directory layout and manifest schema

A backup produced by `abp backup -o DIR` looks like:

```
DIR/
  manifest.json
  apks/
    <package>/
      base.apk
      split_config.arm64_v8a.apk   (if present)
  data/
    <package>.tar.gz               (root mode, or run-as in standard mode)
  shared_storage.tar                (root mode; single tar of /sdcard)
  shared_storage/                   (standard mode; plain directory tree, pulled via adb pull)
  filesystem/                       (--all-files / --pull-path; one directory per captured path)
    data/                             ("/data")
    system/                           ("/system")
  legacy_backup.ab                  (standard mode; raw `adb backup` archive,
                                     only for packages run-as could not reach)
```

Every path referenced from `manifest.json` is relative to `DIR`, so a
backup directory is self-contained and can be moved/copied as a whole.

## `manifest.json`

```jsonc
{
  "format_version": 3,
  "abp_version": "1.0.0",
  "created_at_utc": "2026-01-01T12:00:00Z",
  "mode": "root",                    // or "standard"
  "device": {
    "serial": "ABC123",
    "model": "Pixel 8",
    "manufacturer": "Google",
    "android_release": "15",
    "sdk_int": 35,
    "rooted": true,
    "root_method": "su_binary"       // "adbd_root" | "su_binary" | "none"
  },
  "shared_storage_included": true,
  "shared_storage_is_directory": false, // true in standard mode (a pulled dir tree, not a tar)
  "shared_storage_archive": "shared_storage.tar",
  "shared_storage_archive_bytes": 123456789,
  "shared_storage_archive_sha256": "…",  // empty when shared_storage_is_directory is true

  "legacy_adb_backup_file": "",       // set to "legacy_backup.ab" in standard mode

  // Whole device paths copied verbatim with `adb pull` (--all-files /
  // --pull-path). Empty unless one of those was used.
  "filesystem_captures": [
    {
      "device_path": "/data",           // absolute path on the device
      "local_path": "filesystem/data",  // relative to the backup directory
      "bytes": 2147483648,
      "complete": false,                // false if adb could not read all of it
      "note": "Permission denied on /data/data"
    }
  ],

  "packages": [
    {
      "name": "com.example.app",
      "system_app": false,

      "apk_included": true,
      "apk_files": ["apks/com.example.app/base.apk"],

      "data_included": true,
      "data_capture_method": "root_tar",  // root_tar | run_as_tar | legacy_adb_backup | none
      "data_archive": "data/com.example.app.tar.gz",
      "data_archive_bytes": 45678,
      "data_archive_sha256": "…",

      "external_data_included": false,
      "external_data_archive": "",
      "external_data_archive_bytes": 0,
      "external_data_archive_sha256": "",

      "error": ""                     // non-empty if this package failed
    }
  ]
}
```

Notes:

- Object key order in the file is stable (insertion order, as written
  above) — `abp`'s JSON writer preserves it, which keeps manifests
  diff-friendly across repeated backups.
- `error` on a package entry does not mean the whole backup failed —
  `abp backup`'s exit code reflects fatal errors (couldn't connect,
  couldn't create the output directory, ...); per-package errors are
  reported in the summary and left in the manifest for inspection.
- `data_included: false` with an empty `error` means there was simply
  nothing to capture (e.g. the app has no data directory yet), not a
  failure.
- `data_capture_method` says how that package's private data was
  obtained, because a standard-mode backup mixes methods:
  - `root_tar` — per-package `tar` of `/data/data/<pkg>` taken as root.
  - `run_as_tar` — per-package `tar` taken as the app's own UID through
    `run-as`, which works without root for apps built with
    `android:debuggable="true"`. Same archive layout as `root_tar`.
  - `legacy_adb_backup` — the package's data (if any) is inside the
    shared `legacy_adb_backup_file` archive, not in a per-package file.
    `data_archive` is empty and there is no checksum, because
    `adb backup` gives no per-package output.
  - `none` — no private data was captured for this package.
  A method this build does not recognise is read as `none`, so an older
  `abp` never mistakes a future capture method for one it can restore.
- Only `root_tar` and `run_as_tar` packages can be restored selectively;
  `legacy_adb_backup` packages share one archive that `adb restore` can
  only write back as a whole.
- `external_data_included`/`external_data_archive*` fields are reserved
  for a future capture of `/sdcard/Android/data/<pkg>` (per-app external
  storage); they are always empty/false today.
- A capture's `local_path` is derived from its device path with the
  slashes replaced (`/data/app` becomes `filesystem/data_app`). That
  mapping is many-to-one, so when two captured paths would reduce to the
  same name the later one gets a numbered suffix (`data_app_2`). Always
  read `local_path` from the manifest rather than recomputing it from
  `device_path`.
- `filesystem_captures` records raw `adb pull` copies of whole device
  paths. `complete: false` means `adb pull` reported errors — almost
  always permission denied on part of the tree, which is the normal
  result of pulling `/data` without root. The partial tree is kept
  regardless, because part of `/data` beats none of it. `abp restore`
  never writes these back; it reports them and leaves them alone.
- `format_version` will be bumped if the schema changes in a
  backwards-incompatible way; `abp` refuses to guess at unknown
  versions rather than silently misinterpreting a newer manifest.
  Version 2 added `data_capture_method` and, with it, per-package
  archives in standard mode — an `abp` that only knows version 1 has no
  notion of those and would silently skip them on restore. Version 1
  manifests are still read: a version 1 entry with a `data_archive` can
  only have come from root mode, and one without can only have come
  from the legacy archive, so the method is inferred on load. Version 3
  added `filesystem_captures`; an older `abp` would not report those as
  part of the backup at all. Versions 1 and 2 still load, with an empty
  `filesystem_captures`.

See [ROOT_BACKUP.md](ROOT_BACKUP.md) for what actually produces the
`data/*.tar.gz` and `shared_storage.tar` archives, and the top-level
[README.md](../README.md) for the CLI that reads/writes this format.
