# Pocket Player — common build / test / tooling tasks.
#
# Builds are pinned to the canonical source path: the repo is reachable from two
# paths sharing an inode, and AUTOMOC breaks unless cmake is driven from the
# Projects copy. So every target uses $(SRC) rather than the current directory.

SRC        := /home/rkb/Projects/p/mp3-player
BUILD      := $(SRC)/build
BUILD_TYPE ?= Release
JOBS       ?= $(shell nproc)

# Build flags forwarded to cmake (see README "Build options" for the full set).
# Discord Rich Presence is ON by default with Pocket Player's app ID baked in by
# CMake; disable with `make build DISCORD_RPC=OFF`, or use your own app with
# `make build DISCORD_APP_ID=<id>`. DISCORD_APP_ID is only forwarded when set, so a
# blank value leaves CMake's baked-in default intact.
MPRIS          ?= ON
DISCORD_RPC    ?= ON
DISCORD_APP_ID ?=
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
               -DENABLE_MPRIS=$(MPRIS) -DENABLE_DISCORD_RPC=$(DISCORD_RPC) \
               $(if $(DISCORD_APP_ID),-DDISCORD_APP_ID=$(DISCORD_APP_ID),)

# ---- macOS (official-Qt build; mirrors notes/macos.md + the `macos` preset) ----
# Unlike the Linux targets above, these use the current directory (there's no
# dual-path AUTOMOC issue on macOS) and the official Qt's qt-cmake, which sets the
# toolchain/prefix. Configure into a separate build-mac/ kept apart from build/.
# QT_DIR auto-detects the newest ~/Qt/<ver>/macos (override on the command line).
QT_DIR        ?= $(shell ls -d $(HOME)/Qt/*/macos 2>/dev/null | sort -V | tail -1)
BUILD_MAC     ?= $(CURDIR)/build-mac
# "arm64;x86_64" for universal (needs universal TagLib); plain arm64 by default.
MACOS_ARCHS   ?= arm64
# Official Qt 6.11 frameworks require macOS 13+ (keeps LSMinimumSystemVersion honest).
DEPLOY_TARGET ?= 13.0

.DEFAULT_GOAL := build

.PHONY: configure build rebuild run clean demo demo-run help \
        macos-configure macos macos-run macos-dmg

## configure: generate the build tree
configure:
	cmake -B $(BUILD) -S $(SRC) $(CMAKE_FLAGS)

## build: compile the app (configures if needed)
build:
	@[ -d $(BUILD) ] || $(MAKE) configure
	cmake --build $(BUILD) -j$(JOBS)

## rebuild: wipe the build tree and build from scratch
rebuild: clean configure build

## run: launch the player
run: build
	$(BUILD)/mp3player $(ARGS)

## demo: build the QRhiWidget shader demo (enables BUILD_SHADER_DEMO)
demo:
	cmake -B $(BUILD) -S $(SRC) $(CMAKE_FLAGS) -DBUILD_SHADER_DEMO=ON
	cmake --build $(BUILD) -j$(JOBS) --target shader_demo

## demo-run: build and launch the shader demo
demo-run: demo
	$(BUILD)/shader_demo $(ARGS)

## macos-configure: configure the official-Qt macOS build (qt-cmake -> build-mac)
macos-configure:
	@[ -n "$(QT_DIR)" ] || { echo "error: no official Qt found under ~/Qt; set QT_DIR=/path/to/Qt/<ver>/macos (see notes/macos.md)"; exit 1; }
	"$(QT_DIR)/bin/qt-cmake" -B $(BUILD_MAC) -S $(CURDIR) -G Ninja \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_OSX_ARCHITECTURES="$(MACOS_ARCHS)" \
		-DCMAKE_OSX_DEPLOYMENT_TARGET=$(DEPLOY_TARGET)

## macos: build the macOS .app bundle (configures if needed)
macos:
	@[ -d $(BUILD_MAC) ] || $(MAKE) macos-configure
	cmake --build $(BUILD_MAC) -j$(JOBS)

## macos-run: build and launch the .app via open (needed for the media session)
macos-run: macos
	open $(BUILD_MAC)/mp3player.app

## macos-dmg: build a self-contained, ad-hoc-signed .dmg (scripts/package-macos.sh)
macos-dmg:
	ARCHS="$(MACOS_ARCHS)" scripts/package-macos.sh

## clean: remove the build tree (both Linux and macOS)
clean:
	rm -rf $(BUILD) $(BUILD_MAC)

## help: list targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'
