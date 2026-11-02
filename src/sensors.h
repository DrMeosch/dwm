/* See LICENSE file for copyright and license details. */

/*
 * CPU temperature and fan sensors for dwmsysmon, found in the Linux hwmon and
 * thermal classes below a sysfs root ("/sys", or a fixture tree in tests), and
 * the GPU usage of the cards in the DRM class: amdgpu's gpu_busy_percent, and
 * for an Intel (i915) GPU the busy time of its engines from its perf PMU.
 * Opening those counters takes CAP_PERFMON (make install gives dwmsysmon that
 * capability as permitted, and the sampler makes it effective only to open the
 * counters, first thing, so a system without it just shows no Intel GPU), and
 * then the sampler gives up every privilege; it reads them through the open
 * descriptors from then on. Only the sampler reads sensors; dwm never does.
 *
 * Reading the usage of a GPU that is runtime-suspended would wake it up (or
 * fail with EBUSY on newer kernels), and a read of an awake one may restart
 * its autosuspend timer. So a GPU, or an Intel GPU's counters, is only read
 * while its power/runtime_status says it is active (or has no runtime power
 * management); it counts as idle without a read while it is suspended, and is
 * read again only after its autosuspend delay has passed: the sampler neither
 * wakes a GPU nor keeps it awake. Opening i915 counters, even of a suspended
 * GPU, and keeping them open do not wake it or keep it awake either: the
 * kernel counts engine busy time in software and samples only engines that
 * are already awake. There is no unprivileged GPU usage to read on NVIDIA's
 * driver.
 */
#include "sysmon.h"

#define SENSORS_PATHLEN 256
#define SENSORS_TEMPS 8 /* temperature inputs, one per CPU package */
#define SENSORS_INTERVAL 6000 /* ms between sensor reads, slower than the CPU */
#define SENSORS_MAXDELAY 60000 /* ms between sensor reads at most, however slow they are */
#define SENSORS_REFIND 60000 /* ms before missing or failing sources are looked for again */
#define SENSORS_GPUS 4 /* GPUs read */
#define SENSORS_GPUMARGIN 1000 /* ms past a GPU's autosuspend delay before it is read again */
#define SENSORS_ENGINES 16 /* engine busy counters of an Intel GPU */
#define SENSORS_PENDING (-2) /* a GPU usage that has no value yet, but no gap in its history either */

typedef struct {
	char status[SENSORS_PATHLEN]; /* power/runtime_status of its device */
	char busy[SENSORS_PATHLEN]; /* amdgpu: its gpu_busy_percent; empty for an Intel GPU */
	/* Intel: the busy counters of its engines in its perf PMU */
	unsigned int pmutype; /* the PMU's perf event type */
	int cpu; /* the CPU its events are counted on */
	int nengine;
	unsigned long long config[SENSORS_ENGINES]; /* each engine's busy event */
	int fd[SENSORS_ENGINES]; /* open from the start, else -1 */
	unsigned long long busyns[SENSORS_ENGINES], enabledns[SENSORS_ENGINES]; /* at the last read */
	int counting; /* there is a reading to count from, taken since it last woke up */
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
	int gpuopen; /* Intel GPUs that are found get their counters opened: only at the start */
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
/*
 * Look for the GPUs below sys whose usage can be read; Intel counters that are
 * already open stay open for the same GPU, and the others are closed. An Intel
 * GPU without counters is only shown once they could be opened.
 */
void sensors_findgpus(Sensors *s, const char *sys);
/*
 * Look for the GPUs at monotonic ms now and open the counters of the Intel
 * ones, before sensors_dropcaps(); later finds open none.
 */
void sensors_startgpus(Sensors *s, const char *sys, long long now);
/*
 * Give up every capability, ambient, inheritable, permitted and effective,
 * for good (no_new_privs); 0 on success.
 */
int sensors_dropcaps(void);
/*
 * The usage of the busiest GPU, 0..100; SENSORS_PENDING when none has a value
 * but one has none only because it just woke up; -1 when none could be read.
 * A suspended GPU is idle.
 */
int sensors_readgpus(Sensors *s);
/*
 * Push the GPU usage into v, looking for the GPUs first when that is due at
 * monotonic ms now; a usage still pending leaves the history alone.
 */
void sensors_samplegpus(Sensors *s, const char *sys, long long now, SysmonValues *v);
/*
 * ms until the next GPU read, for the sampler's interval in ms, when this one
 * took spent ms: never within the autosuspend delay of the GPUs.
 */
long long sensors_gpudelay(const Sensors *s, long interval, long long spent);
