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
#include <string.h>

#include "dds/ddsrt/attributes.h"
#include "dds/ddsrt/heap.h"

/* nano-ros (issue 0832): route ddsrt's heap through the platform allocation
   funnel when nano-ros builds this tree with -DNROS_DDSRT_PLATFORM_FUNNEL.

   The tier model promises `unified` = every allocation reaches
   `nros_platform_alloc`. ddsrt called libc directly, so on a native cyclone
   image the funnel was DEFINED and UNREFERENCED — the promise was false, and a
   gate keying on the symbol being PRESENT would have passed anyway (0832's
   named trap). On an embedded port it is worse than bookkeeping: ddsrt's heap
   and libc's are genuinely different heaps there, so a block allocated through
   one and freed through the other is a real bug.

   A COMPILE-TIME switch, not a weak symbol: weak linkage keeps both branches
   in the binary, so the libc edge survives for any gate reading the call graph
   — which is precisely what has to become absent. Undefined (every standalone
   cyclone build, including its own ctest suite) leaves this file byte-identical
   to stock. */
#ifdef NROS_DDSRT_PLATFORM_FUNNEL
extern void *nros_platform_alloc (size_t size);
extern void *nros_platform_realloc (void *ptr, size_t size);
extern void nros_platform_dealloc (void *ptr);
#endif

void *
ddsrt_malloc_s(size_t size)
{
  size_t n = size ? size : 1; /* Allocate memory even if size == 0 */
#ifdef NROS_DDSRT_PLATFORM_FUNNEL
  return nros_platform_alloc (n);
#else
  return malloc(n);
#endif
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
#ifdef NROS_DDSRT_PLATFORM_FUNNEL
  {
    /* No funnel calloc: allocate + zero, which is what calloc guarantees.
       The multiply is the one thing calloc does that malloc cannot, so keep
       its overflow check rather than trusting the product. */
    size_t total = count * size;
    void *ptr;
    if (total / size != count)
      return NULL; /* overflow */
    ptr = nros_platform_alloc (total);
    if (ptr != NULL)
      memset (ptr, 0, total);
    return ptr;
  }
#else
  return calloc(count, size);
#endif
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
#ifdef NROS_DDSRT_PLATFORM_FUNNEL
  return nros_platform_realloc (memblk, size ? size : 1);
#else
  return realloc(memblk, size ? size : 1);
#endif
}

void
ddsrt_free(void *ptr)
{
  if (ptr) {
#ifdef NROS_DDSRT_PLATFORM_FUNNEL
    nros_platform_dealloc (ptr);
#else
    free (ptr);
#endif
  }
}
