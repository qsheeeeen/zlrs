/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Independent expected values generated with Python hashlib/hmac. */
static const struct {
	uint16_t key_len;
	uint16_t len;
	const char *want;
} hmac_vectors[] = {
	{32U, 0U, "d38b42096d80f45f826b44a9d5607de72496a415d3f4a1a8c88e3bb9da8dc1cb"},
	{32U, 1U, "e711546e3faad4c7c4aa756bc26cad6abea8241984a0f6b0839c70ca61c4ef88"},
	{32U, 55U, "b478e4cbd63871759702a8a4c9828359869bc9e20d3df429ecd08f5a5d3d9340"},
	{32U, 56U, "e5d1f65e9e9359d05c577b6890044f08c9a1f7969b683f1237ef07db70e5f862"},
	{32U, 63U, "d37a8dadb82b15310342ceabf0de8cb8991ee9bd55dd3e4813e952081cb24bf1"},
	{32U, 64U, "173206781c3b828a0dc2a716fe0ddb5e6e56ec171170952ff6b3f4de44fa18d7"},
	{32U, 65U, "22084084cc171f63dfdd6ca4bcb0c29be8d4ff1cc6b1d0d21e10e2a2a0bfce9c"},
	{32U, 251U, "9f3c65f884ad74347badf93743fbbb6acfe8d1ce807e8217a5fc2d6838d7fcf9"},
	{131U, 200U, "c4b86c4234bc84b889b659e1d14fb2f63a633db5f37618306da3b036564d9ee9"},
};
