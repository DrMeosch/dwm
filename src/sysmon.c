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
	memset(v, 0, sizeof *v); /* unused fans too, so equal values compare equal */
	v->cpu = v->temp = -1;
}

/* Append to buf, which holds len bytes; -1 when it does not fit. */
#define APPEND(...) do { \
	n = snprintf(buf + len, size - len, __VA_ARGS__); \
	if (n < 0 || (size_t)n >= size - len) \
		return -1; \
	len += n; \
} while (0)

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
	return (int)len;
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

int
sysmon_decode(const char *data, unsigned long len, SysmonValues *v)
{
	enum { Cpu = 1, Temp = 2, Fan = 4 };
	char buf[SYSMON_MAXLEN + 1];
	const char *p, *q, *end;
	SysmonValues got;
	int field, ok, n, seen = 0, bad = 0;

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
			/* comma separated RPM values, one per fan */
			for (q = p + 4, got.nfan = 0;; q++) {
				if (got.nfan == SYSMON_FANS || (n = number(q, 5, SYSMON_MAXFAN, &q)) < 0)
					break;
				got.fan[got.nfan++] = n;
				if (*q != ',') {
					ok = q == end;
					break;
				}
			}
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
	return v->cpu >= 0 || v->temp >= 0 || v->nfan ? 0 : -1;
}
