/*
 * nano-ros: Zephyr-native ddsrt sync types (nano-ros issue 0496).
 *
 * Zephyr has a POSIX layer, so ddsrt built happily against sync/posix.h — but
 * Zephyr's pthread_mutex_t and pthread_cond_t are handles into FIXED STATIC
 * POOLS sized by CONFIG_MAX_PTHREAD_MUTEX_COUNT / CONFIG_MAX_PTHREAD_COND_COUNT.
 * Cyclone puts a mutex in every entity (three, for a writer: e.lock, qos_lock,
 * rdary_lock), so pool demand scaled with the number of entities — local AND
 * proxy — which made "how large a graph can this image join" a compile-time RAM
 * constant. Joining a ~40-participant Autoware graph needed 16384 slots at
 * 32 bytes of k_mutex each plus a type byte and a bitarray bit, and blew up as
 * an anonymous abort() when it ran out (issue 0371).
 *
 * k_mutex and k_condvar are ordinary structs that live wherever you put them.
 * Embedding them removes the pool from the picture entirely rather than moving
 * the ceiling, and it is not a layer violation so much as a layer REMOVAL:
 * Zephyr's pthread_mutex is itself a k_mutex behind a handle table, so this
 * makes exactly the same kernel calls with one less indirection.
 *
 * ddsrt_rwlock_t deliberately stays on pthread_rwlock: cyclone creates exactly
 * one rwlock in production code (the log sink in ddsrt/src/log.c), so it is not
 * part of the term being fixed, and a hand-rolled reader/writer lock would be
 * new risk for no gain. Same for ddsrt_once_t, whose pthread_once_t is caller-
 * owned rather than pooled.
 */
#ifndef DDSRT_ZEPHYR_SYNC_H
#define DDSRT_ZEPHYR_SYNC_H

#include <pthread.h> /* rwlock + once only; the mutex/cond pools are bypassed */
#include <stdint.h>
#include <zephyr/kernel.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct {
  struct k_condvar cond;
} ddsrt_cond_t;

typedef struct {
  struct k_mutex mutex;
} ddsrt_mutex_t;

typedef struct {
  pthread_rwlock_t rwlock;
} ddsrt_rwlock_t;

typedef pthread_once_t ddsrt_once_t;
#define DDSRT_ONCE_INIT PTHREAD_ONCE_INIT

#if defined(__cplusplus)
}
#endif

#endif /* DDSRT_ZEPHYR_SYNC_H */
