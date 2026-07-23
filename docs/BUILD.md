# Build and Release Engineering

## Supported Targets

| Target | Compiler/toolchain | Minimum runtime intent |
|---|---|---|
| Linux x86_64 native | GCC/Clang | Build host distribution |
| Linux x86_64 portable | Zig cc, glibc 2.17 target | Modern glibc distributions |
| Windows x86_64 | MinGW-w64 or MSVC | Windows 11 and supported Server |
| macOS x86_64 | Zig cc | macOS 11 |
| macOS arm64 | Zig cc | macOS 11 |

The macOS thin binaries are combined by
`scripts/make_universal_macho.py`. The helper validates Mach-O magic, CPU
types, offsets, alignment, and input cardinality before writing the fat file.

## CMake Options

| Option | Default | Purpose |
|---|---|---|
| `SYSEBA_BUILD_TESTS` | `ON` | Build native tests |
| `SYSEBA_ENABLE_HARDENING` | `ON` | Compiler/linker hardening |
| `SYSEBA_ENABLE_NATIVE_WATCHER` | `ON` | Native watcher where available |
| `BUILD_TESTING` | `ON` | Enable CTest |

## Native Developer Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Sanitizers

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

The SQLite amalgamation is excluded from sanitizer instrumentation because
instrumenting that generated 9+ MB unit is prohibitively expensive. SySeBa
ownership code, cJSON, CivetWeb, tests, and interfaces remain instrumented.

ThreadSanitizer depends on host kernel/virtual-address layout. Some WSL
kernels terminate TSan before `main` with `unexpected memory mapping`; record
that as an environment limitation and run TSan on native Linux CI.

## Static Analysis

```bash
cppcheck --force --enable=warning,style,performance,portability \
  --std=c11 --inline-suppr \
  -I include -I src -I third_party/cjson \
  src include tests_c/test_native.c
shellcheck scripts/*.sh tests_c/*.sh packaging/**/*.sh
```

Third-party amalgamations are not treated as first-party static-analysis
findings. Their exact versions and upstream origins are documented in
`THIRD_PARTY_NOTICES.md`.

## Full Local Release

```bash
./scripts/build-release.sh
```

The script builds in an ext4 temporary directory so DEB/RPM ownership and mode
metadata are not distorted by a Windows-mounted filesystem. DEB, RPM, and the
portable bundle use the verified glibc 2.17 target. It copies only finished
artifacts into `dist/syseba-2.0.0`.

From PowerShell:

```powershell
.\scripts\build-release.ps1
```

Optional environment variables:

| Variable | Meaning |
|---|---|
| `SYSEBA_RELEASE_ROOT` | Temporary Linux build root |
| `SYSEBA_DIST_DIR` | Final artifact directory |
| `ZIG` | Zig executable |
| `HFSPLUS_BIN` | `hfsplus` executable |
| `DMG_BIN` | `dmg` executable |
| `EMPTY_HFS_IMAGE` | Empty HFS+ template |

The script never invokes `git push`, package upload, or a release API.

## Reproducibility Notes

- Source assets are embedded in deterministic path order.
- Source archives exclude VCS, build, state, token, lock, DB, and log files.
- Universal Mach-O slices use fixed 16 KiB alignment.
- Package timestamps currently follow build time; byte-for-byte reproducible
  output requires a future `SOURCE_DATE_EPOCH` policy across CPack, NSIS, HFS,
  and compression tools.
- `SHA256SUMS` and `release-manifest.txt` identify every delivered artifact.
