cmake_minimum_required(VERSION 3.21)

project(PocketPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# MPRIS D-Bus media control. On by default on Linux (where D-Bus is the norm),
# off elsewhere. Pulls in Qt6::DBus only when enabled.
if(UNIX AND NOT APPLE)
    option(ENABLE_MPRIS "Enable MPRIS D-Bus media control" ON)
else()
    option(ENABLE_MPRIS "Enable MPRIS D-Bus media control" OFF)
endif()

# Qt Widgets + Multimedia only. No WebEngine, no Qt Quick/QML — keeps the
# dependency surface and runtime CPU/GPU usage low.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Sql Concurrent Network)
# zlib: TagLib uses it (compressed ID3 frames). A dynamic TagLib has it baked in,
# but a *static* TagLib (e.g. our universal macOS build) exports ZLIB::ZLIB in its
# link interface, so the target must exist before find_package(taglib). Harmless
# everywhere else — zlib is a system library on all platforms.
find_package(ZLIB REQUIRED)
# TagLib: lightweight tag reader — parses ID3/Vorbis frames without decoding audio.
find_package(taglib REQUIRED)

if(ENABLE_MPRIS)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
endif()

qt_standard_project_setup()

qt_add_executable(mp3player
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/PlayerController.cpp
    src/PlayerController.h
    src/MediaEngine.cpp
    src/MediaEngine.h
    src/MediaSession.cpp
    src/MediaSession.h
    src/RemoteResolver.cpp
    src/RemoteResolver.h
    src/YtDlpManager.cpp
    src/YtDlpManager.h
    src/Toast.cpp
    src/Toast.h
    src/Notifier.cpp
    src/Notifier.h
    src/Importer.cpp
    src/Importer.h
    src/ImportDialog.cpp
    src/ImportDialog.h
    src/PlaylistModel.cpp
    src/PlaylistModel.h
    src/TrackListModel.cpp
    src/TrackListModel.h
    src/PlaylistEditorDialog.cpp
    src/PlaylistEditorDialog.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/MusicLibrary.cpp
    src/MusicLibrary.h
    src/PlaylistStore.cpp
    src/PlaylistStore.h
    src/Theme.cpp
    src/Theme.h
    src/Logger.cpp
    src/Logger.h
    src/LibraryFolder.h
    src/TrackUri.h
    src/ProcUtil.h
    src/CoverLabel.h
    src/AudioFormats.h
    src/TimeFormat.h
    resources/resources.qrc
)

target_link_libraries(mp3player PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Sql
    Qt6::Concurrent
    Qt6::Network
    TagLib::tag
)

if(ENABLE_MPRIS)
    target_sources(mp3player PRIVATE src/MprisController.cpp src/MprisController.h)
    target_link_libraries(mp3player PRIVATE Qt6::DBus)
    target_compile_definitions(mp3player PRIVATE HAVE_MPRIS)
endif()

message(STATUS "MPRIS D-Bus support: ${ENABLE_MPRIS}")

# Discord Rich Presence — cross-platform (talks Discord's local IPC socket directly
# over QLocalSocket, so no extra dependency). On by default; turn off with
# -DENABLE_DISCORD_RPC=OFF. The application/client ID is baked in at build time and
# defaults to Pocket Player's own app; override with -DDISCORD_APP_ID=<id> or at
# runtime via Settings.
option(ENABLE_DISCORD_RPC "Enable Discord Rich Presence" ON)
set(DISCORD_APP_ID "1513210138440503327"
    CACHE STRING "Discord application (client) ID for Rich Presence")
if(ENABLE_DISCORD_RPC)
    target_sources(mp3player PRIVATE src/DiscordPresence.cpp src/DiscordPresence.h)
    target_compile_definitions(mp3player PRIVATE
        HAVE_DISCORD_RPC DISCORD_APP_ID="${DISCORD_APP_ID}")
    if(DISCORD_APP_ID STREQUAL "")
        message(WARNING "ENABLE_DISCORD_RPC is ON but DISCORD_APP_ID is empty — "
                        "presence will stay idle until set (build with "
                        "-DDISCORD_APP_ID=<id> or via Settings).")
    endif()
endif()
message(STATUS "Discord Rich Presence: ${ENABLE_DISCORD_RPC}")

# ---- macOS .app bundle (for macdeployqt / .dmg packaging) ----
# A relocatable, self-contained .app is required before macdeployqt can gather
# the Qt frameworks + FFmpeg backend and produce a .dmg. Plain executables can't
# be deployed. Bundle metadata comes from resources/macos/Info.plist.in.
if(APPLE)
    # Native media session: MPNowPlayingInfoCenter + MPRemoteCommandCenter, in
    # Objective-C++. Compiled only here; MediaSession::create() returns it under
    # Q_OS_MACOS. Mirrors the Linux MprisController.
    enable_language(OBJCXX)
    target_sources(mp3player PRIVATE
        src/NowPlayingSession.mm
        src/NowPlayingSession.h)
    # ARC: the .mm allocates NSImage/MPMediaItemArtwork on every metadata update;
    # without automatic reference counting those leak each track/state change.
    set_source_files_properties(src/NowPlayingSession.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(mp3player PRIVATE
        "-framework MediaPlayer"
        "-framework AppKit")

    set(MACOSX_BUNDLE_BUNDLE_NAME "Pocket Player")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "dev.rkben.pocketplayer")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_COPYRIGHT "© 2026 Pocket Player")

    # App icon: only embedded if resources/macos/AppIcon.icns exists (generate it
    # with scripts/make-icns.sh). Without it the app uses the default Qt icon.
    set(APP_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/AppIcon.icns")
    if(EXISTS "${APP_ICNS}")
        set(MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")
        target_sources(mp3player PRIVATE "${APP_ICNS}")
        set_source_files_properties("${APP_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
    endif()

    set_target_properties(mp3player PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in")
endif()

# ---- Linux install rules (populate an AppDir for linuxdeploy / AppImage) ----
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS mp3player RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES resources/linux/pocketplayer.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/linux/pocketplayer.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# ---- Static-Qt plugin imports (Windows single-exe build) ----
# With dynamic Qt, plugins are DLLs loaded at runtime and nothing here is needed.
# With a STATIC Qt they must be linked in explicitly (Q_IMPORT_PLUGIN), or features
# silently break at runtime. Detect a static Qt via the Qt6::Core target type so
# this is a no-op for the macOS/Linux dynamic builds. See notes/windows.md.
if(TARGET Qt6::Core)
    get_target_property(_qt_core_type Qt6::Core TYPE)
    if(_qt_core_type STREQUAL "STATIC_LIBRARY")
        message(STATUS "Static Qt detected — importing required plugins")
        # qschannel is the critical one: without it every https:// fetch (yt-dlp
        # stream resolution + cover downloads) fails at runtime in a build that
        # otherwise looks fine. qwindows is required for the app to start at all.
        qt_import_plugins(mp3player
            INCLUDE Qt6::QWindowsIntegrationPlugin
                    Qt6::QSchannelBackendPlugin
                    Qt6::QJpegPlugin
                    Qt6::QSvgPlugin)
    endif()
endif()

# ---- Widgets-native shader demo (QRhiWidget, Qt 6.7+) ----
# Standalone experiment for "shader shenanigans" behind the album art, with no
# QML/Qt Quick. Off by default: -DBUILD_SHADER_DEMO=ON to build it.
option(BUILD_SHADER_DEMO "Build the QRhiWidget shader demo" OFF)
if(BUILD_SHADER_DEMO)
    find_package(Qt6 REQUIRED COMPONENTS ShaderTools GuiPrivate Multimedia)
    qt_add_executable(shader_demo 
        demo/shader_demo.cpp
        src/ShaderArt.cpp
        src/ShaderArt.h
    )
    
    # ADD THIS LINE: This passes the source folder path to your C++ code
    target_compile_definitions(shader_demo PRIVATE SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/demo")

    qt_add_shaders(shader_demo "shader_demo_shaders"
        PREFIX "/shaders"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/demo"
        FILES demo/fullscreen.vert demo/plasma.frag)
    target_link_libraries(shader_demo PRIVATE Qt6::Widgets Qt6::GuiPrivate Qt6::Multimedia)
    message(STATUS "Shader demo: ON")
endif()
