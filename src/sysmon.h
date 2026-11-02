/* See LICENSE file for copyright and license details. */

/*
 * Contract between dwm and the dwmsysmon sampler, which are separate processes.
 * The sampler publishes the latest values as a short ASCII string of space
 * separated fields ("cpu=42 temp=55 fan=2400,2350 cpuhist=40,38,42") in the
 * SYSMON_ATOM property of the root window; a field whose source is missing is
 * left out, and unknown fields are skipped. cpuhist holds the latest CPU usage
 * samples, oldest first, the last one being the cpu value. dwm publishes whether
 * the section is shown as a CARDINAL (0 or 1), followed by a window that exists
 * as long as dwm does, the ms between samples and how many bytes of the
 * payload it reads, in SYSMON_ENABLED_ATOM: the sampler exits when the section
 * is hidden or that window is gone, and dwm starts a new one when the section
 * is shown. The sampler leaves the history out of a payload that would not fit
 * what dwm reads, so a dwm from before the history keeps its values.
 */
#include <stddef.h>

#define SYSMON_ATOM "_DWM_SYSMON"
#define SYSMON_ENABLED_ATOM "_DWM_SYSMON_ENABLED"
#define SYSMON_OWNER_ATOM "_DWM_SYSMON_OWNER" /* selection: the sampler of a display */
#define SYSMON_MAXLEN 128 /* bytes dwm reads; multiple of 4 */
#define SYSMON_OLDMAXLEN 64 /* bytes read by a dwm that does not say */
#define SYSMON_INTERVAL 4000 /* ms between samples when dwm names none, or one out of range */
#define SYSMON_MININTERVAL 250 /* ms */
#define SYSMON_MAXINTERVAL 60000 /* ms */
#define SYSMON_MAXTEMP 150 /* degrees Celsius */
#define SYSMON_MAXFAN 30000 /* RPM */
#define SYSMON_FANS 4 /* fans in one payload */
#define SYSMON_HIST 16 /* CPU usage samples in one payload */

/* Cumulative CPU time counters in clock ticks; idle includes iowait. */
typedef struct {
	unsigned long long total, idle;
} SysmonCpu;

/* What the section shows: -1 (or nfan 0) where the source is missing. */
typedef struct {
	int cpu;  /* percent, 0..100 */
	int temp; /* CPU temperature in degrees Celsius, 0..SYSMON_MAXTEMP */
	int nfan; /* fans in fan[], 0..SYSMON_FANS */
	int fan[SYSMON_FANS]; /* RPM, 0..SYSMON_MAXFAN */
	int nhist; /* CPU usage samples in hist[], 0..SYSMON_HIST */
	int hist[SYSMON_HIST]; /* the latest CPU usage samples, oldest first, 0..100 */
} SysmonValues;

/* Parse the aggregate "cpu" line of /proc/stat; 0 on success, -1 if malformed. */
int sysmon_cpu_parse(const char *line, SysmonCpu *out);
/* Busy share 0..100 between two readings, or -1 if they do not describe a sane interval. */
int sysmon_cpu_percent(const SysmonCpu *prev, const SysmonCpu *cur);
/* Set every value to missing; two cleared and equally decoded values are equal with memcmp. */
void sysmon_clear(SysmonValues *v);
/*
 * Take a CPU usage reading (-1 when there is none) as the value and the newest
 * sample of the history, dropping the oldest one when it is full; a missing
 * reading clears the history, so it never spans a gap.
 */
void sysmon_push(SysmonValues *v, int cpu);
/*
 * Write the property payload with the values that are in range; returns its
 * length, 0 when there is none, or -1 if it does not fit. The history is left
 * out when only it does not fit.
 */
int sysmon_encode(char *buf, size_t size, const SysmonValues *v);
/*
 * Values from a property payload. A field that is malformed, out of range or
 * given twice is missing; returns 0 if any value is present, -1 if none is.
 */
int sysmon_decode(const char *data, unsigned long len, SysmonValues *v);
