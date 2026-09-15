# Default target is deliberately hardware-free.
.DEFAULT_GOAL := help
PYTHON ?= python3
BUILD_DIR ?= build/arm-validation
RELEASE_DIR ?= build/releases
PROFILE ?= config/updater.json
PICOTOOL ?= picotool
TEST_TIER ?= fast
JOBS ?= 4
ROLLOUT_TIMEOUT ?= 90

# A caller-selected frozen manifest must not trigger a different build.
ifeq ($(origin MANIFEST),undefined)
MANIFEST := build/updater/latest.json
FLASH_PREREQUISITE := release
endif

UPDATER = $(PYTHON) scripts/update_firmware.py
PREPARE_ARGS = --build-dir "$(BUILD_DIR)" --release-dir "$(RELEASE_DIR)" --tier "$(TEST_TIER)" --jobs "$(JOBS)" $(if $(TOOLCHAIN_DIR),--toolchain-dir "$(TOOLCHAIN_DIR)") $(if $(FORCE),--force)
DEVICE_ARGS = --manifest "$(MANIFEST)" --profile "$(PROFILE)" --picotool "$(PICOTOOL)" --rollout-timeout "$(ROLLOUT_TIMEOUT)" $(if $(PORT),--port "$(PORT)") $(if $(TARGET),--target "$(TARGET)")

.PHONY: help test test-updater release flash-plan flash flash-bootloader verify
help:
	@echo 'DeskHop: make release | flash-plan | flash | flash-bootloader | verify | test | test-updater'
	@echo 'Default is help; release/flash-plan/test never access hardware. flash writes hardware.'
	@echo 'Options: PROFILE=... PORT=/dev/cu... TARGET=A|B PICOTOOL=... TOOLCHAIN_DIR=...'
	@echo 'MANIFEST=... selects a frozen candidate without rebuilding; TEST_TIER=deep extends validation.'
	@echo 'See docs/updater.md for safety, prerequisites, evidence and recovery.'
test:
	$(PYTHON) tests/run.py "$(TEST_TIER)"
test-updater:
	$(PYTHON) -m unittest discover -s tests/updater -v
release:
	$(UPDATER) prepare $(PREPARE_ARGS)
flash-plan: $(FLASH_PREREQUISITE)
	$(UPDATER) plan $(DEVICE_ARGS)
flash: $(FLASH_PREREQUISITE)
	$(UPDATER) flash $(DEVICE_ARGS)
flash-bootloader: $(FLASH_PREREQUISITE)
	$(UPDATER) flash $(DEVICE_ARGS) --already-bootloader
verify:
	$(UPDATER) verify $(DEVICE_ARGS)
