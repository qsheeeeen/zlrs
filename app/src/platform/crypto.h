/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

struct shell;

/* Debug shell only: independent known-answer vectors and full-frame tag timing. */
int zlrs_crypto_self_test(const struct shell *sh);
