# RTDSP-F4 top-level build.
#
#   make unit      host C unit tests (ASan + UBSan)
#   make sil       host shared library for Python software-in-the-loop tests
#   make tables    regenerate firmware tables/golden vectors from Python
#   make fw        cross-compile the FreeRTOS firmware (TARGET=qemu|hw)
#   make test      unit + full pytest suite (SIL + emulator PIL)
#   make run       boot the firmware in QEMU (console on stdio, data link on tcp:5555)
#   make demo      end-to-end demo: design -> upload -> stream -> plots in build/reports
#   make viz       interactive viewer: input/output window + equation window per filter
#   make play      adjustable filter playground: sine + noise, live LP/HP/gain on the board
#   make lint      cppcheck static analysis (if installed)

PY      ?= .venv/bin/python
TARGET  ?= qemu
BUILD   := build
HOSTDIR := $(BUILD)/host
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  SHLIB_EXT := dylib
  SHLIB_FLAGS := -dynamiclib
else
  SHLIB_EXT := so
  SHLIB_FLAGS := -shared
endif

HOST_CC     ?= cc
HOST_CFLAGS := -std=c11 -O2 -g -ffp-contract=off -Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Werror \
               -Ilibs/dsp/include -Ilibs/link/include
# ASan is on for Linux/CI. On macOS sandboxes ASan can hang at startup, so the
# default there is UBSan only; override with `make unit SAN=address,undefined`.
ifeq ($(UNAME_S),Darwin)
  SAN ?= undefined
else
  SAN ?= address,undefined
endif
SAN_FLAGS   := -fsanitize=$(SAN) -fno-omit-frame-pointer -fno-sanitize-recover=all

LIB_SRCS := $(wildcard libs/dsp/src/*.c) $(wildcard libs/link/src/*.c)
SIL_LIB  := $(HOSTDIR)/librtdsp_sil.$(SHLIB_EXT)
UNIT_BIN := $(HOSTDIR)/unit_tests

.PHONY: all unit sil tables fw test test-sil test-pil run demo viz play lint clean

all: unit sil fw

$(HOSTDIR):
	@mkdir -p $@

$(UNIT_BIN): $(LIB_SRCS) tests/unit/test_main.c | $(HOSTDIR)
	$(HOST_CC) $(HOST_CFLAGS) $(SAN_FLAGS) $^ -lm -o $@

$(SIL_LIB): $(LIB_SRCS) host/sil_api.c | $(HOSTDIR)
	$(HOST_CC) $(HOST_CFLAGS) -fPIC -fvisibility=hidden $(SHLIB_FLAGS) $^ -o $@

unit: $(UNIT_BIN)
	./$(UNIT_BIN)

sil: $(SIL_LIB)

tables:
	$(PY) python/tools/gen_firmware_tables.py

fw:
	$(MAKE) -C firmware TARGET=$(TARGET)

test: unit sil fw
	$(PY) -m pytest python/tests -v

test-sil: sil
	$(PY) -m pytest python/tests -v -m "not emulator"

test-pil: fw
	$(PY) -m pytest python/tests -v -m emulator

run: fw
	qemu-system-arm -M netduinoplus2 -display none -monitor none \
	  -kernel $(BUILD)/fw/$(TARGET)/rtdsp.elf \
	  -serial tcp:127.0.0.1:5555,server=on,wait=on -serial stdio

demo: sil fw
	$(PY) python/tools/demo.py

viz: fw
	$(PY) python/tools/visualize.py

play: fw
	$(PY) python/tools/playground.py

lint:
	cppcheck --enable=warning,style,performance,portability --inline-suppr --error-exitcode=1 \
	  --std=c11 -Ilibs/dsp/include -Ilibs/link/include --suppress=missingIncludeSystem \
	  libs firmware/app firmware/bsp

clean:
	rm -rf $(BUILD)
