# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# 协议核心与链路层的主机侧测试（不跑 Zephyr，也不需要 west 工作区）：
#
#   make -C tests/host run
#
# 协议核心与链路层用的是 lib/ 里跟设备侧同一份源码；密码学是本目录下的
# crypto_soft.c（同接口、零依赖，设备侧那份走 PSA）。所以这里只要一个 C 编译器，
# 不用先搭工作区。

CC      ?= cc
# -MMD -MP：把头文件依赖写进 .d，改头文件后自动重编，避免旧对象混着新布局
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -MMD -MP
CPPFLAGS += -I../../include

# 默认就带 ASan + UBSan：主机测试是抓越界与未定义行为最便宜的地方，忘了开等于白跑。
#   SANITIZE=0  关掉，换快一点的构建（单独目录，两边不会互相污染）
#   LEAKS=1     打开 LeakSanitizer。默认关：lib 与用例里没有任何动态分配，而且受限
#               容器缺 ptrace 时 LeakSanitizer 直接起不来
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

# 模糊测试的迭代次数（默认见 test_fuzz.c）
ifneq ($(FUZZ_ITERS),)
CPPFLAGS += -DFUZZ_ITERS=$(FUZZ_ITERS)
endif

VPATH := .:../../lib/proto:../../lib/link:../../lib/crsf:../../lib/config:../../lib/region:../../lib/led

SRCS := main.c test_proto.c test_link.c \
        test_crsf.c test_config.c test_region.c test_led.c test_power.c \
        test_fuzz.c \
        crsf.c config.c region.c led.c \
        crypto_soft.c crc.c fhss.c frag.c frame.c tlm.c \
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
