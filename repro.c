// A pthread condvar stops waking anyone, permanently, once one waiter has been passed over for 2^23 newer waits (macOS 27.0.1, libpthread-553.0.1, psynch condvar).
//
// A pool of 4 workers waits on one condvar for jobs. The main thread submits one job at a time: push under the mutex, unlock, pthread_cond_signal, then wait for a worker to take it. Submitting back to back soon lets one worker get passed over. From then on, the main thread waits for every worker to be blocked in the kernel before each submission. The kernel keeps waking the other three, so the passed-over worker never gets a job. Once 2^23 newer waits have gone by, pthread_cond_signal and pthread_cond_broadcast wake nobody, with a job queued and all four workers waiting.
//
// Build and run: clang -O2 -o repro repro.c && ./repro   (about 1 to 2 minutes on an M-series Mac)

#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static long queued = 0, taken = 0, last_taken[WORKERS];
static int waiting = 0;
static atomic_long completed = 0;

static void *worker(void *argument) {
	int self = (int)(intptr_t)argument;
	for (;;) {
		pthread_mutex_lock(&mutex);
		while (queued == 0) {
			waiting++;
			pthread_cond_wait(&condition, &mutex);
			waiting--;
		}
		queued--;
		last_taken[self] = ++taken;
		pthread_mutex_unlock(&mutex);
		atomic_fetch_add(&completed, 1);
	}
	return NULL;
}

// Whether the thread is blocked in the kernel, rather than on its way into or out of pthread_cond_wait.
static int blocked(pthread_t thread) {
	struct thread_basic_info info;
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	if (thread_info(pthread_mach_thread_np(thread), THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) {
		return 0;
	}
	return info.run_state == TH_STATE_WAITING;
}

static long most_passed_over(void) {
	long most = 0;
	for (int i = 0; i < WORKERS; i++) {
		if (taken - last_taken[i] > most) most = taken - last_taken[i];
	}
	return most;
}

int main(void) {
	pthread_t workers[WORKERS];
	for (int i = 0; i < WORKERS; i++) {
		pthread_create(&workers[i], NULL, worker, (void *)(intptr_t)i);
	}
	int settle = 0;
	time_t start = time(NULL), status = start;
	for (long job = 1;; job++) {
		if (settle) {
			for (;;) {
				pthread_mutex_lock(&mutex);
				int all = waiting == WORKERS;
				pthread_mutex_unlock(&mutex);
				if (all) break;
			}
			for (int i = 0; i < WORKERS; i++) {
				while (!blocked(workers[i])) {
				}
			}
		} else {
			pthread_mutex_lock(&mutex);
			if (most_passed_over() >= 100000) {
				settle = 1;
				printf("job %ld: a worker has been passed over for 100000 jobs; now submitting only while every worker is blocked\n", job);
			}
			pthread_mutex_unlock(&mutex);
		}

		pthread_mutex_lock(&mutex);
		queued++;
		pthread_mutex_unlock(&mutex);
		pthread_cond_signal(&condition);

		struct timespec began, now;
		clock_gettime(CLOCK_MONOTONIC, &began);
		while (atomic_load(&completed) < job) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec - began.tv_sec < 2) continue;
			pthread_mutex_lock(&mutex);
			uint32_t *words = (uint32_t *)&condition; // L, S, U sequence words (libpthread's pthread_cond_t layout)
			printf("STUCK at job %ld: %ld queued, %d of %d workers waiting, one worker passed over for %ld jobs; condvar L=%u S=%u U=%u\n",
				job, queued, waiting, WORKERS, most_passed_over(), words[6] >> 8, words[7] >> 8, words[8] >> 8);
			pthread_mutex_unlock(&mutex);
			pthread_cond_broadcast(&condition);
			sleep(1);
			printf("after pthread_cond_broadcast: %s\n", atomic_load(&completed) >= job ? "recovered" : "still stuck");
			return 1;
		}

		if (time(NULL) - status >= 15) {
			status = time(NULL);
			pthread_mutex_lock(&mutex);
			printf("%ld s: %ld jobs, one worker passed over for %ld\n", (long)(status - start), taken, most_passed_over());
			pthread_mutex_unlock(&mutex);
			fflush(stdout);
		}
	}
}
