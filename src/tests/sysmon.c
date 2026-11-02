/*
 * Checks for the sampler/dwm contract: /proc/stat parsing, CPU usage, the
 * property payload, and the sampler's temperature and fan sources in fake
 * sysfs trees.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../sensors.h"

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

/* The payload decoder dwm had before temperatures and fans, kept to check a new sampler against it. */
static int
legacy_decode(const char *data, unsigned long len)
{
	char buf[32 + 1];
	const char *p;
	int v, digits;

	if (!data || len == 0 || len > 32)
		return -1;
	memcpy(buf, data, len);
	buf[len] = '\0';
	if (strlen(buf) != len)
		return -1;
	for (p = buf; *p;) {
		if (!strncmp(p, "cpu=", 4)) {
			for (p += 4, v = digits = 0; *p >= '0' && *p <= '9' && digits < 3; p++, digits++)
				v = v * 10 + *p - '0';
			if (!digits || v > 100 || (*p && *p != ' '))
				return -1;
			return v;
		}
		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}
	return -1;
}

/* "cpu temp fan,fan /sample,sample" with -1 for a missing value, as a decoded payload is compared */
static void
values_text(const SysmonValues *v, char *buf, size_t size)
{
	int i;

	snprintf(buf, size, "%d %d", v->cpu, v->temp);
	for (i = 0; i < v->nfan; i++)
		snprintf(buf + strlen(buf), size - strlen(buf), "%c%d", i ? ',' : ' ', v->fan[i]);
	for (i = 0; i < v->nhist; i++)
		snprintf(buf + strlen(buf), size - strlen(buf), "%s%d", i ? "," : " /", v->hist[i]);
}

static void
test_payload(void)
{
	static const struct { const char *text; const char *want; } cases[] = {
		{ "cpu=0", "0 -1" }, { "cpu=7", "7 -1" }, { "cpu=42", "42 -1" }, { "cpu=100", "100 -1" },
		{ "cpu=042", "42 -1" }, { "cpu=5 gpu=9", "5 -1" }, { "gpu=9 cpu=5", "5 -1" }, { "x cpu=5", "5 -1" },
		{ "cpu=5  ", "5 -1" }, { " cpu=5", "5 -1" },
		{ "cpu=42 temp=55 fan=2400,2350", "42 55 2400,2350" },
		{ "fan=2400 temp=55 cpu=42", "42 55 2400" }, { "temp=0", "-1 0" }, { "temp=150", "-1 150" },
		{ "fan=0", "-1 -1 0" }, { "fan=30000", "-1 -1 30000" }, { "fan=1,2,3,4", "-1 -1 1,2,3,4" },
		{ "cpu=42 gpu=9 temp=55 x=y fan=2400", "42 55 2400" },
		/* a malformed, out-of-range or repeated field is missing; the others stay */
		{ "", "-1 -1" }, { "cpu", "-1 -1" }, { "cpu=", "-1 -1" }, { "cpu= 5", "-1 -1" }, { "cpu=101", "-1 -1" },
		{ "cpu=1000", "-1 -1" }, { "cpu=-5", "-1 -1" }, { "cpu=+5", "-1 -1" }, { "cpu=5x", "-1 -1" },
		{ "cpu=5.5", "-1 -1" }, { "cpu=abc", "-1 -1" }, { "CPU=5", "-1 -1" }, { "xcpu=5", "-1 -1" },
		{ "cpu:5", "-1 -1" }, { "gpu=5", "-1 -1" }, { "cpu=5,6", "-1 -1" }, { "cpu=0x10", "-1 -1" },
		{ "\377\377\377", "-1 -1" }, { "cpu=\033[2J", "-1 -1" },
		{ "cpu=5 cpu=6", "-1 -1" }, { "cpu=5 temp=50 temp=50", "5 -1" }, { "cpu=5 fan=1 fan=1", "5 -1" },
		{ "cpu=42 temp=151 fan=2400", "42 -1 2400" }, { "cpu=42 temp=1000", "42 -1" },
		{ "cpu=42 temp=-5", "42 -1" }, { "cpu=42 temp=55C", "42 -1" }, { "cpu=42 temp=", "42 -1" },
		{ "cpu=42 TEMP=55", "42 -1" }, { "cpu=42 temp=5.5", "42 -1" },
		{ "cpu=42 temp=55 fan=30001", "42 55" }, { "cpu=42 fan=100000", "42 -1" }, { "cpu=42 fan=", "42 -1" },
		{ "cpu=42 fan=1,", "42 -1" }, { "cpu=42 fan=,1", "42 -1" }, { "cpu=42 fan=1,,2", "42 -1" },
		{ "cpu=42 fan=1,2,3,4,5", "42 -1" }, { "cpu=42 fan=-1", "42 -1" }, { "cpu=42 fan=1;2", "42 -1" },
		{ "cpu=42 fan=1 2", "42 -1 1" }, { "cpu=42 fan=2400rpm", "42 -1" }, { "cpu=x temp=55 fan=9", "-1 55 9" },
		/* the CPU usage history: up to SYSMON_HIST samples of 0-100, all or nothing */
		{ "cpu=42 cpuhist=40,42", "42 -1 /40,42" }, { "cpuhist=7", "-1 -1 /7" },
		{ "cpuhist=0,100,042,5", "-1 -1 /0,100,42,5" },
		{ "cpuhist=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", "-1 -1 /1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16" },
		{ "cpu=42 temp=55 fan=2400,2350 cpuhist=40,42", "42 55 2400,2350 /40,42" },
		{ "cpuhist=40,42 fan=2400 cpu=42", "42 -1 2400 /40,42" },
		{ "cpu=42 cpuhist=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17", "42 -1" },
		{ "cpu=42 cpuhist=", "42 -1" }, { "cpu=42 cpuhist", "42 -1" }, { "cpu=42 cpuhist=101", "42 -1" },
		{ "cpu=42 cpuhist=40,101", "42 -1" }, { "cpu=42 cpuhist=1000", "42 -1" }, { "cpu=42 cpuhist=0042", "42 -1" },
		{ "cpu=42 cpuhist=-1", "42 -1" }, { "cpu=42 cpuhist=+1", "42 -1" }, { "cpu=42 cpuhist=1,", "42 -1" },
		{ "cpu=42 cpuhist=,1", "42 -1" }, { "cpu=42 cpuhist=1,,2", "42 -1" }, { "cpu=42 cpuhist=1;2", "42 -1" },
		{ "cpu=42 cpuhist=1.5", "42 -1" }, { "cpu=42 cpuhist=4x", "42 -1" }, { "cpu=42 cpuhist=0x10", "42 -1" },
		{ "cpu=42 CPUHIST=1", "42 -1" }, { "cpu=42 cpuhist:1", "42 -1" }, { "cpu=42 xcpuhist=1", "42 -1" },
		{ "cpu=42 cpuhist=1 cpuhist=1", "42 -1" }, { "cpu=42 cpuhist=1 2", "42 -1 /1" },
		{ "cpu=4x2 temp=55 cpuhist=40,42", "-1 55 /40,42" }, { "cpu=42 temp=55 cpuhist=4,,2", "42 55" },
	};
	char buf[SYSMON_MAXLEN + 64], exact[SYSMON_MAXLEN + 64], got[128];
	SysmonValues v, back;
	size_t i;
	int n, rc, cpu, temp, fans, hist;

	for (i = 0; i < LENGTH(cases); i++) {
		size_t len = strlen(cases[i].text);

		/* decode gets exactly len bytes, as X hands them over: no terminator to lean on */
		memset(exact, 'z', sizeof exact);
		memcpy(exact, cases[i].text, len);
		rc = sysmon_decode(exact, len, &v);
		values_text(&v, got, sizeof got);
		CHECK(!strcmp(got, cases[i].want), "decode(\"%s\") = \"%s\", want \"%s\"", cases[i].text, got, cases[i].want);
		CHECK((rc == 0) == (strcmp(cases[i].want, "-1 -1") != 0), "decode(\"%s\") returned %d", cases[i].text, rc);
	}
	CHECK(sysmon_decode(NULL, 5, &v) == -1 && v.cpu == -1, "decode accepted a NULL payload");
	CHECK(sysmon_decode("cpu=5", 0, &v) == -1 && v.cpu == -1, "decode accepted an empty payload");
	CHECK(sysmon_decode("cpu=5\0cpu=6", 11, &v) == -1 && v.cpu == -1, "decode accepted an embedded NUL");
	CHECK(sysmon_decode("cpu=5\0", 6, &v) == -1 && v.cpu == -1, "decode accepted a trailing NUL");
	memset(buf, ' ', sizeof buf);
	memcpy(buf, "cpu=5", 5);
	CHECK(sysmon_decode(buf, SYSMON_MAXLEN, &v) == 0 && v.cpu == 5, "decode rejected a payload of the maximum length");
	CHECK(sysmon_decode(buf, SYSMON_MAXLEN + 1, &v) == -1 && v.cpu == -1, "decode accepted an oversized payload");
	CHECK(sysmon_decode(buf, (unsigned long)-1, &v) == -1 && v.cpu == -1, "decode accepted a huge length");
	/* a failed decode leaves nothing of an earlier one behind */
	sysmon_decode("cpu=5 temp=50 fan=1,2", 21, &v);
	CHECK(sysmon_decode("junk", 4, &v) == -1 && v.cpu == -1 && v.temp == -1 && v.nfan == 0, "decode kept old values");

	/* every combination of values goes through encode and decode unchanged */
	for (cpu = -1; cpu <= 100; cpu += 101 / 3)
		for (temp = -1; temp <= SYSMON_MAXTEMP; temp += 38)
			for (fans = 0; fans <= SYSMON_FANS; fans++)
				for (hist = 0; hist <= SYSMON_HIST; hist += SYSMON_HIST / 2 - 1) {
					sysmon_clear(&v);
					v.cpu = cpu;
					v.temp = temp;
					for (v.nfan = 0; v.nfan < fans; v.nfan++)
						v.fan[v.nfan] = v.nfan * (SYSMON_MAXFAN / 3);
					for (v.nhist = 0; v.nhist < hist; v.nhist++)
						v.hist[v.nhist] = v.nhist * 100 / (SYSMON_HIST - 1);
					n = sysmon_encode(buf, SYSMON_MAXLEN + 1, &v);
					CHECK(n >= 0 && n <= SYSMON_MAXLEN, "encode of %d/%d/%d fans/%d samples gave %d",
					      cpu, temp, fans, hist, n);
					if (n == 0) {
						CHECK(cpu < 0 && temp < 0 && !fans && !hist, "encode left out values");
						continue;
					}
					CHECK(sysmon_decode(buf, (unsigned long)n, &back) == 0 && !memcmp(&v, &back, sizeof v),
					      "\"%s\" did not decode to what was encoded", buf);
				}
	/* the longest payload fits */
	sysmon_clear(&v);
	v.cpu = 100;
	v.temp = SYSMON_MAXTEMP;
	for (v.nfan = 0; v.nfan < SYSMON_FANS; v.nfan++)
		v.fan[v.nfan] = SYSMON_MAXFAN;
	for (v.nhist = 0; v.nhist < SYSMON_HIST; v.nhist++)
		v.hist[v.nhist] = 100;
	n = sysmon_encode(buf, SYSMON_MAXLEN + 1, &v);
	CHECK(n > 0 && n <= SYSMON_MAXLEN && sysmon_decode(buf, (unsigned long)n, &back) == 0
	      && !memcmp(&v, &back, sizeof v), "the longest payload (%d bytes) does not fit", n);
	/* a buffer one byte short of the longest payload overran or left something out */
	CHECK(sysmon_encode(buf, (size_t)n, &v) == -1, "encode overran a buffer one byte short of the history");
	v.nhist = 0;
	n = sysmon_encode(buf, sizeof buf, &v);
	CHECK(sysmon_encode(buf, (size_t)n, &v) == -1, "encode overran a buffer one byte short");
	CHECK(sysmon_encode(buf, 0, &v) == -1, "encode wrote to an empty buffer");
	/* values out of range are left out, never sent */
	sysmon_clear(&v);
	v.cpu = 101;
	v.temp = SYSMON_MAXTEMP + 1;
	v.nfan = 3;
	v.fan[0] = -1;
	v.fan[1] = 1200;
	v.fan[2] = SYSMON_MAXFAN + 1;
	n = sysmon_encode(buf, sizeof buf, &v);
	CHECK(n > 0 && !strcmp(buf, "fan=1200"), "encode sent out-of-range values: \"%s\"", buf);
	v.cpu = -1;
	v.temp = -1;
	v.nfan = 0;
	CHECK(sysmon_encode(buf, sizeof buf, &v) == 0 && !buf[0], "encode of nothing was not empty");
	CHECK(sysmon_encode(buf, 5, &(SysmonValues){ .cpu = 42, .temp = -1 }) == -1, "encode overran a small buffer");
	/* a history with a sample out of range, or too many samples, is left out, never sent in part */
	sysmon_clear(&v);
	v.cpu = 42;
	v.nhist = 3;
	v.hist[0] = 40;
	v.hist[1] = 101;
	v.hist[2] = 42;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42"), "encode sent a bad history: \"%s\"", buf);
	v.hist[1] = -1;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42"), "encode sent a bad history: \"%s\"", buf);
	v.hist[1] = 41;
	v.nhist = SYSMON_HIST + 1;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42"), "encode sent an overlong history: \"%s\"", buf);
	v.nhist = -1;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42"), "encode sent a negative history: \"%s\"", buf);
	v.nhist = 3;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42 cpuhist=40,41,42"), "encode of a history: \"%s\"", buf);

	/* old and new: a dwm from before temperatures and fans still reads the CPU of a new sampler */
	sysmon_clear(&v);
	v.cpu = 100;
	v.temp = 99;
	v.nfan = 2;
	v.fan[0] = v.fan[1] = 9999;
	n = sysmon_encode(buf, sizeof buf, &v);
	CHECK(legacy_decode(buf, (unsigned long)n) == 100, "an older dwm cannot read the CPU of \"%s\"", buf);
	v.nfan = 0;
	v.temp = -1;
	n = sysmon_encode(buf, sizeof buf, &v);
	CHECK(!strcmp(buf, "cpu=100") && legacy_decode(buf, (unsigned long)n) == 100, "a CPU-only payload changed: \"%s\"", buf);
	/* and a new dwm reads an old sampler */
	CHECK(sysmon_decode("cpu=37", 6, &v) == 0 && v.cpu == 37 && v.temp == -1 && !v.nfan, "an old sampler's payload is not read");
}

/* The sampler's history: the latest SYSMON_HIST readings, oldest first, cleared by a missing one. */
static void
test_push(void)
{
	SysmonValues v, want;
	int i;

	sysmon_clear(&v);
	sysmon_push(&v, 7);
	CHECK(v.cpu == 7 && v.nhist == 1 && v.hist[0] == 7, "the first reading is not the history");
	for (i = 1; i < SYSMON_HIST + 5; i++)
		sysmon_push(&v, i * 3);
	CHECK(v.cpu == (SYSMON_HIST + 4) * 3 && v.nhist == SYSMON_HIST, "a full history has %d samples", v.nhist);
	for (i = 0; i < SYSMON_HIST; i++)
		CHECK(v.hist[i] == (i + 5) * 3, "sample %d of a full history is %d, want %d", i, v.hist[i], (i + 5) * 3);
	v.temp = 55;
	v.nfan = 1;
	v.fan[0] = 2400;
	sysmon_push(&v, -1);
	sysmon_clear(&want);
	want.temp = 55;
	want.nfan = 1;
	want.fan[0] = 2400;
	CHECK(!memcmp(&v, &want, sizeof v), "a missing reading did not clear the CPU usage and its history alone");
	sysmon_push(&v, 100);
	sysmon_push(&v, 0);
	sysmon_push(&v, 101);
	CHECK(v.cpu == -1 && v.nhist == 0, "a reading out of range was kept");
	sysmon_push(&v, 0);
	CHECK(v.cpu == 0 && v.nhist == 1 && v.hist[0] == 0, "the history did not start again after a missing reading");
}

/* Fake sysfs trees below a scratch directory. */
static char sys[256];

static void
mkparents(const char *path)
{
	char buf[512], *p;

	snprintf(buf, sizeof buf, "%s", path);
	for (p = buf + 1; (p = strchr(p, '/')); p++) {
		*p = '\0';
		mkdir(buf, 0755);
		*p = '/';
	}
}

/* Write an attribute below sys; NULL makes it a directory, which cannot be read like a broken attribute. */
static void
put(const char *rel, const char *text)
{
	char path[512];
	FILE *fp;

	snprintf(path, sizeof path, "%s/%s", sys, rel);
	mkparents(path);
	unlink(path);
	rmdir(path);
	if (!text) {
		mkdir(path, 0755);
		return;
	}
	if ((fp = fopen(path, "w"))) {
		fputs(text, fp);
		fclose(fp);
	}
}

/* A hwmon chip as the kernel shows it: class/hwmon/<hwmon> links to a device directory. */
static void
chip(const char *hwmon, const char *name)
{
	char rel[128], link[512], target[128];

	snprintf(rel, sizeof rel, "devices/%s/name", hwmon);
	put(rel, name);
	snprintf(link, sizeof link, "%s/class/hwmon/%s", sys, hwmon);
	snprintf(target, sizeof target, "../../devices/%s", hwmon);
	mkparents(link);
	if (symlink(target, link) < 0)
		CHECK(0, "cannot link %s", link);
}

/* An attribute of a chip made with chip() */
static void
attr(const char *hwmon, const char *name, const char *text)
{
	char rel[128];

	snprintf(rel, sizeof rel, "devices/%s/%s", hwmon, name);
	put(rel, text);
}

static void
reset(void)
{
	char cmd[300];

	snprintf(cmd, sizeof cmd, "rm -rf '%s'", sys);
	if (system(cmd) != 0)
		CHECK(0, "cannot clear %s", sys);
	mkdir(sys, 0755);
}

/* Find and read the sources of the current tree as "temp fan,fan". */
static const char *
sample(int *rc)
{
	static char text[64];
	Sensors s;
	SysmonValues v;

	sysmon_clear(&v);
	sensors_init(&s);
	sensors_find(&s, sys);
	*rc = sensors_read(&s, &v);
	values_text(&v, text, sizeof text);
	return text + 3; /* past "-1 " of the CPU */
}

#define EXPECT(want, wantrc, what) do { \
	int rc_; \
	const char *got_ = sample(&rc_); \
	CHECK(!strcmp(got_, want) && rc_ == (wantrc), "%s: \"%s\" (%d), want \"%s\" (%d)", \
	      what, got_, rc_, want, wantrc); \
} while (0)

/* A Dell laptop: sparse coretemp numbering, the same two fans on a fast and a slow chip, unrelated chips. */
static void
dell_laptop(void)
{
	chip("hwmon0", "ACAD\n");
	chip("hwmon1", "coretemp\n");
	attr("hwmon1", "temp1_input", "52000\n");
	attr("hwmon1", "temp1_label", "Package id 0\n");
	attr("hwmon1", "temp2_input", "50000\n");
	attr("hwmon1", "temp2_label", "Core 0\n");
	attr("hwmon1", "temp6_input", "61000\n");
	attr("hwmon1", "temp6_label", "Core 4\n");
	attr("hwmon1", "temp33_input", "64000\n");
	attr("hwmon1", "temp33_label", "Core 31\n");
	chip("hwmon2", "dell_ddv\n");
	attr("hwmon2", "fan1_input", "2401\n");
	attr("hwmon2", "fan2_input", "2351\n");
	attr("hwmon2", "temp1_input", "45000\n");
	chip("hwmon3", "dell_smm\n");
	attr("hwmon3", "fan1_input", "2400\n");
	attr("hwmon3", "fan2_input", "2350\n");
	attr("hwmon3", "fan1_label", NULL); /* the driver answers EINVAL */
	attr("hwmon3", "temp1_input", "70000\n");
	chip("hwmon4", "nvme\n");
	attr("hwmon4", "temp1_input", "40000\n");
	chip("hwmon5", "iwlwifi_1\n");
	attr("hwmon5", "temp1_input", "41000\n");
	chip("hwmon10", "BAT0\n");
	chip("hwmon11", "amdgpu\n");
	attr("hwmon11", "fan1_input", "1234\n");
	attr("hwmon11", "temp1_input", "80000\n");
	put("class/thermal/thermal_zone0/type", "x86_pkg_temp\n");
	put("class/thermal/thermal_zone0/temp", "99000\n");
}

static void
test_sensors(void)
{
	static const struct { const char *text; const char *want; } temps[] = {
		{ "52000\n", "52" }, { "52000", "52" }, { "52499\n", "52" }, { "52500\n", "53" },
		{ "150000\n", "150" }, { "1\n", "0" },
		/* malformed or implausible readings are missing */
		{ "", "-1" }, { "\n", "-1" }, { "abc\n", "-1" }, { "52x\n", "-1" }, { "52000\n7\n", "-1" },
		{ " 52000\n", "-1" }, { "+52000\n", "-1" }, { "-5000\n", "-1" }, { "0\n", "-1" }, { "-\n", "-1" },
		{ "150001\n", "-1" }, { "999999999\n", "-1" }, { "99999999999999999999\n", "-1" }, { "52.5\n", "-1" },
		{ "52000 \n", "-1" }, { "52000 ", "-1" }, { "52000\r\n", "-1" },
		{ "000000000000000000000000000000000000000052000\n", "-1" }, /* longer than any reading */
	};
	static const struct { const char *text; const char *want; } fans[] = {
		{ "0\n", "-1 0" }, { "2400\n", "-1 2400" }, { "30000\n", "-1 30000" },
		{ "30001\n", "-1" }, { "65535\n", "-1" }, { "-1\n", "-1" }, { "fast\n", "-1" }, { "", "-1" },
	};
	char dir[] = "/tmp/dwm-sensors-XXXXXX", rel[512];
	Sensors s;
	SysmonValues v;
	size_t i;

	if (!mkdtemp(dir)) {
		CHECK(0, "cannot create a scratch directory");
		return;
	}
	snprintf(sys, sizeof sys, "%s/sys", dir);

	/* nothing at all, as in WSL2 or a container: no values, and no failure */
	reset();
	EXPECT("-1", 0, "an empty tree");
	snprintf(sys, sizeof sys, "%s/missing", dir);
	EXPECT("-1", 0, "a missing tree");
	snprintf(sys, sizeof sys, "%s/sys", dir);
	chip("hwmon0", "ACAD\n");
	put("class/thermal/thermal_zone0/type", "acpitz\n");
	put("class/thermal/thermal_zone0/temp", "45000\n");
	EXPECT("-1", 0, "a tree without CPU sensors or fans");

	/* the Dell laptop: package temperature, both fans once and from the fast chip */
	reset();
	dell_laptop();
	EXPECT("52 2401,2351", 0, "the Dell laptop");
	/* only the slow chip has the fans: then it is read */
	snprintf(rel, sizeof rel, "%s/devices/hwmon2/fan1_input", sys);
	unlink(rel);
	snprintf(rel, sizeof rel, "%s/devices/hwmon2/fan2_input", sys);
	unlink(rel);
	EXPECT("52 2400,2350", 0, "fans only on the slow chip");
	/* one fan, then none */
	snprintf(rel, sizeof rel, "%s/devices/hwmon3/fan2_input", sys);
	unlink(rel);
	EXPECT("52 2400", 0, "one fan");
	snprintf(rel, sizeof rel, "%s/devices/hwmon3/fan1_input", sys);
	unlink(rel);
	EXPECT("52", 0, "no fans");

	/* a fan that fails to read is missing, the others stay, and the read reports it */
	reset();
	dell_laptop();
	attr("hwmon2", "fan2_input", NULL);
	EXPECT("52 2401", -1, "a failing fan");
	attr("hwmon2", "fan1_input", NULL);
	EXPECT("52", -1, "every fan failing");
	attr("hwmon1", "temp1_input", NULL);
	EXPECT("-1", -1, "a failing temperature");

	/* labels: the package sensor wherever it is, else the first; unreadable labels are no labels */
	reset();
	chip("hwmon7", "coretemp\n");
	attr("hwmon7", "temp2_input", "50000\n");
	attr("hwmon7", "temp2_label", "Core 0\n");
	attr("hwmon7", "temp10_input", "57000\n");
	attr("hwmon7", "temp10_label", "Package id 0\n");
	attr("hwmon7", "temp3_input", "60000\n");
	attr("hwmon7", "temp3_label", "Core 1\n");
	EXPECT("57", 0, "the package label on a later input");
	attr("hwmon7", "temp10_label", NULL);
	attr("hwmon7", "temp2_label", NULL);
	attr("hwmon7", "temp3_label", NULL);
	EXPECT("50", 0, "unreadable labels");
	attr("hwmon7", "temp10_label", "");
	EXPECT("50", 0, "an empty label");

	/* AMD: the die temperature over the control temperature and the CCDs */
	reset();
	chip("hwmon2", "k10temp\n");
	attr("hwmon2", "temp1_input", "70000\n");
	attr("hwmon2", "temp1_label", "Tctl\n");
	attr("hwmon2", "temp2_input", "60000\n");
	attr("hwmon2", "temp2_label", "Tdie\n");
	attr("hwmon2", "temp3_input", "80000\n");
	attr("hwmon2", "temp3_label", "Tccd1\n");
	EXPECT("60", 0, "k10temp with Tdie");
	attr("hwmon2", "temp2_label", "Tccd2\n");
	EXPECT("70", 0, "k10temp with Tctl only");
	chip("hwmon3", "zenpower\n");
	attr("hwmon3", "temp1_input", "90000\n");
	attr("hwmon3", "temp1_label", "Tdie\n");
	EXPECT("70", 0, "k10temp ranks above zenpower");
	chip("hwmon1", "coretemp\n");
	attr("hwmon1", "temp1_input", "40000\n");
	EXPECT("40", 0, "coretemp ranks above k10temp");
	reset();
	chip("hwmon3", "zenpower\n");
	attr("hwmon3", "temp1_input", "90000\n");
	attr("hwmon3", "temp1_label", "Tdie\n");
	EXPECT("90", 0, "zenpower");
	reset();
	chip("hwmon0", "cpu_thermal\n");
	attr("hwmon0", "temp1_input", "48000\n");
	EXPECT("48", 0, "an ARM board");

	/* two packages: the hotter one */
	reset();
	chip("hwmon1", "coretemp\n");
	attr("hwmon1", "temp1_input", "52000\n");
	attr("hwmon1", "temp1_label", "Package id 0\n");
	chip("hwmon2", "coretemp\n");
	attr("hwmon2", "temp1_input", "58000\n");
	attr("hwmon2", "temp1_label", "Package id 1\n");
	EXPECT("58", 0, "two packages");
	attr("hwmon2", "temp1_input", NULL);
	EXPECT("52", -1, "two packages, one failing");

	/* no CPU chip: the package thermal zone, never another zone */
	reset();
	chip("hwmon0", "nvme\n");
	attr("hwmon0", "temp1_input", "40000\n");
	put("class/thermal/thermal_zone0/type", "acpitz\n");
	put("class/thermal/thermal_zone0/temp", "45000\n");
	put("class/thermal/thermal_zone1/type", "x86_pkg_temp\n");
	put("class/thermal/thermal_zone1/temp", "57000\n");
	put("class/thermal/thermal_zone2/type", NULL);
	EXPECT("57", 0, "the package thermal zone");
	put("class/thermal/thermal_zone1/temp", "garbage\n");
	EXPECT("-1", -1, "a malformed thermal zone");

	/* readings */
	for (i = 0; i < LENGTH(temps); i++) {
		reset();
		chip("hwmon0", "coretemp\n");
		attr("hwmon0", "temp1_input", temps[i].text);
		EXPECT(temps[i].want, strcmp(temps[i].want, "-1") ? 0 : -1, temps[i].text);
	}
	for (i = 0; i < LENGTH(fans); i++) {
		reset();
		chip("hwmon0", "thinkpad\n");
		attr("hwmon0", "fan1_input", fans[i].text);
		EXPECT(fans[i].want, strcmp(fans[i].want, "-1") ? 0 : -1, fans[i].text);
	}

	/* more chips than are looked at, and labels longer than any: no overrun */
	reset();
	for (i = 0; i < 40; i++) {
		char hwmon[16];

		snprintf(hwmon, sizeof hwmon, "hwmon%zu", i);
		chip(hwmon, "coretemp\n");
		attr(hwmon, "temp1_input", "52000\n");
		attr(hwmon, "temp1_label", "Package id 0 and a label that goes on far longer than any real one\n");
		attr(hwmon, "fan1_input", "1000\n");
	}
	EXPECT("52 1000,1000,1000,1000", 0, "forty chips");

	/* more fans than a payload holds: the first ones, by number and chip */
	reset();
	chip("hwmon0", "nct6775\n");
	attr("hwmon0", "fan10_input", "1000\n");
	attr("hwmon0", "fan2_input", "200\n");
	attr("hwmon0", "fan1_input", "100\n");
	chip("hwmon2", "thinkpad\n");
	attr("hwmon2", "fan1_input", "300\n");
	chip("hwmon10", "it87\n");
	attr("hwmon10", "fan1_input", "400\n");
	attr("hwmon10", "fan2_input", "500\n");
	EXPECT("-1 100,200,1000,300", 0, "more fans than fit");

	/* a sysfs root too long for a path: nothing, and no overflow */
	memset(sys, 'x', sizeof sys - 1);
	sys[0] = '/';
	sys[sizeof sys - 1] = '\0';
	EXPECT("-1", 0, "a root too long");
	/* a root that leaves room for the chip's name but not for its inputs: they are not used cut short */
	snprintf(sys, sizeof sys, "%s/", dir);
	while (strlen(sys) < SENSORS_PATHLEN - 28)
		strcat(sys, strlen(sys) % 50 ? "y" : "/");
	sys[strlen(sys) - 1] = 'y'; /* no trailing slash */
	reset();
	chip("hwmon0", "coretemp\n");
	attr("hwmon0", "temp1_input", "52000\n");
	attr("hwmon0", "fan1_input", "2000\n");
	{
		Sensors cut;

		sensors_init(&cut);
		sensors_find(&cut, sys);
		CHECK(cut.ntemp == 0 && cut.nfan == 0, "inputs were used with a path cut short (%d, %d)", cut.ntemp, cut.nfan);
	}
	EXPECT("-1", 0, "a root that leaves no room for the inputs");
	snprintf(sys, sizeof sys, "%s/sys", dir);

	/*
	 * Sources are looked for once; again only when one is missing or failed,
	 * and then at most every SENSORS_REFIND ms, so a machine without them
	 * does not walk sysfs at every read.
	 */
	reset();
	chip("hwmon1", "coretemp\n");
	attr("hwmon1", "temp1_input", "52000\n");
	sensors_init(&s);
	sensors_sample(&s, sys, 1000, &v);
	CHECK(v.temp == 52 && v.nfan == 0 && s.found == 1000 && s.stale, "the first sample did not look for sources");
	chip("hwmon2", "thinkpad\n");
	attr("hwmon2", "fan1_input", "2000\n");
	sensors_sample(&s, sys, 1000 + SENSORS_REFIND - 1, &v);
	CHECK(v.nfan == 0 && s.found == 1000, "sources were looked for again too soon");
	sensors_sample(&s, sys, 1000 + SENSORS_REFIND, &v);
	CHECK(v.temp == 52 && v.nfan == 1 && v.fan[0] == 2000 && !s.stale, "a fan that appeared was not found");
	attr("hwmon2", "fan2_input", "2100\n");
	sensors_sample(&s, sys, 1000 + 5 * SENSORS_REFIND, &v);
	CHECK(v.nfan == 1 && s.found == 1000 + SENSORS_REFIND, "complete sources were looked for again");
	attr("hwmon2", "fan1_input", NULL);
	sensors_sample(&s, sys, 1000 + 5 * SENSORS_REFIND + 1, &v);
	CHECK(v.nfan == 0 && v.temp == 52 && s.stale && s.found == 1000 + SENSORS_REFIND,
	      "a failing fan was not missing at once, or sources were looked for too soon");
	/* the failing fan went away, as after a module reload */
	snprintf(rel, sizeof rel, "%s/devices/hwmon2/fan1_input", sys);
	rmdir(rel);
	sensors_sample(&s, sys, 1000 + 6 * SENSORS_REFIND + 1, &v);
	CHECK(v.nfan == 1 && v.fan[0] == 2100 && !s.stale, "sources were not looked for again after a failure");
	sensors_init(&s);
	sensors_sample(&s, sys, 0, &v);
	CHECK(s.found == 0 && v.temp == 52, "sensors_init did not forget the sources");

	/* the cadence: never faster than SENSORS_INTERVAL, slow sources at most 5% of the time */
	CHECK(sensors_delay(0) == SENSORS_INTERVAL, "delay(0) = %lld", sensors_delay(0));
	CHECK(sensors_delay(4) == SENSORS_INTERVAL, "delay(4) = %lld", sensors_delay(4));
	CHECK(sensors_delay(SENSORS_INTERVAL / 20) == SENSORS_INTERVAL, "delay at the interval boundary");
	CHECK(sensors_delay(500) == 10000, "delay(500) = %lld", sensors_delay(500));
	CHECK(sensors_delay(SENSORS_MAXDELAY / 20) == SENSORS_MAXDELAY, "delay at the cap boundary");
	CHECK(sensors_delay(SENSORS_MAXDELAY / 20 + 1) == SENSORS_MAXDELAY, "delay above the cap");
	CHECK(sensors_delay(1LL << 60) == SENSORS_MAXDELAY, "delay of a huge read overflowed");

	snprintf(sys, sizeof sys, "%s", dir);
	reset();
	rmdir(dir);
}

int
main(void)
{
	test_parse();
	test_percent();
	test_payload();
	test_push();
	test_sensors();
	if (failures)
		return EXIT_FAILURE;
	puts("sysmon regression checks passed");
	return EXIT_SUCCESS;
}
