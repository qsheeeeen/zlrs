# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 协议核心与链路层的主机侧测试（不跑 Zephyr）：
#
#   make -C tests/host run
#
# 密码学原语用 mbedTLS，而且用的是**跟设备侧同一份源码**：west 工作区里的
# mbedtls 模块。所以先跑过 tools/setup-zephyr.sh；工作区不在默认位置时用
# MBEDTLS_DIR=<path>/modules/crypto/mbedtls 指定。

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../../include -I$(CURDIR) -I$(MBEDTLS_DIR)/include \
            -DMBEDTLS_CONFIG_FILE='"mbedtls_config_host.h"'

# mbedTLS 自己的源码不用我们的告警等级
MBEDTLS_CFLAGS ?= -std=c11 -O2

# west 工作区根 = 仓库的上一级目录
MBEDTLS_DIR ?= $(abspath ../../..)/modules/crypto/mbedtls

ifeq ($(wildcard $(MBEDTLS_DIR)/library/sha256.c),)
$(error 找不到 mbedTLS 源码（$(MBEDTLS_DIR)）；先跑 tools/setup-zephyr.sh，或用 MBEDTLS_DIR=<path> 指定)
endif

BUILD := build
VPATH := .:../../lib/proto:../../lib/link

SRCS := main.c test_proto.c test_link.c \
        crypto.c fhss.c frag.c frame.c link.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))

MBEDTLS_SRCS := sha256.c md.c hkdf.c constant_time.c platform_util.c
MBEDTLS_OBJS := $(addprefix $(BUILD)/mbedtls/,$(MBEDTLS_SRCS:.c=.o))

BIN := $(BUILD)/test_zlrs

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJS) $(MBEDTLS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/mbedtls/%.o: $(MBEDTLS_DIR)/library/%.c
	@mkdir -p $(BUILD)/mbedtls
	$(CC) $(MBEDTLS_CFLAGS) -I$(CURDIR) -I$(MBEDTLS_DIR)/include \
		-DMBEDTLS_CONFIG_FILE='"mbedtls_config_host.h"' -c -o $@ $<

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD)
