/*
 * Copyright(c) 2006 to 2019 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <stdlib.h>

#include "dds/ddsrt/attributes.h"
#include "dds/ddsrt/heap.h"

/* nano-ros (issue 0832): this port's heap is compiled out when the tree is
   built with -DNROS_DDSRT_PLATFORM_FUNNEL. Cyclone links above nano-ros's
   platform layer, so every ddsrt allocation goes to
   `nros_platform_{alloc,realloc,dealloc}` instead, implemented once in
   ../nros/heap.c rather than once per port. Undefined — every standalone
   cyclone build — this file is stock and live. */
#ifndef NROS_DDSRT_PLATFORM_FUNNEL
void *
ddsrt_malloc_s(size_t size)
{
  return malloc(size ? size : 1); /* Allocate memory even if size == 0 */
}

void *
ddsrt_malloc(size_t size)
{
  void *ptr = ddsrt_malloc_s(size);

  if (ptr == NULL) {
    /* Heap exhausted */
    abort();
  }

  return ptr;
}

void *
ddsrt_calloc(size_t count, size_t size)
{
  char *ptr;

  ptr = ddsrt_calloc_s(count, size);

  if (ptr == NULL) {
    /* Heap exhausted */
    abort();
  }

  return ptr;
}

void *
ddsrt_calloc_s(size_t count, size_t size)
{
  if (count == 0 || size == 0) {
    count = size = 1;
  }
  return calloc(count, size);
}

void *
ddsrt_realloc(void *memblk, size_t size)
{
  void *ptr;

  ptr = ddsrt_realloc_s(memblk, size);

  if (ptr == NULL){
    /* Heap exhausted */
    abort();
  }

  return ptr;
}

void *
ddsrt_realloc_s(void *memblk, size_t size)
{
  /* Even though newmem = realloc(mem, 0) is equivalent to calling free(mem),
     not all platforms will return newmem == NULL. We consistently do, so the
     result of a non-failing ddsrt_realloc_s always needs to be free'd, like
     ddsrt_malloc_s(0). */
  return realloc(memblk, size ? size : 1);
}

void
ddsrt_free(void *ptr)
{
  if (ptr) {
    free (ptr);
  }
}

#endif /* !NROS_DDSRT_PLATFORM_FUNNEL */
