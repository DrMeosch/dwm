/* Checks for the sampler/dwm contract: /proc/stat parsing, CPU usage and the property payload. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sysmon.h"

static int failures;

#define CHECK(condition, ...) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: "); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		failures++; \
	} \
} while (0)

#define LENGTH(X) (sizeof (X) / sizeof (X)[0])

static void
test_parse(void)
{
	static const struct { const char *line; int ok; unsigned long long total, idle; } cases[] = {
		{ "cpu  100 20 30 400 50 6 7 8 9 10\ncpu0 1 2 3 4\n", 1, 621, 450 },
		{ "cpu  100 20 30 400 50 6 7 8\n", 1, 621, 450 },
		{ "cpu 1 2 3 4\n", 1, 10, 4 },
		{ "cpu 1 2 3 4", 1, 10, 4 },
		{ "cpu   1  2  3  4  \n", 1, 10, 4 },
		{ "cpu  0 0 0 0 0 0 0 0 0 0\n", 1, 0, 0 },
		{ "cpu 18446744073709551615 0 0 0\n", 1, 18446744073709551615ULL, 0 },
		/* anything else is no reading */
		{ "", 0, 0, 0 }, { "\n", 0, 0, 0 }, { "cpu\n", 0, 0, 0 }, { "cpu \n", 0, 0, 0 },
		{ "cpu 1 2 3\n", 0, 0, 0 }, { "cpu0 1 2 3 4\n", 0, 0, 0 }, { "cpux 1 2 3 4\n", 0, 0, 0 },
		{ "CPU 1 2 3 4\n", 0, 0, 0 }, { " cpu 1 2 3 4\n", 0, 0, 0 },
		{ "cpu 1 2 x 4\n", 0, 0, 0 }, { "cpu 1 2 3 4x\n", 0, 0, 0 }, { "cpu 1 2 -3 4\n", 0, 0, 0 },
		{ "cpu 1 2 +3 4\n", 0, 0, 0 }, { "cpu 1 2 3.5 4\n", 0, 0, 0 }, { "cpu 1 2 3\t4\n", 0, 0, 0 },
		{ "cpu 99999999999999999999 0 0 0\n", 0, 0, 0 },
		{ "cpu 18446744073709551615 1 0 0\n", 0, 0, 0 },
		{ "cpu \001\002\003\n", 0, 0, 0 }, { "\377\377\377\377", 0, 0, 0 },
	};
	size_t i;
	SysmonCpu c;

	for (i = 0; i < LENGTH(cases); i++) {
		int rc = sysmon_cpu_parse(cases[i].line, &c);

		CHECK((rc == 0) == cases[i].ok, "parse(\"%s\") returned %d", cases[i].line, rc);
		if (rc == 0 && cases[i].ok)
			CHECK(c.total == cases[i].total && c.idle == cases[i].idle,
			      "parse(\"%s\") = %llu/%llu, want %llu/%llu", cases[i].line,
			      c.total, c.idle, cases[i].total, cases[i].idle);
	}
}

static void
test_percent(void)
{
	static const struct { SysmonCpu a, b; int want; } cases[] = {
		{ { 1000, 800 }, { 1400, 1000 }, 50 },
		{ { 1000, 800 }, { 1100, 900 }, 0 },
		{ { 1000, 800 }, { 1100, 800 }, 100 },
		{ { 0, 0 }, { 3, 1 }, 67 },   /* 66.7 rounds up */
		{ { 0, 0 }, { 1000, 1 }, 100 },
		/* counters that do not describe an interval are no value */
		{ { 1000, 800 }, { 1000, 800 }, -1 },   /* no time passed */
		{ { 1000, 800 }, { 900, 700 }, -1 },    /* went backwards */
		{ { 1000, 800 }, { 1200, 700 }, -1 },   /* idle went backwards */
		{ { 1000, 800 }, { 1100, 1000 }, -1 },  /* more idle than elapsed */
		{ { 0, 0 }, { 18446744073709551615ULL, 0 }, -1 },
	};
	size_t i;

	for (i = 0; i < LENGTH(cases); i++) {
		int got = sysmon_cpu_percent(&cases[i].a, &cases[i].b);

		CHECK(got == cases[i].want, "percent case %zu = %d, want %d", i, got, cases[i].want);
	}
}

static void
test_payload(void)
{
	static const struct { const char *text; int want; } cases[] = {
		{ "cpu=0", 0 }, { "cpu=7", 7 }, { "cpu=42", 42 }, { "cpu=100", 100 },
		{ "cpu=042", 42 }, { "cpu=5 gpu=9", 5 }, { "gpu=9 cpu=5", 5 }, { "x cpu=5", 5 },
		{ "cpu=5  ", 5 },
		/* malformed or out of range: no value */
		{ "", -1 }, { "cpu", -1 }, { "cpu=", -1 }, { "cpu= 5", -1 }, { "cpu=101", -1 },
		{ "cpu=1000", -1 }, { "cpu=-5", -1 }, { "cpu=+5", -1 }, { "cpu=5x", -1 },
		{ "cpu=5.5", -1 }, { "cpu=abc", -1 }, { "CPU=5", -1 }, { "xcpu=5", -1 },
		{ "cpu:5", -1 }, { "gpu=5", -1 }, { "cpu=5,6", -1 }, { "cpu=0x10", -1 },
		{ "\377\377\377", -1 }, { "cpu=\033[2J", -1 },
	};
	char buf[SYSMON_MAXLEN + 64], exact[SYSMON_MAXLEN + 64];
	size_t i;
	int n;

	for (i = 0; i < LENGTH(cases); i++) {
		size_t len = strlen(cases[i].text);
		int got;

		/* decode gets exactly len bytes, as X hands them over: no terminator to lean on */
		memset(exact, 'z', sizeof exact);
		memcpy(exact, cases[i].text, len);
		got = sysmon_decode(exact, len);
		CHECK(got == cases[i].want, "decode(\"%s\") = %d, want %d", cases[i].text, got, cases[i].want);
	}
	CHECK(sysmon_decode(NULL, 5) == -1, "decode accepted a NULL payload");
	CHECK(sysmon_decode("cpu=5", 0) == -1, "decode accepted an empty payload");
	CHECK(sysmon_decode("cpu=5\0cpu=6", 11) == -1, "decode accepted an embedded NUL");
	CHECK(sysmon_decode("cpu=5\0", 6) == -1, "decode accepted a trailing NUL");
	memset(buf, ' ', sizeof buf);
	memcpy(buf, "cpu=5", 5);
	CHECK(sysmon_decode(buf, SYSMON_MAXLEN) == 5, "decode rejected a payload of the maximum length");
	CHECK(sysmon_decode(buf, SYSMON_MAXLEN + 1) == -1, "decode accepted an oversized payload");
	CHECK(sysmon_decode(buf, (unsigned long)-1) == -1, "decode accepted a huge length");

	for (i = 0; i <= 100; i++) {
		n = sysmon_encode(buf, sizeof buf, (int)i);
		CHECK(n > 0 && sysmon_decode(buf, (unsigned long)n) == (int)i, "encode/decode of %zu", i);
	}
	CHECK(sysmon_encode(buf, sizeof buf, -1) == -1, "encode accepted -1");
	CHECK(sysmon_encode(buf, sizeof buf, 101) == -1, "encode accepted 101");
	CHECK(sysmon_encode(buf, 5, 42) == -1, "encode overran a small buffer");
}

int
main(void)
{
	test_parse();
	test_percent();
	test_payload();
	if (failures)
		return EXIT_FAILURE;
	puts("sysmon regression checks passed");
	return EXIT_SUCCESS;
}
