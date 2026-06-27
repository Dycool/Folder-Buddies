cmake_minimum_required(VERSION 3.21)
project(FolderBuddies VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)


if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

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

qt_add_executable(folderbuddies WIN32 MACOSX_BUNDLE ${FB_COMMON_SOURCES})

target_include_directories(folderbuddies PRIVATE ${FB_INCLUDE_DIRS})
target_compile_definitions(folderbuddies PRIVATE _FILE_OFFSET_BITS=64)
target_link_libraries(folderbuddies PRIVATE Qt6::Widgets Qt6::Network Threads::Threads)

if(WIN32)
  get_target_property(_fb_qt_core_type Qt6::Core TYPE)
  if(_fb_qt_core_type STREQUAL "STATIC_LIBRARY")
    foreach(_required_plugin
        Qt6::QWindowsIntegrationPlugin
        Qt6::QModernWindowsStylePlugin)
      if(NOT TARGET ${_required_plugin})
        message(FATAL_ERROR "Static Windows Qt build is missing ${_required_plugin}")
      endif()
    endforeach()

    set(_fb_static_qt_plugins
      Qt6::QWindowsIntegrationPlugin
      Qt6::QModernWindowsStylePlugin)

    foreach(_optional_plugin
        Qt6::QICOPlugin
        Qt6::QGifPlugin
        Qt6::QJpegPlugin
        Qt6::QNLMNIPlugin
        Qt6::QSchannelBackendPlugin
        Qt6::QTlsBackendCertOnlyPlugin
        Qt6::QTuioTouchPlugin)
      if(TARGET ${_optional_plugin})
        list(APPEND _fb_static_qt_plugins ${_optional_plugin})
      endif()
    endforeach()

    list(REMOVE_DUPLICATES _fb_static_qt_plugins)
    qt6_import_plugins(folderbuddies INCLUDE ${_fb_static_qt_plugins})
    message(STATUS "Static Qt plugins: ${_fb_static_qt_plugins}")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(folderbuddies PRIVATE -Wno-stringop-overflow)
endif()

set(FB_SIGNALING_URL "" CACHE STRING "Hardcoded Cloudflare Worker base URL")
if(FB_SIGNALING_URL)
  target_compile_definitions(folderbuddies PRIVATE FB_SIGNALING_URL="${FB_SIGNALING_URL}")
endif()

set(FB_FIREBASE_DATABASE_URL "" CACHE STRING "Hardcoded Firebase Realtime Database URL")
if(FB_FIREBASE_DATABASE_URL)
  target_compile_definitions(folderbuddies PRIVATE FB_FIREBASE_DATABASE_URL="${FB_FIREBASE_DATABASE_URL}")
endif()

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

if(WIN32)
  target_compile_definitions(folderbuddies PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(folderbuddies PRIVATE ws2_32 iphlpapi shell32 advapi32 Rpcrt4 gdi32 user32)
elseif(APPLE)
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

set(FB_ICON_PNG "${CMAKE_SOURCE_DIR}/src/icons/icon.png")
set(FB_ICON_ICNS "${CMAKE_SOURCE_DIR}/src/icons/icon.icns")
set(FB_ICON_ICO "${CMAKE_SOURCE_DIR}/src/icons/icon.ico")

if(APPLE)
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
  configure_file(${CMAKE_SOURCE_DIR}/src/icons/appicon.rc.in ${CMAKE_BINARY_DIR}/appicon.rc @ONLY)
  target_sources(folderbuddies PRIVATE ${CMAKE_BINARY_DIR}/appicon.rc)
endif()

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

  add_executable(fb_signaling_selftest src/net/signaling.cpp src/crypto/crypto.cpp src/crypto/base91.cpp src/crypto/token.cpp)
  target_include_directories(fb_signaling_selftest PRIVATE ${FB_INCLUDE_DIRS})
  target_compile_definitions(fb_signaling_selftest PRIVATE FB_SIGNALING_SELFTEST)
  target_link_libraries(fb_signaling_selftest PRIVATE Qt6::Network Threads::Threads ${FB_SODIUM_TARGET})
  if(WIN32)
    target_link_libraries(fb_signaling_selftest PRIVATE ws2_32)
  endif()
  add_test(NAME signaling_selftest COMMAND fb_signaling_selftest)

  if(WIN32)
    add_executable(fb_projfs_mount_selftest
      src/fs/projfs_mount_selftest.cpp
      src/fs/projfs_mount.cpp
      src/fs/projfs_loader.cpp
      src/fs/fuse_backend.cpp
      src/fs/ram_cache.cpp)
    target_include_directories(fb_projfs_mount_selftest PRIVATE ${FB_INCLUDE_DIRS})
    target_compile_definitions(fb_projfs_mount_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(fb_projfs_mount_selftest PRIVATE
      Qt6::Core Threads::Threads shell32 advapi32 Rpcrt4 user32)
    if(MSVC)
      target_compile_options(fb_projfs_mount_selftest PRIVATE /EHsc)
    endif()
    add_test(NAME projfs_mount_selftest COMMAND fb_projfs_mount_selftest)
    set_tests_properties(projfs_mount_selftest PROPERTIES
      RUN_SERIAL TRUE
      SKIP_RETURN_CODE 77)
  endif()
endif()
