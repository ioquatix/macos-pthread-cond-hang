# macOS pthread condition variable hang

On macOS, a `pthread_cond_t` can stop waking anyone, permanently. It happens once one waiter has been passed over for 2^23 (8,388,608) newer waits. After that, `pthread_cond_signal` and `pthread_cond_broadcast` return without waking any thread, even with work queued and every thread waiting on the condition variable. Nothing recovers it.

Observed on macOS 27.0.1 (26A434), Darwin 27.0.0, xnu-13432.1.9, `libsystem_pthread` libpthread-553.0.1, Apple silicon. The mechanism below is read from the most recent published source, [libpthread-539](https://github.com/apple-oss-distributions/libpthread), which has the same code. It affects the default ("psynch") condition variable, which is what you get with a default mutex. Rust's `std::sync::Condvar`, among others, uses it.

## Reproduce

``` bash
clang -O2 -o repro repro.c && ./repro
```

[`repro.c`](repro.c) runs a pool of 4 workers that wait on one condition variable for jobs. The main thread submits one job at a time: push the job under the mutex, unlock, call `pthread_cond_signal`, then wait for a worker to take the job.

1. Submitting back to back soon leaves one worker passed over.
2. After that, the main thread waits until every worker is blocked in the kernel before each submission. The kernel keeps waking the other three workers, so the passed-over worker never gets a job. POSIX allows this.
3. After 2^23 newer waits the condition variable is dead. One job is queued and all four workers are waiting, but nobody is woken, not even by a broadcast.

On an M-series Mac this takes about 75 seconds:

```
job 688749: a worker has been passed over for 100000 jobs; now submitting only while every worker is blocked
15 s: 2099986 jobs, one worker passed over for 1511238
30 s: 3869285 jobs, one worker passed over for 3280537
45 s: 5647001 jobs, one worker passed over for 5058253
60 s: 7430095 jobs, one worker passed over for 6841347
STUCK at job 8977357: 1 queued, 4 of 4 workers waiting, one worker passed over for 8388608 jobs; condvar L=8977360 S=8977356 U=8977357
after pthread_cond_broadcast: still stuck
```

The hang always happens at exactly 8,388,608 jobs passed over. `L`, `S` and `U` are the condition variable's sequence counters, read out of the `pthread_cond_t`.

## Mechanism

A psynch condition variable keeps three 32-bit sequence words in userspace. Each word counts in steps of `PTHRW_INC` (0x100), which leaves 24 bits of count:

- `L`: how many waits have started.
- `S`: how many waits have finished.
- `U`: how many signals have been sent.

`pthread_cond_wait` increments `L` and `S`, then calls `__psynch_cvwait` with its sequence number. `pthread_cond_signal` returns early if there are no waiters (`L == S`) or if every waiter has already been signalled (`L == U`). Otherwise it increments `U` and calls `__psynch_cvsignal`. In the kernel (`kern_synch.c`), each condition variable's waiters are kept in a queue sorted by sequence number.

Sequence numbers wrap, so the kernel compares them with `is_seqhigher` and `is_seqlower`. These treat one number as higher than another when it is less than half the range ahead: `PTHRW_MAX_READERS / 2`, which is 2^23 counts. A waiter more than 2^23 waits older than the newest one therefore compares as being in the future.

The kernel searches the queue from the head, which is the oldest waiter, and stops at the first waiter whose sequence number is higher than the one it is signalling up to:

- `ksyn_queue_find_signalseq`, used by `pthread_cond_signal`, returns no waiter. The signal is stored as a prepost instead.
- `ksyn_handle_cvbroad`, used by `pthread_cond_broadcast`, breaks out of its loop before waking anyone.
- `ksyn_queue_find_cvpreposeq`, which runs when a new waiter arrives, stops in the same place, so new waiters never consume the stored preposts.

The starved waiter sits at the head of the queue, so every search stops at it. Signals turn into preposts that nothing consumes, while `U` keeps climbing. When `U` reaches `L`, userspace decides every waiter has been signalled and stops calling into the kernel at all. From then on the condition variable never wakes anyone.

When a waiter sleeps through a whole 2^24 wrap, the kernel logs `PSYNCH: pid[N]: cvwait: thread entry with same sequence already present` (`log show --predicate 'eventMessage CONTAINS "PSYNCH"'`).

## Is 2^23 a realistic number?

Yes. A condition variable passes 2^23 waits in:

| Waits per second | Time to hang |
| ---------------- | ------------ |
| 100,000          | 84 seconds   |
| 10,000           | 14 minutes   |
| 1,000            | 2.3 hours    |
| 100              | 23 hours     |

A starved waiter is ordinary. POSIX doesn't promise fairness, and in a thread pool sized for peak load, a spare worker can sit idle for days while the others handle every job. This was first seen as a server that stopped accepting connections after hours of normal traffic ([base-rs/base-scheduler#83](https://github.com/base-rs/base-scheduler/issues/83)). In that server the stalled pool's starved worker had been passed over for 8,388,609 jobs.

## Can the operating system fix it?

Yes. The kernel assumes no waiter falls more than half the sequence space behind the newest one, and nothing enforces that assumption. Some possible fixes:

- Compare sequence numbers relative to the oldest waiter in the queue, rather than with a fixed half window.
- Use wider sequence numbers.
- Make the ulock condition variable the default. libpthread already has one (`_pthread_ulock_cond_wait` and `_pthread_ulock_cond_signal`). It doesn't make windowed comparisons, and it's used when the mutex is ulock-based. Running `PTHREAD_MUTEX_USE_ULOCK=1 ./repro` selects it. In that run no worker was passed over for more than a handful of jobs in 69 million, so the starvation phase never started. That shows the ulock path is fairer, but it isn't a like-for-like test of the hang.

## Workarounds

- Wait with a timeout (`pthread_cond_timedwait`, or `Condvar::wait_timeout` in Rust). When the wait times out, the starved waiter leaves the kernel queue and comes back with a new sequence number, so it never falls 2^23 behind. A timeout of a few seconds is enough.
- Use `pthread_cond_broadcast` instead of `pthread_cond_signal`, so every waiter wakes and rejoins the queue. This costs a thundering herd on every signal.
- Set `PTHREAD_MUTEX_USE_ULOCK=1` in the environment, so mutexes and their condition variables use ulock. libpthread reads it when the process starts, so it has to be set before launch.
