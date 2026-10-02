/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "zlrs/link/link.h"

struct shell;

/* Called only by the RF owner. An active bench replaces serial input/output. */
bool zlrs_bench_pump(zlrs_link_t *link);
bool zlrs_bench_receive(const uint8_t *data, uint16_t len);
int zlrs_bench_command(const struct shell *sh, size_t argc, char **argv);
