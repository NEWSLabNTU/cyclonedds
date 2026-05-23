/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <tx_api.h>

#include "dds/ddsrt/time.h"
#include "dds/ddsrt/time/threadx.h"

DDS_EXPORT extern inline ULONG ddsrt_duration_to_ticks_ceil(dds_duration_t reltime);

static dds_time_t ticks_to_nsec(ULONG ticks)
{
  return (dds_time_t) ticks * DDSRT_NSECS_PER_TICK;
}

dds_time_t dds_time(void)
{
  return ticks_to_nsec(tx_time_get());
}

ddsrt_wctime_t ddsrt_time_wallclock(void)
{
  return (ddsrt_wctime_t) { dds_time() };
}

ddsrt_mtime_t ddsrt_time_monotonic(void)
{
  return (ddsrt_mtime_t) { ticks_to_nsec(tx_time_get()) };
}

ddsrt_etime_t ddsrt_time_elapsed(void)
{
  return (ddsrt_etime_t) { ticks_to_nsec(tx_time_get()) };
}

void dds_sleepfor(dds_duration_t reltime)
{
  ULONG ticks = ddsrt_duration_to_ticks_ceil(reltime);
  if (ticks != 0) {
    (void) tx_thread_sleep(ticks);
  }
}
