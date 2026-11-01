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

int
sysmon_encode(char *buf, size_t size, int cpu)
{
	int n;

	if (cpu < 0 || cpu > 100)
		return -1;
	n = snprintf(buf, size, "cpu=%d", cpu);
	return n > 0 && (size_t)n < size ? n : -1;
}

int
sysmon_decode(const char *data, unsigned long len)
{
	char buf[SYSMON_MAXLEN + 1];
	const char *p;
	int v, digits;

	if (!data || len == 0 || len > SYSMON_MAXLEN)
		return -1;
	memcpy(buf, data, len);
	buf[len] = '\0';
	if (strlen(buf) != len)
		return -1;
	/* space separated key=value fields; unknown keys are skipped */
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
