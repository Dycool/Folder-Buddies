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

set(FB_COMMON_SOURCES
  src/main.cpp
  src/mainwindow.cpp
  src/token.cpp
  src/base91.cpp
  src/crypto.cpp
  src/netutil.cpp
  src/signaling.cpp
  src/session.cpp
  src/cli.cpp
  src/server.cpp
  src/client.cpp
  src/upnp.cpp
  src/fuse_backend.cpp
  resources.qrc
)

if(WIN32)
  list(APPEND FB_COMMON_SOURCES src/projfs_mount.cpp)
else()
  list(APPEND FB_COMMON_SOURCES src/fuse_fs.cpp)
endif()

add_executable(folderbuddies WIN32 MACOSX_BUNDLE ${FB_COMMON_SOURCES})

target_include_directories(folderbuddies PRIVATE include)
target_compile_definitions(folderbuddies PRIVATE _FILE_OFFSET_BITS=64)
target_link_libraries(folderbuddies PRIVATE Qt6::Widgets Qt6::Network Threads::Threads)

# Hardcode your Worker endpoint at build time, e.g.
#   -DFB_SIGNALING_URL="https://folderbuddies-signaling.<subdomain>.workers.dev"
set(FB_SIGNALING_URL "" CACHE STRING "Hardcoded Cloudflare Worker base URL")
if(FB_SIGNALING_URL)
  target_compile_definitions(folderbuddies PRIVATE FB_SIGNALING_URL="${FB_SIGNALING_URL}")
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
# /icon.png is the single canonical source image. The generated per-OS files in
# /branding are derived from it by branding/make-icons.sh.
set(FB_ICON_PNG "${CMAKE_SOURCE_DIR}/icon.png")
set(FB_ICON_ICNS "${CMAKE_SOURCE_DIR}/branding/icon.icns")
set(FB_ICON_ICO "${CMAKE_SOURCE_DIR}/branding/icon.ico")

if(APPLE AND EXISTS "${FB_ICON_ICNS}")
  set_source_files_properties("${FB_ICON_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(folderbuddies PRIVATE "${FB_ICON_ICNS}")
  set_target_properties(folderbuddies PROPERTIES
    MACOSX_BUNDLE_ICON_FILE icon.icns
    MACOSX_BUNDLE_BUNDLE_NAME "Folder Buddies"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.folderbuddies.app")
elseif(WIN32 AND EXISTS "${FB_ICON_ICO}")
  configure_file(${CMAKE_SOURCE_DIR}/src/appicon.rc.in ${CMAKE_BINARY_DIR}/appicon.rc @ONLY)
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
install(FILES icon.png DESTINATION share/icons/hicolor/1024x1024/apps RENAME folderbuddies.png)
install(FILES packaging/folderbuddies.desktop DESTINATION share/applications)

# ---- Crypto/Base91 self-test (no external deps; runs in CI) ---------------
option(FB_BUILD_TESTS "Build the crypto self-test" ON)
if(FB_BUILD_TESTS)
  enable_testing()
  add_executable(fb_crypto_selftest src/crypto.cpp src/base91.cpp)
  target_include_directories(fb_crypto_selftest PRIVATE include)
  target_compile_definitions(fb_crypto_selftest PRIVATE FB_CRYPTO_SELFTEST)
  if(WIN32)
    target_link_libraries(fb_crypto_selftest PRIVATE ws2_32)
  endif()
  add_test(NAME crypto_selftest COMMAND fb_crypto_selftest)

  # Round-trips the share-code sealing (offline blob + Cloudflare record).
  add_executable(fb_signaling_selftest src/signaling.cpp src/crypto.cpp src/base91.cpp src/token.cpp)
  target_include_directories(fb_signaling_selftest PRIVATE include)
  target_compile_definitions(fb_signaling_selftest PRIVATE FB_SIGNALING_SELFTEST)
  target_link_libraries(fb_signaling_selftest PRIVATE Qt6::Network Threads::Threads ${FB_SODIUM_TARGET})
  if(WIN32)
    target_link_libraries(fb_signaling_selftest PRIVATE ws2_32)
  endif()
  add_test(NAME signaling_selftest COMMAND fb_signaling_selftest)
endif()
