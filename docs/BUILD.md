# Build and Release Engineering

[Italiano](BUILD.it.md) | [Documentation index](README.md)

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

## Publication Workflow

Publication is intentionally separated from compilation:

1. tag the tested source and create a GitHub Release;
2. attach the seven platform/source artifacts, `SHA256SUMS`, and
   `release-manifest.txt`;
3. manually dispatch `.github/workflows/publish-packages.yml` with the release
   version;
4. let the workflow download the Release, verify checksums and the exact
   nine-file set, and publish `ghcr.io/okno/syseba-packages`;
5. verify the public package page, tags, digest, and anonymous pull.

The workflow grants only:

```yaml
permissions:
  contents: read
  packages: write
```

It uses the repository `GITHUB_TOKEN`; no personal package token is stored as
a secret. The checkout action is pinned to a full commit SHA. The published
OCI image is based on `scratch` and carries static files under `/packages`.

Maintainers can dispatch a known release with:

```bash
gh workflow run publish-packages.yml \
  --repo okno/SySeBa \
  --ref main \
  -f version=2.0.0
```

The current package is public at
`ghcr.io/okno/syseba-packages:2.0.0`, also tagged `latest`.

## Reproducibility Notes

- Source assets are embedded in deterministic path order.
- Source archives exclude VCS, build, state, token, lock, DB, and log files.
- Universal Mach-O slices use fixed 16 KiB alignment.
- Package timestamps currently follow build time; byte-for-byte reproducible
  output requires a future `SOURCE_DATE_EPOCH` policy across CPack, NSIS, HFS,
  and compression tools.
- `SHA256SUMS` and `release-manifest.txt` identify every delivered artifact.
- Re-running the OCI mirror workflow can produce a different container digest
  if archive layer metadata changes even when payload file hashes remain
  identical. The release-level `SHA256SUMS` remains the payload authority.
