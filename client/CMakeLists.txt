cmake_minimum_required(VERSION 3.21)
project(FolderBuddies VERSION 1.0.0 LANGUAGES CXX)

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
  include/app/mainwindow.h
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
  list(APPEND FB_COMMON_SOURCES src/fs/projfs_mount.cpp src/fs/projfs_loader.cpp)
else()
  list(APPEND FB_COMMON_SOURCES src/fs/fuse_fs.cpp)
endif()

# Use qt_add_executable (not plain add_executable) so the target is finalized and
# the Qt plugins the app needs are imported into the binary. This matters for the
# STATIC Windows build: without the plugins it falls back to a basic style (rough
# text, checkboxes with no tick marks) and loses image-format handlers used for
# icons. Dynamic builds get these as separate DLLs via windeployqt, which is why
# only the static build looked broken.
qt_add_executable(folderbuddies WIN32 MACOSX_BUNDLE ${FB_COMMON_SOURCES})

# For STATIC Qt only: in a static build the Qt plugins are not separate DLLs
# loaded at runtime — every plugin the app needs (platform integration, the
# native Windows style, image-format handlers) must be linked in at build time
# via Q_IMPORT_PLUGIN. qt_add_executable's automatic import covers the platform
# plugin (the app starts) but NOT the native style, which is why the static build
# rendered with the basic Fusion/common style: rough text, checkboxes with no
# tick marks. Rather than guess the style plugin's target name (it varies across
# Qt versions and packagings), import every plugin Qt exposes for the Gui and
# Widgets modules — Qt populates these lists for us. We also print them so the CI
# configure log shows exactly what was imported instead of failing silently.
# Guarded to static Qt so the (working) dynamic Windows build is left untouched.
if(WIN32)
  get_target_property(_fb_qt_core_type Qt6::Core TYPE)
  if(_fb_qt_core_type STREQUAL "STATIC_LIBRARY")
    message(STATUS "Static Qt detected; locating plugin targets to import.")
    # The Qt6Gui_PLUGINS / Qt6Widgets_PLUGINS lists come up empty with vcpkg's
    # static qtbase, so enumerate the imported targets directly instead: every
    # Qt plugin is an imported target whose name ends in "Plugin"
    # (Qt6::QWindowsIntegrationPlugin, Qt6::QModernWindowsStylePlugin,
    # Qt6::QICOPlugin, ...). This is version- and packaging-agnostic. We import
    # all of them so the native style and image-format handlers are linked in,
    # and print the list so the CI configure log is conclusive.
    get_property(_fb_imported_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                 PROPERTY IMPORTED_TARGETS)
    set(_fb_plugin_targets "")
    set(_fb_style_plugins "")
    foreach(_t ${_fb_imported_targets})
      if(_t MATCHES "Plugin$")
        # Skip redundant platform plugins: QWindowsDirect2DIntegrationPlugin
        # redefines all the same symbols as QWindowsIntegrationPlugin, causing
        # LNK2005 multiply-defined errors in a static link. Keep the primary
        # integration plugin (qwindows) but discard the Direct2D variant.
        if(_t MATCHES "Direct2DIntegration")
          continue()
        endif()
        list(APPEND _fb_plugin_targets ${_t})
        if(_t MATCHES "Style")
          list(APPEND _fb_style_plugins ${_t})
        endif()
      endif()
    endforeach()
    # Hard requirement: if there are no plugin targets at all, the static build
    # cannot link in the platform/style/image plugins. Crash instead of silently
    # shipping a broken binary, and dump every imported target so we can see the
    # real names.
    if(NOT _fb_plugin_targets)
      message(FATAL_ERROR
        "No Qt *Plugin imported targets are defined for the static build, so no "
        "plugins (platform, native style, image formats) can be linked in.\n"
        "All imported targets were:\n  ${_fb_imported_targets}")
    endif()
    qt6_import_plugins(folderbuddies INCLUDE ${_fb_plugin_targets})
    message(STATUS "  Imported plugin targets: ${_fb_plugin_targets}")
    # The whole point of this block: guarantee the native Windows style plugin
    # (Qt6::QModernWindowsStylePlugin / Qt6::QWindowsVistaStylePlugin) is one of
    # them. Without it the static build renders with the basic style (rough text,
    # unchecked checkboxes). Refuse to produce that binary.
    if(NOT _fb_style_plugins)
      message(FATAL_ERROR
        "No native Qt style plugin was found among the imported plugin targets, "
        "so the static build would fall back to the basic style (rough text, "
        "unchecked checkboxes). Refusing to produce a broken binary.\n"
        "Plugin targets found were:\n  ${_fb_plugin_targets}")
    endif()
    message(STATUS "  Native style plugin(s): ${_fb_style_plugins}")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(folderbuddies PRIVATE -Wno-stringop-overflow)
endif()

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


# ---- WebRTC compatibility transport (REQUIRED) -----------------------------
# Native↔native runs on TCP; native↔browser uses RTCDataChannel via
# libdatachannel. This is a hard requirement: a build without it silently loses
# webapp interop, so fail loudly instead.
find_package(LibDataChannel CONFIG QUIET)
if(NOT LibDataChannel_FOUND)
  find_package(libdatachannel CONFIG QUIET)
endif()
if(NOT (LibDataChannel_FOUND OR libdatachannel_FOUND))
  message(FATAL_ERROR
    "libdatachannel is required (native<->browser WebRTC bridge) but was not found.\n"
    "  Windows (vcpkg): vcpkg install libdatachannel:<triplet>\n"
    "  macOS/Linux:     build from source (see .github/workflows/build.yml)")
endif()
# The imported target name varies between upstream, vcpkg, and Homebrew
# packagings (datachannel / datachannel-static / LibDataChannel::LibDataChannel).
# Pick the first that actually exists so we never define FB_HAVE_LIBDATACHANNEL
# without an accompanying link (which would fail at the linker).
set(_fb_dc_target "")
foreach(_cand
    LibDataChannel::LibDataChannel
    LibDataChannel::datachannel
    LibDataChannel::datachannel-static
    libdatachannel::libdatachannel
    libdatachannel::datachannel
    datachannel
    datachannel-static)
  if(TARGET ${_cand})
    set(_fb_dc_target ${_cand})
    break()
  endif()
endforeach()
if(NOT _fb_dc_target)
  message(FATAL_ERROR "libdatachannel was found but no known imported target was exposed.")
endif()
target_compile_definitions(folderbuddies PRIVATE FB_HAVE_LIBDATACHANNEL)
target_link_libraries(folderbuddies PRIVATE ${_fb_dc_target})
message(STATUS "WebRTC compatibility: enabled (libdatachannel target ${_fb_dc_target})")

# ---- Mount backend (per platform) -----------------------------------------
if(WIN32)
  # Windows now uses Microsoft's native Projected File System (ProjFS), not WinFsp.
  # ProjectedFSLib.h ships with the Windows SDK. At runtime projfs_loader.cpp loads
  # ProjectedFSLib.dll dynamically via LoadLibrary+GetProcAddress, so the app can
  # start without it. fuse_backend.cpp then enables the optional feature with DISM.
  target_compile_definitions(folderbuddies PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  # gdi32/user32: GetDIBits/LoadImageW etc. used to load the embedded .ico for the
  # window icon (see src/app/main.cpp).
  target_link_libraries(folderbuddies PRIVATE ws2_32 iphlpapi shell32 advapi32 Rpcrt4 gdi32 user32)
elseif(APPLE)
  # FUSE-T/macFUSE installers do not always expose a stable fuse3.pc on macOS.
  # Prefer pkg-config when present, but fall back to the well-known installer
  # locations so CI and clean user Macs do not fail at configure time.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FUSE QUIET IMPORTED_TARGET fuse3)
  endif()

  if(FUSE_FOUND)
    target_link_libraries(folderbuddies PRIVATE PkgConfig::FUSE)
    message(STATUS "macOS FUSE3: using pkg-config module fuse3")
  else()
    set(FB_MACOS_FUSE_HINTS
      "$ENV{HOMEBREW_PREFIX}"
      /opt/homebrew
      /usr/local
      "/Library/Application Support/fuse-t"
      /Library/Filesystems/fuse-t.fs/Contents/Resources
      /Library/Filesystems/macfuse.fs/Contents/Resources
      /Library/Frameworks/fuse_t.framework/Versions/Current
      /Library/Frameworks/fuse_t.framework/Versions/A
      /Library/Frameworks/macFUSE.framework/Versions/Current
      /Library/Frameworks/macFUSE.framework/Versions/A
    )
    find_path(FB_FUSE3_INCLUDE_DIR
      NAMES fuse.h
      HINTS ${FB_MACOS_FUSE_HINTS}
      PATH_SUFFIXES include/fuse3 include Headers
    )
    find_library(FB_FUSE3_LIBRARY
      NAMES fuse3 fuse3.4
      HINTS ${FB_MACOS_FUSE_HINTS}
      PATH_SUFFIXES lib Resources/lib Resources
    )

    if(NOT FB_FUSE3_INCLUDE_DIR OR NOT FB_FUSE3_LIBRARY)
      message(FATAL_ERROR
        "FUSE3 was not found on macOS. Install FUSE-T, then reconfigure from a clean build directory:\n"
        "  brew install --cask fuse-t\n"
        "Looked for fuse.h and libfuse3/libfuse3.4 in Homebrew, FUSE-T, and macFUSE locations.")
    endif()

    target_include_directories(folderbuddies PRIVATE ${FB_FUSE3_INCLUDE_DIR})
    target_link_libraries(folderbuddies PRIVATE ${FB_FUSE3_LIBRARY})
    message(STATUS "macOS FUSE3: include=${FB_FUSE3_INCLUDE_DIR}")
    message(STATUS "macOS FUSE3: library=${FB_FUSE3_LIBRARY}")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FUSE REQUIRED IMPORTED_TARGET fuse3)
  target_link_libraries(folderbuddies PRIVATE PkgConfig::FUSE)
endif()

# ---- Code signing (macOS) ---------------------------------------------------
if(APPLE)
  set(FB_CODESIGN_IDENTITY "" CACHE STRING "Developer ID Application identity for signing the .app")
  if(FB_CODESIGN_IDENTITY)
    add_custom_command(TARGET folderbuddies POST_BUILD
      COMMAND codesign --force --deep --options runtime
              --entitlements "${CMAKE_SOURCE_DIR}/src/macos/folderbuddies.entitlements"
              --sign "${FB_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:folderbuddies>"
      COMMENT "Codesigning Folder Buddies.app")
  endif()
endif()

# ---- App icon (all platforms) ---------------------------------------------
# src/icons/icon.png is the single canonical source image. The generated per-OS files in
# src/icons are derived from it by src/icons/make-icons.sh.
set(FB_ICON_PNG "${CMAKE_SOURCE_DIR}/src/icons/icon.png")
set(FB_ICON_ICNS "${CMAKE_SOURCE_DIR}/src/icons/icon.icns")
set(FB_ICON_ICO "${CMAKE_SOURCE_DIR}/src/icons/icon.ico")

if(APPLE)
  # A custom Info.plist carries the TCC folder usage-description strings macOS
  # needs to prompt for (rather than silently deny) access to the user's
  # Desktop/Documents/Downloads/external volumes when hosting or mounting.
  set_target_properties(folderbuddies PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Folder Buddies"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.folderbuddies.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/macos/Info.plist.in")
  if(EXISTS "${FB_ICON_ICNS}")
    set_source_files_properties("${FB_ICON_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(folderbuddies PRIVATE "${FB_ICON_ICNS}")
    set_target_properties(folderbuddies PROPERTIES MACOSX_BUNDLE_ICON_FILE icon.icns)
  endif()
elseif(WIN32 AND EXISTS "${FB_ICON_ICO}")
  # Ensure the resource file is properly configured and embedded
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
    message(FATAL_ERROR
      "miniupnpc is required (UPnP port mapping) but was not found.\n"
      "  Windows (vcpkg): vcpkg install miniupnpc:<triplet>\n"
      "  macOS:           brew install miniupnpc\n"
      "  Linux:           apt-get install libminiupnpc-dev")
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
install(FILES src/dev.folderbuddies.app.metainfo.xml DESTINATION share/metainfo)

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
