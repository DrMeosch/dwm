/* See LICENSE file for copyright and license details. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/capability.h>
#include <linux/perf_event.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

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
#define MIN(A, B) ((A) < (B) ? (A) : (B))

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
	s->ntemp = s->nfan = s->ngpu = 0;
	s->found = s->gpufound = -1;
	s->stale = s->gpustale = 1;
	s->gpuawake = 0;
	s->gpuopen = 0;
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

/* The name of the directory a sysfs link leads to, or -1. */
static int
target(const char *link, char *name, size_t size)
{
	char path[PATH_MAX], *p;

	if (!realpath(link, path))
		return -1;
	p = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	if (strlen(p) >= size)
		return -1;
	strcpy(name, p);
	return 0;
}

static int
byconfig(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;

	return x < y ? -1 : x > y;
}

/* The busy event of an engine: "config=<n>" below 2^20 with sample 0, as the i915 PMU names them. */
static int
busyconfig(const char *path, unsigned long long *config)
{
	char buf[64], *end;

	if (readattr(path, buf, sizeof buf) < 0 || strncmp(buf, "config=", 7)
	|| buf[7] < '0' || buf[7] > '9')
		return -1;
	errno = 0;
	*config = strtoull(buf + 7, &end, 0);
	return !errno && !*end && *config < 0x100000 && !(*config & 0xf) ? 0 : -1;
}

/*
 * An i915 card's PMU: its perf event type, the CPU its events are counted on
 * and the busy event of every engine. i915 names it after the PCI device for a
 * discrete card, and "i915" for the integrated one. Only sysfs attributes of
 * the PMU are read, never anything the GPU answers.
 */
static int
intelpmu(const char *sys, const char *card, Gpu *g)
{
	char path[SENSORS_PATHLEN], dir[SENSORS_PATHLEN], name[64], buf[32];
	unsigned long long config[4 * SENSORS_ENGINES];
	struct dirent *e;
	DIR *d;
	long v;
	size_t i, n;
	int nconfig = 0;

	if (join(path, card, "device/driver") < 0 || target(path, name, sizeof name) < 0 || strcmp(name, "i915"))
		return -1;
	strcpy(name, "i915_");
	if (join(path, card, "device") < 0 || target(path, name + 5, sizeof name - 5) < 0)
		name[4] = '\0';
	for (i = 5; name[i]; i++)
		if (name[i] == ':')
			name[i] = '_';
	if (snprintf(dir, sizeof dir, "%s/bus/event_source/devices/%s", sys, name) >= (int)sizeof dir
	|| access(dir, F_OK) < 0) {
		name[4] = '\0';
		if (snprintf(dir, sizeof dir, "%s/bus/event_source/devices/%s", sys, name) >= (int)sizeof dir)
			return -1;
	}
	if (join(path, dir, "type") < 0 || readvalue(path, &v) < 0 || v <= 0)
		return -1;
	g->pmutype = (unsigned int)v;
	/* the first CPU of its cpumask ("0", "0-3" or "0,2") */
	g->cpu = 0;
	if (join(path, dir, "cpumask") == 0 && readattr(path, buf, sizeof buf) == 0)
		g->cpu = atoi(buf);
	if (join(path, dir, "events") < 0 || !(d = opendir(path)))
		return -1;
	while (nconfig < (int)LENGTH(config) && (e = readdir(d))) {
		n = strlen(e->d_name);
		if (n <= 5 || strcmp(e->d_name + n - 5, "-busy"))
			continue;
		if (snprintf(dir, sizeof dir, "%s/%s", path, e->d_name) < (int)sizeof dir
		&& busyconfig(dir, &config[nconfig]) == 0)
			nconfig++;
	}
	closedir(d);
	/* the first engines by class and instance */
	qsort(config, nconfig, sizeof *config, byconfig);
	g->nengine = MIN(nconfig, SENSORS_ENGINES);
	memcpy(g->config, config, g->nengine * sizeof *config);
	return g->nengine ? 0 : -1;
}

static void
closecounters(Gpu *g)
{
	int i;

	for (i = 0; i < SENSORS_ENGINES; i++) {
		if (g->fd[i] >= 0)
			close(g->fd[i]);
		g->fd[i] = -1;
	}
	g->counting = 0;
}

/* Open an engine's busy counter, counting system-wide; -1 without CAP_PERFMON or an i915 PMU. */
static int
opencounter(const Gpu *g, unsigned long long config)
{
#ifdef __linux__
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof attr);
	attr.type = g->pmutype;
	attr.size = sizeof attr;
	attr.config = config;
	attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED;
	return (int)syscall(SYS_perf_event_open, &attr, -1, g->cpu, -1, PERF_FLAG_FD_CLOEXEC);
#else
	(void)g;
	(void)config;
	return -1;
#endif
}

/* A counter's busy and enabled time in ns; -1 if it cannot be read. */
static int
readcounter(int fd, unsigned long long *busy, unsigned long long *enabled)
{
	unsigned long long v[2];

	if (read(fd, v, sizeof v) != (ssize_t)sizeof v)
		return -1;
	*busy = v[0];
	*enabled = v[1];
	return 0;
}

/* Open the busy counter of every engine; -1, with none left open, if one cannot be opened. */
static int
opencounters(Gpu *g)
{
	int i;

	for (i = 0; i < g->nengine; i++)
		if ((g->fd[i] = opencounter(g, g->config[i])) < 0) {
			closecounters(g);
			return -1;
		}
	return 0;
}

/*
 * The usage of an awake Intel GPU since the last reading: the share of the
 * time its busiest engine was busy, as intel_gpu_top and btop show it. The
 * first reading since it woke up has no value yet (SENSORS_PENDING), so no
 * sample averages over the time it slept; -1 without counters, or when they
 * cannot be read (closed then, as when its driver goes) or do not advance.
 */
static int
intelusage(Gpu *g)
{
	unsigned long long busy[SENSORS_ENGINES], enabled[SENSORS_ENGINES];
	int i, u, best = 0;

	if (g->fd[0] < 0)
		return -1;
	for (i = 0; i < g->nengine; i++)
		if (readcounter(g->fd[i], &busy[i], &enabled[i]) < 0) {
			closecounters(g);
			return -1;
		}
	for (i = 0; g->counting && i < g->nengine; i++) {
		if (busy[i] < g->busyns[i] || enabled[i] <= g->enabledns[i]) {
			g->counting = 0; /* start over from this reading */
			best = -1;
			break;
		}
		u = busy[i] - g->busyns[i] >= enabled[i] - g->enabledns[i] ? 100
		  : (int)((200 * (busy[i] - g->busyns[i]) / (enabled[i] - g->enabledns[i]) + 1) / 2);
		if (u > best)
			best = u;
	}
	memcpy(g->busyns, busy, g->nengine * sizeof *busy);
	memcpy(g->enabledns, enabled, g->nengine * sizeof *enabled);
	if (!g->counting) {
		g->counting = 1;
		return best < 0 ? -1 : SENSORS_PENDING;
	}
	return best;
}

void
sensors_findgpus(Sensors *s, const char *sys)
{
	Chip cards[CHIPS];
	Gpu old[SENSORS_GPUS];
	char dir[SENSORS_PATHLEN], path[SENSORS_PATHLEN];
	struct dirent *e;
	DIR *d;
	Gpu *g;
	long delay;
	int n = 0, i, j, nold = s->ngpu;

	memcpy(old, s->gpu, nold * sizeof *old);
	s->ngpu = 0;
	s->gpuawake = 0;
	if (join(dir, sys, "class/drm") == 0 && (d = opendir(dir))) {
		/* cardN, not its connectors (cardN-DP-1) or render nodes */
		while (n < CHIPS && (e = readdir(d)))
			if (entrynum(e->d_name, "card", "") >= 0 && join(cards[n].dir, dir, e->d_name) == 0)
				n++;
		closedir(d);
	}
	qsort(cards, n, sizeof *cards, bydir);
	/* nothing the driver answers is read here, only what attributes there are and the power settings */
	for (i = 0; i < n && s->ngpu < SENSORS_GPUS; i++) {
		g = &s->gpu[s->ngpu];
		memset(g, 0, sizeof *g);
		for (j = 0; j < SENSORS_ENGINES; j++)
			g->fd[j] = -1;
		/* without its power state it could be asleep: it is never read */
		if (join(g->status, cards[i].dir, "device/power/runtime_status") < 0 || access(g->status, F_OK) < 0)
			continue;
		if (join(g->busy, cards[i].dir, "device/gpu_busy_percent") == 0 && access(g->busy, F_OK) == 0)
			; /* amdgpu */
		else if ((g->busy[0] = '\0', intelpmu(sys, cards[i].dir, g)) < 0)
			continue;
		/* without autosuspend (EIO) a device suspends as soon as it is idle */
		if (join(path, cards[i].dir, "device/power/autosuspend_delay_ms") == 0
		&& readvalue(path, &delay) == 0 && delay > s->gpuawake)
			s->gpuawake = delay;
		/* the counters of the same GPU and engines stay open; others are opened only at the start */
		for (j = 0; !g->busy[0] && j < nold; j++)
			if (old[j].fd[0] >= 0 && !strcmp(old[j].status, g->status) && old[j].pmutype == g->pmutype
			&& old[j].cpu == g->cpu && old[j].nengine == g->nengine
			&& !memcmp(old[j].config, g->config, g->nengine * sizeof *g->config)) {
				*g = old[j];
				old[j].fd[0] = -1;
				break;
			}
		if (!g->busy[0] && g->fd[0] < 0 && s->gpuopen)
			opencounters(g);
		s->ngpu++;
	}
	for (j = 0; j < nold; j++)
		if (old[j].fd[0] >= 0)
			closecounters(&old[j]);
}

void
sensors_startgpus(Sensors *s, const char *sys, long long now)
{
	s->gpuopen = 1;
	sensors_findgpus(s, sys);
	s->gpuopen = 0;
	s->gpufound = now;
}

int
sensors_dropcaps(void)
{
#ifdef __linux__
	struct __user_cap_header_struct head;
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];

	memset(&head, 0, sizeof head);
	memset(data, 0, sizeof data);
	head.version = _LINUX_CAPABILITY_VERSION_3;
	/* the ambient set goes with the permitted one, but older kernels have none */
	if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0 && errno != EINVAL)
		return -1;
	if (syscall(SYS_capset, &head, data) < 0)
		return -1;
	return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ? -1 : 0;
#else
	return 0;
#endif
}

/*
 * The usage of a GPU, 0..100, SENSORS_PENDING, or -1 when it cannot be read;
 * a suspended GPU is idle and left alone. An Intel GPU without counters has
 * no usage while it sleeps either, so it is never shown.
 */
static int
gpuusage(Gpu *g)
{
	char status[32];
	long busy;

	if (readattr(g->status, status, sizeof status) < 0)
		return -1;
	if (!strcmp(status, "suspended") || !strcmp(status, "suspending")) {
		g->counting = 0; /* the reading after it wakes up starts over */
		return g->busy[0] || g->fd[0] >= 0 ? 0 : -1;
	}
	/* resuming, or in error: not now */
	if (strcmp(status, "active") && strcmp(status, "unsupported"))
		return -1;
	if (!g->busy[0])
		return intelusage(g);
	/* one that suspended since fails the read with EBUSY (older kernels wake it, a rare race) */
	if (readvalue(g->busy, &busy) < 0 || busy < 0 || busy > 100)
		return -1;
	return (int)busy;
}

int
sensors_readgpus(Sensors *s)
{
	int i, u, best = -1;

	s->gpustale = !s->ngpu;
	for (i = 0; i < s->ngpu; i++) {
		if ((u = gpuusage(&s->gpu[i])) == SENSORS_PENDING) {
			if (best == -1)
				best = SENSORS_PENDING;
		} else if (u < 0) {
			s->gpustale = 1;
		} else if (u > best) {
			best = u;
		}
	}
	return best;
}

void
sensors_samplegpus(Sensors *s, const char *sys, long long now, SysmonValues *v)
{
	int u;

	if (s->gpufound < 0 || (s->gpustale && now - s->gpufound >= SENSORS_REFIND)) {
		sensors_findgpus(s, sys);
		s->gpufound = now;
	}
	if ((u = sensors_readgpus(s)) == SENSORS_PENDING)
		v->gpu = -1; /* no value, and no sample: the history goes on */
	else
		sysmon_pushgpu(v, u);
}

long long
sensors_gpudelay(const Sensors *s, long interval, long long spent)
{
	long long delay = interval;

	/* a read keeps an awake GPU from suspending for its autosuspend delay */
	if (s->ngpu && s->gpuawake + SENSORS_GPUMARGIN > delay)
		delay = s->gpuawake + SENSORS_GPUMARGIN;
	/* and a slow one takes at most 5% of the time */
	if (spent > SENSORS_MAXDELAY / 20)
		spent = SENSORS_MAXDELAY / 20;
	return spent * 20 > delay ? spent * 20 : delay;
}
