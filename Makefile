# Pocket Player — common build / test / tooling tasks.
#
# Builds are pinned to the canonical source path: the repo is reachable from two
# paths sharing an inode, and AUTOMOC breaks unless cmake is driven from the
# Projects copy. So every target uses $(SRC) rather than the current directory.

SRC        := /home/rkb/Projects/p/mp3-player
BUILD      := $(SRC)/build
BUILD_TYPE ?= Release
JOBS       ?= $(shell nproc)

.DEFAULT_GOAL := build

.PHONY: configure build rebuild run clean demo demo-run help

## configure: generate the build tree
configure:
	cmake -B $(BUILD) -S $(SRC) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

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
	cmake -B $(BUILD) -S $(SRC) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DBUILD_SHADER_DEMO=ON
	cmake --build $(BUILD) -j$(JOBS) --target shader_demo

## demo-run: build and launch the shader demo
demo-run: demo
	$(BUILD)/shader_demo $(ARGS)

## clean: remove the build tree
clean:
	rm -rf $(BUILD)

## help: list targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'
