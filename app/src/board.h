/*
 * 板级派生量：从 devicetree 读出来的"这块板子是什么"。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_BOARD_H_
#define ZLRS_APP_BOARD_H_

#include <zephyr/devicetree.h>

#include "zlrs/region/region.h"

/*
 * 本机 sub-G 硬件（HF/LF SKU）：板级在每个变体的 dtsi 里声明。
 * HF = 850–930 MHz（跑 868 / 915），LF = 410–510 MHz（跑 470 / 433）。
 * 地区必须与它匹配——LF 板跑不了 915，HF 板跑不了 470/433，见 lib/region。
 */
#define ZLRS_SUBG_BAND DT_PROP_OR(DT_PATH(chosen), zlrs_subg_band, ZLRS_BAND_SUBG_HF)

#endif /* ZLRS_APP_BOARD_H_ */
