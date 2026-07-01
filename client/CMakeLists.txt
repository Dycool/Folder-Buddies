cmake_minimum_required(VERSION 3.21)
project(FolderBuddies VERSION 1.0.0 LANGUAGES C CXX)

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
  src/net/stream.cpp
  src/net/native_quic.cpp
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

# Native-to-native direct transport: libjuice performs ICE/STUN without TURN,
# while the native C picoquic stack supplies QUIC packets carried over the
# ICE-selected path. No Rust toolchain is involved.
option(FB_NATIVE_QUIC "Build native ICE/STUN + QUIC transport" ON)
if(FB_NATIVE_QUIC)
  include(FetchContent)
  find_package(OpenSSL REQUIRED)
  if(WIN32 AND NOT PKG_CONFIG_EXECUTABLE)
    # Picotls uses FindPkgConfig even when OpenSSL came from vcpkg. pkgconf is
    # installed as a vcpkg host tool and is not necessarily added to PATH.
    find_program(PKG_CONFIG_EXECUTABLE
      NAMES pkgconf pkg-config
      HINTS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/pkgconf"
        "${VCPKG_INSTALLED_DIR}/x64-windows/tools/pkgconf"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/pkgconf")
  endif()
  find_package(LibJuice CONFIG QUIET)
  if(NOT TARGET LibJuice::LibJuiceStatic AND NOT TARGET LibJuice::LibJuice)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libjuice
      GIT_REPOSITORY https://github.com/paullouisageneau/libjuice.git
      GIT_TAG v1.7.2
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libjuice)
  endif()
  if(TARGET LibJuice::LibJuiceStatic)
    set(_fb_juice_target LibJuice::LibJuiceStatic)
  elseif(TARGET LibJuice::LibJuice)
    set(_fb_juice_target LibJuice::LibJuice)
  else()
    message(FATAL_ERROR "libjuice target was not available")
  endif()

  # Disable picoquic's applications, log tools, tests, and optional assembly
  # engine. We only need its core C library with picotls/OpenSSL.
  set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
  set(BUILD_HTTP OFF CACHE BOOL "" FORCE)
  set(BUILD_PQBENCH OFF CACHE BOOL "" FORCE)
  set(BUILD_LOGLIB OFF CACHE BOOL "" FORCE)
  set(BUILD_LOGREADER OFF CACHE BOOL "" FORCE)
  set(BUILD_PICO_SIM OFF CACHE BOOL "" FORCE)
  set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "" FORCE)
  set(WITH_OPENSSL ON CACHE BOOL "" FORCE)
  set(WITH_FUSION OFF CACHE BOOL "" FORCE)
  set(WITH_AEGIS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(picoquic
    GIT_REPOSITORY https://github.com/private-octopus/picoquic.git
    GIT_TAG d3a80307200d28c53a6470d257bdd0801fad7971)
  FetchContent_MakeAvailable(picoquic)
  FetchContent_GetProperties(picotls)
  if(WIN32 AND picotls_SOURCE_DIR)
    # Upstream picotls' CMake omits its MSVC compatibility header directory.
    foreach(_ptls_lib picotls-core picotls-minicrypto picotls-openssl)
      if(TARGET ${_ptls_lib})
        target_include_directories(${_ptls_lib} PRIVATE
          "${picotls_SOURCE_DIR}/picotlsvs/picotls")
      endif()
    endforeach()
    target_sources(picotls-core PRIVATE
      "${picotls_SOURCE_DIR}/picotlsvs/picotls/wintimeofday.c")
  endif()
  # Picotls declares its standalone CLI/tests unconditionally. Keep them out
  # of the application build; picoquic-core only needs the three libraries.
  foreach(_ptls_tool cli test-minicrypto.t test-openssl.t test-fusion.t ptlsbench)
    if(TARGET ${_ptls_tool})
      set_target_properties(${_ptls_tool} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
  endforeach()
  # Picotls keeps several assertion-only result variables in its release C
  # sources. NDEBUG removes their readers, producing harmless warning noise.
  # Keep our own warnings intact and silence only those two upstream classes.
  foreach(_ptls_lib picotls-core picotls-minicrypto picotls-openssl)
    if(TARGET ${_ptls_lib})
      if(MSVC)
        # Picotls unconditionally adds Unix flags (-std=c99, -g, -Wall) and
        # enables thousands of diagnostics in its own C/OpenSSL headers under
        # MSVC. Treat the fetched dependency as external; warnings in Folder
        # Buddies targets remain enabled.
        target_compile_options(${_ptls_lib} PRIVATE /w /wd9002)
      elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${_ptls_lib} PRIVATE
          -Wno-unused-variable
          -Wno-unused-but-set-variable)
      endif()
    endif()
  endforeach()
  target_link_libraries(folderbuddies PRIVATE
    ${_fb_juice_target} picoquic::picoquic-core OpenSSL::Crypto)
  target_compile_definitions(folderbuddies PRIVATE FB_HAVE_NATIVE_QUIC)
  message(STATUS "Native transport: ICE/STUN + picoquic (C, no TURN, no Rust)")
endif()

if(WIN32)
  target_compile_definitions(folderbuddies PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WINDOWS)
  target_link_libraries(folderbuddies PRIVATE ws2_32 iphlpapi shell32 advapi32
    bcrypt Rpcrt4 gdi32 user32)
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

  # End-to-end protocol test: real Server + Client over loopback TCP with the
  # encrypted framing. Covers filesystem ops, path traversal rejection,
  # read-only enforcement, malformed payloads, and invalidation broadcast.
  add_executable(fb_server_selftest
    src/server/server_selftest.cpp
    src/server/server.cpp
    src/clients/client.cpp
    src/net/stream.cpp
    src/crypto/crypto.cpp
    src/crypto/base91.cpp
    src/crypto/token.cpp)
  target_include_directories(fb_server_selftest PRIVATE ${FB_INCLUDE_DIRS})
  target_link_libraries(fb_server_selftest PRIVATE Qt6::Core Threads::Threads)
  if(WIN32)
    target_compile_definitions(fb_server_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(fb_server_selftest PRIVATE ws2_32)
  endif()
  add_test(NAME server_selftest COMMAND fb_server_selftest)
  set_tests_properties(server_selftest PROPERTIES TIMEOUT 120)

  # RamCache behavior against a fake in-memory remote: block-boundary reads,
  # metadata/directory/negative caching, and write-through invalidation.
  add_executable(fb_cache_selftest
    src/fs/ram_cache_selftest.cpp
    src/fs/ram_cache.cpp
    src/clients/client.cpp
    src/net/stream.cpp
    src/crypto/crypto.cpp
    src/crypto/base91.cpp
    src/crypto/token.cpp)
  target_include_directories(fb_cache_selftest PRIVATE ${FB_INCLUDE_DIRS})
  target_link_libraries(fb_cache_selftest PRIVATE Qt6::Core Threads::Threads)
  if(WIN32)
    target_compile_definitions(fb_cache_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(fb_cache_selftest PRIVATE ws2_32)
  endif()
  add_test(NAME cache_selftest COMMAND fb_cache_selftest)
  set_tests_properties(cache_selftest PROPERTIES TIMEOUT 60)

  if(FB_NATIVE_QUIC)
    add_executable(fb_native_quic_selftest
      src/net/native_quic_selftest.cpp
      src/net/native_quic.cpp
      src/net/stream.cpp)
    target_include_directories(fb_native_quic_selftest PRIVATE ${FB_INCLUDE_DIRS})
    target_compile_definitions(fb_native_quic_selftest PRIVATE FB_HAVE_NATIVE_QUIC)
    target_link_libraries(fb_native_quic_selftest PRIVATE
      Qt6::Core Threads::Threads ${_fb_juice_target}
      picoquic::picoquic-core OpenSSL::Crypto)
    if(WIN32)
      target_compile_definitions(fb_native_quic_selftest PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WINDOWS)
      target_link_libraries(fb_native_quic_selftest PRIVATE ws2_32 bcrypt)
    endif()
    add_test(NAME native_quic_selftest COMMAND fb_native_quic_selftest)
    set_tests_properties(native_quic_selftest PROPERTIES TIMEOUT 45)
  endif()

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
