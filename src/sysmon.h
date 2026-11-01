/* See LICENSE file for copyright and license details. */

/*
 * Contract between dwm and the dwmsysmon sampler, which are separate processes.
 * The sampler publishes the latest CPU usage as a short ASCII string ("cpu=42")
 * in the SYSMON_ATOM property of the root window. dwm publishes whether the
 * section is shown as a CARDINAL (0 or 1) in SYSMON_ENABLED_ATOM so an idle
 * sampler can stop waking up.
 */
#include <stddef.h>

#define SYSMON_ATOM "_DWM_SYSMON"
#define SYSMON_ENABLED_ATOM "_DWM_SYSMON_ENABLED"
#define SYSMON_MAXLEN 32 /* bytes dwm reads; multiple of 4 */

/* Cumulative CPU time counters in clock ticks; idle includes iowait. */
typedef struct {
	unsigned long long total, idle;
} SysmonCpu;

/* Parse the aggregate "cpu" line of /proc/stat; 0 on success, -1 if malformed. */
int sysmon_cpu_parse(const char *line, SysmonCpu *out);
/* Busy share 0..100 between two readings, or -1 if they do not describe a sane interval. */
int sysmon_cpu_percent(const SysmonCpu *prev, const SysmonCpu *cur);
/* Write the property payload; returns its length, or -1 if cpu is outside 0..100. */
int sysmon_encode(char *buf, size_t size, int cpu);
/* CPU percent from a property payload, or -1 for anything missing or malformed. */
int sysmon_decode(const char *data, unsigned long len);
