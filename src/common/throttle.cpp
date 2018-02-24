#include "throttle.h"

#include <iostream>
#include <thread>

#include <stdio.h>
#include <sys/sysctl.h>				/* sysctl() */
#include <unistd.h>					/* sysconf(_SC_NPROCESSORS_ONLN) */
#include <libproc.h>				/* proc_pidinfo() */
#include <pwd.h>					/* getpwuid() */
#include <mach/mach.h>				/* mach_absolute_time() */
#include <mach/mach_time.h>
#include <stdlib.h>					/* malloc */
#include <string.h>					/* memset */
#include <time.h>					/* nanosleep */
#include <math.h>
#include <signal.h>
#include <libproc.h>				/* proc_pidinfo */
#include <dispatch/dispatch.h>		/* dispatch_queue */
#include <libkern/OSAtomic.h>
#include <errno.h>					/* errno */
#include <pthread.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_port.h>

int system_ncpu(void) {
	static int ncpu = 0;
	if (ncpu)
		return ncpu;
	
#ifdef _SC_NPROCESSORS_ONLN
	ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
#else
	int mib[2];
	mib[0] = CTL_HW;
	mig[1] = HW_NCPU;
	size_t len = sizeof(ncpu);
	sysctl(mib, 2, &ncpu, &len, NULL, 0);
#endif
	return ncpu;
}

#define NANOSEC_PER_SEC 1000000000
#define LONG_LONG_MAX 9223372036854775807LL
#define ULONG_LONG_MAX (LONG_LONG_MAX * 2ULL + 1)

static struct timespec timespec_from_ns(int64_t nanoseconds) {
	struct timespec timef;
	if (nanoseconds >= NANOSEC_PER_SEC) {
		timef.tv_sec = (int)floor(nanoseconds / NANOSEC_PER_SEC);
		timef.tv_nsec = nanoseconds % NANOSEC_PER_SEC;
	} else {
		timef.tv_sec = 0;
		timef.tv_nsec = nanoseconds;
	}

	return timef;
}

static struct timespec timespec_from_ms(int64_t microseconds) {
	return timespec_from_ns(microseconds * 1000);
}

static uint64_t _schedule_interval = 30000;

struct thread_stats_s {
	thread_act_t thread;
	uint64_t tid;
	uint64_t time;		/* system_time + user_time */
	uint64_t sleep_time;
	int is_sleeping;
	int is_processed;
	struct thread_stats_s *next;
};
typedef struct thread_stats_s *thread_stats_t;

static thread_stats_t _thread_stats_list;	/* pointer to the first task in list */

// find thread in list or create when list is empty
static thread_stats_t get_thread(thread_act_t _thread, uint64_t tid) {
	thread_stats_t thread;
	thread_stats_t head = _thread_stats_list;

	// find in the list if not empty
	if (_thread_stats_list != NULL) {
		for (thread = _thread_stats_list; thread != NULL; thread = thread->next) {
			if (thread->tid == tid) {
				return thread;
			}
		}
	}

	thread = (thread_stats_t)malloc(sizeof(struct thread_stats_s));
	thread->thread = _thread;
	thread->tid = tid;
	thread->time = 0;
	thread->sleep_time = 0;
	thread->is_sleeping = 0;
	thread->is_processed = 0;
	thread->next = head;

	_thread_stats_list = thread;

	return thread;
}

static void delete_thread(uint64_t tid) {
	thread_stats_t head = _thread_stats_list;
	thread_stats_t thread;
	thread_stats_t prev = NULL;
	for (thread = head; thread != NULL; prev = thread, thread = thread->next) {
		if (thread->tid == tid) {
			// if it's the first task in the list simply make the head point to the next task
			if (thread == head) {
				_thread_stats_list = thread->next;
			} else if (prev) {				// if it's not the only task in the list
				prev->next = thread->next;
			}
			free(thread);
			break;
		}
	}
}

void print_threads_stats(const char *s) {
	printf("------> %s\n", s);

	thread_stats_t thread;
	for (thread = _thread_stats_list; thread != NULL; thread = thread->next) {
		printf("tid # %llu, time: %llu, sleep_time: %llu, is_sleep: %d\n",
			thread->tid,
			thread->time,
			thread->sleep_time,
			thread->is_sleeping
		);
	}
}

uint64_t threads_execsleeptime(void) {
	uint64_t pt_sleep_min;
	short there_are_proc_sleeping = 0;
	short all_awake = 0;
	thread_stats_t thread;
	uint64_t sleptns = 0;
	struct timespec sleepspec;

	pt_sleep_min = ULONG_LONG_MAX;
	for (thread = _thread_stats_list; thread != NULL; thread = thread->next) {
		if (thread->sleep_time != 0) {
			if (thread_suspend(thread->thread) != KERN_SUCCESS) {
				fputs("Error: could not suspend thread.\n", stderr);
				continue;
			}
			
			thread->is_sleeping = 1;
			there_are_proc_sleeping = 1;
			if (thread->sleep_time < pt_sleep_min) {
				pt_sleep_min = thread->sleep_time;
			}
		}
	}
	
	if (!there_are_proc_sleeping) {
		return sleptns;
	}

	sleepspec = timespec_from_ms((int64_t)pt_sleep_min);
	nanosleep(&sleepspec, NULL);
	sleptns = pt_sleep_min;

	while (!all_awake) {
		all_awake = 1;
		pt_sleep_min = ULONG_LONG_MAX;
		
		for (thread = _thread_stats_list; thread != NULL; thread = thread->next) {
			if (thread->sleep_time == 0 || thread->is_sleeping == 0)
				continue;
			else if (thread->sleep_time >= sleptns) {
				if (thread_resume(thread->thread) != KERN_SUCCESS) {
					fputs("Error: could not resume thread.\n", stderr);
				}
				thread->is_sleeping = 0;
			} else if (thread->sleep_time < pt_sleep_min) {
				pt_sleep_min = thread->sleep_time;
				all_awake = 0;
			}
		}
		
		if (!all_awake) {
			sleepspec = timespec_from_ms((int64_t)(pt_sleep_min - sleptns));
			nanosleep(&sleepspec, NULL);
			sleptns = pt_sleep_min;
		}
		
	}

	return sleptns;
}

// https://stackoverflow.com/a/6788396
uint64_t threads_info(float lim, pid_t pid, uint64_t tid) {	
    task_t port;
    task_for_pid(mach_task_self(), pid, &port);        

    task_info_data_t tinfo;
    mach_msg_type_number_t task_info_count;

    task_info_count = TASK_INFO_MAX;
    kern_return_t kr = task_info(port, TASK_BASIC_INFO, (task_info_t)tinfo, &task_info_count);
    if (kr != KERN_SUCCESS) {
    	return 0; // TODO handle error
    }

    task_basic_info_t      basic_info;
    thread_array_t         thread_list;
    mach_msg_type_number_t thread_count;

    thread_info_data_t     thinfo;
    mach_msg_type_number_t thread_info_count;

    thread_basic_info_t basic_info_th;
    uint32_t stat_thread = 0; // Mach threads

    thread_identifier_info_t identifier_info_th;

    basic_info = (task_basic_info_t)tinfo;

    // get threads in the task
    kr = task_threads(port, &thread_list, &thread_count);
    if (kr != KERN_SUCCESS) {
    	return 0; // TODO handle error
    }
    if (thread_count > 0) {
        stat_thread += thread_count;
    }

    // reset processed marker in the list
    thread_stats_t thread;
	for (thread = _thread_stats_list; thread != NULL; thread = thread->next) {
		thread->is_processed = 0;
	}

	uint64_t time_prev;
	uint64_t time_diff;
	int64_t sleep_time;
	int64_t work_time;
	float cpuload;

    int j;
    for (j = 0; j < thread_count; j++) {
        thread_info_count = THREAD_IDENTIFIER_INFO_COUNT;
		kr = thread_info(thread_list[j], THREAD_IDENTIFIER_INFO, (thread_info_t)thinfo, &thread_info_count);
    	if(kr != KERN_SUCCESS) {
	        continue;
    	}
		identifier_info_th = (thread_identifier_info_t)thinfo;

		if (identifier_info_th->thread_id == tid) {
			// skip current thread
			continue;
		}

		thread = get_thread(thread_list[j], identifier_info_th->thread_id);

        thread_info_count = THREAD_INFO_MAX;
        kr = thread_info(thread_list[j], THREAD_BASIC_INFO, (thread_info_t)thinfo, &thread_info_count);
        if (kr != KERN_SUCCESS) {
            // error
            continue;
        }
        basic_info_th = (thread_basic_info_t)thinfo;

        // basic_info_th->flags == TH_FLAGS_IDLE

		time_prev = thread->time;
        thread->time = (basic_info_th->system_time.seconds * 1000000) + basic_info_th->system_time.microseconds + (basic_info_th->user_time.seconds * 1000000) + basic_info_th->user_time.microseconds;
        thread->is_processed = 1;

		if (time_prev == 0) {
			continue;
		}
		
		time_diff = thread->time - time_prev;
		cpuload = (float)time_diff / _schedule_interval;
		
		work_time = (int64_t)(_schedule_interval - thread->sleep_time);
		if (work_time == 0) {
			work_time = (int64_t)_schedule_interval;
		}

		sleep_time = (int64_t)floor(thread->sleep_time + work_time * (cpuload - lim) / MAX(cpuload, lim));

		if (sleep_time < 0) {
			sleep_time = 0;
		} else if (sleep_time > (int64_t)_schedule_interval) {
			sleep_time = (int64_t)_schedule_interval;
		}
		
		thread->sleep_time = (uint64_t)sleep_time;

/*
		if (cpuload > 0.01) {
			printf("tid # %llu, time_diff: %llu, cpuload: %.3f, work_time: %llu, sleep_time: %llu\n",
				thread->tid,
				time_diff,
				cpuload,
				work_time,
				sleep_time
			);
		}
*/		
    }

    // delete thread stat from list when not processed
	for (thread = _thread_stats_list; thread != NULL; thread = thread->next) {
		if (thread->is_processed == 0) {
			delete_thread(thread->tid);
		}
	}

	// print_threads_stats("before sleep");

	return threads_execsleeptime();
}

//void *threads_info_thread(void *ptr) {
//	pid_t pid = *((pid_t *)ptr);
 void threads_info_thread(float limit, pid_t pid) {
 	uint64_t tid;
 	pthread_threadid_np(NULL, &tid);

	int64_t sleepns;
	uint64_t loop_slept;
	struct timespec sleepspec;

	while (true) {
		loop_slept = threads_info(limit, pid, tid);

		sleepns = (int64_t)(_schedule_interval - loop_slept);
		if (sleepns > 0) {
			sleepspec = timespec_from_ms(sleepns);
			nanosleep(&sleepspec, NULL);
		}
	}
	// return NULL;
}
