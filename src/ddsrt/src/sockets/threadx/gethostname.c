/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <stddef.h>

#include "dds/ddsrt/retcode.h"
#include "dds/ddsrt/string.h"

dds_return_t ddsrt_gethostname(char *hostname, size_t buffersize)
{
  if (ddsrt_strlcpy(hostname, "threadx", buffersize) >= buffersize) {
    return DDS_RETCODE_NOT_ENOUGH_SPACE;
  }
  return DDS_RETCODE_OK;
}
