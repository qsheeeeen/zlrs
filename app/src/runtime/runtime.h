/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

int zlrs_runtime_run(void);
void zlrs_runtime_request_reboot(uint32_t delay_ms);
void zlrs_runtime_cancel_reboot(void);
