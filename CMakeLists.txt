cmake_minimum_required(VERSION 3.21)

project(MobileMp3Player VERSION 0.1 LANGUAGES CXX)

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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Sql Concurrent Test)
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
    src/PlaylistModel.cpp
    src/PlaylistModel.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/MusicLibrary.cpp
    src/MusicLibrary.h
    src/YearParser.cpp
    src/YearParser.h
    src/Theme.cpp
    src/Theme.h
    src/LibraryFolder.h
    src/CoverLabel.h
    resources/resources.qrc
)

target_link_libraries(mp3player PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Sql
    Qt6::Concurrent
    TagLib::tag
)

if(ENABLE_MPRIS)
    target_sources(mp3player PRIVATE src/MprisController.cpp src/MprisController.h)
    target_link_libraries(mp3player PRIVATE Qt6::DBus)
    target_compile_definitions(mp3player PRIVATE HAVE_MPRIS)
endif()

message(STATUS "MPRIS D-Bus support: ${ENABLE_MPRIS}")

# ---- Tests ----
enable_testing()
qt_add_executable(year_test tests/YearParserTest.cpp src/YearParser.cpp src/YearParser.h)
target_include_directories(year_test PRIVATE src)
target_link_libraries(year_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME year_test COMMAND year_test)
