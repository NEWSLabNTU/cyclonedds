/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */

/* nano-ros (issue 0832): THE ddsrt heap when this tree is built with
   -DNROS_DDSRT_PLATFORM_FUNNEL.

   Cyclone is linked ABOVE nano-ros's platform layer, so it has no business
   reaching a heap the platform does not own. Every ddsrt allocation lands on
   `nros_platform_{alloc,realloc,dealloc}` here, which is one route on every
   port rather than one per port.

   Why a whole file instead of arms inside each port's heap.c: there are four
   of those (posix, freertos, threadx, vxworks) and the funnel is identical in
   all four, so arms would be four copies of one rule — the shape that drifts.
   Each port's heap.c is instead compiled out by the same switch, so exactly
   one implementation is live either way.

   Why this file is in every target's source list rather than swapped in by
   CMake: `ddsrt` is an INTERFACE library and `ddsrt-internal` compiles the
   SAME `INTERFACE_SOURCES`. Swapping sources would hand the funnel to
   `ddsrt-internal` as well, and that is what idlc and confgen link — host
   tools with no platform layer, where the three symbols are undefined. The
   switch is a PRIVATE compile definition on `ddsc`, so it reaches the library
   and not the tools, and the file set stays the same for both.

   A COMPILE-TIME switch, not a weak symbol: weak linkage keeps both branches
   in the binary, so the libc edge survives for any gate reading the call
   graph — which is precisely what has to become absent. Undefined (every
   standalone cyclone build, including its own ctest suite) this file is
   empty and each port keeps its stock heap. */

#include "dds/ddsrt/heap.h"

#ifdef NROS_DDSRT_PLATFORM_FUNNEL

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *nros_platform_alloc (size_t size);
extern void *nros_platform_realloc (void *ptr, size_t size);
extern void nros_platform_dealloc (void *ptr);

void *
ddsrt_malloc_s(size_t size)
{
  /* Allocate memory even if size == 0, so the result is always free-able. */
  return nros_platform_alloc (size ? size : 1);
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
ddsrt_calloc_s(size_t count, size_t size)
{
  size_t total;
  void *ptr;

  if (count == 0 || size == 0) {
    count = size = 1;
  }

  /* The multiply is the one thing calloc does that malloc cannot, so keep its
     overflow check rather than trusting the product. */
  total = count * size;
  if (total / size != count) {
    errno = ERANGE;
    return NULL;
  }

  if ((ptr = nros_platform_alloc (total)) != NULL) {
    memset (ptr, 0, total);
  }

  return ptr;
}

void *
ddsrt_calloc(size_t count, size_t size)
{
  void *ptr = ddsrt_calloc_s(count, size);

  if (ptr == NULL) {
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
     ddsrt_malloc_s(0).

     No size header is kept here, unlike the freertos and threadx heaps: those
     carry one because their RTOS allocator has no realloc and the old size has
     to come from somewhere. `nros_platform_realloc` is specified with libc
     realloc semantics (NULL ptr -> fresh alloc, contents preserved to
     min(old,new)), so the prefix and its alignment arithmetic go away. */
  return nros_platform_realloc (memblk, size ? size : 1);
}

void *
ddsrt_realloc(void *memblk, size_t size)
{
  void *ptr = ddsrt_realloc_s(memblk, size);

  if (ptr == NULL) {
    /* Heap exhausted */
    abort();
  }

  return ptr;
}

void
ddsrt_free(void *ptr)
{
  if (ptr) {
    nros_platform_dealloc (ptr);
  }
}

#endif /* NROS_DDSRT_PLATFORM_FUNNEL */
