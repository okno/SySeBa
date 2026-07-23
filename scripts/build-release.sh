#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'
umask 022

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
SOURCE_ROOT="$(dirname "$SCRIPT_DIR")"
VERSION="$(
    sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9.]+).*/\1/p' \
        "$SOURCE_ROOT/CMakeLists.txt" | head -n 1
)"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    printf 'Unable to determine project version.\n' >&2
    exit 1
}

RELEASE_ROOT="${SYSEBA_RELEASE_ROOT:-/var/tmp/syseba-release-${USER:-user}-${VERSION}}"
DIST_DIR="${SYSEBA_DIST_DIR:-${SOURCE_ROOT}/dist/syseba-${VERSION}}"
RAW_DIR="${RELEASE_ROOT}/raw"
STAGE_DIR="${RELEASE_ROOT}/stage"
JOBS="${SYSEBA_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 2)}"

ZIG="${ZIG:-$(command -v zig 2>/dev/null || true)}"
HFSPLUS_BIN="${HFSPLUS_BIN:-/opt/syseba-toolchains/libdmg-hfsplus-0.5/install/bin/hfsplus}"
DMG_BIN="${DMG_BIN:-/opt/syseba-toolchains/libdmg-hfsplus-0.5/install/bin/dmg}"
EMPTY_HFS_IMAGE="${EMPTY_HFS_IMAGE:-/opt/syseba-toolchains/libdmg-hfsplus-0.5/test/empty.hfs}"

log() {
    printf '[release] %s\n' "$*"
}

die() {
    printf '[release] ERROR: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        die "required command not found: $1"
}

canonical_parent_path() {
    local path="$1"
    local parent
    parent="$(dirname "$path")"
    mkdir -p -- "$parent"
    printf '%s/%s\n' "$(CDPATH='' cd -- "$parent" && pwd -P)" "$(basename "$path")"
}

safe_reset_directory() {
    local path="$1"
    local allowed_parent="$2"
    local resolved allowed
    resolved="$(canonical_parent_path "$path")"
    allowed="$(canonical_parent_path "$allowed_parent")"
    [[ "$resolved" == "$allowed"/* && "$resolved" != "$allowed" ]] ||
        die "refusing to reset directory outside $allowed: $resolved"
    if [[ -e "$resolved" ]]; then
        rm -rf -- "$resolved"
    fi
    mkdir -p -- "$resolved"
}

cmake_configure() {
    local build="$1"
    shift
    cmake -S "$SOURCE_ROOT" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSYSEBA_ENABLE_HARDENING=ON \
        "$@"
}

copy_single_match() {
    local directory="$1"
    local pattern="$2"
    local destination="$3"
    local -a matches=()
    while IFS= read -r -d '' match; do
        matches+=("$match")
    done < <(find "$directory" -maxdepth 1 -type f -name "$pattern" -print0)
    ((${#matches[@]} == 1)) ||
        die "expected one $pattern in $directory, found ${#matches[@]}"
    cp -- "${matches[0]}" "$destination"
}

verify_linux_abi() {
    local binary="$1"
    local maximum
    maximum="$(
        objdump -T "$binary" |
            sed -nE 's/.*GLIBC_([0-9]+\.[0-9]+).*/\1/p' |
            sort -V |
            tail -n 1
    )"
    [[ -z "$maximum" || "$(printf '%s\n%s\n' "$maximum" 2.17 | sort -V | tail -n 1)" == "2.17" ]] ||
        die "portable Linux binary requires GLIBC_${maximum}, expected at most 2.17"
    printf '%s\n' "${maximum:-none}"
}

run_windows_integration() {
    local executable="$1"
    if command -v powershell.exe >/dev/null 2>&1 &&
       command -v wslpath >/dev/null 2>&1; then
        log "Running native Windows integration test"
        powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$(wslpath -w "$SOURCE_ROOT/tests_c/integration_windows.ps1")" \
            -Binary "$(wslpath -w "$executable")"
    else
        log "PowerShell bridge unavailable; performing PE structural checks only"
    fi
}

build_linux_native_packages() {
    local build="${RELEASE_ROOT}/build-linux-native"
    log "Building and testing native Linux target"
    cmake_configure "$build" -DSYSEBA_BUILD_TESTS=ON
    cmake --build "$build" --parallel "$JOBS"
    ctest --test-dir "$build" --output-on-failure
}

build_linux_portable() {
    local build="${RELEASE_ROOT}/build-linux-portable"
    local root="${STAGE_DIR}/syseba-${VERSION}-linux-x86_64"
    local output="${RAW_DIR}/linux-portable-packages"
    local abi
    log "Building portable Linux target for glibc 2.17"
    [[ -n "$ZIG" && -x "$ZIG" ]] || die "Zig compiler not found"
    export ZIG
    cmake_configure "$build" \
        -DCMAKE_TOOLCHAIN_FILE="$SOURCE_ROOT/cmake/toolchains/zig-linux-x86_64-glibc217.cmake" \
        -DSYSEBA_PORTABLE_LINUX_PACKAGE=ON \
        -DSYSEBA_BUILD_TESTS=OFF
    cmake --build "$build" --parallel "$JOBS"
    "$build/syseba" --version
    abi="$(verify_linux_abi "$build/syseba")"
    mkdir -p -- "$root"
    DESTDIR="$root" cmake --install "$build" --prefix /usr
    tar --sort=name --owner=0 --group=0 --numeric-owner \
        -C "$STAGE_DIR" -czf \
        "$DIST_DIR/syseba-${VERSION}-linux-x86_64.tar.gz" \
        "$(basename "$root")"
    mkdir -p -- "$output"
    cpack --config "$build/CPackConfig.cmake" -G DEB -B "$output"
    cpack --config "$build/CPackConfig.cmake" -G RPM -B "$output"
    copy_single_match "$output" '*.deb' \
        "$DIST_DIR/syseba_${VERSION}_amd64.deb"
    copy_single_match "$output" '*.rpm' \
        "$DIST_DIR/syseba-${VERSION}-1.x86_64.rpm"
    printf 'portable_linux_max_glibc=%s\n' "$abi" \
        >>"$RELEASE_ROOT/release-data.txt"
}

build_source_archive() {
    local build="${RELEASE_ROOT}/build-linux-native"
    local output="${RAW_DIR}/source"
    local normalized="${STAGE_DIR}/source-normalized"
    local archive root
    local -a matches=()
    log "Building source archive"
    mkdir -p -- "$output"
    cpack --config "$build/CPackSourceConfig.cmake" -G TGZ -B "$output"
    while IFS= read -r -d '' archive; do
        matches+=("$archive")
    done < <(find "$output" -maxdepth 1 -type f -name '*.tar.gz' -print0)
    ((${#matches[@]} == 1)) ||
        die "expected one source archive, found ${#matches[@]}"
    safe_reset_directory "$normalized" "$STAGE_DIR"
    tar -xzf "${matches[0]}" -C "$normalized"
    root="${normalized}/syseba-${VERSION}-source"
    [[ -d "$root" ]] || die "unexpected source archive root"
    find "$root" -type d -exec chmod 0755 {} +
    find "$root" -type f -exec chmod 0644 {} +
    find "$root" -type f \
        \( -name '*.sh' -o -name '*.py' \) \
        -exec chmod 0755 {} +
    tar --sort=name --owner=0 --group=0 --numeric-owner \
        -C "$normalized" -czf \
        "$DIST_DIR/syseba-${VERSION}-source.tar.gz" \
        "$(basename "$root")"
}

build_windows() {
    local build="${RELEASE_ROOT}/build-windows"
    local output="${RAW_DIR}/windows"
    log "Building Windows 11/Server x86_64 target"
    cmake_configure "$build" \
        -DCMAKE_TOOLCHAIN_FILE="$SOURCE_ROOT/cmake/toolchains/mingw-w64-x86_64.cmake" \
        -DSYSEBA_BUILD_TESTS=ON
    cmake --build "$build" --parallel "$JOBS"
    file "$build/SySeBa.exe" | grep -q 'PE32+.*x86-64' ||
        die "Windows executable is not PE32+ x86-64"
    run_windows_integration "$build/SySeBa.exe"
    mkdir -p -- "$output"
    cpack --config "$build/CPackConfig.cmake" -G ZIP -B "$output"
    cpack --config "$build/CPackConfig.cmake" -G NSIS -B "$output"
    copy_single_match "$output" '*.zip' \
        "$DIST_DIR/SySeBa-${VERSION}-windows-x86_64.zip"
    copy_single_match "$output" '*.exe' \
        "$DIST_DIR/SySeBa-${VERSION}-windows-x86_64-setup.exe"
}

build_macos() {
    local x86_build="${RELEASE_ROOT}/build-macos-x86_64"
    local arm_build="${RELEASE_ROOT}/build-macos-arm64"
    local volume="${STAGE_DIR}/macos-volume"
    local universal="${volume}/bin/syseba"
    local image="${RELEASE_ROOT}/syseba.hfs"
    local output="${DIST_DIR}/SySeBa-${VERSION}-macos-universal.dmg"
    log "Building macOS x86_64 and arm64 targets"
    [[ -x "$HFSPLUS_BIN" ]] || die "hfsplus tool not found: $HFSPLUS_BIN"
    [[ -x "$DMG_BIN" ]] || die "dmg tool not found: $DMG_BIN"
    [[ -f "$EMPTY_HFS_IMAGE" ]] ||
        die "empty HFS+ template not found: $EMPTY_HFS_IMAGE"
    export ZIG
    cmake_configure "$x86_build" \
        -DCMAKE_TOOLCHAIN_FILE="$SOURCE_ROOT/cmake/toolchains/zig-macos-x86_64.cmake" \
        -DSYSEBA_BUILD_TESTS=OFF
    cmake --build "$x86_build" --parallel "$JOBS"
    cmake_configure "$arm_build" \
        -DCMAKE_TOOLCHAIN_FILE="$SOURCE_ROOT/cmake/toolchains/zig-macos-arm64.cmake" \
        -DSYSEBA_BUILD_TESTS=OFF
    cmake --build "$arm_build" --parallel "$JOBS"
    mkdir -p -- "$volume"
    cmake --install "$x86_build" --prefix "$volume"
    python3 "$SOURCE_ROOT/scripts/make_universal_macho.py" \
        "$universal" "$x86_build/syseba" "$arm_build/syseba"
    file "$universal" | grep -q 'Mach-O universal binary' ||
        die "macOS executable is not a universal Mach-O"
    cp -- "$EMPTY_HFS_IMAGE" "$image"
    python3 "$SOURCE_ROOT/scripts/rename_hfs_label.py" \
        "$image" Firefox SySeBa2
    "$HFSPLUS_BIN" "$image" grow 134217728
    "$HFSPLUS_BIN" "$image" addall "$volume" /
    "$DMG_BIN" --compression zlib --level 9 build "$image" "$output"
    file "$output" | grep -qi 'data' ||
        die "DMG output was not created"
}

verify_packages() {
    local dmg_extract mac_binary rpm_database
    log "Inspecting package contents"
    tar -tzf "$DIST_DIR/syseba-${VERSION}-linux-x86_64.tar.gz" \
        | grep '/usr/bin/syseba$' >/dev/null
    tar -tzf "$DIST_DIR/syseba-${VERSION}-source.tar.gz" \
        | grep '/src/main.c$' >/dev/null
    dpkg-deb --contents "$DIST_DIR/syseba_${VERSION}_amd64.deb" \
        | grep '/usr/bin/syseba$' >/dev/null
    rpm_database="${RELEASE_ROOT}/rpmdb"
    mkdir -p -- "$rpm_database"
    rpm --dbpath "$rpm_database" \
        -qlp "$DIST_DIR/syseba-${VERSION}-1.x86_64.rpm" \
        | grep '/usr/bin/syseba$' >/dev/null
    unzip -Z1 "$DIST_DIR/SySeBa-${VERSION}-windows-x86_64.zip" \
        | grep -i 'bin/SySeBa.exe$' >/dev/null
    7z l "$DIST_DIR/SySeBa-${VERSION}-macos-universal.dmg" \
        | grep 'SySeBa2/bin/syseba$' >/dev/null
    dmg_extract="${RELEASE_ROOT}/verify-dmg"
    safe_reset_directory "$dmg_extract" "$RELEASE_ROOT"
    7z x -y -o"$dmg_extract" \
        "$DIST_DIR/SySeBa-${VERSION}-macos-universal.dmg" >/dev/null
    mac_binary="$(
        find "$dmg_extract" -type f -path '*/bin/syseba' -print -quit
    )"
    [[ -n "$mac_binary" ]] ||
        die "macOS executable was not found after DMG extraction"
    file "$mac_binary" | grep -q 'Mach-O universal binary' ||
        die "extracted DMG executable is not Universal 2"
}

write_manifest() {
    local commit dirty checksum_temporary
    commit="$(git -C "$SOURCE_ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
    dirty="$(git -C "$SOURCE_ROOT" status --porcelain 2>/dev/null | wc -l)"
    {
        printf 'SySeBa local release\n'
        printf 'version=%s\n' "$VERSION"
        printf 'created_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'source_commit=%s\n' "$commit"
        printf 'source_dirty_entries=%s\n' "$dirty"
        printf 'host=%s\n' "$(uname -a)"
        printf 'cmake=%s\n' "$(cmake --version | head -n 1)"
        printf 'native_compiler=%s\n' "$(cc --version | head -n 1)"
        printf 'zig=%s\n' "$("$ZIG" version)"
        cat "$RELEASE_ROOT/release-data.txt"
        printf '\nArtifacts:\n'
        find "$DIST_DIR" -maxdepth 1 -type f \
            ! -name SHA256SUMS \
            ! -name release-manifest.txt \
            -printf '%f %s bytes\n' | sort
        printf '\nValidation:\n'
        printf 'native_linux_ctest=passed\n'
        printf 'windows_integration=%s\n' \
            "$(command -v powershell.exe >/dev/null 2>&1 && printf passed || printf structural-only)"
        printf 'macos_execution=cross-build-structural-only\n'
        printf 'published=false\n'
    } >"$DIST_DIR/release-manifest.txt"
    checksum_temporary="$RELEASE_ROOT/SHA256SUMS.tmp"
    (
        cd "$DIST_DIR"
        find . -maxdepth 1 -type f \
            ! -name SHA256SUMS \
            -printf '%f\0' |
            sort -z |
            xargs -0 sha256sum -- >"$checksum_temporary"
        mv -- "$checksum_temporary" SHA256SUMS
        sha256sum -c SHA256SUMS
    )
}

main() {
    local release_parent dist_parent
    for command in cmake cpack ctest ninja cc git sed tar find sort sha256sum \
                   objdump file dpkg-deb rpmbuild rpm makensis unzip 7z \
                   python3 xargs; do
        require_command "$command"
    done
    release_parent="$(dirname "$RELEASE_ROOT")"
    dist_parent="$(dirname "$DIST_DIR")"
    mkdir -p -- "$release_parent" "$dist_parent"
    if [[ "${SYSEBA_RELEASE_RESUME:-0}" == "1" ]]; then
        [[ -d "$RELEASE_ROOT" && -d "$DIST_DIR" ]] ||
            die "resume requested but release directories do not exist"
        log "Resuming the existing local release workspace"
    else
        safe_reset_directory "$RELEASE_ROOT" "$release_parent"
        safe_reset_directory "$DIST_DIR" "$dist_parent"
    fi
    mkdir -p -- "$RAW_DIR" "$STAGE_DIR"
    : >"$RELEASE_ROOT/release-data.txt"

    build_linux_native_packages
    build_linux_portable
    build_source_archive
    build_windows
    build_macos
    verify_packages
    write_manifest

    log "Release completed locally: $DIST_DIR"
    log "Nothing was published or pushed."
}

main "$@"
