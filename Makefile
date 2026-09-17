# Default target is deliberately hardware-free.
.DEFAULT_GOAL := help
PYTHON ?= python3
BUILD_DIR ?= build/arm-validation
RELEASE_DIR ?= build/releases
PROFILE ?= config/updater.json
PICOTOOL ?= picotool
TEST_TIER ?= fast
VERIFY_MODE ?= normal
JOBS ?= 4
ROLLOUT_TIMEOUT ?= 90
HELPER_INSTALL_DIR ?= $(HOME)/Applications
HELPER_SIGN_IDENTITY ?= -

# A caller-selected frozen manifest must not trigger a different build.
ifeq ($(origin MANIFEST),undefined)
MANIFEST := build/updater/latest.json
FLASH_PREREQUISITE := release
endif

UPDATER = $(PYTHON) scripts/update_firmware.py
PREPARE_ARGS = --build-dir "$(BUILD_DIR)" --release-dir "$(RELEASE_DIR)" --tier "$(TEST_TIER)" --jobs "$(JOBS)" $(if $(TOOLCHAIN_DIR),--toolchain-dir "$(TOOLCHAIN_DIR)") $(if $(FORCE),--force)
DEVICE_ARGS = --manifest "$(MANIFEST)" --profile "$(PROFILE)" --picotool "$(PICOTOOL)" --rollout-timeout "$(ROLLOUT_TIMEOUT)" --verification-mode "$(VERIFY_MODE)" $(if $(PORT),--port "$(PORT)") $(if $(TARGET),--target "$(TARGET)")

.PHONY: help test test-updater release flash-plan flash flash-bootloader verify helper-app install-helper-app
help:
	@echo 'DeskHop: make release | flash-plan | flash | flash-bootloader | verify | test | test-updater'
	@echo 'Default is help; release/flash-plan/test never access hardware. flash writes hardware.'
	@echo 'Options: PROFILE=... PORT=/dev/cu... TARGET=A|B PICOTOOL=... TOOLCHAIN_DIR=...'
	@echo 'MANIFEST=... selects a frozen candidate without rebuilding; TEST_TIER=deep extends validation.'
	@echo 'VERIFY_MODE=normal (default) checks each Pico once; thorough retains extra upgrade/read-only diagnostics.'
	@echo 'See docs/updater.md for safety, prerequisites, evidence and recovery.'
	@echo 'macOS helper: make helper-app | install-helper-app (builds first; defaults to ~/Applications)'
	@echo 'Helper options: HELPER_INSTALL_DIR=... HELPER_SIGN_IDENTITY=... (default: ad-hoc signing)'
helper-app:
	$(PYTHON) scripts/build_clipboard_app.py --identity "$(HELPER_SIGN_IDENTITY)"
install-helper-app: helper-app
	xcrun swift -module-cache-path build/clipboard-module-cache macos/DeskHopClipboard/Tools/install.swift "build/clipboard-app/DeskHop Clipboard.app" "$(HELPER_INSTALL_DIR)"
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
