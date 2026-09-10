# 🔐 What standard (non-root) mode does

Standard mode is what `abp` uses when no root access is detected, or when
you pass `--standard`. It is not a single mechanism: `abp` tries the
strongest option available for each package and records which one it used
in `manifest.json`.

See the [coverage table](../README.md#-what-each-mode-can-save) for a
side-by-side comparison with root mode.

## The three mechanisms

| Mechanism | Applies to | Quality |
|---|---|---|
| `adb pull` of APK paths | Every package | Complete — base plus every split APK |
| `run-as` + `tar` | Apps with `android:debuggable="true"` | Complete private data, per package |
| Legacy `adb backup` | Everything else | Partial, opt-in, no per-package granularity |

APK extraction is handled by `BackupManager` in both modes and needs no
privileges at all; the rest is `StandardBackend`.

## App data via `run-as`

`run-as <pkg> <command>` runs a command as an app's own UID. Android only
allows this for packages built with `android:debuggable="true"`, which in
practice means your own debug builds plus a reasonable number of F-Droid
and sideloaded apps. It is the only way to read `/data/data/<pkg>`
without root.

`abp` probes every selected package in **one** on-device shell pass:

```sh
run-as <pkg> id -u >/dev/null 2>&1 && echo "@@abp-runas:<pkg>"; ...
```

Probing one package per `adb shell` call would cost a round trip each,
which is tens of seconds on a device with a few hundred apps.

For each package that answers, `abp` streams:

```sh
run-as <pkg> tar -czf - -C /data/data <pkg>
```

straight into `data/<pkg>.tar.gz` via `adb exec-out`, then SHA-256
checksums it into the manifest with
`data_capture_method: "run_as_tar"`.

The `-C /data/data <pkg>` form (rather than `-C /data/data/<pkg> .`) is
deliberate: it produces an archive **byte-identical in layout** to the one
root mode produces, so both restore paths — and anyone inspecting a backup
by hand — see one format.

## App data via legacy `adb backup`

Whatever `run-as` could not reach falls back to:

```sh
adb backup -f legacy_backup.ab -noapk -noshared <pkg> <pkg> ...
```

Only the leftover packages are passed, so nothing is captured twice. This
path has real limitations, none of which `abp` can work around:

- It requires you to unlock the device and tap **"Back up my data"**.
- It silently skips apps with `android:allowBackup="false"`.
- On Android 12 and later it skips app data entirely unless the app
  explicitly opts in.
- It produces one opaque archive with no per-package output, so there is
  nothing to checksum per package and nothing to restore selectively.

If `run-as` covered every selected package, `abp` skips this step
altogether — and with it the on-device prompt.

## Restoring

APKs are reinstalled first, exactly as in root mode (`adb install-multiple`
for split sets).

**`run_as_tar` packages** are restored one at a time:

1. Verify the archive's SHA-256 against the manifest.
2. `am force-stop <pkg>`, so the app is not running while its own data
   directory is replaced underneath it.
3. `run-as <pkg> tar -xzf - -C /data/data`, streamed from the local file.

Note what step 3 does *not* need: root mode has to snapshot the app's UID
beforehand and `chown -R` afterwards, because a root `tar` restores the
archive's original ownership. Here `tar` is already running as the app's
own UID and cannot chown at all, so every extracted file lands owned by
the app by construction. There is no UID remapping step and nothing to get
wrong.

This also means `--only` and `--exclude` work for these packages, which is
new in manifest format version 2.

**`legacy_adb_backup` packages** are restored with `adb restore
legacy_backup.ab`, which writes back the whole archive. `abp` only invokes
it if at least one selected package needs it, and warns when doing so will
also restore packages you deselected — `adb restore` offers no way to
filter.

## Failure modes worth knowing

- An app that was debuggable at backup time but is not at restore time
  (rebuilt as a release build) cannot have its `run_as_tar` archive
  restored; `run-as` will refuse and the package is recorded as failed.
- `run-as` also fails if the package is not installed for the current
  user, which is why APKs are reinstalled before data is restored.
- A checksum mismatch on a `run_as_tar` archive skips that package
  entirely rather than unpacking a corrupted archive over live app data.

## Pulling whole partitions without root

`--all-files` and `--pull-path` copy device trees verbatim with
`adb pull -a`, and are not root-specific — but what they can read is.
`adb pull` runs as whatever user adbd runs as, so on a non-rooted device
it sees `/sdcard` and the read-only system partitions (`/system`,
`/vendor`, `/product`, ...) but almost nothing under `/data`, which is
mode 0700 root.

A `su` binary does **not** close this gap. `su` elevates commands run
through the shell; `adb pull` is a separate file-transfer service that
cannot be routed through it. So on a device rooted with Magisk but
without `adb root`, `--all-files` still captures only the shell-readable
parts — and marks the rest `"complete": false` in the manifest rather
than presenting a partial copy as a whole one.

For app data on such a device, the `run-as` capture above is the better
tool: it reaches the complete private directory of every debuggable app,
which `adb pull` cannot touch at all.

See [ROOT_BACKUP.md](ROOT_BACKUP.md) for the root-mode equivalents and
[MANIFEST.md](MANIFEST.md) for the on-disk schema.
