/* See LICENSE file for copyright and license details. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sensors.h"

#define CHIPS 32

typedef struct {
	char dir[SENSORS_PATHLEN];
	char name[32];
} Chip;

/* hwmon chips with the CPU temperature, best first */
static const char *tempchips[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal" };
/* thermal zones with the CPU temperature when no such chip exists, best first */
static const char *tempzones[] = { "x86_pkg_temp", "cpu-thermal", "cpu_thermal" };
/* GPU fans are not the system's, and reading a sleeping GPU wakes it up */
static const char *gpuchips[] = { "amdgpu", "radeon", "nouveau", "i915", "xe" };
/* the same fans as a faster chip on Dell laptops, at milliseconds per read */
static const char *slowchips[] = { "dell_smm" };

#define LENGTH(X) (sizeof (X) / sizeof (X)[0])

static int
oneof(const char *s, const char **list, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (!strcmp(s, list[i]))
			return (int)i;
	return -1;
}

/* Join dir and name; -1 when it does not fit. */
static int
join(char *buf, const char *dir, const char *name)
{
	int n = snprintf(buf, SENSORS_PATHLEN, "%s/%s", dir, name);

	return n > 0 && n < SENSORS_PATHLEN ? 0 : -1;
}

/* A sysfs attribute as a string; -1 when it cannot be read, as some labels cannot. */
static int
readraw(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

/* The first line of an attribute */
static int
readattr(const char *path, char *buf, size_t size)
{
	if (readraw(path, buf, size) < 0)
		return -1;
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

/* An integer attribute: an optional minus, at most 9 digits and an optional newline. */
static int
readvalue(const char *path, long *v)
{
	char buf[32], *p;
	int digits = 0, neg;

	if (readraw(path, buf, sizeof buf) < 0)
		return -1;
	p = buf;
	if ((neg = *p == '-'))
		p++;
	for (*v = 0; *p >= '0' && *p <= '9' && digits < 9; p++, digits++)
		*v = *v * 10 + *p - '0';
	if (*p == '\n')
		p++;
	if (!digits || *p)
		return -1;
	if (neg)
		*v = -*v;
	return 0;
}

/* N for an entry named <prefix>N<suffix>, or -1. */
static int
entrynum(const char *entry, const char *prefix, const char *suffix)
{
	size_t n = strlen(prefix);
	char *end;
	long i;

	if (strncmp(entry, prefix, n) || entry[n] < '0' || entry[n] > '9')
		return -1;
	i = strtol(entry + n, &end, 10);
	return i < 1000000 && !strcmp(end, suffix) ? (int)i : -1;
}

static int
bynumber(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int
bydir(const void *a, const void *b)
{
	const Chip *x = a, *y = b;
	size_t lx = strlen(x->dir), ly = strlen(y->dir);

	/* hwmon2 before hwmon10 */
	return lx != ly ? (lx < ly ? -1 : 1) : strcmp(x->dir, y->dir);
}

/*
 * The entries of dir named <prefix>N with the attribute in name, in order of N
 * (sysfs numbers are not stable across boots, so never assume them).
 */
static int
listdir(const char *dir, const char *prefix, const char *name, Chip *chips, int max)
{
	char path[SENSORS_PATHLEN];
	struct dirent *e;
	DIR *d;
	int n = 0;

	if (!(d = opendir(dir)))
		return 0;
	while (n < max && (e = readdir(d))) {
		if (entrynum(e->d_name, prefix, "") < 0 || join(chips[n].dir, dir, e->d_name) < 0
		|| join(path, chips[n].dir, name) < 0
		|| readattr(path, chips[n].name, sizeof chips[n].name) < 0)
			continue;
		n++;
	}
	closedir(d);
	qsort(chips, n, sizeof *chips, bydir);
	return n;
}

/* The numbers N of a chip's <prefix>N_input entries, in order. */
static int
inputs(const char *dir, const char *prefix, int *num, int max)
{
	struct dirent *e;
	DIR *d;
	int n = 0, i;

	if (!(d = opendir(dir)))
		return 0;
	while (n < max && (e = readdir(d)))
		if ((i = entrynum(e->d_name, prefix, "_input")) >= 0)
			num[n++] = i;
	closedir(d);
	qsort(num, n, sizeof *num, bynumber);
	return n;
}

/* The CPU temperature input of a chip: the package or die sensor by its label, else the first. */
static int
cputemp(const char *dir, char *path)
{
	static const char *labels[] = { "Tctl", "Tdie", "Package id" }; /* worst first */
	char name[32], label[32];
	int num[256], n, i, j, score, best = -1, at = 0;

	n = inputs(dir, "temp", num, LENGTH(num));
	for (i = 0; i < n; i++) {
		score = 0;
		snprintf(name, sizeof name, "temp%d_label", num[i]);
		if (join(path, dir, name) == 0 && readattr(path, label, sizeof label) == 0)
			for (j = 0; j < (int)LENGTH(labels); j++)
				if (!strncmp(label, labels[j], strlen(labels[j])))
					score = j + 1;
		if (score > best) {
			best = score;
			at = num[i];
		}
	}
	if (best < 0)
		return -1;
	snprintf(name, sizeof name, "temp%d_input", at);
	return join(path, dir, name);
}

void
sensors_init(Sensors *s)
{
	s->ntemp = s->nfan = 0;
	s->found = -1;
	s->stale = 1;
}

void
sensors_find(Sensors *s, const char *sys)
{
	Chip chips[CHIPS];
	char dir[SENSORS_PATHLEN], name[32];
	int num[16], n, nchips, i, j, k, fast = 0;

	s->ntemp = s->nfan = 0;
	nchips = join(dir, sys, "class/hwmon") == 0 ? listdir(dir, "hwmon", "name", chips, CHIPS) : 0;

	/* every chip of the best kind present: one per CPU package */
	for (k = 0; k < (int)LENGTH(tempchips) && !s->ntemp; k++)
		for (i = 0; i < nchips && s->ntemp < SENSORS_TEMPS; i++)
			if (!strcmp(chips[i].name, tempchips[k]) && cputemp(chips[i].dir, s->temp[s->ntemp]) == 0)
				s->ntemp++;
	if (!s->ntemp) {
		Chip zones[CHIPS];
		int nzones;

		nzones = join(dir, sys, "class/thermal") == 0 ? listdir(dir, "thermal_zone", "type", zones, CHIPS) : 0;
		for (k = 0; k < (int)LENGTH(tempzones) && !s->ntemp; k++)
			for (i = 0; i < nzones && s->ntemp < SENSORS_TEMPS; i++)
				if (!strcmp(zones[i].name, tempzones[k]) && join(s->temp[s->ntemp], zones[i].dir, "temp") == 0)
					s->ntemp++;
	}

	/* every fan, but a slow chip only when no other chip has fans */
	for (i = 0; i < nchips; i++)
		if (oneof(chips[i].name, gpuchips, LENGTH(gpuchips)) < 0
		&& oneof(chips[i].name, slowchips, LENGTH(slowchips)) < 0
		&& inputs(chips[i].dir, "fan", num, LENGTH(num)))
			fast = 1;
	for (i = 0; i < nchips && s->nfan < SYSMON_FANS; i++) {
		if (oneof(chips[i].name, gpuchips, LENGTH(gpuchips)) >= 0
		|| (fast && oneof(chips[i].name, slowchips, LENGTH(slowchips)) >= 0))
			continue;
		n = inputs(chips[i].dir, "fan", num, LENGTH(num));
		for (j = 0; j < n && s->nfan < SYSMON_FANS; j++) {
			snprintf(name, sizeof name, "fan%d_input", num[j]);
			if (join(s->fan[s->nfan], chips[i].dir, name) == 0)
				s->nfan++;
		}
	}
}

int
sensors_read(const Sensors *s, SysmonValues *v)
{
	long t;
	int i, failed = 0;

	v->temp = -1;
	for (i = 0; i < s->ntemp; i++) {
		/* millidegrees; 0 and below is what absent sensors report */
		if (readvalue(s->temp[i], &t) < 0 || t <= 0 || t > SYSMON_MAXTEMP * 1000L)
			failed = 1;
		else if ((t + 500) / 1000 > v->temp)
			v->temp = (int)((t + 500) / 1000);
	}
	v->nfan = 0;
	for (i = 0; i < s->nfan; i++) {
		if (readvalue(s->fan[i], &t) < 0 || t < 0 || t > SYSMON_MAXFAN)
			failed = 1;
		else
			v->fan[v->nfan++] = (int)t;
	}
	return failed ? -1 : 0;
}

void
sensors_sample(Sensors *s, const char *sys, long long now, SysmonValues *v)
{
	if (s->found < 0 || (s->stale && now - s->found >= SENSORS_REFIND)) {
		sensors_find(s, sys);
		s->found = now;
	}
	s->stale = sensors_read(s, v) < 0 || !s->ntemp || !s->nfan;
}

long long
sensors_delay(long long spent)
{
	/* a source that is slow to read is read less often, so it takes at most 5% of the time */
	if (spent > SENSORS_MAXDELAY / 20)
		return SENSORS_MAXDELAY;
	return spent * 20 > SENSORS_INTERVAL ? spent * 20 : SENSORS_INTERVAL;
}
