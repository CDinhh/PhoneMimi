cmake_minimum_required(VERSION 3.20)
project(PhoneMimi VERSION 3.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(ENABLE_AIRPLAY "Build AirPlay receiver module" ON)
option(ENABLE_MIRACAST "Build Miracast receiver module (Windows only)" ON)
option(ENABLE_CAST "Build Google Cast receiver module" ON)
option(ENABLE_ANDROID "Build Android (scrcpy-based) screen mirror receiver" ON)
option(ENABLE_MCP "Build the MCP server (Claude Code control)" ON)
option(ENABLE_WEBCAM "Build webcam drawer (Media Foundation, Windows only)" ON)

# --- Dependencies via vcpkg or find_package ---
find_package(PkgConfig QUIET)

# FFmpeg
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)
find_library(SWRESAMPLE_LIBRARY swresample)
find_library(AVFILTER_LIBRARY avfilter)

# SDL2
find_package(SDL2 CONFIG REQUIRED)

# OpenSSL (for AirPlay crypto)
find_package(OpenSSL REQUIRED)

# DNS-SD / Bonjour (for AirPlay mDNS)
# mDNS: Built-in multicast responder — no Bonjour SDK needed.
# On Windows, uses winsock2 multicast (ws2_32 + iphlpapi).
# On Linux, you can optionally link avahi-compat-libdns_sd.

# --- Source files ---
set(MEDIA_SOURCES
    src/media/decoder.cpp
    src/media/renderer.cpp
    src/media/audio_output.cpp
    src/media/phone_frame.cpp
    src/media/recorder.cpp
    src/media/clip_player.cpp
)

if(WIN32)
    list(APPEND MEDIA_SOURCES src/media/ocr.cpp)
endif()

# Webcam drawer (always compiled when the option is on so the header
# resolves; webcam.cpp itself has an #else branch that stubs out the
# API for non-Windows / disabled builds).
list(APPEND MEDIA_SOURCES src/media/webcam.cpp)

set(NETWORK_SOURCES
    src/network/tcp_server.cpp
    src/network/rtsp_server.cpp
    src/network/update_check.cpp
)

set(CORE_SOURCES
    src/main.cpp
    src/app.cpp
    src/settings.cpp
    src/usage_log.cpp
    ${MEDIA_SOURCES}
    ${NETWORK_SOURCES}
    lib/stb_image_write_impl.c
    lib/stb_image_impl.c
    lib/qrcodegen/qrcodegen.cpp
)

if(ENABLE_AIRPLAY)
    list(APPEND CORE_SOURCES
        src/airplay/airplay_server.cpp
        src/airplay/mdns_service.cpp
        src/airplay/airplay_mirror.cpp
        src/airplay/pairing.cpp
        src/airplay/srp_pin.cpp
        src/airplay/bplist.cpp
        src/airplay/mirror_buffer.cpp
        lib/playfair/playfair.c
        lib/playfair/hand_garble.c
        lib/playfair/modified_md5.c
        lib/playfair/omg_hax.c
        lib/playfair/sap_hash.c
    )
endif()

if(ENABLE_MIRACAST AND WIN32)
    list(APPEND CORE_SOURCES
        src/miracast/miracast_receiver.cpp
    )
endif()

if(ENABLE_CAST)
    list(APPEND CORE_SOURCES
        src/cast/cast_receiver.cpp
    )
endif()

if(ENABLE_MCP)
    list(APPEND CORE_SOURCES
        src/mcp/mcp_protocol.cpp
        src/mcp/mcp_util.cpp
        src/mcp/frame_tap.cpp
        src/mcp/mcp_tools.cpp
        src/mcp/mcp_server.cpp
        src/mcp/mcp_self_test.cpp
        src/app_mcp.cpp
    )
endif()

if(ENABLE_ANDROID)
    list(APPEND CORE_SOURCES
        src/android/scrcpy_receiver.cpp
        src/android/scrcpy_util.cpp
        src/android/setup_hint.cpp
        src/android/android_dialog.cpp
    )
endif()

# --- Target ---
add_executable(PhoneMimi ${CORE_SOURCES})

target_include_directories(PhoneMimi PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib
    ${AVCODEC_INCLUDE_DIR}
)

# Derive C++ version macros from the single source of truth (project()).
# Keeps OPM_VERSION_* in sync with package version automatically.
target_compile_definitions(PhoneMimi PRIVATE
    OPM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    OPM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    OPM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_link_libraries(PhoneMimi PRIVATE
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVFILTER_LIBRARY}
    SDL2::SDL2
    SDL2::SDL2main
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(ENABLE_AIRPLAY)
    target_compile_definitions(PhoneMimi PRIVATE ENABLE_AIRPLAY=1)
    if(WIN32)
        target_link_libraries(PhoneMimi PRIVATE iphlpapi)
    endif()
endif()

if(ENABLE_MIRACAST AND WIN32)
    target_compile_definitions(PhoneMimi PRIVATE ENABLE_MIRACAST=1)
    # Only needs WinRT for WiFi Direct (no D3D11/DXGI — uses FFmpeg for decode)
    target_link_libraries(PhoneMimi PRIVATE
        windowsapp.lib
        RuntimeObject.lib
    )
endif()

if(ENABLE_CAST)
    target_compile_definitions(PhoneMimi PRIVATE ENABLE_CAST=1)
    target_include_directories(PhoneMimi PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

if(ENABLE_MCP)
    target_compile_definitions(PhoneMimi PRIVATE ENABLE_MCP=1)
endif()
if(ENABLE_ANDROID)
    target_compile_definitions(PhoneMimi PRIVATE ENABLE_ANDROID=1)
endif()

if(ENABLE_WEBCAM AND WIN32)
    target_compile_definitions(PhoneMimi PRIVATE ENABLE_WEBCAM=1)
    # Media Foundation libs are part of the Windows SDK — no vcpkg dep.
    target_link_libraries(PhoneMimi PRIVATE mf mfplat mfreadwrite mfuuid shlwapi)
endif()

if(WIN32)
    target_link_libraries(PhoneMimi PRIVATE ws2_32 winhttp)
    # WindowsApp.lib provides the C++/WinRT runtime exports used by the
    # OCR copy feature (Windows.Media.Ocr). Linked unconditionally on
    # Windows so it's available even when ENABLE_MIRACAST is OFF.
    target_link_libraries(PhoneMimi PRIVATE windowsapp.lib)
    target_compile_definitions(PhoneMimi PRIVATE NOMINMAX)
    # UI text is UTF-8 end to end, including narrow literals with \u escapes.
    target_compile_options(PhoneMimi PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    # Embed app manifest for DPI awareness (disable auto-generated manifest to avoid duplicate)
    target_sources(PhoneMimi PRIVATE resources/phonemimi.rc)
    set_target_properties(PhoneMimi PROPERTIES LINK_FLAGS "/MANIFEST:NO /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

# --- Install ---
install(TARGETS PhoneMimi RUNTIME DESTINATION bin)

# Copy icon PNGs to build output directory
add_custom_command(TARGET PhoneMimi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/apple_icon.png"
        "$<TARGET_FILE_DIR:PhoneMimi>/apple_icon.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/android_icon.png"
        "$<TARGET_FILE_DIR:PhoneMimi>/android_icon.png"
)

# Stage the vendored runtime helpers (adb + scrcpy-server.jar) next to
# the EXE on every build. src/app.cpp looks for tools/adb/adb.exe and
# tools/scrcpy-server.jar relative to the executable, and package.ps1
# harvests the whole build/<Config> folder into the MSI, so this single
# copy keeps both `cmake --build` and `package.ps1` correct.
if (ENABLE_ANDROID AND EXISTS "${CMAKE_SOURCE_DIR}/tools")
    add_custom_command(TARGET PhoneMimi POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/tools"
            "$<TARGET_FILE_DIR:PhoneMimi>/tools"
        COMMENT "Staging tools/ (adb, scrcpy-server.jar) into $<TARGET_FILE_DIR:PhoneMimi>"
    )
elseif (ENABLE_ANDROID)
    message(WARNING
        "ENABLE_ANDROID is ON but '${CMAKE_SOURCE_DIR}/tools' is missing. "
        "Android mirroring will fail at runtime until tools/adb/adb.exe and "
        "tools/scrcpy-server.jar are placed next to the built EXE.")
endif()
