/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/usb/usbd.h>

/* Registered before enabling the controller, including events before main(). */
void zlrs_usb_message(struct usbd_context *ctx, const struct usbd_msg *msg);
