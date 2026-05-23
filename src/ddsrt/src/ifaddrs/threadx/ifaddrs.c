/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <string.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/ifaddrs.h"
#include "dds/ddsrt/retcode.h"
#include "dds/ddsrt/string.h"

extern const int *const os_supp_afs;

/*
 * NetX Duo does not expose a portable getifaddrs-style BSD API. nano-ros
 * boards can override this weak provider with board-owned IP/netmask data.
 */
__attribute__((weak)) int ddsrt_threadx_get_primary_ipv4(
  uint32_t *addr,
  uint32_t *netmask,
  uint32_t *broadcast,
  char *name,
  size_t name_size)
{
  (void)addr;
  (void)netmask;
  (void)broadcast;
  (void)name;
  (void)name_size;
  return -1;
}

static int wants_ipv4(const int *afs)
{
  if (afs == NULL) {
    afs = os_supp_afs;
  }
  for (int i = 0; afs[i] != DDSRT_AF_TERM; i++) {
    if (afs[i] == AF_INET) {
      return 1;
    }
  }
  return 0;
}

dds_return_t
ddsrt_getifaddrs(ddsrt_ifaddrs_t **ifap, const int *afs)
{
  uint32_t addr, netmask, broadcast;
  char ifname[16] = "nx0";
  ddsrt_ifaddrs_t *ifa = NULL;
  struct sockaddr_in sa;

  assert(ifap != NULL);
  *ifap = NULL;

  if (!wants_ipv4(afs)) {
    return DDS_RETCODE_OK;
  }
  if (ddsrt_threadx_get_primary_ipv4(&addr, &netmask, &broadcast, ifname, sizeof(ifname)) != 0) {
    return DDS_RETCODE_OK;
  }

  if ((ifa = ddsrt_calloc_s(1, sizeof(*ifa))) == NULL ||
      (ifa->name = ddsrt_strdup(ifname)) == NULL ||
      (ifa->addr = ddsrt_calloc_s(1, sizeof(struct sockaddr_in))) == NULL ||
      (ifa->netmask = ddsrt_calloc_s(1, sizeof(struct sockaddr_in))) == NULL ||
      (ifa->broadaddr = ddsrt_calloc_s(1, sizeof(struct sockaddr_in))) == NULL)
  {
    ddsrt_freeifaddrs(ifa);
    return DDS_RETCODE_OUT_OF_RESOURCES;
  }

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = addr;
  memcpy(ifa->addr, &sa, sizeof(sa));
  sa.sin_addr.s_addr = netmask;
  memcpy(ifa->netmask, &sa, sizeof(sa));
  sa.sin_addr.s_addr = broadcast;
  memcpy(ifa->broadaddr, &sa, sizeof(sa));

  ifa->index = 0;
  ifa->flags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
  ifa->type = DDSRT_IFTYPE_UNKNOWN;
  *ifap = ifa;
  return DDS_RETCODE_OK;
}
