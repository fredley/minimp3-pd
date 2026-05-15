HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = minimp3demo.pdx

# Locate the SDK
SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif

ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

VPATH += src

SRC = src/main.c src/minimp3_impl.c

# Pulls in this repo's minimp3.h via #include "minimp3.h" from the parent dir.
UINCDIR = ..
UASRC =
UDEFS =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

# Build the simulator dylib + .pdx, then hand it to the simulator app.
# `make` (default) keeps the SDK's stock behavior (device + simulator).
.PHONY: run
run: simulator
	open -a "Playdate Simulator" "$(CURDIR)/$(PRODUCT)"
