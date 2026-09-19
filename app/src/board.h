/*
 * Board-derived values: what this board is, read from the devicetree.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/devicetree.h>

#include "zlrs/region/region.h"

/* Local sub-G SKU (declared in each variant's dtsi); the region has to match it, see core/region.
 * It must come from a DT node: DT macros cannot read custom-prefixed /chosen properties. */
#define ZLRS_SUBG_BAND DT_PROP(DT_NODELABEL(zlrs_subg_band), band)
