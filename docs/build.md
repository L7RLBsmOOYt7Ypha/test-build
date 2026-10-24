# Build

Use Linux with CMake 3.22+, GNU make, Python 3, and the matching DDK kernel tree and compiler
for each KMI. The optional measurement tool below is the one thing in this tree that wants an
Android NDK.

Each KMI builds with its DDK compiler so that Clang CFI and shadow call stack
instrumentation match. Kbuild links the module and writes its objects into
`build/kmi/<KMI>`, a directory holding symlinks to `src/`.

## Package build

With all default DDKs under `/opt/ddk`:

```sh
cmake -S . -B build -DDDK_ROOT=/opt/ddk
cmake --build build -j8
```

Every KMI is built: android12-5.10, android13-5.10, android13-5.15, android14-5.15,
android14-6.1, android15-6.6, android16-6.12 and android17-6.18. The 5.x four used to sit
behind an option that was off by default; the guard that answers the reason for that is in
`src/patch.c`, and keeping them out of the build only meant they were never built.

Modules go to `build/ko`. The default target also packs the flashable zip into `build/dist`:
it carries the ko files, the userspace half (`sync-tool`, built from `src/tools`), the loader
(`lkmloader`, built from the `external/lkmloader` submodule, an upstream MIT project pinned
by `.gitmodules` + gitlink) and the module template with its boot scripts. `lkmloader` is
what the module's scripts load the ko with when no `ksud` is installed: it does not depend
on a particular root solution, which is why it is bundled. The zip wants an Android NDK for
those two binaries -- `-DTOSYA_NDK=/path/to/ndk`, or `ANDROID_NDK_HOME` in the environment;
without one the ko targets still build and the package target says so instead of failing.
Clone with `--recurse-submodules`, or run `git submodule update --init`.

The version and `versionCode` in the packed `module.prop` come from the git tag (falling back to the
values in `module/module.prop` for a tree without git). `-DTOSYA_USE_PREBUILT_KO=ON` packs the modules
already in `build/ko`, which is how the CI packaging job assembles a zip from separately built KMI
jobs.

## Separate KMI builds

For environments with one DDK each, CMake and CI share `scripts/build-kmi.sh`:

| KMI | DDK compiler |
| --- | --- |
| android12-5.10 | clang-r416183b |
| android13-5.10 | clang-r450784e |
| android13-5.15 | clang-r450784e |
| android14-5.15 | clang-r487747c |
| android14-6.1 | clang-r487747c |
| android15-6.6 | clang-r510928 |
| android16-6.12 | clang-r536225 |
| android17-6.18 | clang-r584948c |

For example, run this inside the 6.1 build environment:

```sh
TOSYA_KBUILD_JOBS=8 bash scripts/build-kmi.sh \
  android14-6.1 clang-r487747c \
  build/kmi/android14-6.1 build/ko/android14-6.1_arm64_tosya.ko
```

Collect the desired modules in `build/ko`, then package them with the NDK:

```sh
cmake -S . -B build -DTOSYA_USE_PREBUILT_KO=ON -DTOSYA_NDK=/path/to/ndk
cmake --build build -j8
```

Prebuilt mode packages the files present in `build/ko`; keep that directory limited
to the intended candidate. `TOSYA_DEBUG=ON` enables permanent kernel diagnostics.
Normal packages leave it off; the `debug=1` module parameter enables them for 60 seconds.

## Userspace half

`src/tools` holds the two programs a device runs: `sync-tool` and `uidbench`.

`sync-tool` is the half that turns an app's configuration into what the kernel takes. It reads
HMA's or HMA-OSS's `config.json`, the package database (`/data/system/packages.xml`, in either
form) and the user list, works out the (caller, target) pairs the way the apps themselves do,
and pushes them -- with the apk records that name an isolated child -- over the module's
netlink family. It follows the files and keeps the previous policy when a push does not land;
`--once`, `--explain CALLER TARGET`, `--list CALLER` and `--template CALLER
[--write-config]` answer questions without pushing anything. `scripts/gen_oss_presets.py`
regenerates the written-in half of the HMA-OSS presets in `src/tools/oss_presets.hpp` from
the app's sources. `service.sh` starts it and logs to `state/sync.log`.

## Loading

Install the zip through KernelSU and reboot. `customize.sh` selects the matching
KMI. `post-fs-data.sh` loads early; `service.sh` retries if needed . Both prefer `/data/adb/ksud insmod` and use the bundled loader only
when ksud is absent. Logs are in the installed module's `state/sync.log`.

For manual loading from a root shell on a boot without this module active:

```sh
/data/adb/ksud insmod /data/local/tmp/tosya.ko 'uid_tier=inline setuid_tier=inline'
```

Forcing the inline tiers disables fallback for that attempt. Normal package loading
uses automatic selection; check module status for the installed mechanisms and
errors. Published inline hooks remain loaded until reboot, so updating package
files does not replace the running module. A vermagic release-string difference
alone does not establish incompatibility, but the loader cannot repair incompatible
KMI structure layouts.

## Tests

- `scripts/run-hosttest.sh` compiles the real `src/policy.c` against the `linux/*` shims in
  `scripts/hosttest/` and checks every configured `(caller, target)` pair, and the same for the ABX
  reader against the first bytes of a real `packages.xml`. Real sources, so word sizes, field order
  and the encoding of a branch written into kernel text are covered.
- `scripts/inline_reloc_test.c` relocates a real kernel function (`find_user`, taken from a device
  image) and checks the trampoline prefixes, the native resume targets and the entry patch;
  `scripts/tiers_test.c` drives the real `tiers.c` with a table of fakes, so order, forcing and
  what a revert takes back are checked without a kernel.
- The C++ half is built and run here too, plain and under ASan/UBSan: the ABX reader, the package
  database in both of its forms, both rule formats driven with configs instead of a device, the
  rule places, and the wire format of a stream pinned against the values
  `scripts/test-netlink-flow.py` drives the module's own handlers with.
- `scripts/branch_encode_test.c` checks the branch encoding on its own, in user space.
- `scripts/lookup_model.py` states what a query touches as a function of `(caller, target)` and the
  load counts, which the C cannot assert about itself.
- `scripts/run-hosttest-arm.sh` runs the host tests through an arm64 compiler and qemu;
  `scripts/run-qemu.sh` boots the module in qemu and drives it the way the userspace half does.
- `scripts/test-netlink-flow.py` compiles the module's own netlink handlers against a
  deterministic shim and drives a stream through them: ownership, the idle timeout, malformed
  batches and the record layouts.
- `scripts/check-undefined.sh` checks every undefined symbol against the DDK's per-KMI
  `Module.symvers`, the same table kbuild uses; `scripts/test-kmi-map.sh` checks `uname -r` to KMI
  mapping and that the zip carries the result.
- `scripts/run-clang-tidy.sh` analyses both halves: the module with the flags kbuild really
  compiled with, taken from its `.cmd` files, and the userspace helper on its own.
- `scripts/check-format.sh` covers what neither tool reaches -- the CMake files, the shell scripts,
  the module template, the workflow -- and runs `clang-format --dry-run` over every source file. The
  module and the test sources use the kernel's `.clang-format`, `src/tools/` uses LLVM's.

## uidbench

This optional measurement tool is not a package component. Cross-compile it and
run as an app uid with hiding rules:

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang" \
  -O2 -static -o uidbench src/tools/uidbench.c
```

It samples hidden, absent and unhooked cases in one round. Compare latency on a
controlled device workload; functional success alone does not establish a speedup.
