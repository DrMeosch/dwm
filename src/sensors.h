/* See LICENSE file for copyright and license details. */

/*
 * CPU temperature and fan sensors for dwmsysmon, found in the Linux hwmon and
 * thermal classes below a sysfs root ("/sys", or a fixture tree in tests), and
 * the GPU usage, from the DRM class. Only the sampler reads sensors; dwm never
 * does.
 *
 * Reading the usage of a GPU that is runtime-suspended would wake it up (or
 * fail with EBUSY on newer kernels), and every read of an awake one restarts
 * its autosuspend timer. So a GPU is only read while its power/runtime_status
 * says it is active (or has no runtime power management), counts as idle
 * without a read while it is suspended, and is read again only after its
 * autosuspend delay has passed: the sampler neither wakes a GPU nor keeps it
 * awake. There is no unprivileged GPU usage to read on NVIDIA's driver.
 */
#include "sysmon.h"

#define SENSORS_PATHLEN 256
#define SENSORS_TEMPS 8 /* temperature inputs, one per CPU package */
#define SENSORS_INTERVAL 6000 /* ms between sensor reads, slower than the CPU */
#define SENSORS_MAXDELAY 60000 /* ms between sensor reads at most, however slow they are */
#define SENSORS_REFIND 60000 /* ms before missing or failing sources are looked for again */
#define SENSORS_GPUS 4 /* GPUs read */
#define SENSORS_GPUMARGIN 1000 /* ms past a GPU's autosuspend delay before it is read again */

typedef struct {
	char status[SENSORS_PATHLEN]; /* power/runtime_status of its device */
	char busy[SENSORS_PATHLEN]; /* amdgpu: gpu_busy_percent; Intel: gt_act_freq_mhz */
	char max[SENSORS_PATHLEN]; /* Intel: gt_max_freq_mhz, the busy share being the frequency's; else empty */
} Gpu;

typedef struct {
	char temp[SENSORS_TEMPS][SENSORS_PATHLEN];
	int ntemp;
	char fan[SYSMON_FANS][SENSORS_PATHLEN];
	int nfan;
	long long found; /* when the sources were looked for, -1 for never */
	int stale; /* a source is missing or failed since */
	Gpu gpu[SENSORS_GPUS];
	int ngpu;
	long long gpuawake; /* ms a read keeps the GPUs awake: their longest autosuspend delay */
	long long gpufound; /* when the GPUs were looked for, -1 for never */
	int gpustale; /* there is no GPU, or one failed since */
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
/* Look for the GPUs below sys whose usage can be read without privileges. */
void sensors_findgpus(Sensors *s, const char *sys);
/* The usage of the busiest GPU, 0..100, or -1 when none could be read; a suspended GPU is idle. */
int sensors_readgpus(Sensors *s);
/* Push the GPU usage into v, looking for the GPUs first when that is due at monotonic ms now. */
void sensors_samplegpus(Sensors *s, const char *sys, long long now, SysmonValues *v);
/*
 * ms until the next GPU read, for the sampler's interval in ms, when this one
 * took spent ms: never within the autosuspend delay of the GPUs.
 */
long long sensors_gpudelay(const Sensors *s, long interval, long long spent);
