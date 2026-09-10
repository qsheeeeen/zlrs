# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 协议核心是纯 C、不依赖 Zephyr，可以直接在主机上编译运行：
#
#   make -C tests/host run

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../../include

BUILD    := build
PROTO    := ../../lib/proto

# 让 make 能在当前目录和协议核心目录里找到源文件，目标文件统一落在 build/
VPATH    := .:$(PROTO)

OBJS     := $(addprefix $(BUILD)/, test_proto.o crc.o uid.o fhss.o)
BIN      := $(BUILD)/test_proto

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
