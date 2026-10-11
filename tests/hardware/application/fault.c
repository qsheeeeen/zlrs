/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdbool.h>

/* Simulate a failed LR2021 init while exercising the real app, USB, NVS and management code. */
int __wrap_zlrs_radio_init(void)
{
	return -EIO;
}

/* Exercise the confirmed-image fault policy without confirming this test image in flash.
 * The runner reboots through recovery after checking management, so MCUboot restores the
 * previous firmware. Never confirm or ship this fixture. */
bool __wrap_boot_is_img_confirmed(void)
{
	return true;
}
