/*
 * Checks for the sampler/dwm contract: /proc/stat parsing, CPU usage, the
 * property payload, and the sampler's temperature, fan and GPU sources in fake
 * sysfs trees.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/perf_event.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
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

/*
 * A fake i915 PMU behind perf_event_open: a counter is a file in perfdir named
 * by its event config, holding its busy and enabled time in ns, which a read
 * returns as the kernel does. Opens fail with perferrno when it is set, as
 * without CAP_PERFMON.
 */
#define PERFTYPE 42 /* the fake PMU's perf event type */
#define PERFCPU 3 /* the first CPU of its cpumask */
static char perfdir[512];
static int perfopens, perferrno;
static int capsets, capsetzero; /* capset calls, and whether the last asked for no capability at all */

long
syscall(long number, ...)
{
	struct perf_event_attr *attr;
	char path[600];
	unsigned long flags;
	int pid, cpu, group;
	va_list ap;

	if (number == SYS_capset) {
		struct __user_cap_header_struct *head;
		struct __user_cap_data_struct *data;
		int i;

		va_start(ap, number);
		head = va_arg(ap, struct __user_cap_header_struct *);
		data = va_arg(ap, struct __user_cap_data_struct *);
		va_end(ap);
		capsets++;
		capsetzero = head->version == _LINUX_CAPABILITY_VERSION_3 && head->pid == 0;
		for (i = 0; i < _LINUX_CAPABILITY_U32S_3; i++)
			capsetzero = capsetzero && !data[i].effective && !data[i].permitted && !data[i].inheritable;
		return 0; /* this process has none to give up */
	}
	if (number != SYS_perf_event_open) {
		errno = ENOSYS;
		return -1;
	}
	va_start(ap, number);
	attr = va_arg(ap, struct perf_event_attr *);
	pid = va_arg(ap, int);
	cpu = va_arg(ap, int);
	group = va_arg(ap, int);
	flags = va_arg(ap, unsigned long);
	va_end(ap);
	perfopens++;
	if (perferrno) {
		errno = perferrno;
		return -1;
	}
	/* a system-wide counter of the PMU's type on its CPU, with the time it was enabled */
	if (attr->type != PERFTYPE || attr->size != sizeof *attr || pid != -1 || cpu != PERFCPU || group != -1
	|| attr->read_format != PERF_FORMAT_TOTAL_TIME_ENABLED || !(flags & PERF_FLAG_FD_CLOEXEC)) {
		errno = EINVAL;
		return -1;
	}
	snprintf(path, sizeof path, "%s/%llx", perfdir, (unsigned long long)attr->config);
	return open(path, O_RDONLY | O_CLOEXEC);
}

/* Set a fake counter's busy and enabled time; its open descriptors see the change. */
static void
counter(unsigned long long config, unsigned long long busy, unsigned long long enabled)
{
	char path[600];
	FILE *fp;

	snprintf(path, sizeof path, "%s/%llx", perfdir, config);
	if ((fp = fopen(path, "w"))) {
		fprintf(fp, "%llu %llu", busy, enabled);
		fclose(fp);
	}
}

/* Open descriptors of this process. */
static int
openfds(void)
{
	struct dirent *e;
	DIR *d;
	int n = 0;

	if (!(d = opendir("/proc/self/fd")))
		return -1;
	while ((e = readdir(d)))
		n += e->d_name[0] != '.';
	closedir(d);
	return n - 1; /* the directory's own */
}

/*
 * The sampler's reads go through here: a read of a GPU's usage, or of an Intel
 * GPU's counter, is counted, and while busy names the attribute it fails with
 * EBUSY, as amdgpu answers for a GPU that is not awake.
 */
static int gpureads;
static const char *busy;

ssize_t
read(int fd, void *buf, size_t size)
{
	char link[64], path[1024], text[64];
	unsigned long long v[2];
	const char *name;
	struct iovec iov;
	ssize_t n;

	snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
	if ((n = readlink(link, path, sizeof path - 1)) > 0) {
		path[n] = '\0';
		name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
		if (perfdir[0] && !strncmp(path, perfdir, strlen(perfdir)) && path[strlen(perfdir)] == '/') {
			gpureads++;
			n = pread(fd, text, sizeof text - 1, 0);
			text[n > 0 ? n : 0] = '\0';
			if (size < sizeof v || sscanf(text, "%llu %llu", &v[0], &v[1]) != 2) {
				errno = EIO;
				return -1;
			}
			memcpy(buf, v, sizeof v);
			return sizeof v;
		}
		if (!strcmp(name, "gpu_busy_percent"))
			gpureads++;
		if (busy && !strcmp(name, busy)) {
			errno = EBUSY;
			return -1;
		}
	}
	iov.iov_base = buf;
	iov.iov_len = size;
	return readv(fd, &iov, 1);
}

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

/*
 * "cpu temp fan,fan /sample,sample gpu=usage //sample,sample" with -1 for a
 * missing value, as a decoded payload is compared; the GPU only when it is there
 */
static void
values_text(const SysmonValues *v, char *buf, size_t size)
{
	int i;

	snprintf(buf, size, "%d %d", v->cpu, v->temp);
	for (i = 0; i < v->nfan; i++)
		snprintf(buf + strlen(buf), size - strlen(buf), "%c%d", i ? ',' : ' ', v->fan[i]);
	for (i = 0; i < v->nhist; i++)
		snprintf(buf + strlen(buf), size - strlen(buf), "%s%d", i ? "," : " /", v->hist[i]);
	if (v->gpu >= 0 || v->ngpuhist)
		snprintf(buf + strlen(buf), size - strlen(buf), " gpu=%d", v->gpu);
	for (i = 0; i < v->ngpuhist; i++)
		snprintf(buf + strlen(buf), size - strlen(buf), "%s%d", i ? "," : " //", v->gpuhist[i]);
}

static void
test_payload(void)
{
	static const struct { const char *text; const char *want; } cases[] = {
		{ "cpu=0", "0 -1" }, { "cpu=7", "7 -1" }, { "cpu=42", "42 -1" }, { "cpu=100", "100 -1" },
		{ "cpu=042", "42 -1" }, { "cpu=5 cpu2=9", "5 -1" }, { "cpu2=9 cpu=5", "5 -1" }, { "x cpu=5", "5 -1" },
		{ "cpu=5  ", "5 -1" }, { " cpu=5", "5 -1" },
		{ "cpu=42 temp=55 fan=2400,2350", "42 55 2400,2350" },
		{ "fan=2400 temp=55 cpu=42", "42 55 2400" }, { "temp=0", "-1 0" }, { "temp=150", "-1 150" },
		{ "fan=0", "-1 -1 0" }, { "fan=30000", "-1 -1 30000" }, { "fan=1,2,3,4", "-1 -1 1,2,3,4" },
		{ "cpu=42 vram=9 temp=55 x=y fan=2400", "42 55 2400" },
		/* a malformed, out-of-range or repeated field is missing; the others stay */
		{ "", "-1 -1" }, { "cpu", "-1 -1" }, { "cpu=", "-1 -1" }, { "cpu= 5", "-1 -1" }, { "cpu=101", "-1 -1" },
		{ "cpu=1000", "-1 -1" }, { "cpu=-5", "-1 -1" }, { "cpu=+5", "-1 -1" }, { "cpu=5x", "-1 -1" },
		{ "cpu=5.5", "-1 -1" }, { "cpu=abc", "-1 -1" }, { "CPU=5", "-1 -1" }, { "xcpu=5", "-1 -1" },
		{ "cpu:5", "-1 -1" }, { "vram=5", "-1 -1" }, { "cpu=5,6", "-1 -1" }, { "cpu=0x10", "-1 -1" },
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
		/* the GPU usage and its history, checked as the CPU's, each on its own */
		{ "gpu=0", "-1 -1 gpu=0" }, { "gpu=7", "-1 -1 gpu=7" }, { "gpu=100", "-1 -1 gpu=100" },
		{ "gpu=007", "-1 -1 gpu=7" }, { "cpu=5 gpu=9", "5 -1 gpu=9" }, { "gpu=9 cpu=5", "5 -1 gpu=9" },
		{ "gpu=7 gpuhist=5,7", "-1 -1 gpu=7 //5,7" }, { "gpuhist=5", "-1 -1 gpu=-1 //5" },
		{ "gpuhist=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", "-1 -1 gpu=-1 //1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16" },
		{ "cpu=42 temp=55 fan=2400,2350 cpuhist=40,42 gpu=7 gpuhist=5,7", "42 55 2400,2350 /40,42 gpu=7 //5,7" },
		{ "gpuhist=5,7 cpuhist=40,42 gpu=7 cpu=42", "42 -1 /40,42 gpu=7 //5,7" },
		{ "cpu=42 gpu=", "42 -1" }, { "cpu=42 gpu", "42 -1" }, { "cpu=42 gpu=101", "42 -1" },
		{ "cpu=42 gpu=1000", "42 -1" }, { "cpu=42 gpu=-1", "42 -1" }, { "cpu=42 gpu=+1", "42 -1" },
		{ "cpu=42 gpu=4x", "42 -1" }, { "cpu=42 gpu=4.5", "42 -1" }, { "cpu=42 gpu=4,5", "42 -1" },
		{ "cpu=42 GPU=4", "42 -1" }, { "cpu=42 gpu:4", "42 -1" }, { "cpu=42 xgpu=4", "42 -1" },
		{ "cpu=42 gpu=4 gpu=4", "42 -1" }, { "cpu=42 gpu=0x10", "42 -1" }, { "gpu=\033[2J", "-1 -1" },
		{ "cpu=42 gpu=7 gpuhist=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17", "42 -1 gpu=7" },
		{ "cpu=42 gpu=7 gpuhist=", "42 -1 gpu=7" }, { "cpu=42 gpu=7 gpuhist=101", "42 -1 gpu=7" },
		{ "cpu=42 gpu=7 gpuhist=0042", "42 -1 gpu=7" }, { "cpu=42 gpu=7 gpuhist=-1", "42 -1 gpu=7" },
		{ "cpu=42 gpu=7 gpuhist=1,", "42 -1 gpu=7" }, { "cpu=42 gpu=7 gpuhist=,1", "42 -1 gpu=7" },
		{ "cpu=42 gpu=7 gpuhist=1,,2", "42 -1 gpu=7" }, { "cpu=42 gpu=7 gpuhist=1;2", "42 -1 gpu=7" },
		{ "cpu=42 gpu=7 gpuhist=4x", "42 -1 gpu=7" }, { "cpu=42 gpu=7 GPUHIST=1", "42 -1 gpu=7" },
		{ "cpu=42 gpu=7 gpuhist=1 gpuhist=1", "42 -1 gpu=7" }, { "cpu=42 gpu=7 gpuhist=1 2", "42 -1 gpu=7 //1" },
		{ "cpu=42 gpu=7x gpuhist=5,7", "42 -1 gpu=-1 //5,7" }, { "cpu=42 cpuhist=1,,2 gpu=7 gpuhist=5", "42 -1 gpu=7 //5" },
		{ "cpu=42 cpuhist=40,42 gpu=7 gpuhist=5,,7", "42 -1 /40,42 gpu=7" },
	};
	char buf[SYSMON_MAXLEN + 64], exact[SYSMON_MAXLEN + 64], got[256];
	SysmonValues v, back;
	size_t i;
	int n, rc, cpu, temp, fans, hist, gpu, gpuhist;

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
				for (hist = 0; hist <= SYSMON_HIST; hist += SYSMON_HIST / 2 - 1)
					for (gpu = -1; gpu <= 100; gpu += 101 / 2)
						for (gpuhist = 0; gpuhist <= SYSMON_HIST; gpuhist += SYSMON_HIST / 2 - 1) {
							sysmon_clear(&v);
							v.cpu = cpu;
							v.temp = temp;
							for (v.nfan = 0; v.nfan < fans; v.nfan++)
								v.fan[v.nfan] = v.nfan * (SYSMON_MAXFAN / 3);
							for (v.nhist = 0; v.nhist < hist; v.nhist++)
								v.hist[v.nhist] = v.nhist * 100 / (SYSMON_HIST - 1);
							v.gpu = gpu;
							for (v.ngpuhist = 0; v.ngpuhist < gpuhist; v.ngpuhist++)
								v.gpuhist[v.ngpuhist] = 100 - v.ngpuhist * 100 / (SYSMON_HIST - 1);
							n = sysmon_encode(buf, SYSMON_MAXLEN + 1, &v);
							CHECK(n >= 0 && n <= SYSMON_MAXLEN, "encode of %d/%d/%d fans/%d samples/%d/%d samples gave %d",
							      cpu, temp, fans, hist, gpu, gpuhist, n);
							if (n == 0) {
								CHECK(cpu < 0 && temp < 0 && !fans && !hist && gpu < 0 && !gpuhist,
								      "encode left out values");
								continue;
							}
							CHECK(sysmon_decode(buf, (unsigned long)n, &back) == 0 && !memcmp(&v, &back, sizeof v),
							      "\"%s\" did not decode to what was encoded", buf);
						}
	/* the longest payload fits, and without a GPU it still fits the 128 bytes a dwm from before the GPU reads */
	sysmon_clear(&v);
	v.cpu = 100;
	v.temp = SYSMON_MAXTEMP;
	for (v.nfan = 0; v.nfan < SYSMON_FANS; v.nfan++)
		v.fan[v.nfan] = SYSMON_MAXFAN;
	for (v.nhist = 0; v.nhist < SYSMON_HIST; v.nhist++)
		v.hist[v.nhist] = 100;
	n = sysmon_encode(buf, SYSMON_MAXLEN + 1, &v);
	CHECK(n > 0 && n <= 128, "the longest payload without a GPU (%d bytes) does not fit an older dwm", n);
	v.gpu = 100;
	for (v.ngpuhist = 0; v.ngpuhist < SYSMON_HIST; v.ngpuhist++)
		v.gpuhist[v.ngpuhist] = 100;
	n = sysmon_encode(buf, SYSMON_MAXLEN + 1, &v);
	CHECK(n > 0 && n <= SYSMON_MAXLEN && sysmon_decode(buf, (unsigned long)n, &back) == 0
	      && !memcmp(&v, &back, sizeof v), "the longest payload (%d bytes) does not fit", n);
	/* a buffer one byte short of the longest payload overran or left something out */
	CHECK(sysmon_encode(buf, (size_t)n, &v) == -1, "encode overran a buffer one byte short of the history");
	v.ngpuhist = 0;
	n = sysmon_encode(buf, sizeof buf, &v);
	CHECK(sysmon_encode(buf, (size_t)n, &v) == -1, "encode overran a buffer one byte short of the GPU usage");
	v.gpu = -1;
	n = sysmon_encode(buf, sizeof buf, &v);
	CHECK(sysmon_encode(buf, (size_t)n, &v) == -1, "encode overran a buffer one byte short of the CPU history");
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
	/* the same for the GPU usage and its history, which leave the others alone */
	v.gpu = 101;
	v.ngpuhist = 2;
	v.gpuhist[0] = 5;
	v.gpuhist[1] = 101;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42 cpuhist=40,41,42"), "encode sent a bad GPU usage: \"%s\"", buf);
	v.gpuhist[1] = -1;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42 cpuhist=40,41,42"), "encode sent a bad GPU history: \"%s\"", buf);
	v.gpuhist[1] = 7;
	v.ngpuhist = SYSMON_HIST + 1;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42 cpuhist=40,41,42"), "encode sent an overlong GPU history: \"%s\"", buf);
	v.ngpuhist = 2;
	v.gpu = 7;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "cpu=42 cpuhist=40,41,42 gpu=7 gpuhist=5,7"),
	      "encode of the GPU usage: \"%s\"", buf);
	v.cpu = -1;
	v.nhist = 0;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "gpu=7 gpuhist=5,7"), "encode of the GPU usage alone: \"%s\"", buf);
	v.gpu = -1;
	CHECK(sysmon_encode(buf, sizeof buf, &v) > 0 && !strcmp(buf, "gpuhist=5,7"), "encode of a GPU history alone: \"%s\"", buf);

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

	/* the GPU usage has a history of its own, and the two leave each other alone */
	sysmon_clear(&v);
	for (i = 0; i < SYSMON_HIST + 3; i++)
		sysmon_pushgpu(&v, i);
	sysmon_push(&v, 50);
	CHECK(v.gpu == SYSMON_HIST + 2 && v.ngpuhist == SYSMON_HIST && v.gpuhist[0] == 3
	      && v.gpuhist[SYSMON_HIST - 1] == SYSMON_HIST + 2 && v.cpu == 50 && v.nhist == 1,
	      "the GPU usage history is not the latest readings, or mixed with the CPU's");
	sysmon_pushgpu(&v, -1);
	CHECK(v.gpu == -1 && v.ngpuhist == 0 && v.gpuhist[0] == 0 && v.cpu == 50 && v.nhist == 1,
	      "a missing GPU reading did not clear the GPU usage and its history alone");
	sysmon_pushgpu(&v, 101);
	CHECK(v.gpu == -1 && v.ngpuhist == 0, "a GPU reading out of range was kept");
	sysmon_push(&v, -1);
	sysmon_pushgpu(&v, 9);
	CHECK(v.gpu == 9 && v.ngpuhist == 1 && v.cpu == -1 && v.nhist == 0, "a missing CPU reading cleared the GPU usage");
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

/* Remove an attribute below sys, as when the kernel does not have it. */
static void
drop(const char *rel)
{
	char path[512];

	snprintf(path, sizeof path, "%s/%s", sys, rel);
	unlink(path);
	rmdir(path);
}

/* A GPU in the DRM class: card is "cardN", status its runtime power state (NULL: none). */
static void
gpu(const char *card, const char *status)
{
	char rel[128];

	snprintf(rel, sizeof rel, "class/drm/%s/device/power/runtime_status", card);
	put(rel, status ? status : "");
	if (!status)
		drop(rel);
}

/* An attribute of a GPU made with gpu(), below its card directory; NULL makes it unreadable. */
static void
gpuattr(const char *card, const char *name, const char *text)
{
	char rel[128];

	snprintf(rel, sizeof rel, "class/drm/%s/%s", card, name);
	put(rel, text);
}

/* Link below sys: rel points at the directory to, which is made. */
static void
link_dir(const char *rel, const char *to)
{
	char path[512], dest[512];

	snprintf(dest, sizeof dest, "%s/%s/.keep", sys, to);
	mkparents(dest);
	snprintf(dest, sizeof dest, "%s/%s", sys, to);
	mkdir(dest, 0755);
	snprintf(path, sizeof path, "%s/%s", sys, rel);
	mkparents(path);
	unlink(path);
	if (symlink(dest, path) < 0)
		CHECK(0, "cannot link %s", path);
}

/* The engines of the fake Intel GPU: render, copy, video and video enhance, by their busy event config */
static const unsigned long long engines[] = { 0x0, 0x1000, 0x2000, 0x3000 };

/* The i915 PMU of an integrated Intel GPU, named pmu, with the four engines and events that are not theirs. */
static void
intelpmu(const char *pmu, const char *type)
{
	static const struct { const char *name, *text; } events[] = {
		{ "rcs0-busy", "config=0x0\n" }, { "bcs0-busy", "config=0x1000\n" }, { "vcs0-busy", "config=0x2000\n" },
		{ "vecs0-busy", "config=0x3000\n" }, { "rcs0-busy.unit", "ns\n" }, { "rcs0-wait", "config=0x1\n" },
		{ "actual-frequency", "config=0x100000\n" }, { "gt0-busy", "config=0x100000\n" },
		{ "xcs0-busy", "config=0x4001\n" }, { "ycs0-busy", "garbage\n" }, { "zcs0-busy", "config=\n" },
		{ "wcs0-busy", "config=0x5000x\n" },
	};
	char rel[256];
	size_t i;

	snprintf(rel, sizeof rel, "bus/event_source/devices/%s/type", pmu);
	put(rel, type);
	snprintf(rel, sizeof rel, "bus/event_source/devices/%s/cpumask", pmu);
	put(rel, "3\n");
	for (i = 0; i < LENGTH(events); i++) {
		snprintf(rel, sizeof rel, "bus/event_source/devices/%s/events/%s", pmu, events[i].name);
		put(rel, events[i].text);
	}
}

/* An Intel GPU driven by i915, with the PMU "i915". */
static void
intel(const char *card, const char *status)
{
	char rel[128];

	gpu(card, status);
	snprintf(rel, sizeof rel, "class/drm/%s/device/driver", card);
	link_dir(rel, "bus/pci/drivers/i915");
	intelpmu("i915", "42\n");
}

/* The counters of the four engines: each one's busy time, and the time they were enabled, in ns */
static void
counters(unsigned long long rcs, unsigned long long bcs, unsigned long long vcs, unsigned long long vecs,
         unsigned long long enabled)
{
	counter(engines[0], rcs, enabled);
	counter(engines[1], bcs, enabled);
	counter(engines[2], vcs, enabled);
	counter(engines[3], vecs, enabled);
}

/* Read the GPUs of s as the sampler does: the usage, with how many counters were opened and reads made. */
static int
readgpus(Sensors *s, int *opens, int *reads)
{
	int u;

	perfopens = gpureads = 0;
	u = sensors_readgpus(s);
	*opens = perfopens;
	*reads = gpureads;
	return u;
}

#define EXPECTREAD(s, want, wantopens, wantreads, what) do { \
	int opens_, reads_, got_ = readgpus((s), &opens_, &reads_); \
	CHECK(got_ == (want) && opens_ == (wantopens) && reads_ == (wantreads), \
	      "%s: %d%% with %d opens and %d reads, want %d%% with %d and %d", \
	      what, got_, opens_, reads_, want, wantopens, wantreads); \
} while (0)

/* Find and read the GPUs of the current tree: the usage, how many reads woke them, and whether they went stale. */
static int
gpusample(int *reads, int *stale)
{
	Sensors s;
	int u;

	sensors_init(&s);
	sensors_findgpus(&s, sys);
	gpureads = 0;
	u = sensors_readgpus(&s);
	*reads = gpureads;
	*stale = s.gpustale;
	return u;
}

#define EXPECTGPU(want, wantreads, wantstale, what) do { \
	int reads_, stale_, got_ = gpusample(&reads_, &stale_); \
	CHECK(got_ == (want) && reads_ == (wantreads) && stale_ == (wantstale), \
	      "%s: %d%% with %d reads (stale %d), want %d%% with %d (stale %d)", \
	      what, got_, reads_, stale_, want, wantreads, wantstale); \
} while (0)

/*
 * The GPU usage: amdgpu's busy share and Intel's frequency, read only while
 * the GPU is awake, idle without a read while it is suspended, nothing when a
 * read fails or there is no GPU, and never more often than the GPU takes to
 * suspend after a read.
 */
static void
test_gpus(void)
{
	static const struct { const char *text; int want; } busyreads[] = {
		{ "0\n", 0 }, { "37\n", 37 }, { "37", 37 }, { "100\n", 100 },
		{ "101\n", -1 }, { "-1\n", -1 }, { "", -1 }, { "abc\n", -1 }, { "37%\n", -1 }, { "3.5\n", -1 },
		{ " 37\n", -1 }, { "99999999999999999999\n", -1 },
	};
	static const struct { const char *status; int want, reads; } states[] = {
		{ "active\n", 37, 1 }, { "unsupported\n", 37, 1 }, { "active", 37, 1 },
		/* asleep: idle, and left alone */
		{ "suspended\n", 0, 0 }, { "suspending\n", 0, 0 },
		/* waking up, failed or unknown: not now */
		{ "resuming\n", -1, 0 }, { "error\n", -1, 0 }, { "", -1, 0 }, { "Active\n", -1, 0 },
		{ "active and more\n", -1, 0 }, { NULL, -1, 0 },
	};
	char dir[] = "/tmp/dwm-gpus-XXXXXX", rel[600];
	Sensors s;
	SysmonValues v;
	size_t i;
	int fds;

	if (!mkdtemp(dir)) {
		CHECK(0, "cannot create a scratch directory");
		return;
	}
	snprintf(sys, sizeof sys, "%s/sys", dir);

	/* no GPU, or none with a usage to read without privileges: nothing, and looked for again later */
	reset();
	EXPECTGPU(-1, 0, 1, "an empty tree");
	put("class/drm/version", "drm 1.1.0 20060810\n");
	put("class/drm/renderD128/device/gpu_busy_percent", "50\n");
	put("class/drm/card0-DP-1/device/gpu_busy_percent", "50\n");
	put("class/drm/card0-DP-1/device/power/runtime_status", "active\n");
	gpu("card0", "active\n"); /* nouveau or NVIDIA's driver */
	gpuattr("card0", "device/hwmon/hwmon3/temp1_input", "50000\n");
	EXPECTGPU(-1, 0, 1, "a GPU without a usage, connectors and render nodes");
	snprintf(sys, sizeof sys, "%s/missing", dir);
	EXPECTGPU(-1, 0, 1, "a missing tree");
	snprintf(sys, sizeof sys, "%s/sys", dir);

	/* amdgpu */
	reset();
	gpu("card0", "active\n");
	for (i = 0; i < LENGTH(busyreads); i++) {
		gpuattr("card0", "device/gpu_busy_percent", busyreads[i].text);
		EXPECTGPU(busyreads[i].want, 1, busyreads[i].want < 0, busyreads[i].text);
	}
	gpuattr("card0", "device/gpu_busy_percent", "37\n");
	for (i = 0; i < LENGTH(states); i++) {
		gpu("card0", states[i].status);
		EXPECTGPU(states[i].want, states[i].reads, states[i].want < 0, states[i].status ? states[i].status : "no status");
	}
	/* a GPU that suspended between its status and its usage fails the read with EBUSY: no value */
	gpu("card0", "active\n");
	busy = "gpu_busy_percent";
	EXPECTGPU(-1, 1, 1, "a read that fails with EBUSY");
	busy = NULL;
	EXPECTGPU(37, 1, 0, "the read after EBUSY");
	gpuattr("card0", "device/gpu_busy_percent", NULL);
	EXPECTGPU(-1, 1, 1, "a usage that cannot be read");

	/*
	 * Intel: the busy time of its engines in the i915 PMU over the time the
	 * counters were enabled, of the busiest engine. The counters are opened
	 * once, at the start, even of a GPU asleep (which does not wake it), and
	 * never later; they are read only while it is awake, and the first
	 * reading since it woke up has no value yet (SENSORS_PENDING), so no
	 * sample averages over the time it slept.
	 */
	reset();
	snprintf(perfdir, sizeof perfdir, "%s/perf", dir);
	mkdir(perfdir, 0755);
	intel("card0", "suspended\n");
	counters(0, 0, 0, 0, 1000);
	fds = openfds();
	sensors_init(&s);
	perfopens = 0;
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 1 && s.gpu[0].nengine == 4 && s.gpu[0].pmutype == PERFTYPE && s.gpu[0].cpu == PERFCPU
	      && !s.gpu[0].busy[0], "the Intel GPU and its four engines were not found (%d GPUs)", s.ngpu);
	CHECK(perfopens == 0 && openfds() == fds, "counters were opened after the start");
	EXPECTREAD(&s, -1, 0, 0, "an asleep Intel GPU without counters");
	gpu("card0", "active\n");
	EXPECTREAD(&s, -1, 0, 0, "an awake Intel GPU without counters");
	CHECK(s.gpustale, "an Intel GPU without counters was not looked for again");
	gpu("card0", "suspended\n");
	perfopens = gpureads = 0;
	sensors_startgpus(&s, sys, 5000);
	CHECK(perfopens == 4 && gpureads == 0 && openfds() == fds + 4 && s.gpufound == 5000 && !s.gpuopen,
	      "the start did not open the four counters of a suspended Intel GPU without reading them (%d opens)", perfopens);
	EXPECTREAD(&s, 0, 0, 0, "a suspended Intel GPU with counters");
	gpu("card0", "active\n");
	EXPECTREAD(&s, SENSORS_PENDING, 0, 4, "the first reading of an Intel GPU");
	CHECK(!s.gpustale, "a pending Intel GPU was looked for again");
	counters(500000000, 100000000, 0, 0, 1000000000 + 1000);
	EXPECTREAD(&s, 50, 0, 4, "the render engine busy half the time");
	counters(510000000, 1000000000, 0, 0, 2000000000 + 1000);
	EXPECTREAD(&s, 90, 0, 4, "the copy engine busy 90% of the time");
	counters(510000000, 1000000000, 4949, 0, 2000010000 + 1000);
	EXPECTREAD(&s, 49, 0, 4, "49.49%");
	counters(510000000, 1000000000, 4949 + 4950, 0, 2000020000 + 1000);
	EXPECTREAD(&s, 50, 0, 4, "49.5%");
	counters(510000000, 1000000000, 4949 + 4950, 0, 2000030000 + 1000);
	EXPECTREAD(&s, 0, 0, 4, "idle engines");
	counters(510000000, 1000000000, 4949 + 4950, 50000, 2000040000 + 1000);
	EXPECTREAD(&s, 100, 0, 4, "an engine busier than the time counted");
	/* asleep: idle and left alone; the reading after it wakes up starts over */
	gpu("card0", "suspending\n");
	EXPECTREAD(&s, 0, 0, 0, "an Intel GPU going to sleep");
	gpu("card0", "suspended\n");
	EXPECTREAD(&s, 0, 0, 0, "a suspended Intel GPU");
	gpu("card0", "active\n");
	counters(510000000 + 900000000, 1000000000, 4949 + 4950, 50000, 3000040000 + 1000);
	EXPECTREAD(&s, SENSORS_PENDING, 0, 4, "the first reading after a suspend");
	counters(510000000 + 900002500, 1000000000, 4949 + 4950, 50000, 3000050000 + 1000);
	EXPECTREAD(&s, 25, 0, 4, "the second reading after a suspend");
	CHECK(!s.gpustale && openfds() == fds + 4, "the counting Intel GPU was stale or holds %d descriptors", openfds() - fds);
	/* looking for the GPUs again keeps the same counters counting */
	sensors_findgpus(&s, sys);
	counters(510000000 + 900005000, 1000000000, 4949 + 4950, 50000, 3000060000 + 1000);
	EXPECTREAD(&s, 25, 0, 4, "a reading after the GPUs were looked for again");
	CHECK(openfds() == fds + 4, "looking for the GPUs again leaked or closed counters");
	/* counters that do not advance or go backwards: no value, and counting starts over */
	EXPECTREAD(&s, -1, 0, 4, "counters whose time did not advance");
	CHECK(s.gpustale && openfds() == fds + 4, "counters that stopped were closed, or the GPU not looked for again");
	counters(510000000 + 900007500, 1000000000, 4949 + 4950, 50000, 3000070000 + 1000);
	EXPECTREAD(&s, 25, 0, 4, "the reading after counters stopped");
	counters(510000000 + 900010000, 1000000000, 0, 50000, 3000080000 + 1000);
	EXPECTREAD(&s, -1, 0, 4, "a counter that went backwards");
	counters(510000000 + 900012500, 1000000000, 0, 50000, 3000090000 + 1000);
	EXPECTREAD(&s, 25, 0, 4, "the reading after a counter went backwards");
	/* a counter that cannot be read, as when the driver goes: closed for good, and the GPU no longer shown */
	{
		char path[600];
		FILE *fp;

		snprintf(path, sizeof path, "%s/%llx", perfdir, engines[1]);
		if ((fp = fopen(path, "w"))) {
			fputs("garbage", fp);
			fclose(fp);
		}
	}
	EXPECTREAD(&s, -1, 0, 2, "a counter that cannot be read");
	CHECK(openfds() == fds, "a counter that cannot be read left counters open");
	gpu("card0", "suspended\n");
	EXPECTREAD(&s, -1, 0, 0, "an asleep Intel GPU whose counters were closed");
	perfopens = 0;
	sensors_findgpus(&s, sys);
	gpu("card0", "active\n");
	EXPECTREAD(&s, -1, 0, 0, "an Intel GPU whose counters were closed, looked for again");
	CHECK(openfds() == fds, "counters were opened again after the start");
	/* an engine whose counter cannot be opened at the start: none is left open */
	counters(0, 0, 0, 0, 1000);
	snprintf(rel, sizeof rel, "%s/%llx", perfdir, engines[3]);
	unlink(rel);
	perfopens = 0;
	sensors_startgpus(&s, sys, 6000);
	CHECK(perfopens == 4 && openfds() == fds && s.gpu[0].fd[0] < 0, "counters were left open when one could not be opened");
	EXPECTREAD(&s, -1, 0, 0, "an Intel GPU whose counters could not all be opened");
	/* another set of engines after the GPUs are looked for again: the old counters are closed, and no new ones opened */
	counters(0, 0, 0, 0, 1000);
	sensors_startgpus(&s, sys, 7000);
	CHECK(openfds() == fds + 4, "the start did not open the counters");
	put("bus/event_source/devices/i915/events/ccs0-busy", "config=0x4000\n");
	counter(0x4000, 0, 1000);
	sensors_findgpus(&s, sys);
	CHECK(s.gpu[0].nengine == 5 && s.gpu[0].fd[0] < 0 && openfds() == fds, "the counters of the old engines stayed open, or new ones were opened");
	/* a GPU that went away: its counters are closed */
	sensors_startgpus(&s, sys, 8000);
	CHECK(openfds() == fds + 5, "the start did not open the counters of five engines");
	reset();
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 0 && openfds() == fds, "the counters of a GPU that went away stayed open");
	intel("card0", "active\n");

	/* without CAP_PERFMON the counters cannot be opened: the Intel GPU is never shown, awake or asleep */
	{
		static const int denied[] = { EACCES, EPERM, ENOENT, ENODEV, EINVAL };

		for (i = 0; i < LENGTH(denied); i++) {
			perferrno = denied[i];
			fds = openfds();
			sysmon_clear(&v);
			sensors_init(&s);
			gpu("card0", "active\n");
			sensors_startgpus(&s, sys, 0);
			sensors_samplegpus(&s, sys, 1000, &v);
			sensors_samplegpus(&s, sys, 2000, &v);
			gpu("card0", "suspended\n");
			sensors_samplegpus(&s, sys, 3000, &v);
			CHECK(v.gpu == -1 && v.ngpuhist == 0 && s.gpustale && openfds() == fds,
			      "an Intel GPU whose counters fail with %s was shown (%d) or left descriptors", strerror(denied[i]), v.gpu);
		}
		perferrno = 0;
	}

	/* a reading still pending after the GPU woke up shows no value, and leaves the history going on */
	reset();
	intel("card0", "active\n");
	counters(0, 0, 0, 0, 1000);
	sysmon_clear(&v);
	sensors_init(&s);
	sensors_startgpus(&s, sys, 0);
	sensors_samplegpus(&s, sys, 1000, &v);
	CHECK(v.gpu == -1 && v.ngpuhist == 0, "the first reading of an Intel GPU had a value");
	counters(400, 0, 0, 0, 2000);
	sensors_samplegpus(&s, sys, 2000, &v);
	gpu("card0", "suspended\n");
	sensors_samplegpus(&s, sys, 3000, &v);
	CHECK(v.gpu == 0 && v.ngpuhist == 2 && v.gpuhist[0] == 40 && v.gpuhist[1] == 0, "an Intel GPU's history is not 40%%, then idle");
	gpu("card0", "active\n");
	counters(400 + 9000, 0, 0, 0, 2000 + 10000);
	sensors_samplegpus(&s, sys, 4000, &v);
	CHECK(v.gpu == -1 && v.ngpuhist == 2 && v.gpuhist[1] == 0, "the first reading after a suspend was not pending, or ended the history");
	counters(400 + 9000 + 300, 0, 0, 0, 2000 + 10000 + 1000);
	sensors_samplegpus(&s, sys, 5000, &v);
	CHECK(v.gpu == 30 && v.ngpuhist == 3 && v.gpuhist[2] == 30, "the history did not go on after a pending reading");
	reset();
	sensors_findgpus(&s, sys);
	CHECK(openfds() == fds, "the counters were not closed when the GPU went away");

	/* not an Intel GPU this source can read */
	reset();
	intel("card0", "active\n");
	sensors_init(&s);
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 1, "an Intel GPU was not found");
	link_dir("class/drm/card0/device/driver", "bus/pci/drivers/xe");
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 0, "a GPU driven by xe was taken for i915");
	link_dir("class/drm/card0/device/driver", "bus/pci/drivers/i915");
	drop("bus/event_source/devices/i915/type");
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 0, "an i915 PMU without a type was used");
	put("bus/event_source/devices/i915/type", "0\n");
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 0, "an i915 PMU of type 0 was used");
	put("bus/event_source/devices/i915/type", "42\n");
	{
		static const char *events[] = { "rcs0-busy", "bcs0-busy", "vcs0-busy", "vecs0-busy" };

		for (i = 0; i < LENGTH(events); i++) {
			snprintf(rel, sizeof rel, "bus/event_source/devices/i915/events/%s", events[i]);
			drop(rel);
		}
	}
	EXPECTGPU(-1, 0, 1, "an i915 PMU without engines");
	reset();
	gpu("card0", "active\n");
	intelpmu("i915", "42\n");
	EXPECTGPU(-1, 0, 1, "a card without a driver");

	/* the PMU of a discrete card is named after its PCI device, the integrated one's is not */
	reset();
	put("devices/pci0000:00/0000:03:00.0/power/runtime_status", "suspended\n");
	link_dir("class/drm/card1/device", "devices/pci0000:00/0000:03:00.0");
	link_dir("devices/pci0000:00/0000:03:00.0/driver", "bus/pci/drivers/i915");
	intelpmu("i915_0000_03_00.0", "43\n");
	intel("card0", "suspended\n");
	put("bus/event_source/devices/i915/cpumask", "0-3\n");
	sensors_init(&s);
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 2 && s.gpu[0].pmutype == 42 && s.gpu[0].cpu == 0 && s.gpu[1].pmutype == 43 && s.gpu[1].cpu == 3,
	      "the PMUs of an integrated and a discrete Intel GPU were not told apart (%d GPUs)", s.ngpu);
	drop("bus/event_source/devices/i915/cpumask");
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 2 && s.gpu[0].cpu == 0, "a PMU without a cpumask was not counted on CPU 0");

	/* more engines than are counted: the first SENSORS_ENGINES */
	reset();
	intel("card0", "active\n");
	for (i = SENSORS_ENGINES + 4; i-- > 0;) { /* made last first, as a directory may list them in any order */
		char text[32];

		snprintf(rel, sizeof rel, "bus/event_source/devices/i915/events/ccs%zu-busy", i);
		snprintf(text, sizeof text, "config=0x%zx\n", 0x4000 + (i << 4));
		put(rel, text);
	}
	sensors_init(&s);
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 1 && s.gpu[0].nengine == SENSORS_ENGINES && s.gpu[0].config[0] == engines[0]
	      && s.gpu[0].config[SENSORS_ENGINES - 1] == 0x4000 + ((SENSORS_ENGINES - 5) << 4),
	      "%d engines were counted, or not the first ones", s.ngpu ? s.gpu[0].nengine : 0);
	for (i = 1; s.ngpu && i < (size_t)s.gpu[0].nengine; i++)
		CHECK(s.gpu[0].config[i - 1] < s.gpu[0].config[i], "engine %zu is not in order of class and instance", i);

	/* a hybrid laptop: the busiest GPU; the sleeping one is idle and stays asleep */
	reset();
	intel("card0", "active\n");
	counters(0, 0, 0, 0, 1000);
	gpu("card1", "suspended\n");
	gpuattr("card1", "device/gpu_busy_percent", "80\n");
	fds = openfds();
	sensors_init(&s);
	sensors_startgpus(&s, sys, 0);
	EXPECTREAD(&s, 0, 0, 4, "an awake Intel GPU's first reading beside a suspended amdgpu");
	counters(300, 0, 0, 0, 2000);
	EXPECTREAD(&s, 30, 0, 4, "an awake Intel GPU beside a suspended amdgpu");
	gpu("card1", "active\n");
	counters(600, 0, 0, 0, 3000);
	EXPECTREAD(&s, 80, 0, 5, "two awake GPUs");
	busy = "gpu_busy_percent";
	counters(900, 0, 0, 0, 4000);
	EXPECTREAD(&s, 30, 0, 5, "a GPU failing with EBUSY beside one that is read");
	CHECK(s.gpustale, "a GPU failing with EBUSY was not looked for again");
	busy = NULL;
	gpu("card0", "suspended\n");
	EXPECTREAD(&s, 80, 0, 1, "an awake amdgpu beside a suspended Intel GPU");
	gpu("card0", "active\n");
	gpu("card1", "suspended\n");
	EXPECTREAD(&s, 0, 0, 4, "a pending Intel GPU beside a suspended amdgpu");
	gpu("card0", "suspended\n");
	EXPECTREAD(&s, 0, 0, 0, "every GPU suspended");
	reset();
	sensors_findgpus(&s, sys);
	CHECK(openfds() == fds, "the hybrid laptop's counters were not closed");
	perfdir[0] = '\0';

	/* the sampler gives up every capability for good once its counters are open */
	{
		char line[128];
		FILE *fp;
		int nnp = -1, caps = 0;

		CHECK(sensors_dropcaps() == 0, "dropping the capabilities failed");
		CHECK(capsets == 1 && capsetzero, "the capabilities were not all cleared (%d capset calls)", capsets);
		if ((fp = fopen("/proc/self/status", "r"))) {
			while (fgets(line, sizeof line, fp)) {
				if (!strncmp(line, "NoNewPrivs:", 11))
					nnp = atoi(line + 11);
				if (!strncmp(line, "CapInh:", 7) || !strncmp(line, "CapPrm:", 7) || !strncmp(line, "CapEff:", 7)
				|| !strncmp(line, "CapAmb:", 7))
					caps += strtoull(line + 7, NULL, 16) != 0;
			}
			fclose(fp);
		}
		CHECK(nnp == 1 && caps == 0, "the process can still gain privileges (NoNewPrivs %d) or holds %d capability sets", nnp, caps);
	}

	/* more cards than are read: the first ones by number */
	reset();
	{
		static const char *cards[] = { "card10", "card2", "card0", "card3", "card1" };
		char text[16];

		for (i = 0; i < LENGTH(cards); i++) {
			gpu(cards[i], "active\n");
			snprintf(text, sizeof text, "%s\n", cards[i] + 4);
			gpuattr(cards[i], "device/gpu_busy_percent", text);
		}
		EXPECTGPU(3, SENSORS_GPUS, 0, "more GPUs than are read");
	}

	/* a GPU stays awake for its autosuspend delay after a read: it is not read again within it */
	reset();
	gpu("card0", "active\n");
	gpuattr("card0", "device/gpu_busy_percent", "37\n");
	intel("card1", "suspended\n");
	gpuattr("card0", "device/power/autosuspend_delay_ms", "5000\n");
	gpuattr("card1", "device/power/autosuspend_delay_ms", "10000\n");
	sensors_init(&s);
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 2 && s.gpuawake == 10000, "the longest autosuspend delay of two GPUs is %lld", s.gpuawake);
	CHECK(sensors_gpudelay(&s, 4000, 0) == 11000, "GPUs that stay awake 10 s were read every %lld ms",
	      sensors_gpudelay(&s, 4000, 0));
	CHECK(sensors_gpudelay(&s, 30000, 0) == 30000, "GPUs were read more often than the sampler samples");
	CHECK(sensors_gpudelay(&s, 4000, 1000) == 20000, "a slow GPU read took more than 5%% of the time");
	CHECK(sensors_gpudelay(&s, 4000, 1LL << 60) == SENSORS_MAXDELAY, "the delay of a huge GPU read overflowed");
	gpuattr("card1", "device/power/autosuspend_delay_ms", NULL); /* no autosuspend: the read fails (EIO) */
	sensors_findgpus(&s, sys);
	CHECK(s.gpuawake == 5000 && sensors_gpudelay(&s, 4000, 0) == 6000, "a GPU that stays awake 5 s was read every %lld ms",
	      sensors_gpudelay(&s, 4000, 0));
	gpuattr("card0", "device/power/autosuspend_delay_ms", "-1\n"); /* never suspends */
	sensors_findgpus(&s, sys);
	CHECK(s.gpuawake == 0 && sensors_gpudelay(&s, 4000, 0) == 4000, "GPUs that do not autosuspend were not read every sample");
	CHECK(sensors_gpudelay(&s, 250, 0) == SENSORS_GPUMARGIN, "GPUs were read every 250 ms");
	gpuattr("card0", "device/power/autosuspend_delay_ms", "999999999\n");
	sensors_findgpus(&s, sys);
	CHECK(sensors_gpudelay(&s, 4000, 0) == 999999999LL + SENSORS_GPUMARGIN, "a long autosuspend delay was cut short");
	reset();
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 0 && sensors_gpudelay(&s, 4000, 0) == 4000, "no GPU took a delay");

	/*
	 * The sampler pushes the usage into its history; GPUs are looked for once,
	 * and again only when there is none or one failed, at most every
	 * SENSORS_REFIND ms.
	 */
	reset();
	sysmon_clear(&v);
	sensors_init(&s);
	sensors_samplegpus(&s, sys, 1000, &v);
	CHECK(v.gpu == -1 && v.ngpuhist == 0 && s.gpufound == 1000 && s.gpustale, "a tree without GPUs gave a usage");
	gpu("card0", "active\n");
	gpuattr("card0", "device/gpu_busy_percent", "37\n");
	sensors_samplegpus(&s, sys, 1000 + SENSORS_REFIND - 1, &v);
	CHECK(v.gpu == -1 && s.gpufound == 1000, "GPUs were looked for again too soon");
	sensors_samplegpus(&s, sys, 1000 + SENSORS_REFIND, &v);
	CHECK(v.gpu == 37 && v.ngpuhist == 1 && !s.gpustale, "a GPU that appeared was not found");
	gpuattr("card0", "device/gpu_busy_percent", "40\n");
	sensors_samplegpus(&s, sys, 1000 + 5 * SENSORS_REFIND, &v);
	CHECK(v.gpu == 40 && v.ngpuhist == 2 && v.gpuhist[0] == 37 && s.gpufound == 1000 + SENSORS_REFIND,
	      "the GPU history is not its readings, or a GPU was looked for again");
	gpu("card0", "suspended\n");
	sensors_samplegpus(&s, sys, 1000 + 5 * SENSORS_REFIND + 1, &v);
	CHECK(v.gpu == 0 && v.ngpuhist == 3 && !s.gpustale, "a suspended GPU did not show as idle");
	busy = "gpu_busy_percent";
	gpu("card0", "active\n");
	sensors_samplegpus(&s, sys, 1000 + 5 * SENSORS_REFIND + 2, &v);
	busy = NULL;
	CHECK(v.gpu == -1 && v.ngpuhist == 0 && s.gpustale, "a GPU failing with EBUSY kept its history");

	/* a sysfs root that leaves no room for a GPU's attributes: they are not used cut short */
	snprintf(sys, sizeof sys, "%s/", dir);
	while (strlen(sys) < SENSORS_PATHLEN - 32)
		strcat(sys, strlen(sys) % 50 ? "y" : "/");
	sys[strlen(sys) - 1] = 'y';
	reset();
	gpu("card0", "active\n");
	gpuattr("card0", "device/gpu_busy_percent", "37\n");
	sensors_init(&s);
	sensors_findgpus(&s, sys);
	CHECK(s.ngpu == 0, "a GPU was used with its paths cut short");

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
	test_gpus();
	if (failures)
		return EXIT_FAILURE;
	puts("sysmon regression checks passed");
	return EXIT_SUCCESS;
}
