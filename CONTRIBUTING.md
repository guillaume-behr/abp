# Contributing to abp

Thanks for considering a contribution! This project intentionally stays
small and dependency-free, so the bar for adding new dependencies is high,
but bug fixes, portability improvements, docs, and well-scoped features
are all welcome.

## Getting set up

```sh
git clone https://github.com/guillaume-behr/abp.git
cd abp
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DABP_WARNINGS_AS_ERRORS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Building with `-DABP_WARNINGS_AS_ERRORS=ON` matches what CI enforces
(GCC and Clang, `-Wall -Wextra -Wpedantic` plus a handful of stricter
flags — see `cmake/CompilerWarnings.cmake`). Please make sure your change
builds warning-free under both compilers before opening a PR; CI will
otherwise fail.

## Code style

- C++17, formatted per the included `.clang-format` (run
  `clang-format -i` on files you touch).
- No comments explaining *what* code does — name things well instead.
  Comments are for non-obvious *why* (a workaround, an invariant, a
  surprising platform quirk).
- No new third-party dependencies without discussion first. `abp` is
  deliberately self-contained (its own minimal JSON parser, SHA-256, and
  subprocess wrapper) so it builds anywhere with just a C++17 toolchain
  and CMake — that's a feature, not an oversight.
- Prefer small, focused pull requests over large ones.

## Project layout

```
include/abp/   Public headers (one class/concept per header)
src/util/      Dependency-free utilities: Process, Json, Sha256, StringUtil, FsUtil, Logger
src/adb/       AdbClient: the only place that shells out to `adb`
src/backup/    Manifest, IBackupBackend, RootBackend, StandardBackend, BackupManager
src/cli/       Argument parsing and command dispatch
tests/         Unit tests (self-contained test runner, no external framework)
docs/          Design docs: architecture, root-mode internals, manifest schema
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how these pieces fit
together.

## Testing

`ABP_BUILD_TESTS` (default `ON`) builds `abp_tests`, covering the
pure-logic pieces (`Json`, `Sha256`, `StringUtil`, `Manifest`). Please add
test coverage for any new pure-logic code.

Because `AdbClient`, `RootBackend`, and `StandardBackend` need a real (or
simulated) device, they aren't covered by the unit test binary. When
changing them, the most effective manual check is a fake `adb` script
that redirects on-device paths into a local sandbox directory and lets
real `tar`/`mkdir`/`stat` run against it — this exercises the full
backup → restore round trip, including checksum verification, without
needing a physical device. If you're changing device-facing logic,
describe in your PR how you validated it (fake-adb script, real device
model, etc).

## Reporting bugs / requesting features

Please open an issue with:
- `abp --version` output and your OS/distro.
- The exact command you ran and its full output (`-v` for verbose logs).
- For device-specific issues: manufacturer, Android version, and whether
  the device is rooted and how (Magisk, KernelSU, userdebug build, ...).

## Security

`abp` shells out to `adb` and, in root mode, to an on-device root shell.
If you find a way for backup content (package names, file paths, archive
contents) to result in unintended command execution on the host or the
device, please report it privately rather than opening a public issue.
