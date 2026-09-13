# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 协议核心与链路层的主机侧测试（不跑 Zephyr，也不需要 west 工作区）：
#
#   make -C tests/host run
#
# 密码学原语是**跟设备侧同一份源码**（lib/proto/crypto_soft.c），所以这里
# 只要一个 C 编译器，不用先搭工作区。

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../../include

BUILD := build
VPATH := .:../../lib/proto:../../lib/link:../../lib/crsf

SRCS := main.c test_proto.c test_link.c \
        test_crsf.c crsf.c \
        crypto_soft.c fhss.c frag.c frame.c tlm.c link.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))


BIN := $(BUILD)/test_zlrs

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
