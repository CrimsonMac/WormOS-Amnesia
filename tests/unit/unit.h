/* A minimal check macro for the unit tests in tests/unit. */
#ifndef WORMOS_UNIT_H
#define WORMOS_UNIT_H

#include <stdio.h>

static int unit_passed, unit_failed;

#define CHECK(cond) do { \
	if (cond) { \
		unit_passed++; \
	} else { \
		unit_failed++; \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static inline int unit_report(const char *name)
{
	printf("%-24s %d/%d checks passed\n", name, unit_passed, unit_passed + unit_failed);
	return unit_failed ? 1 : 0;
}

#endif
