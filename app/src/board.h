/*
 * 板级派生量：从 devicetree 读出来的"这块板子是什么"。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZLRS_APP_BOARD_H_
#define ZLRS_APP_BOARD_H_

#include <zephyr/devicetree.h>

#include "zlrs/region/region.h"

/* 本机 sub-G SKU（板级在各变体 dtsi 里声明）；地区必须与它匹配，见 lib/region。
 * 只能从 DT 节点读：/chosen 里自定义前缀的属性 DT 宏读不到（会静默用默认值）。 */
#define ZLRS_SUBG_BAND DT_PROP(DT_NODELABEL(zlrs_subg_band), band)

#endif /* ZLRS_APP_BOARD_H_ */
