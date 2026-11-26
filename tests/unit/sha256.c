/*
 * sha256 (src/snap/sha256.c, also used by wh and amnesia-update): the FIPS
 * 180-4 example messages, and input fed in odd-sized pieces.
 */

#include <string.h>

#include "../../src/snap/sha256.h"

#include "unit.h"

static int digest_is(const void *data, size_t n, size_t piece, const char *hex)
{
	struct sha256 c;
	unsigned char sum[SHA256_LEN];
	char out[SHA256_HEX];
	const unsigned char *p = data;

	sha256_init(&c);
	while (n > 0) {
		size_t k = n < piece ? n : piece;

		sha256_update(&c, p, k);
		p += k;
		n -= k;
	}
	sha256_final(&c, sum);
	sha256_hex(sum, out);
	return strcmp(out, hex) == 0;
}

int main(void)
{
	static const char abc_hex[] =
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
	static const char two_blocks[] =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	static char million[1000000];

	CHECK(digest_is("", 0, 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	CHECK(digest_is("abc", 3, 3, abc_hex));
	CHECK(digest_is("abc", 3, 1, abc_hex));
	for (size_t piece = 1; piece <= 64; piece += 7)
		CHECK(digest_is(two_blocks, strlen(two_blocks), piece,
				"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	memset(million, 'a', sizeof(million));
	CHECK(digest_is(million, sizeof(million), 4096,
			"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
	CHECK(digest_is(million, sizeof(million), 999999,
			"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
	return unit_report("sha256");
}
