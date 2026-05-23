/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <tx_api.h>

#include "threads_priv.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/retcode.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/sync.h"

typedef struct thread_context {
  TX_THREAD thread;
  TX_SEMAPHORE done;
  ddsrt_thread_routine_t func;
  void *arg;
  void *stack;
  ULONG stack_size;
  uint32_t ret;
  thread_cleanup_t *dtors;
} thread_context_t;

static ddsrt_thread_local thread_context_t *thread_context;

ddsrt_tid_t
ddsrt_gettid(void)
{
  return (ddsrt_tid_t)(uintptr_t) tx_thread_identify();
}

DDS_EXPORT ddsrt_tid_t
ddsrt_gettid_for_thread(ddsrt_thread_t thread)
{
  return (ddsrt_tid_t)(uintptr_t) thread.thread;
}

ddsrt_thread_t
ddsrt_thread_self(void)
{
  return (ddsrt_thread_t) { .thread = tx_thread_identify(), .context = thread_context };
}

bool ddsrt_thread_equal(ddsrt_thread_t a, ddsrt_thread_t b)
{
  return a.thread == b.thread;
}

size_t
ddsrt_thread_getname(char *__restrict name, size_t size)
{
  TX_THREAD *thread = tx_thread_identify();
  CHAR *thread_name = NULL;

  assert(name != NULL);
  assert(size >= 1);

  if (thread != TX_NULL) {
    (void) tx_thread_info_get(thread, &thread_name, TX_NULL, TX_NULL, TX_NULL,
                              TX_NULL, TX_NULL, TX_NULL, TX_NULL);
  }

  return ddsrt_strlcpy(name, thread_name != TX_NULL ? thread_name : "", size);
}

static void run_cleanup(thread_context_t *ctx)
{
  thread_cleanup_t *tail;

  while ((tail = ctx->dtors) != NULL) {
    ctx->dtors = tail->prev;
    if (tail->routine != 0) {
      tail->routine(tail->arg);
    }
    ddsrt_free(tail);
  }
}

static void
thread_start_routine(ULONG arg)
{
  thread_context_t *ctx = (thread_context_t *)(uintptr_t)arg;

  thread_context = ctx;
  ctx->ret = ctx->func(ctx->arg);
  run_cleanup(ctx);
  (void) tx_semaphore_put(&ctx->done);
}

dds_return_t
ddsrt_thread_create(
  ddsrt_thread_t *thread,
  const char *name,
  const ddsrt_threadattr_t *attr,
  ddsrt_thread_routine_t start_routine,
  void *arg)
{
  UINT prio;
  thread_context_t *ctx;
  ULONG stack_size;

  assert(thread != NULL);
  assert(name != NULL);
  assert(attr != NULL);
  assert(start_routine != 0);

  if (attr->schedClass != DDSRT_SCHED_DEFAULT &&
      attr->schedClass != DDSRT_SCHED_REALTIME)
  {
    return DDS_RETCODE_BAD_PARAMETER;
  }

  stack_size = attr->stackSize != 0 ? attr->stackSize : (32u * 1024u);
  prio = attr->schedPriority > 0 ? (UINT) attr->schedPriority : 8u;

  if ((ctx = ddsrt_calloc_s(1, sizeof(*ctx))) == NULL) {
    return DDS_RETCODE_OUT_OF_RESOURCES;
  }
  if ((ctx->stack = ddsrt_malloc_s(stack_size)) == NULL) {
    ddsrt_free(ctx);
    return DDS_RETCODE_OUT_OF_RESOURCES;
  }
  ctx->stack_size = stack_size;
  ctx->func = start_routine;
  ctx->arg = arg;

  if (tx_semaphore_create(&ctx->done, (CHAR *)"ddsrt_join", 0) != TX_SUCCESS) {
    ddsrt_free(ctx->stack);
    ddsrt_free(ctx);
    return DDS_RETCODE_OUT_OF_RESOURCES;
  }
  if (tx_thread_create(&ctx->thread, (CHAR *)name, thread_start_routine,
                       (ULONG)(uintptr_t)ctx, ctx->stack, ctx->stack_size,
                       prio, prio, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    (void) tx_semaphore_delete(&ctx->done);
    ddsrt_free(ctx->stack);
    ddsrt_free(ctx);
    return DDS_RETCODE_OUT_OF_RESOURCES;
  }

  thread->thread = &ctx->thread;
  thread->context = ctx;
  return DDS_RETCODE_OK;
}

void
ddsrt_thread_init(uint32_t reason)
{
  (void) reason;
}

void
ddsrt_thread_fini(uint32_t reason)
{
  (void) reason;
  if (thread_context != NULL) {
    run_cleanup(thread_context);
  }
}

dds_return_t
ddsrt_thread_join(ddsrt_thread_t thread, uint32_t *thread_result)
{
  thread_context_t *ctx = (thread_context_t *) thread.context;

  if (ctx == NULL) {
    return DDS_RETCODE_BAD_PARAMETER;
  }

  if (tx_semaphore_get(&ctx->done, TX_WAIT_FOREVER) != TX_SUCCESS) {
    return DDS_RETCODE_ERROR;
  }
  if (thread_result != NULL) {
    *thread_result = ctx->ret;
  }

  (void) tx_thread_terminate(&ctx->thread);
  (void) tx_thread_delete(&ctx->thread);
  (void) tx_semaphore_delete(&ctx->done);
  ddsrt_free(ctx->stack);
  ddsrt_free(ctx);
  return DDS_RETCODE_OK;
}

dds_return_t
ddsrt_thread_cleanup_push(void (*routine)(void *), void *arg)
{
  thread_cleanup_t *tail;

  assert(routine != NULL);

  if (thread_context == NULL) {
    return DDS_RETCODE_OK;
  }
  if ((tail = ddsrt_malloc_s(sizeof(*tail))) == NULL) {
    return DDS_RETCODE_OUT_OF_RESOURCES;
  }

  tail->prev = thread_context->dtors;
  tail->routine = routine;
  tail->arg = arg;
  thread_context->dtors = tail;
  return DDS_RETCODE_OK;
}

dds_return_t
ddsrt_thread_cleanup_pop(int execute)
{
  thread_cleanup_t *tail;

  if (thread_context != NULL && (tail = thread_context->dtors) != NULL) {
    thread_context->dtors = tail->prev;
    if (execute) {
      tail->routine(tail->arg);
    }
    ddsrt_free(tail);
  }

  return DDS_RETCODE_OK;
}
