cmake_minimum_required(VERSION 3.21)
project(FolderBuddies LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

# Headers live in role-based subfolders mirroring src/. Sources include them by
# bare name (e.g. #include "common.h"), so every subfolder is on the search path.
set(FB_INCLUDE_DIRS
  include
  include/app
  include/server
  include/clients
  include/net
  include/fs
  include/crypto
)

set(FB_COMMON_SOURCES
  src/app/main.cpp
  src/app/mainwindow.cpp
  src/app/session.cpp
  src/app/cli.cpp
  src/crypto/token.cpp
  src/crypto/base91.cpp
  src/crypto/crypto.cpp
  src/net/netutil.cpp
  src/net/signaling.cpp
  src/net/upnp.cpp
  src/server/server.cpp
  src/clients/client.cpp
  src/clients/web_compat.cpp
  src/fs/fuse_backend.cpp
  src/fs/ram_cache.cpp
  resources.qrc
)

if(WIN32)
  list(APPEND FB_COMMON_SOURCES src/fs/projfs_mount.cpp)
else()
  list(APPEND FB_COMMON_SOURCES src/fs/fuse_fs.cpp)
endif()

add_executable(folderbuddies WIN32 MACOSX_BUNDLE ${FB_COMMON_SOURCES})

target_include_directories(folderbuddies PRIVATE ${FB_INCLUDE_DIRS})
target_compile_definitions(folderbuddies PRIVATE _FILE_OFFSET_BITS=64)
target_link_libraries(folderbuddies PRIVATE Qt6::Widgets Qt6::Network Threads::Threads)

# Hardcode your Worker endpoint at build time, e.g.
#   -DFB_SIGNALING_URL="https://folderbuddies-signaling.<subdomain>.workers.dev"
set(FB_SIGNALING_URL "" CACHE STRING "Hardcoded Cloudflare Worker base URL")
if(FB_SIGNALING_URL)
  target_compile_definitions(folderbuddies PRIVATE FB_SIGNALING_URL="${FB_SIGNALING_URL}")
endif()

# Optional native Firebase Realtime Database fallback. Runtime env
# FOLDERBUDDIES_FIREBASE_DATABASE_URL overrides this build-time value.
set(FB_FIREBASE_DATABASE_URL "" CACHE STRING "Hardcoded Firebase Realtime Database URL")
if(FB_FIREBASE_DATABASE_URL)
  target_compile_definitions(folderbuddies PRIVATE FB_FIREBASE_DATABASE_URL="${FB_FIREBASE_DATABASE_URL}")
endif()


# ---- WebRTC compatibility transport (optional) -----------------------------
# Keeps native↔native on TCP while allowing native↔browser fallback via
# RTCDataChannel when libdatachannel is available.
find_package(LibDataChannel CONFIG QUIET)
if(NOT LibDataChannel_FOUND)
  find_package(libdatachannel CONFIG QUIET)
endif()
if(LibDataChannel_FOUND OR libdatachannel_FOUND)
  target_compile_definitions(folderbuddies PRIVATE FB_HAVE_LIBDATACHANNEL)
  if(TARGET LibDataChannel::datachannel)
    target_link_libraries(folderbuddies PRIVATE LibDataChannel::datachannel)
  elseif(TARGET datachannel)
    target_link_libraries(folderbuddies PRIVATE datachannel)
  elseif(TARGET libdatachannel::datachannel)
    target_link_libraries(folderbuddies PRIVATE libdatachannel::datachannel)
  endif()
  message(STATUS "WebRTC compatibility: enabled (libdatachannel)")
else()
  message(STATUS "WebRTC compatibility: disabled (libdatachannel not found)")
endif()

# ---- Mount backend (per platform) -----------------------------------------
if(WIN32)
  # Windows now uses Microsoft's native Projected File System (ProjFS), not WinFsp.
  # ProjectedFSLib.h / ProjectedFSLib.lib ship with the Windows SDK. At runtime
  # fuse_backend.cpp enables the Client-ProjFS optional feature with UAC + DISM
  # if ProjectedFSLib.dll is not available yet.
  target_link_libraries(folderbuddies PRIVATE ProjectedFSLib ws2_32 iphlpapi shell32 advapi32 Rpcrt4)
else()
  find_package(PkgConfig REQUIRED)
  if(APPLE)
    # Prefer FUSE-T if installed; fall back to other FUSE pkg-config names so
    # developers can still build while packaging the FUSE-T installer separately.
    pkg_check_modules(FUSE fuse-t)
    if(NOT FUSE_FOUND)
      pkg_check_modules(FUSE fuse3)
    endif()
    if(NOT FUSE_FOUND)
      pkg_check_modules(FUSE REQUIRED fuse)
    endif()
  else()
    pkg_check_modules(FUSE REQUIRED fuse3)
  endif()
  target_include_directories(folderbuddies PRIVATE ${FUSE_INCLUDE_DIRS})
  target_link_libraries(folderbuddies PRIVATE ${FUSE_LIBRARIES})
  target_compile_options(folderbuddies PRIVATE ${FUSE_CFLAGS_OTHER})
endif()

# ---- Bundle the macOS FUSE-T backend installer inside the .app ------------
if(APPLE)
  set(FUSET_PKG "" CACHE FILEPATH "Path to a FUSE-T installer (.pkg/.dmg) to bundle")
  set(MACFUSE_PKG "" CACHE FILEPATH "Optional fallback macFUSE installer (.pkg/.dmg) to bundle")
  set(_pkg "")
  if(FUSET_PKG AND EXISTS "${FUSET_PKG}")
    set(_pkg "${FUSET_PKG}")
  elseif(MACFUSE_PKG AND EXISTS "${MACFUSE_PKG}")
    set(_pkg "${MACFUSE_PKG}")
  endif()
  if(_pkg)
    set_source_files_properties("${_pkg}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(folderbuddies PRIVATE "${_pkg}")
    message(STATUS "FUSE backend installer bundled in the .app: ${_pkg}")
  else()
    message(STATUS "No bundled FUSE-T installer supplied; use -DFUSET_PKG=/path/to/FUSE-T.pkg for release packaging")
  endif()

  set(FB_CODESIGN_IDENTITY "" CACHE STRING "Developer ID Application identity for signing the .app")
  if(FB_CODESIGN_IDENTITY)
    add_custom_command(TARGET folderbuddies POST_BUILD
      COMMAND codesign --force --deep --options runtime --sign "${FB_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:folderbuddies>"
      COMMENT "Codesigning Folder Buddies.app")
  endif()
endif()

# ---- App icon (all platforms) ---------------------------------------------
# src/icons/icon.png is the single canonical source image. The generated per-OS files in
# src/icons are derived from it by src/icons/make-icons.sh.
set(FB_ICON_PNG "${CMAKE_SOURCE_DIR}/src/icons/icon.png")
set(FB_ICON_ICNS "${CMAKE_SOURCE_DIR}/src/icons/icon.icns")
set(FB_ICON_ICO "${CMAKE_SOURCE_DIR}/src/icons/icon.ico")

if(APPLE AND EXISTS "${FB_ICON_ICNS}")
  set_source_files_properties("${FB_ICON_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(folderbuddies PRIVATE "${FB_ICON_ICNS}")
  set_target_properties(folderbuddies PROPERTIES
    MACOSX_BUNDLE_ICON_FILE icon.icns
    MACOSX_BUNDLE_BUNDLE_NAME "Folder Buddies"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.folderbuddies.app")
elseif(WIN32 AND EXISTS "${FB_ICON_ICO}")
  configure_file(${CMAKE_SOURCE_DIR}/src/icons/appicon.rc.in ${CMAKE_BINARY_DIR}/appicon.rc @ONLY)
  target_sources(folderbuddies PRIVATE ${CMAKE_BINARY_DIR}/appicon.rc)
endif()

# ---- UPnP (optional) ------------------------------------------------------
find_package(miniupnpc CONFIG QUIET)
if(miniupnpc_FOUND)
  target_link_libraries(folderbuddies PRIVATE miniupnpc::miniupnpc)
  target_compile_definitions(folderbuddies PRIVATE HAVE_MINIUPNPC)
  message(STATUS "UPnP support: enabled (CMake package)")
else()
  find_path(MINIUPNPC_INC miniupnpc/miniupnpc.h)
  find_library(MINIUPNPC_LIB NAMES miniupnpc)
  if(MINIUPNPC_INC AND MINIUPNPC_LIB)
    target_include_directories(folderbuddies PRIVATE ${MINIUPNPC_INC})
    target_link_libraries(folderbuddies PRIVATE ${MINIUPNPC_LIB})
    target_compile_definitions(folderbuddies PRIVATE HAVE_MINIUPNPC)
    if(WIN32)
      target_link_libraries(folderbuddies PRIVATE iphlpapi)
    endif()
    message(STATUS "UPnP support: enabled")
  else()
    message(STATUS "UPnP support: disabled (miniupnpc not found)")
  endif()
endif()

# ---- libsodium (Argon2id hardening for the short share code) --------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SODIUM IMPORTED_TARGET libsodium)
endif()
if(SODIUM_FOUND)
  set(FB_SODIUM_TARGET PkgConfig::SODIUM)
else()
  find_path(SODIUM_INCLUDE_DIR sodium.h)
  find_library(SODIUM_LIBRARY NAMES sodium libsodium)
  if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
    message(FATAL_ERROR "libsodium not found (needed for Argon2id). Install libsodium-dev, "
                        "brew install libsodium, or vcpkg install libsodium.")
  endif()
  add_library(fb_sodium INTERFACE)
  target_include_directories(fb_sodium INTERFACE ${SODIUM_INCLUDE_DIR})
  target_link_libraries(fb_sodium INTERFACE ${SODIUM_LIBRARY})
  set(FB_SODIUM_TARGET fb_sodium)
endif()
target_link_libraries(folderbuddies PRIVATE ${FB_SODIUM_TARGET})

install(TARGETS folderbuddies RUNTIME DESTINATION bin BUNDLE DESTINATION .)
install(FILES src/icons/icon.png DESTINATION share/icons/hicolor/1024x1024/apps RENAME folderbuddies.png)
install(FILES src/folderbuddies.desktop DESTINATION share/applications)

# ---- Crypto/Base91 self-test (no external deps; runs in CI) ---------------
option(FB_BUILD_TESTS "Build the crypto self-test" ON)
if(FB_BUILD_TESTS)
  enable_testing()
  add_executable(fb_crypto_selftest src/crypto/crypto.cpp src/crypto/base91.cpp)
  target_include_directories(fb_crypto_selftest PRIVATE ${FB_INCLUDE_DIRS})
  target_compile_definitions(fb_crypto_selftest PRIVATE FB_CRYPTO_SELFTEST)
  if(WIN32)
    target_link_libraries(fb_crypto_selftest PRIVATE ws2_32)
  endif()
  add_test(NAME crypto_selftest COMMAND fb_crypto_selftest)

  # Round-trips the share-code sealing (offline blob + Cloudflare record).
  add_executable(fb_signaling_selftest src/net/signaling.cpp src/crypto/crypto.cpp src/crypto/base91.cpp src/crypto/token.cpp)
  target_include_directories(fb_signaling_selftest PRIVATE ${FB_INCLUDE_DIRS})
  target_compile_definitions(fb_signaling_selftest PRIVATE FB_SIGNALING_SELFTEST)
  target_link_libraries(fb_signaling_selftest PRIVATE Qt6::Network Threads::Threads ${FB_SODIUM_TARGET})
  if(WIN32)
    target_link_libraries(fb_signaling_selftest PRIVATE ws2_32)
  endif()
  add_test(NAME signaling_selftest COMMAND fb_signaling_selftest)
endif()
