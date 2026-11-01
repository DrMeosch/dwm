/* See LICENSE file for copyright and license details. */

/*
 * CPU temperature and fan sensors for dwmsysmon, found in the Linux hwmon and
 * thermal classes below a sysfs root ("/sys", or a fixture tree in tests).
 * Only the sampler reads sensors; dwm never does.
 */
#include "sysmon.h"

#define SENSORS_PATHLEN 256
#define SENSORS_TEMPS 8 /* temperature inputs, one per CPU package */
#define SENSORS_INTERVAL 6000 /* ms between sensor reads, slower than the CPU */
#define SENSORS_MAXDELAY 60000 /* ms between sensor reads at most, however slow they are */
#define SENSORS_REFIND 60000 /* ms before missing or failing sources are looked for again */

typedef struct {
	char temp[SENSORS_TEMPS][SENSORS_PATHLEN];
	int ntemp;
	char fan[SYSMON_FANS][SENSORS_PATHLEN];
	int nfan;
	long long found; /* when the sources were looked for, -1 for never */
	int stale; /* a source is missing or failed since */
} Sensors;

/* Forget the sources, so the next sample looks for them. */
void sensors_init(Sensors *s);
/* Look for the CPU temperature inputs and the fans below sys. */
void sensors_find(Sensors *s, const char *sys);
/* Read the temperature and fans into v; -1 if a source that was found failed. */
int sensors_read(const Sensors *s, SysmonValues *v);
/* sensors_read, looking for the sources first when that is due at monotonic ms now. */
void sensors_sample(Sensors *s, const char *sys, long long now, SysmonValues *v);
/* ms until the next sensor read when this one took spent ms. */
long long sensors_delay(long long spent);
