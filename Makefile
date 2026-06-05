# Pocket Player — common build / test / tooling tasks.
#
# Builds are pinned to the canonical source path: the repo is reachable from two
# paths sharing an inode, and AUTOMOC breaks unless cmake is driven from the
# Projects copy. So every target uses $(SRC) rather than the current directory.

SRC        := /home/rkb/Projects/p/mp3-player
BUILD      := $(SRC)/build
BUILD_TYPE ?= Release
JOBS       ?= $(shell nproc)

# File passed to data-tooling targets (override: make vd FILE=foo.tsv).
FILE       ?= dates.tsv
# Directory the date-extraction probe walks (override: make extract DIR=~/Music).
DIR        ?= $(HOME)/Music

.DEFAULT_GOAL := build

.PHONY: configure build rebuild run test clean \
        extract cases misses vd help

## configure: generate the build tree
configure:
	cmake -B $(BUILD) -S $(SRC) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

## build: compile the app, tests, and tools (configures if needed)
build:
	@[ -d $(BUILD) ] || $(MAKE) configure
	cmake --build $(BUILD) -j$(JOBS)

## rebuild: wipe the build tree and build from scratch
rebuild: clean configure build

## run: launch the player
run: build
	$(BUILD)/mp3player $(ARGS)

## test: build, then run the test suite
test: build
	ctest --test-dir $(BUILD) --output-on-failure

## clean: remove the build tree
clean:
	rm -rf $(BUILD)

## extract: dump a directory's raw date tags to $(FILE)  (DIR=, FILE=)
extract:
	uv run $(SRC)/scripts/extract_dates.py $(DIR) -o $(FILE)

## cases: filter extracted dates into the parser-test fixture (needs `build`)
cases: build
	uv run $(SRC)/scripts/extract_dates.py $(DIR) -o - \
		| uv run $(SRC)/scripts/build_year_cases.py -

## misses: find dates where the parser misses a visible year, open in visidata  (DIR=)
MISSES_FILE := $(SRC)/tests/year_cases.tsv
misses: build
	uv run $(SRC)/scripts/extract_dates.py $(DIR) -o - \
		| uv run $(SRC)/scripts/build_year_cases.py - --misses-only -o $(MISSES_FILE)
	uvx visidata $(MISSES_FILE)

## vd: open a TSV in visidata for human-readable browsing  (FILE=)
vd:
	uvx visidata $(FILE)

## help: list targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'
