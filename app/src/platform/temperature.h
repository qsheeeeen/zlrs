/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

/* CPU die temperature in hundredths of a degree C; negative errno when unavailable. */
int zlrs_temperature_read(int16_t *centidegrees);
