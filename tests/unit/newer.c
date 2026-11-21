/*
 * newer() (src/amnesia/update.c): amnesia-update offers a release only if
 * its version is newer, compared number by number (1.10 after 1.9).
 */

#define main amnesia_update_main
#include "../../src/amnesia/update.c"
#undef main

#include "unit.h"

int main(void)
{
	CHECK(newer("1.10", "1.9"));
	CHECK(!newer("1.9", "1.10"));
	CHECK(!newer("1.10", "1.10"));
	CHECK(newer("2.0", "1.99"));
	CHECK(newer("1.10.1", "1.10"));
	CHECK(!newer("1.10", "1.10.1"));
	CHECK(!newer("1.10.0", "1.10"));
	CHECK(!newer("1.10", "1.10.0"));
	CHECK(newer("10", "9"));
	return unit_report("amnesia-update newer");
}
