/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <tx_api.h>

#include "dds/ddsrt/log.h"
#include "dds/ddsrt/sync.h"
#include "dds/ddsrt/time.h"
#include "dds/ddsrt/time/threadx.h"

void ddsrt_mutex_init(ddsrt_mutex_t *mutex)
{
  assert(mutex != NULL);
  (void) memset(mutex, 0, sizeof(*mutex));
  if (tx_mutex_create(&mutex->mutex, (CHAR *)"ddsrt_mutex", TX_INHERIT) != TX_SUCCESS) {
    abort();
  }
}

void ddsrt_mutex_destroy(ddsrt_mutex_t *mutex)
{
  assert(mutex != NULL);
  (void) tx_mutex_delete(&mutex->mutex);
  (void) memset(mutex, 0, sizeof(*mutex));
}

void ddsrt_mutex_lock(ddsrt_mutex_t *mutex)
{
  assert(mutex != NULL);
  if (tx_mutex_get(&mutex->mutex, TX_WAIT_FOREVER) != TX_SUCCESS) {
    DDS_FATAL("Failed to lock 0x%p", (void *)mutex);
  }
}

bool ddsrt_mutex_trylock(ddsrt_mutex_t *mutex)
{
  assert(mutex != NULL);
  return tx_mutex_get(&mutex->mutex, TX_NO_WAIT) == TX_SUCCESS;
}

void ddsrt_mutex_unlock(ddsrt_mutex_t *mutex)
{
  assert(mutex != NULL);
  if (tx_mutex_put(&mutex->mutex) != TX_SUCCESS) {
    DDS_FATAL("Failed to unlock 0x%p", (void *)mutex);
  }
}

void ddsrt_cond_init(ddsrt_cond_t *cond)
{
  assert(cond != NULL);
  (void) memset(cond, 0, sizeof(*cond));
  if (tx_semaphore_create(&cond->sem, (CHAR *)"ddsrt_cond", 0) != TX_SUCCESS ||
      tx_mutex_create(&cond->lock, (CHAR *)"ddsrt_cond_lock", TX_INHERIT) != TX_SUCCESS)
  {
    abort();
  }
}

void ddsrt_cond_destroy(ddsrt_cond_t *cond)
{
  assert(cond != NULL);
  (void) tx_semaphore_delete(&cond->sem);
  (void) tx_mutex_delete(&cond->lock);
  (void) memset(cond, 0, sizeof(*cond));
}

static bool cond_timedwait(ddsrt_cond_t *cond, ddsrt_mutex_t *mutex, dds_duration_t reltime)
{
  UINT status;
  ULONG ticks = ddsrt_duration_to_ticks_ceil(reltime);

  assert(cond != NULL);
  assert(mutex != NULL);

  (void) tx_mutex_get(&cond->lock, TX_WAIT_FOREVER);
  cond->waiters++;
  (void) tx_mutex_put(&cond->lock);

  ddsrt_mutex_unlock(mutex);
  status = tx_semaphore_get(&cond->sem, ticks);
  ddsrt_mutex_lock(mutex);

  (void) tx_mutex_get(&cond->lock, TX_WAIT_FOREVER);
  if (cond->waiters > 0) {
    cond->waiters--;
  }
  (void) tx_mutex_put(&cond->lock);

  return status == TX_SUCCESS;
}

void ddsrt_cond_wait(ddsrt_cond_t *cond, ddsrt_mutex_t *mutex)
{
  (void) cond_timedwait(cond, mutex, DDS_INFINITY);
}

bool ddsrt_cond_waitfor(ddsrt_cond_t *cond, ddsrt_mutex_t *mutex, dds_duration_t reltime)
{
  return cond_timedwait(cond, mutex, reltime);
}

bool ddsrt_cond_waituntil(ddsrt_cond_t *cond, ddsrt_mutex_t *mutex, dds_time_t abstime)
{
  dds_time_t now = dds_time();
  return cond_timedwait(cond, mutex, abstime > now ? abstime - now : 0);
}

void ddsrt_cond_signal(ddsrt_cond_t *cond)
{
  assert(cond != NULL);
  (void) tx_mutex_get(&cond->lock, TX_WAIT_FOREVER);
  if (cond->waiters > 0) {
    (void) tx_semaphore_put(&cond->sem);
  }
  (void) tx_mutex_put(&cond->lock);
}

void ddsrt_cond_broadcast(ddsrt_cond_t *cond)
{
  uint32_t waiters;

  assert(cond != NULL);
  (void) tx_mutex_get(&cond->lock, TX_WAIT_FOREVER);
  waiters = cond->waiters;
  while (waiters-- > 0) {
    (void) tx_semaphore_put(&cond->sem);
  }
  (void) tx_mutex_put(&cond->lock);
}

void ddsrt_rwlock_init(ddsrt_rwlock_t *rwlock)
{
  assert(rwlock != NULL);
  (void) memset(rwlock, 0, sizeof(*rwlock));
  if (tx_mutex_create(&rwlock->mutex, (CHAR *)"ddsrt_rwlock", TX_INHERIT) != TX_SUCCESS) {
    abort();
  }
}

void ddsrt_rwlock_destroy(ddsrt_rwlock_t *rwlock)
{
  assert(rwlock != NULL);
  (void) tx_mutex_delete(&rwlock->mutex);
  (void) memset(rwlock, 0, sizeof(*rwlock));
}

void ddsrt_rwlock_read(ddsrt_rwlock_t *rwlock) { (void) tx_mutex_get(&rwlock->mutex, TX_WAIT_FOREVER); }
void ddsrt_rwlock_write(ddsrt_rwlock_t *rwlock) { (void) tx_mutex_get(&rwlock->mutex, TX_WAIT_FOREVER); }
bool ddsrt_rwlock_tryread(ddsrt_rwlock_t *rwlock) { return tx_mutex_get(&rwlock->mutex, TX_NO_WAIT) == TX_SUCCESS; }
bool ddsrt_rwlock_trywrite(ddsrt_rwlock_t *rwlock) { return tx_mutex_get(&rwlock->mutex, TX_NO_WAIT) == TX_SUCCESS; }
void ddsrt_rwlock_unlock(ddsrt_rwlock_t *rwlock) { (void) tx_mutex_put(&rwlock->mutex); }

#define ONCE_NOT_STARTED (1u << 0)
#define ONCE_IN_PROGRESS (1u << 1)
#define ONCE_FINISHED (1u << 2)

void ddsrt_once(ddsrt_once_t *control, ddsrt_once_fn init_fn)
{
  for (;;) {
    uint32_t stat = ddsrt_atomic_ld32(control);
    assert(stat == ONCE_NOT_STARTED || stat == ONCE_IN_PROGRESS || stat == ONCE_FINISHED);
    if ((stat & ONCE_FINISHED) != 0) {
      return;
    }
    if ((stat & ONCE_IN_PROGRESS) != 0) {
      (void) tx_thread_sleep(1);
    } else if (ddsrt_atomic_cas32(control, ONCE_NOT_STARTED, ONCE_IN_PROGRESS) != 0) {
      init_fn();
      (void) ddsrt_atomic_cas32(control, ONCE_IN_PROGRESS, ONCE_FINISHED);
      return;
    }
  }
}
