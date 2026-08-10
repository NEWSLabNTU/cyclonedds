/*
 * Copyright(c) 2006 to 2022 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "dds/ddsrt/sync.h"
#include "dds/ddsrt/time.h"

#ifdef __ZEPHYR__
#include <zephyr/kernel.h>
#endif

/* nano-ros: name the failing object instead of dying anonymously — issue 0371.
 *
 * Zephyr's pthread_{mutex,cond,rwlock}_init allocate from fixed pools
 * (CONFIG_MAX_PTHREAD_*_COUNT) and return ENOMEM once one is exhausted.
 * Discarding that return does not avoid the crash, it only displaces it: the
 * handle is left as PTHREAD_*_INITIALIZER, and the first lock on it fails
 * EINVAL and reaches one of the bare abort()s below — typically seconds later,
 * on another thread, in an unrelated function, with nothing logged. That is
 * exactly how issue 0371 presented: a native_sim image died ~19 s into a large
 * Autoware graph with a bare "ZEPHYR FATAL ERROR 4" and no diagnostic.
 *
 * These pools are a real ceiling rather than a theoretical one, because cyclone
 * takes one pool object per proxy entity AND one per addrset — so demand scales
 * with the size of the graph being joined, not with the local participant. The
 * message therefore has to say which pool ran out. */
static void sync_init_failed (const char *what, const char *knob, int err)
{
#ifdef __ZEPHYR__
  printk ("ddsrt: pthread_%s_init failed (%d)\n", what, err);
  if (err == ENOMEM)
    printk ("ddsrt: the Zephyr POSIX %s pool is exhausted -- raise %s\n", what, knob);
#else
  (void) knob;
  fprintf (stderr, "ddsrt: pthread_%s_init failed (%d)\n", what, err);
  fflush (stderr);
#endif
  abort ();
}

void ddsrt_mutex_init (ddsrt_mutex_t *mutex)
{
  int err;
  assert (mutex != NULL);
  /* nano-ros phase-292 W2 — on Zephyr the pthread mutex pool
   * (CONFIG_MAX_PTHREAD_MUTEX_COUNT) is finite; a silent init failure
   * here surfaces later as an abort() in ddsrt_mutex_lock with no
   * indication of the real cause. Fail loudly at the failure site. */
  if ((err = pthread_mutex_init (&mutex->mutex, NULL)) != 0)
    sync_init_failed ("mutex", "CONFIG_MAX_PTHREAD_MUTEX_COUNT", err);
}

void ddsrt_mutex_destroy (ddsrt_mutex_t *mutex)
{
  assert (mutex != NULL);

  if (pthread_mutex_destroy (&mutex->mutex) != 0)
    abort();
}

void ddsrt_mutex_lock (ddsrt_mutex_t *mutex)
{
  assert (mutex != NULL);

  if (pthread_mutex_lock (&mutex->mutex) != 0)
    abort();
}

bool
ddsrt_mutex_trylock (ddsrt_mutex_t *mutex)
{
  int err;
  assert (mutex != NULL);

  err = pthread_mutex_trylock (&mutex->mutex);
  if (err != 0 && err != EBUSY)
    abort();
  return (err == 0);
}

void
ddsrt_mutex_unlock (ddsrt_mutex_t *mutex)
{
  assert (mutex != NULL);

  if (pthread_mutex_unlock (&mutex->mutex) != 0)
    abort();
}

void
ddsrt_cond_init (ddsrt_cond_t *cond)
{
  int err;
  assert (cond != NULL);

  if ((err = pthread_cond_init (&cond->cond, NULL)) != 0)
    sync_init_failed ("cond", "CONFIG_MAX_PTHREAD_COND_COUNT", err);
}

void
ddsrt_cond_destroy (ddsrt_cond_t *cond)
{
  assert (cond != NULL);

  if (pthread_cond_destroy (&cond->cond) != 0)
    abort();
}

void
ddsrt_cond_wait (ddsrt_cond_t *cond, ddsrt_mutex_t *mutex)
{
  assert (cond != NULL);
  assert (mutex != NULL);

  if (pthread_cond_wait (&cond->cond, &mutex->mutex) != 0)
    abort();
}

bool
ddsrt_cond_waituntil(
  ddsrt_cond_t *cond,
  ddsrt_mutex_t *mutex,
  dds_time_t abstime)
{
  struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };

  assert(cond != NULL);
  assert(mutex != NULL);

  if (abstime == DDS_NEVER) {
    ddsrt_cond_wait(cond, mutex);
    return true;
  }
  if (abstime > 0) {
    ts.tv_sec = (time_t) (abstime / DDS_NSECS_IN_SEC);
    ts.tv_nsec = (suseconds_t) (abstime % DDS_NSECS_IN_SEC);
  }

  switch (pthread_cond_timedwait(&cond->cond, &mutex->mutex, &ts)) {
    case 0:
      return true;
    case ETIMEDOUT:
      return false;
    default:
      break;
  }

  abort();
}

bool
ddsrt_cond_waitfor(
  ddsrt_cond_t *cond,
  ddsrt_mutex_t *mutex,
  dds_duration_t reltime)
{
  assert(cond != NULL);
  assert(mutex != NULL);

  return ddsrt_cond_waituntil(
    cond, mutex, ddsrt_time_add_duration(dds_time(), reltime));
}

void
ddsrt_cond_signal (ddsrt_cond_t *cond)
{
  assert (cond != NULL);

  if (pthread_cond_signal (&cond->cond) != 0)
    abort();
}

void
ddsrt_cond_broadcast (ddsrt_cond_t *cond)
{
  assert (cond != NULL);

  if (pthread_cond_broadcast (&cond->cond) != 0)
    abort();
}

void
ddsrt_rwlock_init (ddsrt_rwlock_t *rwlock)
{
  int err;
  assert(rwlock != NULL);

#if __SunOS_5_6
  if ((err = pthread_mutex_init(&rwlock->rwlock, NULL)) != 0)
    sync_init_failed ("mutex", "CONFIG_MAX_PTHREAD_MUTEX_COUNT", err);
#else
  /* process-shared attribute is set to PTHREAD_PROCESS_PRIVATE by default */
  if ((err = pthread_rwlock_init(&rwlock->rwlock, NULL)) != 0)
    sync_init_failed ("rwlock", "CONFIG_MAX_PTHREAD_RWLOCK_COUNT", err);
#endif
}

void
ddsrt_rwlock_destroy (ddsrt_rwlock_t *rwlock)
{
  assert(rwlock != NULL);
#if __SunOS_5_6
  if (pthread_mutex_destroy(&rwlock->rwlock) != 0)
    abort();
#else
  if (pthread_rwlock_destroy(&rwlock->rwlock) != 0)
    abort();
#endif
}

void ddsrt_rwlock_read (ddsrt_rwlock_t *rwlock)
{
  int err;

  assert(rwlock != NULL);
#if __SunOS_5_6
  err = pthread_mutex_lock(&rwlock->rwlock);
#else
  err = pthread_rwlock_rdlock(&rwlock->rwlock);
#endif
  assert(err == 0);
  (void)err;
}

void ddsrt_rwlock_write (ddsrt_rwlock_t *rwlock)
{
  int err;

  assert(rwlock != NULL);
#if __SunOS_5_6
  err = pthread_mutex_lock(&rwlock->rwlock);
#else
  err = pthread_rwlock_wrlock(&rwlock->rwlock);
#endif
  assert(err == 0);
  (void)err;
}

bool ddsrt_rwlock_tryread (ddsrt_rwlock_t *rwlock)
{
  int err;

  assert(rwlock != NULL);
#if __SunOS_5_6
  err = pthread_mutex_trylock(&rwlock->rwlock);
#else
  err = pthread_rwlock_tryrdlock(&rwlock->rwlock);
#endif
  assert(err == 0 || err == EBUSY);
  return err == 0;
}

bool ddsrt_rwlock_trywrite (ddsrt_rwlock_t *rwlock)
{
  int err;

  assert(rwlock != NULL);
#if __SunOS_5_6
  err = pthread_mutex_trylock(&rwlock->rwlock);
#else
  err = pthread_rwlock_trywrlock(&rwlock->rwlock);
#endif
  assert(err == 0 || err == EBUSY);

  return err == 0;
}

void ddsrt_rwlock_unlock (ddsrt_rwlock_t *rwlock)
{
  int err;

  assert(rwlock != NULL);
#if __SunOS_5_6
  err = pthread_mutex_unlock(&rwlock->rwlock);
#else
  err = pthread_rwlock_unlock(&rwlock->rwlock);
#endif
  assert(err == 0);
  (void)err;
}

void ddsrt_once (ddsrt_once_t *control, ddsrt_once_fn init_fn)
{
  /* There are no defined errors that can be returned by pthread_once */
  (void)pthread_once(control, init_fn);
}
