/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * usp_zephyr 内部用 LOG_MODULE_DECLARE(usp, ...) 引用日志模块，但没有哪个
 * 文件 LOG_MODULE_REGISTER 它——Semtech 的示例是在自己的 main.c 里注册的。
 * 本文件只负责这件事。
 *
 * 少了它会在链接期报 undefined reference to `log_const_usp'。
 */

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usp, CONFIG_USP_LOG_LEVEL);
