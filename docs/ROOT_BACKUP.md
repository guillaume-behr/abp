# Root-mode backup and restore

This document describes exactly what `abp` does on-device in root mode
(`RootBackend`), so you know what to expect and can debug it yourself if
something goes wrong.

## Detecting root

`AdbClient::detectRoot()` tries, in order:

1. `adb shell id -u` — if this prints `0`, `adbd` itself is already
   running as root (typical of `userdebug`/`eng` builds, or after
   `adb root` on a device that allows it). No `su` is needed for
   anything; commands run directly.
2. `adb shell command -v su` (falling back to `which su`) to find a
   `su` binary reachable from the shell user (Magisk, KernelSU, and
   most other root solutions expose one this way).
3. `adb shell su -c 'id -u'` — if this prints `0`, `abp` uses
   `su -c '<command>'` to run privileged commands. The command is
   always passed as a single quoted argument, because several `su`
   implementations otherwise treat only its first word as the command.
   **This step may
   trigger an on-device Superuser permission prompt** (Magisk's grant
   dialog, etc.) the first time; you'll need to approve it there.

If neither works, `abp` falls back to standard mode (or fails, with
`--root`). Standard mode is not merely a degraded version of this one —
it captures debuggable apps completely, via `run-as`. See
[NON_ROOT_BACKUP.md](NON_ROOT_BACKUP.md).

## Backing up app data

For each package, `abp` runs (conceptually):

```sh
[ -d '/data/data/<pkg>' ] && echo yes || echo no   # skip if no data dir
tar -czf - -C /data/data '<pkg>' 2>/dev/null       # streamed via `adb exec-out`
```

directly to a local `data/<pkg>.tar.gz`, streamed via `adb exec-out` so
the archive never passes through `abp`'s own memory. The result is
SHA-256 checksummed and recorded in `manifest.json`.

The single quotes above are real, not editorial: every value abp
interpolates into a device command is shell-quoted at the point of use as
well as validated beforehand. Under `su`, the whole command is quoted a
second time as the argument to `su -c`.

Apps with no data directory yet (freshly installed, never opened) are
recorded with `data_included: false` and no error — that's expected, not
a failure.

## Backing up shared storage

```sh
tar -cf - -C /sdcard . 2>/dev/null
```

streamed to `shared_storage.tar` (uncompressed — most of what lives on
`/sdcard` is already-compressed media, so gzip would just cost CPU for
no size benefit).

## Restoring app data

This is the part that needs the most care, because Android assigns each
app a UID at install time, and that UID is **not guaranteed to be the
same** across an uninstall/reinstall cycle. A raw `tar -x` as root would
silently restore every file with its *original* backup-time UID/GID,
which may now belong to a different app (or nothing) — the app would
then be unable to read its own data.

`abp` handles this by, for each package:

1. Verifying the archive's SHA-256 against the manifest before touching
   the device at all.
2. Making sure the APK is (re)installed first (`BackupManager` always
   installs APKs before restoring data), so the OS has already created
   a fresh, empty `/data/data/<pkg>` owned by the *current* UID.
3. `am force-stop <pkg>`, so the app is not running while its own data
   directory is replaced underneath it. A live process would otherwise
   see a half-old, half-new view of its files and could write over the
   restored data from its in-memory state.
4. Recording that current ownership: `stat -c '%u:%g' '/data/data/<pkg>'`.
5. Extracting the archive: `tar -xzf - -C /data/data`, streamed from the
   local file via `adb shell ... < file` (the file's contents become
   the remote command's stdin).
6. `chown -R <uid>:<gid> /data/data/<pkg>` back to the UID captured in
   step 4, undoing whatever ownership `tar -x` (running as root) just
   set from the archive.
7. `restorecon -R /data/data/<pkg>` to fix up SELinux security contexts,
   which `tar` also doesn't preserve/regenerate correctly across a
   restore onto a different inode set.

If step 4's `stat` fails (e.g. the app wasn't actually installed first),
`abp` still extracts the archive but logs a warning: the data will be
readable by root but may not be usable by the app until it's relaunched
in a way that triggers Android to fix ownership itself (or until you
`chown` it manually).

If step 1's checksum does not match, that package is skipped entirely
and recorded as failed — a truncated or corrupted archive is never
unpacked over live app data.

## Restoring shared storage

```sh
tar -xf - -C /sdcard 2>/dev/null
```

streamed from the local `shared_storage.tar` via `adb shell ... < file`.
This merges into existing `/sdcard` content rather than wiping it first
— restoring is additive/overwriting, not destructive to files that
aren't in the archive.

## Known limitations

- Requires a `tar` binary on the device that supports `-c`/`-x`/`-z`/`-f`
  (toybox and busybox tar both do; this covers effectively every Android
  6+ device abp has been tested against).
- `restorecon` may not exist on all ROMs/ports; when it's missing, the
  `restorecon` step simply fails silently (its result isn't checked) and
  SELinux contexts are left as `tar` set them, which is usually fine for
  permissive or SELinux-disabled builds but can cause an app to fail to
  read its own data on an enforcing build with a very old restorecon-less
  base image.
- A `su` grant prompt is per-app-session on many root implementations;
  if a long backup appears to hang, check the device screen.
