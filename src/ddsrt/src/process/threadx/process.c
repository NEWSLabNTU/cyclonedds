/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include "dds/ddsrt/process.h"
#include "dds/ddsrt/string.h"

#include <tx_api.h>

ddsrt_pid_t
ddsrt_getpid(void)
{
  return (ddsrt_pid_t)(uintptr_t) tx_thread_identify();
}

char *
ddsrt_getprocessname(void)
{
  TX_THREAD *thread = tx_thread_identify();
  CHAR *name = NULL;

  if (thread != TX_NULL &&
      tx_thread_info_get(thread, &name, TX_NULL, TX_NULL, TX_NULL, TX_NULL,
                         TX_NULL, TX_NULL, TX_NULL) == TX_SUCCESS &&
      name != TX_NULL)
  {
    return ddsrt_strdup(name);
  }

  return ddsrt_strdup("threadx");
}
