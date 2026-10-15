/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/sys/atomic.h>
#include <zlrs/usb.h>

#include "usb_state.h"

enum { HOST_CONFIGURED = BIT(0), BUS_SUSPENDED = BIT(1) };
static atomic_t host_state;

bool zlrs_usb_active(void)
{
	return atomic_get(&host_state) == HOST_CONFIGURED;
}

void zlrs_usb_message(struct usbd_context *ctx, const struct usbd_msg *msg)
{
	ARG_UNUSED(ctx);

	switch (msg->type) {
	case USBD_MSG_CONFIGURATION:
		atomic_set(&host_state, msg->status != 0 ? HOST_CONFIGURED : 0);
		break;
	case USBD_MSG_SUSPEND:
		atomic_or(&host_state, BUS_SUSPENDED);
		break;
	case USBD_MSG_RESUME:
		atomic_and(&host_state, ~BUS_SUSPENDED);
		break;
	case USBD_MSG_RESET:
	case USBD_MSG_VBUS_REMOVED:
	case USBD_MSG_UDC_ERROR:
	case USBD_MSG_STACK_ERROR:
		atomic_clear(&host_state);
		break;
	default:
		break;
	}
}
