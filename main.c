#include <fcntl.h>
#include <gpiod.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "error.h"

#ifdef __GNUG__
	#define likely(x)       __builtin_expect((x), 1)
	#define unlikely(x)     __builtin_expect((x), 0)
#else
	#define likely(x) (x)
	#define unlikely(x) (x)
#endif

#define NTP_KEY	1314148400
#define DEFAULT_FUDGE_FACTOR 0.0

#define CONSUMER "Consumer"

#define BILLION 1000000000

#define BOOL char
#define FALSE 0
#define TRUE 1

char debug = 0;

struct shmTime {
	int    mode; /* 0 - if valid set
		      *       use values, 
		      *       clear valid
		      * 1 - if valid set 
		      *       if count before and after read of 
		      *       values is equal,
		      *         use values 
		      *       clear valid
		      */
	int    count;
	time_t clockTimeStampSec;      /* external clock */
	int    clockTimeStampUSec;     /* external clock */
	time_t receiveTimeStampSec;    /* internal clock, when external value was received */
	int    receiveTimeStampUSec;   /* internal clock, when external value was received */
	int    leap;
	int    precision;
	int    nsamples;
	volatile int valid;
	int    dummy[10]; 
};

int write_pidfile(const char *const fname)
{
	FILE *fh = fopen(fname, "w");
	if (!fh)
		error_exit("write_pidfile: failed creating file %s", fname);

	fprintf(fh, "%d", getpid());

	fclose(fh);

	return 0;
}

struct shmTime * get_shm_pointer(const int unitNr)
{
	void *addr = NULL;
	struct shmTime *pst = NULL;
	int shmid = shmget(NTP_KEY + unitNr, sizeof(struct shmTime), IPC_CREAT);
	if (shmid == -1)
		error_exit("get_shm_pointer: shmget failed");

	addr = shmat(shmid, NULL, 0);
	if (addr == (void *)-1)
		error_exit("get_shm_pointer: shmat failed");

	pst = (struct shmTime *)addr;

	return pst;
}

void notify_ntp(struct shmTime *const pst, int *fudge_s, int *fudge_ns, struct timespec *const ts, long int *wrap, const int rebase, long int *retrieved)
{
	(*retrieved) += pst -> valid == 0;

	pst -> valid = 0;

	static int rebase_count = 0;

	if (rebase > rebase_count)
	{
		static long int rebase_total = 0.0;

		rebase_total += ts -> tv_nsec;

		if (++rebase_count == rebase)
		{
			rebase_total /= rebase_count;

			printf("rebasing to %ldns\n", rebase_total);

			*fudge_s = -(rebase_total / BILLION);
			*fudge_ns = -(rebase_total % BILLION);
		}
	}
	else
	{
		/* apply fudge */
		ts -> tv_sec += *fudge_s;
		ts -> tv_nsec += *fudge_ns;

		if (ts -> tv_nsec > BILLION - 1)
		{
			ts -> tv_nsec -= BILLION;
			ts -> tv_sec++;
		}

		/* if local time is more than 0.5 seconds off, assume
		 * it is the next second
		 */
		pst -> receiveTimeStampSec = ts -> tv_sec;
		pst -> receiveTimeStampUSec = ts -> tv_nsec / 1000;

		if (ts -> tv_nsec >= BILLION / 2)
		{
			ts -> tv_sec++;
			(*wrap)++;
		}

		pst -> clockTimeStampSec = ts -> tv_sec;
		pst -> clockTimeStampUSec = 0;

		pst -> leap = pst -> mode = pst -> count = /* */
		pst -> precision = 0;	/* 0 = precision of 1 sec., -1 = 0.5s */

		pst -> valid = 1;
	}
}

void set_nice(void)
{
	if (nice(-20) == -1)
		error_exit("nice() failed");
}

void set_prio(void)
{
	struct sched_param p;

	p.sched_priority = sched_get_priority_max(SCHED_RR);

	if (sched_setscheduler(0, SCHED_RR, &p) == -1)
		error_exit("sched_setscheduler() failed");
}

int get_value(struct gpiod_line *const line)
{
	int rc = gpiod_line_get_value(line);
	if (rc == -1)
		error_exit("gpiod_line_get_value failed");

	return rc;
}

void wait_for_state(struct gpiod_line *const line, const int what)
{
	int value = 0;

	do
	{
		value = get_value(line);
	}
	while(value != what);
}

void sleep_for_offset(const double idle_factor)
{
	struct timespec ts;
	if (unlikely(clock_gettime(CLOCK_REALTIME, &ts) == -1))
		error_exit("clock_gettime(CLOCK_REALTIME) failed");

	long offset = ts.tv_nsec;
	long next_int = BILLION - offset;
	usleep((long)((double)next_int * idle_factor) / 1000);
}

void debug_log(const struct timespec *const ts, const long int wrap_count, const long int retrieved)
{
	static long int total_count = 0, min_count = 0, max_count = 0;
	static double min_avg = 0, avg_avg = 0, max_avg = 0;

	if (debug)
	{
		double offset = (double)((ts -> tv_nsec >= BILLION / 2) ? -(BILLION - ts -> tv_nsec) : ts -> tv_nsec) / (double)BILLION; 

		if (total_count)
		{
			double cur_avg = avg_avg / (double)total_count;

			if (offset < cur_avg)
			{
				min_avg += offset;
				min_count++;
			}
			else
			{
				max_avg += offset;
				max_count++;
			}
		}

		avg_avg += offset;
		total_count++;

		printf("%ld.%09ld] interrupt #%ld, %.2f%% wraps, %ld retrieved, offset %fs %f/%f/%f\n", ts -> tv_sec, ts -> tv_nsec, total_count, wrap_count * 100. / total_count, retrieved, offset, min_avg/(double)min_count, avg_avg/(double)total_count, max_avg/(double)max_count);
	}
}

void pulse_pin(struct gpiod_line *const pin, int *const memory)
{
	if (pin)
	{
		if (gpiod_line_set_value(pin, *memory) == -1)
			error_exit("gpiod_line_set_value failed");

		*memory = !*memory;
	}
}

void polling_driven(struct shmTime *const pst, int fudge_s, int fudge_ns, struct gpiod_line *const pps_in_line, struct gpiod_line *const pps_out_line, const double idle_factor, int rebase)
{
	long int wrap_count = 0;
	long int retrieved = 0;
	struct timespec ts = { 0, 0 };
	char first = 1;
	int pulse_out_value = 0;

	if (gpiod_line_request_input(pps_in_line, CONSUMER) == -1)
		error_exit("gpiod_line_request_input failed");

	for(;;)
	{
		// wait for high
		wait_for_state(pps_in_line, 1);

		if (unlikely(clock_gettime(CLOCK_REALTIME, &ts) == -1))
			error_exit("clock_gettime(CLOCK_REALTIME) failed");

		pulse_pin(pps_out_line, &pulse_out_value);

		// register offset at ntp
		if (unlikely(first))
			first = 0;
		else
			notify_ntp(pst, &fudge_s, &fudge_ns, &ts, &wrap_count, rebase, &retrieved);

		debug_log(&ts, wrap_count, retrieved);

		sleep_for_offset(idle_factor);
	}
}

void interrupt_driven(struct shmTime *const pst, int fudge_s, int fudge_ns, const char edge_both, struct gpiod_line *const pps_in_line, struct gpiod_line *const pps_out_line, const int rebase)
{
	char dummy = 0;
	int value = 0, gpio_pps_out_pin_value = 0;
	long int retrieved = 0;
	long int wrap_count = 0;

	if (edge_both) {
		if (gpiod_line_request_both_edges_events(pps_in_line, CONSUMER) == -1)
			error_exit("gpiod_line_request_both_edges_events failed");
	}
	else {
		if (gpiod_line_request_rising_edge_events(pps_in_line, CONSUMER) == -1)
			error_exit("gpiod_line_request_rising_edge_events failed");
	}

	for(;;)
	{
		struct gpiod_line_event event = { 0 };
		int rc = gpiod_line_event_wait(pps_in_line, NULL);
		if (rc == -1)
			error_exit("gpiod_line_event_wait failed");

		if (gpiod_line_event_read(pps_in_line, &event) == -1)
			error_exit("gpiod_line_event_read failed");

		if (edge_both)
		{
			value = get_value(pps_in_line);
			if (value == 0)
				continue;
		}

		notify_ntp(pst, &fudge_s, &fudge_ns, &event.ts, &wrap_count, rebase, &retrieved);

		pulse_pin(pps_out_line, &gpio_pps_out_pin_value);

		debug_log(&event.ts, wrap_count, retrieved);
	}
}

void lock_in_memory(void)
{
	if (mlockall(MCL_CURRENT) == -1)
		error_exit("mlockall(MCL_CURRENT)");
}

void help(void)
{
	fprintf(stderr, "-N x    x must be 0...3, it is the NTP shared memory unit number\n");
	fprintf(stderr, "-g x    gpio pin to listen on\n");
	fprintf(stderr, "-G x    name of the GPIOchip (default: gpiochip0)\n");
	fprintf(stderr, "-d      debug mode\n");
	fprintf(stderr, "-F x    fudge factor (in microseconds)\n");
	fprintf(stderr, "-p x    when enabled, toggle GPIO pin x so that you can measure delays using a scope\n");
	fprintf(stderr, "-f      do not fork\n");
	fprintf(stderr, "-b      handle both on rising/falling but ignore falling\n");
	fprintf(stderr, "-P      use polling - for when the device does not support interrupts on gpio state changes\n");
	fprintf(stderr, "-i x    polling: how long shall we sleep (part of a second) and not poll for interrupts. e.g. 0.95\n");
	fprintf(stderr, "-R x    re-base: measure 'x' times the offset, then take the average and then use that as an offset. this can be useful when using e.g. a tcxo or an other non-synced pulse-source\n");
}

int main(int argc, char *argv[])
{
	struct shmTime *pst = NULL;
	int fudge_s = 0, fudge_ns = 0;
	int unit = 0; /* 0...3 */
	int gpio_pps_in_pin = -1, gpio_pps_out_pin = -1;
	const char *gpio_chip = "gpiochip0";
	char do_fork = 1, gpio_pps_out_pin_value = 1;
	int c = -1;
	char edge_both = 0, polling = 0;
	double idle_factor = 0.95;
	int rebase = -1;
	struct gpiod_chip *chip = NULL;
	struct gpiod_line *pps_in_line = NULL;
	struct gpiod_line *pps_out_line = NULL;

	printf("rpi_gpio_ntp v" VERSION ", (C) 2013-2024 by folkert@vanheusden.com\n\n");

	while((c = getopt(argc, argv, "R:G:i:bp:fN:g:F:dPh")) != -1)
	{
		switch(c)
		{
			case 'R':
				rebase = atoi(optarg);
				break;

			case 'P':
				polling = 1;
				break;

			case 'i':
				idle_factor = atof(optarg);
				break;

			case 'b':
				edge_both = 1;
				break;

			case 'G':
				gpio_chip = optarg;
				break;

			case 'p':
				gpio_pps_out_pin = atoi(optarg);
				break;

			case 'f':
				do_fork = 0;
				break;

			case 'N':
				unit = atoi(optarg);
				break;

			case 'g':
				gpio_pps_in_pin = atoi(optarg);
				break;

			case 'F':
				fudge_s = atol(optarg) / 1000000;
				fudge_ns = (atol(optarg) % 1000000) * 1000;
				break;

			case 'd':
				debug = 1;
				break;

			case 'h':
				help();
				return 0;

			default:
				error_exit("%c is an invalid switch", c);
		}
	}

	if (gpio_pps_in_pin == -1)
		error_exit("You need to select a GPIO pin to \"listen\" on.");

	if (gpio_pps_out_pin == gpio_pps_in_pin)
		error_exit("You can't use the same pin for both in- and output.");

	chip = gpiod_chip_open_by_name(gpio_chip);
	if (!chip)
		error_exit("Open chip failed");

	pps_in_line = gpiod_chip_get_line(chip, gpio_pps_in_pin);
	if (!pps_in_line)
		error_exit("Get input line failed");

	if (debug)
	{
		printf("NTP unit : %d\n", unit);
		printf("Fudge    : %d.%09d\n", fudge_s, fudge_ns);

		if (polling)
			printf("Polling mode\n");

		if (do_fork)
		{
			do_fork = 0;

			printf("\"Fork into the background\" disabled because of debug mode.\n");
		}
	}

	lock_in_memory();

	/* connect to ntp */
	pst = get_shm_pointer(unit);

	if (gpio_pps_out_pin != -1)
	{
		pps_out_line = gpiod_chip_get_line(chip, gpio_pps_out_pin);
		if (!pps_out_line)
			error_exit("Get output line failed");

		if (gpiod_line_request_output(pps_out_line, CONSUMER, 0) == -1)
			error_exit("gpiod_line_request_output failed");
	}

	set_nice();

	set_prio();

	if (do_fork && daemon(0, 0) == -1)
		error_exit("daemon() failed");

	if (polling)
		polling_driven(pst, fudge_s, fudge_ns, pps_in_line, pps_out_line, idle_factor, rebase);
	else
		interrupt_driven(pst, fudge_s, fudge_ns, edge_both, pps_in_line, pps_out_line, rebase);

	return 0;
}
