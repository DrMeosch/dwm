/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysmon.h"

int
sysmon_cpu_parse(const char *line, SysmonCpu *out)
{
	unsigned long long v[8] = { 0 }, total = 0;
	const char *p = line;
	char *end;
	int i;

	if (strncmp(p, "cpu", 3) || p[3] != ' ')
		return -1;
	p += 3;
	/* user nice system idle iowait irq softirq steal; later fields are included in these */
	for (i = 0; i < 8; i++) {
		while (*p == ' ')
			p++;
		if (*p == '\n' || *p == '\0')
			break;
		if (*p < '0' || *p > '9')
			return -1;
		errno = 0;
		v[i] = strtoull(p, &end, 10);
		if (errno || (*end != ' ' && *end != '\n' && *end != '\0'))
			return -1;
		if (total + v[i] < total)
			return -1;
		total += v[i];
		p = end;
	}
	if (i < 4) /* idle is the fourth counter */
		return -1;
	out->total = total;
	out->idle = v[3] + v[4];
	return 0;
}

int
sysmon_cpu_percent(const SysmonCpu *prev, const SysmonCpu *cur)
{
	unsigned long long dt, di;

	if (cur->total <= prev->total || cur->idle < prev->idle)
		return -1;
	dt = cur->total - prev->total;
	di = cur->idle - prev->idle;
	if (di > dt || dt > ULLONG_MAX / 200)
		return -1;
	return (int)((100 * (dt - di) + dt / 2) / dt);
}

void
sysmon_clear(SysmonValues *v)
{
	memset(v, 0, sizeof *v); /* unused fans and samples too, so equal values compare equal */
	v->cpu = v->temp = v->gpu = -1;
}

/* Take a usage reading as *now and the newest of the n samples in hist. */
static void
push(int *now, int *hist, int *n, int value)
{
	if (value < 0 || value > 100) {
		*now = -1;
		*n = 0;
		memset(hist, 0, SYSMON_HIST * sizeof *hist);
		return;
	}
	*now = value;
	if (*n == SYSMON_HIST)
		memmove(hist, hist + 1, --*n * sizeof *hist);
	hist[(*n)++] = value;
}

void
sysmon_push(SysmonValues *v, int cpu)
{
	push(&v->cpu, v->hist, &v->nhist, cpu);
}

void
sysmon_pushgpu(SysmonValues *v, int gpu)
{
	push(&v->gpu, v->gpuhist, &v->ngpuhist, gpu);
}

/* Append to buf, which holds len bytes; -1 when it does not fit. */
#define APPEND(...) do { \
	n = snprintf(buf + len, size - len, __VA_ARGS__); \
	if (n < 0 || (size_t)n >= size - len) \
		return -1; \
	len += n; \
} while (0)

/*
 * " <name>=..." for the n samples of hist, with sep for the space; its length,
 * 0 when there is no history to send, or -1 when it does not fit. A history is
 * all or nothing: leaving out a sample would shift the others in time.
 */
static int
history(char *buf, size_t size, const char *sep, const char *name, const int *hist, int nhist)
{
	size_t len = 0;
	int i, n;

	if (nhist <= 0 || nhist > SYSMON_HIST)
		return 0;
	for (i = 0; i < nhist; i++)
		if (hist[i] < 0 || hist[i] > 100)
			return 0;
	APPEND("%s%s=%d", sep, name, hist[0]);
	for (i = 1; i < nhist; i++)
		APPEND(",%d", hist[i]);
	return (int)len;
}

int
sysmon_encode(char *buf, size_t size, const SysmonValues *v)
{
	size_t len = 0;
	int i, n, fans = 0;

	if (!size)
		return -1;
	buf[0] = '\0';
	if (v->cpu >= 0 && v->cpu <= 100)
		APPEND("cpu=%d", v->cpu);
	if (v->temp >= 0 && v->temp <= SYSMON_MAXTEMP)
		APPEND("%stemp=%d", len ? " " : "", v->temp);
	for (i = 0; i < v->nfan && i < SYSMON_FANS; i++) {
		if (v->fan[i] < 0 || v->fan[i] > SYSMON_MAXFAN)
			continue;
		if (fans++)
			APPEND(",%d", v->fan[i]);
		else
			APPEND("%sfan=%d", len ? " " : "", v->fan[i]);
	}
	if ((n = history(buf + len, size - len, len ? " " : "", "cpuhist", v->hist, v->nhist)) < 0)
		return -1;
	len += n;
	if (v->gpu >= 0 && v->gpu <= 100)
		APPEND("%sgpu=%d", len ? " " : "", v->gpu);
	if ((n = history(buf + len, size - len, len ? " " : "", "gpuhist", v->gpuhist, v->ngpuhist)) < 0)
		return -1;
	return (int)len + n;
}

/* 1..digits decimal digits worth at most max, or -1; *end is set past the digits. */
static int
number(const char *p, int digits, int max, const char **end)
{
	int v = 0, n = 0;

	for (; *p >= '0' && *p <= '9' && n < digits; p++, n++)
		v = v * 10 + *p - '0';
	*end = p;
	return n && v <= max ? v : -1;
}

/*
 * 1..max comma separated values of number() that end exactly at end, stored
 * in out; returns how many, or -1 if the list is malformed or too long.
 */
static int
list(const char *p, const char *end, int *out, int max, int digits, int limit)
{
	int n = 0, v;

	for (;; p++) {
		if (n == max || (v = number(p, digits, limit, &p)) < 0)
			return -1;
		out[n++] = v;
		if (*p != ',')
			return p == end ? n : -1;
	}
}

int
sysmon_decode(const char *data, unsigned long len, SysmonValues *v)
{
	enum { Cpu = 1, Temp = 2, Fan = 4, Hist = 8, Gpu = 16, GpuHist = 32 };
	char buf[SYSMON_MAXLEN + 1];
	const char *p, *q, *end;
	SysmonValues got;
	int field, ok, seen = 0, bad = 0;

	sysmon_clear(v);
	if (!data || len == 0 || len > SYSMON_MAXLEN)
		return -1;
	memcpy(buf, data, len);
	buf[len] = '\0';
	if (strlen(buf) != len)
		return -1;
	sysmon_clear(&got);
	/* space separated key=value fields; unknown keys are skipped */
	for (p = buf; *p; p = end) {
		for (end = p; *end && *end != ' '; end++)
			;
		field = ok = 0;
		if (!strncmp(p, "cpu=", 4)) {
			field = Cpu;
			ok = (got.cpu = number(p + 4, 3, 100, &q)) >= 0 && q == end;
		} else if (!strncmp(p, "temp=", 5)) {
			field = Temp;
			ok = (got.temp = number(p + 5, 3, SYSMON_MAXTEMP, &q)) >= 0 && q == end;
		} else if (!strncmp(p, "fan=", 4)) {
			field = Fan;
			ok = (got.nfan = list(p + 4, end, got.fan, SYSMON_FANS, 5, SYSMON_MAXFAN)) > 0;
		} else if (!strncmp(p, "cpuhist=", 8)) {
			field = Hist;
			ok = (got.nhist = list(p + 8, end, got.hist, SYSMON_HIST, 3, 100)) > 0;
		} else if (!strncmp(p, "gpu=", 4)) {
			field = Gpu;
			ok = (got.gpu = number(p + 4, 3, 100, &q)) >= 0 && q == end;
		} else if (!strncmp(p, "gpuhist=", 8)) {
			field = GpuHist;
			ok = (got.ngpuhist = list(p + 8, end, got.gpuhist, SYSMON_HIST, 3, 100)) > 0;
		}
		if (field && (!ok || seen & field))
			bad |= field;
		seen |= field;
		while (*end == ' ')
			end++;
	}
	if (seen & Cpu && !(bad & Cpu))
		v->cpu = got.cpu;
	if (seen & Temp && !(bad & Temp))
		v->temp = got.temp;
	if (seen & Fan && !(bad & Fan)) {
		v->nfan = got.nfan;
		memcpy(v->fan, got.fan, got.nfan * sizeof *got.fan);
	}
	if (seen & Hist && !(bad & Hist)) {
		v->nhist = got.nhist;
		memcpy(v->hist, got.hist, got.nhist * sizeof *got.hist);
	}
	if (seen & Gpu && !(bad & Gpu))
		v->gpu = got.gpu;
	if (seen & GpuHist && !(bad & GpuHist)) {
		v->ngpuhist = got.ngpuhist;
		memcpy(v->gpuhist, got.gpuhist, got.ngpuhist * sizeof *got.gpuhist);
	}
	return v->cpu >= 0 || v->temp >= 0 || v->nfan || v->nhist || v->gpu >= 0 || v->ngpuhist ? 0 : -1;
}
