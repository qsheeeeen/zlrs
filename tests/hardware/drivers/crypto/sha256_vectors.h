/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Independent expected values generated with Python hashlib/hmac. */
static const struct {
	uint16_t len;
	const char *want;
} sha_vectors[] = {
	{0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
	{3U, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
	{55U, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
	{56U, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
	{63U, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
	{64U, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
	{65U, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
	{255U, "3f8591112c6bbe5c963965954e293108b7208ed2af893e500d859368c654eabe"},
};
