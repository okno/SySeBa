cmake_minimum_required(VERSION 3.20)

project(
  SySeBa
  VERSION 2.0.0
  DESCRIPTION "The Syncro Service Backup"
  HOMEPAGE_URL "https://github.com/okno/SySeBa"
  LANGUAGES C
)

include(GNUInstallDirs)
include(CTest)

option(SYSEBA_BUILD_TESTS "Build the native test suite" ON)
option(SYSEBA_ENABLE_HARDENING "Enable compiler and linker hardening" ON)
option(SYSEBA_ENABLE_NATIVE_WATCHER "Use native OS filesystem notifications" ON)
option(
  SYSEBA_PORTABLE_LINUX_PACKAGE
  "Package the glibc 2.17 portable Linux build"
  OFF
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(
  CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE
  GROUP_READ GROUP_EXECUTE
  WORLD_READ WORLD_EXECUTE
)

set(SYSEBA_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SYSEBA_GENERATED_DIR}")

add_custom_command(
  OUTPUT
    "${SYSEBA_GENERATED_DIR}/syseba_assets.c"
    "${SYSEBA_GENERATED_DIR}/syseba_assets.h"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DOUTPUT_C=${SYSEBA_GENERATED_DIR}/syseba_assets.c"
    "-DOUTPUT_H=${SYSEBA_GENERATED_DIR}/syseba_assets.h"
    "-DHTML_FILE=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    "-DJS_FILE=${CMAKE_CURRENT_SOURCE_DIR}/web/syseba_web.js"
    "-DLOGO_FILE=${CMAKE_CURRENT_SOURCE_DIR}/web/SySeBa_Logo.webp"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/syseba_web.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/SySeBa_Logo.webp"
  VERBATIM
)

set(SYSEBA_SOURCES
  src/main.c
  src/cli.c
  src/app.c
  src/config.c
  src/platform.c
  src/queue.c
  src/fs.c
  src/database.c
  src/log.c
  src/watcher.c
  src/watcher_poll.c
  src/web.c
  src/dashboard.c
  "${SYSEBA_GENERATED_DIR}/syseba_assets.c"
  third_party/civetweb/civetweb.c
  third_party/cjson/cJSON.c
  third_party/sqlite/sqlite3.c
)

if(WIN32)
  list(APPEND SYSEBA_SOURCES
       src/watcher_windows.c
       src/service_windows.c
       packaging/windows/syseba.rc)
elseif(APPLE)
  list(APPEND SYSEBA_SOURCES src/service_macos.c)
elseif(UNIX)
  list(APPEND SYSEBA_SOURCES src/watcher_linux.c src/service_linux.c)
endif()

add_executable(syseba ${SYSEBA_SOURCES})
add_executable(SySeBa::syseba ALIAS syseba)

if(WIN32)
  set_target_properties(syseba PROPERTIES OUTPUT_NAME "SySeBa")
endif()

target_include_directories(
  syseba
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/civetweb"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite"
    "${SYSEBA_GENERATED_DIR}"
)

target_compile_definitions(
  syseba
  PRIVATE
    SYSEBA_VERSION="${PROJECT_VERSION}"
    NO_SSL
    NO_CGI
    USE_WEBSOCKET=0
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DEFAULT_MEMSTATUS=0
    $<$<BOOL:${SYSEBA_ENABLE_NATIVE_WATCHER}>:SYSEBA_ENABLE_NATIVE_WATCHER=1>
)

if(MSVC)
  target_compile_options(syseba PRIVATE /W4 /utf-8 /permissive- /sdl)
  if(SYSEBA_ENABLE_HARDENING)
    target_compile_options(syseba PRIVATE /guard:cf)
    target_link_options(syseba PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
  endif()
else()
  target_compile_options(
    syseba
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wformat=2
      -Wshadow
      -Wconversion
      -Wstrict-prototypes
      -Wmissing-prototypes
  )
  if(SYSEBA_ENABLE_HARDENING)
    target_compile_options(syseba PRIVATE -fstack-protector-strong)
    if(UNIX AND NOT APPLE)
      target_compile_definitions(syseba PRIVATE _FORTIFY_SOURCE=2)
      target_link_options(syseba PRIVATE -Wl,-z,relro,-z,now)
    elseif(WIN32)
      target_link_options(
        syseba
        PRIVATE
          -static
          -static-libgcc
          -Wl,--dynamicbase,--nxcompat,--high-entropy-va
      )
    endif()
  endif()
endif()

if(WIN32)
  target_compile_definitions(
    syseba
    PRIVATE
      _CRT_SECURE_NO_WARNINGS
      WIN32_LEAN_AND_MEAN
      _WIN32_WINNT=0x0601
  )
  target_link_libraries(
    syseba
    PRIVATE ws2_32 bcrypt advapi32 shell32 psapi winpthread
  )
elseif(APPLE)
  target_link_libraries(syseba PRIVATE pthread dl m)
else()
  target_compile_definitions(syseba PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
  target_link_libraries(syseba PRIVATE pthread dl m)
endif()

if(NOT MSVC)
  set_source_files_properties(
    third_party/civetweb/civetweb.c
    third_party/cjson/cJSON.c
    PROPERTIES COMPILE_OPTIONS "-w"
  )
  # SQLite is an audited vendored dependency. Instrumenting its 9+ MB
  # amalgamation makes sanitizer builds prohibitively expensive while adding
  # little coverage to SySeBa-owned code.
  set_source_files_properties(
    third_party/sqlite/sqlite3.c
    PROPERTIES COMPILE_OPTIONS "-w;-fno-sanitize=all"
  )
endif()

install(
  TARGETS syseba
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)
if(WIN32)
  install(FILES packaging/windows/syseba.conf
          DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/syseba"
          RENAME syseba.conf
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  install(PROGRAMS
          packaging/windows/install-service.ps1
          packaging/windows/uninstall-service.ps1
          DESTINATION "."
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
elseif(APPLE)
  install(FILES packaging/macos/syseba.conf
          DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/syseba"
          RENAME syseba.conf
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  install(PROGRAMS
          packaging/macos/install.sh
          packaging/macos/uninstall.sh
          DESTINATION "."
          PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
  install(FILES packaging/macos/com.okno.syseba.plist
          DESTINATION "share/syseba"
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
else()
  install(FILES packaging/linux/syseba.conf
          DESTINATION "/etc/syseba"
          RENAME syseba.conf
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ)
  install(FILES packaging/linux/syseba.conf
          DESTINATION "${CMAKE_INSTALL_DATADIR}/syseba"
          RENAME syseba.conf.default
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  install(PROGRAMS scripts/syseba-maintenance.sh
          DESTINATION "${CMAKE_INSTALL_SBINDIR}"
          RENAME syseba-maintenance
          PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endif()
install(FILES README.md README.it.md ReadmeAI.md ReadmeAI.en.md LICENSE
        THIRD_PARTY_NOTICES.md CHANGELOG.md SySeBa_Logo.webp
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
install(DIRECTORY docs/
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/technical"
        DIRECTORY_PERMISSIONS
          OWNER_READ OWNER_WRITE OWNER_EXECUTE
          GROUP_READ GROUP_EXECUTE
          WORLD_READ WORLD_EXECUTE
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
        FILES_MATCHING PATTERN "*.md")

if(UNIX AND NOT APPLE)
  install(FILES packaging/linux/syseba.service
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
endif()

if(SYSEBA_BUILD_TESTS AND BUILD_TESTING)
  add_executable(
    syseba_unit_tests
    tests_c/test_native.c
    src/config.c
    src/platform.c
    src/queue.c
    src/fs.c
    third_party/cjson/cJSON.c
  )
  target_include_directories(
    syseba_unit_tests
    PRIVATE include src third_party/cjson
  )
  target_compile_definitions(
    syseba_unit_tests
    PRIVATE
      SYSEBA_VERSION="${PROJECT_VERSION}"
      $<$<BOOL:WIN32>:WIN32_LEAN_AND_MEAN>
      $<$<BOOL:WIN32>:_WIN32_WINNT=0x0601>
  )
  if(WIN32)
    target_link_libraries(syseba_unit_tests PRIVATE bcrypt advapi32 psapi)
  else()
    target_compile_definitions(syseba_unit_tests PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(syseba_unit_tests PRIVATE pthread m)
  endif()
  add_test(NAME native-unit COMMAND syseba_unit_tests)
  if(UNIX AND NOT APPLE)
    add_test(
      NAME linux-integration
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests_c/integration_linux.sh"
                   "$<TARGET_FILE:syseba>"
    )
    set_tests_properties(linux-integration PROPERTIES TIMEOUT 90)
    add_test(
      NAME maintenance-integration
      COMMAND bash
              "${CMAKE_CURRENT_SOURCE_DIR}/tests_c/maintenance_integration.sh"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/syseba-maintenance.sh"
    )
    set_tests_properties(maintenance-integration PROPERTIES TIMEOUT 90)
  endif()
endif()

set(CPACK_PACKAGE_NAME "syseba")
set(CPACK_PACKAGE_VENDOR "okno")
set(CPACK_PACKAGE_CONTACT "okno")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "syseba-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES ON)
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "syseba-${PROJECT_VERSION}-source")
set(CPACK_SOURCE_IGNORE_FILES
    "/\\\\.git/"
    "/build[^/]*/"
    "/dist/"
    "/out/"
    "/__pycache__/"
    "/\\\\.pytest_cache/"
    "\\\\.db(-wal|-shm)?$"
    "\\\\.log$"
    "\\\\.lock$")

if(WIN32)
  set(CPACK_GENERATOR "ZIP;NSIS")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "SySeBa")
  set(CPACK_NSIS_DISPLAY_NAME "SySeBa")
  set(CPACK_NSIS_PACKAGE_NAME "SySeBa")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY "bin")
  set(CPACK_NSIS_MODIFY_PATH ON)
  set(CPACK_NSIS_MUI_ICON
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/SySeBa.ico")
  set(CPACK_NSIS_MUI_UNIICON
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/SySeBa.ico")
elseif(APPLE)
  set(CPACK_GENERATOR "TGZ")
else()
  set(CPACK_GENERATOR "TGZ;DEB;RPM")
  set(CPACK_PACKAGE_RELOCATABLE FALSE)
  set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "okno")
  if(SYSEBA_PORTABLE_LINUX_PACKAGE)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.17)")
  else()
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  endif()
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/preinst"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/conffiles")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_PRE_INSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/preinstall.sh")
  set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinstall.sh")
  set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/preuninstall.sh")
  set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postuninstall.sh")
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
      /usr /usr/bin /usr/sbin /usr/lib /usr/lib/systemd
      /usr/lib/systemd/system /etc /etc/syseba /usr/share /usr/share/doc)
endif()

include(CPack)
