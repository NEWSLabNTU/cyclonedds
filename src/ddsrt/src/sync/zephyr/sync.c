/*
 * nano-ros: Zephyr-native ddsrt sync backend (nano-ros issue 0496).
 *
 * Replaces sync/posix/sync.c on Zephyr so that ddsrt mutexes and condvars are
 * EMBEDDED k_mutex / k_condvar rather than handles into Zephyr's fixed
 * CONFIG_MAX_PTHREAD_{MUTEX,COND}_COUNT pools. Rationale, and why rwlock/once
 * stay on pthreads, in dds/ddsrt/sync/zephyr.h.
 *
 * Two behavioural notes, both deliberate:
 *
 *  - k_mutex is RECURSIVE for its owner, where a pthread NORMAL mutex
 *    deadlocks. Correct code cannot tell the difference; incorrect code that
 *    re-acquires its own lock silently succeeds here and hangs on POSIX. That
 *    asymmetry is worth knowing when a bug reproduces natively but not on
 *    Zephyr — it is exactly what happened with the striped addrset locks, whose
 *    nesting hazard only ever hung the native build.
 *  - k_mutex gives priority inheritance, as Zephyr's pthread_mutex did before
 *    it (being the same object underneath), so scheduling behaviour is
 *    unchanged.
 *
 * Failures abort, matching the POSIX backend: these calls have no error return,
 * and a broken lock cannot be carried on from. Unlike the POSIX backend, the
 * pool-exhaustion failure mode simply does not exist here — there is no
 * allocation to fail.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>

#include "dds/ddsrt/sync.h"
#include "dds/ddsrt/time.h"

static void sync_fatal (const char *what, int err)
{
  printk ("ddsrt: %s failed (%d)\n", what, err);
  abort ();
}

/* ---- mutex ---------------------------------------------------------------- */

void ddsrt_mutex_init (ddsrt_mutex_t *mutex)
{
  int err;
  assert (mutex != NULL);
  /* Cannot fail for a caller-owned object, but check rather than assume. */
  if ((err = k_mutex_init (&mutex->mutex)) != 0)
    sync_fatal ("k_mutex_init", err);
}

void ddsrt_mutex_destroy (ddsrt_mutex_t *mutex)
{
  assert (mutex != NULL);
  /* Nothing to release: the object is the caller's memory, and that is the
     whole point of this backend. */
  (void) mutex;
}

void ddsrt_mutex_lock (ddsrt_mutex_t *mutex)
{
  int err;
  assert (mutex != NULL);
  if ((err = k_mutex_lock (&mutex->mutex, K_FOREVER)) != 0)
    sync_fatal ("k_mutex_lock", err);
}

bool ddsrt_mutex_trylock (ddsrt_mutex_t *mutex)
{
  int err;
  assert (mutex != NULL);
  err = k_mutex_lock (&mutex->mutex, K_NO_WAIT);
  if (err != 0 && err != -EBUSY)
    sync_fatal ("k_mutex_lock(K_NO_WAIT)", err);
  return err == 0;
}

void ddsrt_mutex_unlock (ddsrt_mutex_t *mutex)
{
  int err;
  assert (mutex != NULL);
  if ((err = k_mutex_unlock (&mutex->mutex)) != 0)
    sync_fatal ("k_mutex_unlock", err);
}

/* ---- condvar -------------------------------------------------------------- */

void ddsrt_cond_init (ddsrt_cond_t *cond)
{
  int err;
  assert (cond != NULL);
  if ((err = k_condvar_init (&cond->cond)) != 0)
    sync_fatal ("k_condvar_init", err);
}

void ddsrt_cond_destroy (ddsrt_cond_t *cond)
{
  assert (cond != NULL);
  (void) cond; /* caller-owned; see ddsrt_mutex_destroy */
}

void ddsrt_cond_wait (ddsrt_cond_t *cond, ddsrt_mutex_t *mutex)
{
  int err;
  assert (cond != NULL);
  assert (mutex != NULL);
  if ((err = k_condvar_wait (&cond->cond, &mutex->mutex, K_FOREVER)) != 0)
    sync_fatal ("k_condvar_wait", err);
}

bool ddsrt_cond_waituntil (ddsrt_cond_t *cond, ddsrt_mutex_t *mutex, dds_time_t abstime)
{
  assert (cond != NULL);
  assert (mutex != NULL);

  if (abstime == DDS_NEVER)
  {
    ddsrt_cond_wait (cond, mutex);
    return true;
  }

  /* k_condvar_wait takes a RELATIVE timeout; abstime is on the same wall clock
     dds_time() reads. Round the conversion UP: a timeout reported early would
     be indistinguishable to the caller from a real one, whereas waiting a tick
     longer than asked is harmless. */
  const dds_time_t now = dds_time ();
  k_timeout_t timeout;
  if (abstime <= now)
    timeout = K_NO_WAIT;
  else
  {
    const dds_duration_t rel_ns = abstime - now;
    const int64_t rel_us = (rel_ns + 999) / 1000;
    timeout = K_USEC (rel_us);
  }

  const int err = k_condvar_wait (&cond->cond, &mutex->mutex, timeout);
  if (err == 0)
    return true;
  if (err == -EAGAIN)
    return false; /* timed out */
  sync_fatal ("k_condvar_wait(timed)", err);
  return false; /* unreachable; sync_fatal aborts */
}

bool ddsrt_cond_waitfor (ddsrt_cond_t *cond, ddsrt_mutex_t *mutex, dds_duration_t reltime)
{
  assert (cond != NULL);
  assert (mutex != NULL);
  return ddsrt_cond_waituntil (cond, mutex, ddsrt_time_add_duration (dds_time (), reltime));
}

void ddsrt_cond_signal (ddsrt_cond_t *cond)
{
  assert (cond != NULL);
  (void) k_condvar_signal (&cond->cond); /* returns the number woken, not an error */
}

void ddsrt_cond_broadcast (ddsrt_cond_t *cond)
{
  assert (cond != NULL);
  (void) k_condvar_broadcast (&cond->cond); /* returns the number woken */
}

/* ---- rwlock: still pthreads, on purpose ---------------------------------- */
/* Cyclone creates exactly one rwlock in production code (the log sink in
   ddsrt/src/log.c), so it is not part of the per-entity term this backend
   exists to remove, and Zephyr has no native reader/writer lock to map onto.
   Hand-rolling one would be new risk for no measurable gain. */

void ddsrt_rwlock_init (ddsrt_rwlock_t *rwlock)
{
  int err;
  assert (rwlock != NULL);
  if ((err = pthread_rwlock_init (&rwlock->rwlock, NULL)) != 0)
    sync_fatal ("pthread_rwlock_init (raise CONFIG_MAX_PTHREAD_RWLOCK_COUNT if ENOMEM)", err);
}

void ddsrt_rwlock_destroy (ddsrt_rwlock_t *rwlock)
{
  assert (rwlock != NULL);
  if (pthread_rwlock_destroy (&rwlock->rwlock) != 0)
    abort ();
}

void ddsrt_rwlock_read (ddsrt_rwlock_t *rwlock)
{
  int err;
  assert (rwlock != NULL);
  err = pthread_rwlock_rdlock (&rwlock->rwlock);
  assert (err == 0);
  (void) err;
}

void ddsrt_rwlock_write (ddsrt_rwlock_t *rwlock)
{
  int err;
  assert (rwlock != NULL);
  err = pthread_rwlock_wrlock (&rwlock->rwlock);
  assert (err == 0);
  (void) err;
}

bool ddsrt_rwlock_tryread (ddsrt_rwlock_t *rwlock)
{
  int err;
  assert (rwlock != NULL);
  err = pthread_rwlock_tryrdlock (&rwlock->rwlock);
  assert (err == 0 || err == EBUSY);
  return err == 0;
}

bool ddsrt_rwlock_trywrite (ddsrt_rwlock_t *rwlock)
{
  int err;
  assert (rwlock != NULL);
  err = pthread_rwlock_trywrlock (&rwlock->rwlock);
  assert (err == 0 || err == EBUSY);
  return err == 0;
}

void ddsrt_rwlock_unlock (ddsrt_rwlock_t *rwlock)
{
  int err;
  assert (rwlock != NULL);
  err = pthread_rwlock_unlock (&rwlock->rwlock);
  assert (err == 0);
  (void) err;
}

/* ---- once: pthread_once_t is caller-owned, not pooled -------------------- */

void ddsrt_once (ddsrt_once_t *control, ddsrt_once_fn init_fn)
{
  /* There are no defined errors that can be returned by pthread_once */
  (void) pthread_once (control, init_fn);
}
