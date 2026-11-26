/*
 * Signatures (src/wh/sig.c: wh's package index, amnesia-update's
 * manifest): hex round trips and malformed hex, a good signature accepted,
 * and a changed file, a changed signature or another key refused.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <monocypher-ed25519.h>

#include "../../src/wh/sig.h"

#include "unit.h"

static void write_file(const char *path, const void *data, size_t n)
{
	FILE *f = fopen(path, "we");

	if (!f || fwrite(data, 1, n, f) != n || fclose(f) != 0)
		exit(2);
}

int main(void)
{
	char dir[] = "/tmp/wormos-unit-sig-XXXXXX", file[64], sigfile[64];
	uint8_t seed[32], sk[64], pk[32], other_pk[32], sig[64], back[64];
	char hex[2 * 64 + 2];
	static const char index[] = "name = tor\nversion = 0.4.9.17\n";

	CHECK(hex_decode("00ff10Ab", back, 4) == 0 && back[0] == 0 && back[1] == 0xff &&
	      back[2] == 0x10 && back[3] == 0xab);
	CHECK(hex_decode("  00ff\n", back, 2) == 0);
	CHECK(hex_decode("00f", back, 2) != 0);
	CHECK(hex_decode("00ffee", back, 2) != 0);
	CHECK(hex_decode("zz", back, 1) != 0);

	if (!mkdtemp(dir))
		return 2;
	snprintf(file, sizeof(file), "%s/index", dir);
	snprintf(sigfile, sizeof(sigfile), "%s/index.sig", dir);
	memset(seed, 8, sizeof(seed));
	crypto_ed25519_key_pair(sk, other_pk, seed);   /* wipes the seed */
	memset(seed, 7, sizeof(seed));
	crypto_ed25519_key_pair(sk, pk, seed);

	write_file(file, index, strlen(index));
	crypto_ed25519_sign(sig, sk, (const uint8_t *)index, strlen(index));
	hex_encode(sig, sizeof(sig), hex);
	CHECK(strlen(hex) == 128);
	CHECK(hex_decode(hex, back, sizeof(back)) == 0 && memcmp(back, sig, sizeof(sig)) == 0);
	strcat(hex, "\n");
	write_file(sigfile, hex, strlen(hex));
	CHECK(sig_verify_file(pk, file, sigfile) == 0);
	CHECK(sig_verify_file(other_pk, file, sigfile) != 0);

	/* A changed index. */
	write_file(file, "name = tor\nversion = 0.4.9.18\n", strlen(index));
	CHECK(sig_verify_file(pk, file, sigfile) != 0);
	write_file(file, index, strlen(index));
	/* A changed signature. */
	hex[5] = hex[5] == '0' ? '1' : '0';
	write_file(sigfile, hex, strlen(hex));
	CHECK(sig_verify_file(pk, file, sigfile) != 0);
	/* A truncated one, and none at all. */
	write_file(sigfile, "abcd\n", 5);
	CHECK(sig_verify_file(pk, file, sigfile) != 0);
	unlink(sigfile);
	CHECK(sig_verify_file(pk, file, sigfile) != 0);

	unlink(file);
	rmdir(dir);
	return unit_report("sig");
}
