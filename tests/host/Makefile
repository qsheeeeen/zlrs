# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 协议核心与链路层的主机侧测试（不含 Zephyr）：
#
#   make -C tests/host run

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../../include

BUILD := build
VPATH := .:../../lib/proto:../../lib/link

SRCS := main.c test_proto.c test_link.c \
        crc.c crypto.c fhss.c frag.c frame.c link.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))
BIN  := $(BUILD)/test_zlrs

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD)
