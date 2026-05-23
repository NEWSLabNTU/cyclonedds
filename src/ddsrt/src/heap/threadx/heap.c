/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tx_api.h>

#include "dds/ddsrt/heap.h"

static TX_BYTE_POOL *ddsrt_threadx_pool;

__attribute__((weak)) TX_BYTE_POOL *zpico_threadx_byte_pool;

void ddsrt_threadx_set_byte_pool(TX_BYTE_POOL *pool)
{
  ddsrt_threadx_pool = pool;
}

static TX_BYTE_POOL *get_pool(void)
{
  return ddsrt_threadx_pool != TX_NULL ? ddsrt_threadx_pool : zpico_threadx_byte_pool;
}

static const size_t ofst = sizeof(size_t);

void *ddsrt_malloc_s(size_t size)
{
  TX_BYTE_POOL *pool = get_pool();
  void *ptr = NULL;

  if (size == 0) {
    size = 1;
  }
  if (pool == TX_NULL) {
    errno = ENOMEM;
  } else if ((SIZE_MAX - size) < ofst) {
    errno = ERANGE;
  } else if (tx_byte_allocate(pool, &ptr, (ULONG)(size + ofst), TX_WAIT_FOREVER) != TX_SUCCESS) {
    errno = ENOMEM;
    ptr = NULL;
  } else {
    *((size_t *)ptr) = size;
    ptr = (unsigned char *) ptr + ofst;
  }

  return ptr;
}

void *ddsrt_malloc(size_t size)
{
  void *ptr = ddsrt_malloc_s(size);
  if (ptr == NULL) {
    abort();
  }
  return ptr;
}

void *ddsrt_calloc_s(size_t nmemb, size_t size)
{
  void *ptr = NULL;

  if (nmemb == 0 || size == 0) {
    nmemb = size = 1;
  }
  if ((SIZE_MAX / nmemb) <= size) {
    errno = ERANGE;
  } else if ((ptr = ddsrt_malloc_s(nmemb * size)) != NULL) {
    (void) memset(ptr, 0, nmemb * size);
  }

  return ptr;
}

void *ddsrt_calloc(size_t nmemb, size_t size)
{
  void *ptr = ddsrt_calloc_s(nmemb, size);
  if (ptr == NULL) {
    abort();
  }
  return ptr;
}

void *ddsrt_realloc_s(void *memblk, size_t size)
{
  void *ptr = NULL;
  size_t origsize = 0;

  if (memblk != NULL) {
    origsize = *((size_t *)((unsigned char *) memblk - ofst));
  }
  if (size != origsize || origsize == 0) {
    if ((ptr = ddsrt_malloc_s(size)) == NULL) {
      return NULL;
    }
    if (memblk != NULL) {
      if (size > 0) {
        (void) memcpy(ptr, memblk, size > origsize ? origsize : size);
      }
      (void) tx_byte_release((unsigned char *) memblk - ofst);
    }
    memblk = ptr;
  }

  return memblk;
}

void *ddsrt_realloc(void *memblk, size_t size)
{
  void *ptr = ddsrt_realloc_s(memblk, size);
  if (ptr == NULL) {
    abort();
  }
  return ptr;
}

void
ddsrt_free(void *ptr)
{
  if (ptr != NULL) {
    (void) tx_byte_release((unsigned char *) ptr - ofst);
  }
}
