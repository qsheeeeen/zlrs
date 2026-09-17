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
 *
 * 从 DT 节点读，不要从 /chosen 读：Zephyr 的 DT 宏只导出 `zephyr,` 前缀的
 * chosen 属性，自定义前缀（`zlrs,subg-band`）会被静默忽略、退回下面的默认值——
 * 那正是 LF 板曾经"永远过不了地区校验、直接 fault_loop"的原因。
 */
#define ZLRS_SUBG_BAND DT_PROP(DT_NODELABEL(zlrs_subg_band), band)

#endif /* ZLRS_APP_BOARD_H_ */
