/*
 * 配置模式：开机时按住按键进入，数据口改跑配置协议（见 doc/01-system.md 2.1）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_CONFIG_PORT_H_
#define ZLRS_APP_CONFIG_PORT_H_

#include <stdint.h>

#include "zlrs/link/link.h"

/*
 * 阻塞式跑配置会话：到超时或收到 EXIT 为止。
 * 命令作用在 control 链路上；失控值写进 failsafe（应用自己那份缓冲）。
 * 返回 0 = 收到 EXIT，1 = 超时。
 */
int zlrs_config_mode_run(zlrs_link_t *control, uint8_t *failsafe);

#endif /* ZLRS_APP_CONFIG_PORT_H_ */
