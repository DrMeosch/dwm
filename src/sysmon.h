/* See LICENSE file for copyright and license details. */

/*
 * Contract between dwm and the dwmsysmon sampler, which are separate processes.
 * The sampler publishes the latest values as a short ASCII string of space
 * separated fields ("cpu=42 temp=55 fan=2400,2350") in the SYSMON_ATOM property
 * of the root window; a field whose source is missing is left out, and unknown
 * fields are skipped. dwm publishes whether the section is shown as a CARDINAL
 * (0 or 1), followed by a window that exists as long as dwm does, in
 * SYSMON_ENABLED_ATOM so an idle sampler can stop waking up and a sampler can
 * tell when dwm is gone.
 */
#include <stddef.h>

#define SYSMON_ATOM "_DWM_SYSMON"
#define SYSMON_ENABLED_ATOM "_DWM_SYSMON_ENABLED"
#define SYSMON_OWNER_ATOM "_DWM_SYSMON_OWNER" /* selection: the sampler of a display */
#define SYSMON_MAXLEN 64 /* bytes dwm reads; multiple of 4 */
#define SYSMON_MAXTEMP 150 /* degrees Celsius */
#define SYSMON_MAXFAN 30000 /* RPM */
#define SYSMON_FANS 4 /* fans in one payload */

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
} SysmonValues;

/* Parse the aggregate "cpu" line of /proc/stat; 0 on success, -1 if malformed. */
int sysmon_cpu_parse(const char *line, SysmonCpu *out);
/* Busy share 0..100 between two readings, or -1 if they do not describe a sane interval. */
int sysmon_cpu_percent(const SysmonCpu *prev, const SysmonCpu *cur);
/* Set every value to missing; two cleared and equally decoded values are equal with memcmp. */
void sysmon_clear(SysmonValues *v);
/*
 * Write the property payload with the values that are in range; returns its
 * length, 0 when there is none, or -1 if it does not fit.
 */
int sysmon_encode(char *buf, size_t size, const SysmonValues *v);
/*
 * Values from a property payload. A field that is malformed, out of range or
 * given twice is missing; returns 0 if any value is present, -1 if none is.
 */
int sysmon_decode(const char *data, unsigned long len, SysmonValues *v);
