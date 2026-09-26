# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Host-side tests for the protocol core and the link layer (no Zephyr, no west workspace):
#
#   make -C tests/host run
#
# The protocol core and link layer use the same lib/ sources as the device; crypto comes from
# crypto_soft.c here (same interface, no dependencies, the device uses PSA). All this needs is a
# C compiler; no workspace setup.

CC      ?= cc
# -MMD -MP: header dependencies go into .d files so a changed header rebuilds, no stale layout
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -MMD -MP
CPPFLAGS += -I../../include

# ASan + UBSan are on by default: the host is the cheapest place to catch overruns and UB.
#   SANITIZE=0  turn them off for a faster build (separate directory, no cross-contamination)
#   LEAKS=1     enable LeakSanitizer. Off by default: nothing allocates here, and restricted
#               containers without ptrace cannot start LeakSanitizer at all
SANITIZE ?= 1
LEAKS    ?= 0
ifeq ($(SANITIZE),1)
CFLAGS  += -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1
LDFLAGS += -fsanitize=address,undefined
SAN_ENV := ASAN_OPTIONS=detect_leaks=$(LEAKS)
BUILD   := build-san
else
BUILD   := build
endif

# Fuzz iterations (the default lives in test_fuzz.c)
ifneq ($(FUZZ_ITERS),)
CPPFLAGS += -DFUZZ_ITERS=$(FUZZ_ITERS)
endif

VPATH := .:../../core/proto:../../core/link:../../core/crsf:../../core/mgmt:../../core/region:../../core/led

SRCS := main.c test_proto.c test_link.c \
        test_crsf.c test_mgmt.c test_region.c test_mod.c test_led.c test_power.c \
        test_fuzz.c \
        crsf.c mgmt.c region.c led.c \
        crypto_soft.c crc.c fhss.c frag.c frame.c mod.c tlm.c \
        link.c rate.c power.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)


BIN := $(BUILD)/test_zlrs

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

run: $(BIN)
	$(SAN_ENV) ./$(BIN)

clean:
	rm -rf build build-san

-include $(DEPS)
