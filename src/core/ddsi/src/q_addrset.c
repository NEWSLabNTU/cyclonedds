/*
 * Copyright(c) 2006 to 2022 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <string.h>
#include <stddef.h>
#include <assert.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/log.h"
#include "dds/ddsrt/sync.h" /* ddsrt_once for the striped locks below */
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/misc.h"
#include "dds/ddsrt/avl.h"
#include "dds/ddsi/ddsi_tran.h"
#include "dds/ddsi/q_log.h"
#include "dds/ddsi/q_misc.h"
#include "dds/ddsi/ddsi_config_impl.h"
#include "dds/ddsi/q_addrset.h"
#include "dds/ddsi/ddsi_domaingv.h" /* gv.mattr */
#include "dds/ddsi/ddsi_udp.h" /* nn_mc4gen_address_t */

/* So what does one do with const & mutexes? I need to take lock in a
   pure function just in case some other thread is trying to change
   something. Arguably, that means the thing isn't const; but one
   could just as easily argue that "const" means "this call won't
   change it". If it is globally visible before the call, it may
   change anyway.

   Today, I'm taking the latter interpretation. But all the
   const-discarding casts get moved into LOCK/UNLOCK macros. */

/* nano-ros issue 0496 — striped locks instead of one mutex per addrset.
   Rationale in q_addrset.h. 64 stripes is 64 mutexes for the whole domain
   instead of one per proxy entity and per SEDP announcement; the critical
   sections here are single AVL lookups/inserts, so sharing a lock between
   unrelated addrsets costs approximately nothing.

   Sharing locks means two addrsets can map to the same mutex, so anything
   that held two addrset locks at once, or held one across a callback, had to
   stop doing that — a same-thread re-acquire is a deadlock, not a wait, and
   these are non-recursive mutexes (on Zephyr, literally k_sleep(K_FOREVER)).
   Two places needed the treatment, both below: the copy_addrset_into_addrset_*
   family, which locked the source and then let the per-locator add lock the
   destination, and the addrset_forall_* family, which ran the callback with
   the lock held. */
#define ADDRSET_NLOCKS 64u /* must be a power of two */
static ddsrt_mutex_t addrset_locks[ADDRSET_NLOCKS];
static ddsrt_once_t addrset_locks_once = DDSRT_ONCE_INIT;

static void addrset_locks_init (void)
{
  for (uint32_t i = 0; i < ADDRSET_NLOCKS; i++)
    ddsrt_mutex_init (&addrset_locks[i]);
}

static ddsrt_mutex_t *addrset_lock_for (const struct addrset *as)
{
  /* Addrsets come out of ddsrt_malloc, so the low bits are constant-ish
     alignment padding: shift them out before masking or every addrset lands
     in a handful of stripes. */
  uintptr_t key = (uintptr_t) as / (2 * sizeof (void *));
  return &addrset_locks[key & (ADDRSET_NLOCKS - 1)];
}

#define LOCK(as) (ddsrt_mutex_lock (addrset_lock_for (as)))
#define TRYLOCK(as) (ddsrt_mutex_trylock (addrset_lock_for (as)))
#define UNLOCK(as) (ddsrt_mutex_unlock (addrset_lock_for (as)))

/* Lock two addrsets. Ordered by stripe address so two concurrent copies in
   opposite directions cannot form a cycle, and collapsed to a single acquire
   when both land in the same stripe. */
static void LOCK2 (const struct addrset *a, const struct addrset *b)
{
  ddsrt_mutex_t * const la = addrset_lock_for (a);
  ddsrt_mutex_t * const lb = addrset_lock_for (b);
  if (la == lb)
    ddsrt_mutex_lock (la);
  else if (la < lb)
  {
    ddsrt_mutex_lock (la);
    ddsrt_mutex_lock (lb);
  }
  else
  {
    ddsrt_mutex_lock (lb);
    ddsrt_mutex_lock (la);
  }
}

static void UNLOCK2 (const struct addrset *a, const struct addrset *b)
{
  ddsrt_mutex_t * const la = addrset_lock_for (a);
  ddsrt_mutex_t * const lb = addrset_lock_for (b);
  ddsrt_mutex_unlock (la);
  if (lb != la)
    ddsrt_mutex_unlock (lb);
}

static int compare_xlocators_vwrap (const void *va, const void *vb);

static const ddsrt_avl_ctreedef_t addrset_treedef =
  DDSRT_AVL_CTREEDEF_INITIALIZER (offsetof (struct addrset_node, avlnode), offsetof (struct addrset_node, loc), compare_xlocators_vwrap, 0);

static int add_addresses_to_addrset_1 (const struct ddsi_domaingv *gv, struct addrset *as, ddsi_locator_t *loc, int port_mode, const char *msgtag)
{
  char buf[DDSI_LOCSTRLEN];
  int32_t maxidx;

  // check whether port number, address type and mode make sense, and prepare the
  // locator by patching the first port number to use if none is given
  if (loc->port != NN_LOCATOR_PORT_INVALID)
  {
    if (port_mode >= 0 && loc->port != (uint32_t) port_mode)
    {
      GVERROR ("%s: %s: port mismatch (expecting no port or %d)\n", msgtag, ddsi_locator_to_string (buf, sizeof(buf), loc), port_mode);
      return -1;
    }
    maxidx = 0;
  }
  else if (port_mode >= 0)
  {
    loc->port = (uint32_t) port_mode;
    maxidx = 0;
  }
  else if (ddsi_is_mcaddr (gv, loc))
  {
    loc->port = ddsi_get_port (&gv->config, DDSI_PORT_MULTI_DISC, 0);
    maxidx = 0;
  }
  else
  {
    loc->port = ddsi_get_port (&gv->config, DDSI_PORT_UNI_DISC, 0);
    maxidx = gv->config.maxAutoParticipantIndex;
  }

  GVLOG (DDS_LC_CONFIG, "%s: add %s", msgtag, ddsi_locator_to_string (buf, sizeof (buf), loc));
  add_locator_to_addrset (gv, as, loc);
  for (int32_t i = 1; i < maxidx; i++)
  {
    loc->port = ddsi_get_port (&gv->config, DDSI_PORT_UNI_DISC, i);
    GVLOG (DDS_LC_CONFIG, ", :%"PRIu32, loc->port);
    add_locator_to_addrset (gv, as, loc);
  }
  GVLOG (DDS_LC_CONFIG, "\n");
  return 0;
}

int add_addresses_to_addrset (const struct ddsi_domaingv *gv, struct addrset *as, const char *addrs, int port_mode, const char *msgtag, int req_mc)
{
  /* port_mode: -1  => take from string, if 0 & unicast, add for a range of participant indices;
     port_mode >= 0 => always set port to port_mode
  */
  DDSRT_WARNING_MSVC_OFF(4996);
  char *addrs_copy, *cursor, *a;
  int retval = -1;
  addrs_copy = ddsrt_strdup (addrs);
  cursor = addrs_copy;
  while ((a = ddsrt_strsep (&cursor, ",")) != NULL)
  {
    ddsi_locator_t loc;
    char buf[DDSI_LOCSTRLEN];

    switch (ddsi_locator_from_string (gv, &loc, a, gv->m_factory))
    {
      case AFSR_OK:
        break;
      case AFSR_INVALID:
        GVERROR ("%s: %s: not a valid address\n", msgtag, a);
        goto error;
      case AFSR_UNKNOWN:
        GVERROR ("%s: %s: unknown address\n", msgtag, a);
        goto error;
      case AFSR_MISMATCH:
        GVERROR ("%s: %s: address family mismatch\n", msgtag, a);
        goto error;
    }

    if (req_mc && !ddsi_is_mcaddr (gv, &loc))
    {
      GVERROR ("%s: %s: not a multicast address\n", msgtag, ddsi_locator_to_string_no_port (buf, sizeof(buf), &loc));
      goto error;
    }

    if (add_addresses_to_addrset_1 (gv, as, &loc, port_mode, msgtag) < 0)
    {
      goto error;
    }
  }
  retval = 0;
 error:
  ddsrt_free (addrs_copy);
  return retval;
  DDSRT_WARNING_MSVC_ON(4996);
}

int compare_locators (const ddsi_locator_t *a, const ddsi_locator_t *b)
{
  int c;
  if (a->kind != b->kind)
    return (int) (a->kind - b->kind);
  else if ((c = memcmp (a->address, b->address, sizeof (a->address))) != 0)
    return c;
  else if (a->port != b->port)
    return (int) (a->port - b->port);
  else
    return 0;
}

int compare_xlocators (const ddsi_xlocator_t *a, const ddsi_xlocator_t *b)
{
  int c;
  if ((c = compare_locators (&a->c, &b->c)) != 0)
    return c;
  else
  {
    const uintptr_t ac = (uintptr_t) a->conn;
    const uintptr_t bc = (uintptr_t) b->conn;
    return (ac == bc) ? 0 : (ac < bc) ? -1 : 1;
  }
}

static int compare_xlocators_vwrap (const void *va, const void *vb)
{
  return compare_xlocators (va, vb);
}

struct addrset *new_addrset (void)
{
  /* Every addrset comes from here, so initialising the stripes on this path is
     enough to have them ready before any LOCK(as) can be reached. */
  ddsrt_once (&addrset_locks_once, addrset_locks_init);
  struct addrset *as = ddsrt_malloc (sizeof (*as));
  ddsrt_atomic_st32 (&as->refc, 1);
  ddsrt_avl_cinit (&addrset_treedef, &as->ucaddrs);
  ddsrt_avl_cinit (&addrset_treedef, &as->mcaddrs);
  return as;
}

struct addrset *ref_addrset (struct addrset *as)
{
  if (as != NULL)
  {
    ddsrt_atomic_inc32 (&as->refc);
  }
  return as;
}

void unref_addrset (struct addrset *as)
{
  if ((as != NULL) && (ddsrt_atomic_dec32_ov (&as->refc) == 1))
  {
    ddsrt_avl_cfree (&addrset_treedef, &as->ucaddrs, ddsrt_free);
    ddsrt_avl_cfree (&addrset_treedef, &as->mcaddrs, ddsrt_free);
    /* no ddsrt_mutex_destroy: the stripe outlives every addrset that hashed
       to it, and is shared with the ones that still exist */
    ddsrt_free (as);
  }
}

void set_unspec_locator (ddsi_locator_t *loc)
{
  loc->kind = NN_LOCATOR_KIND_INVALID;
  loc->port = NN_LOCATOR_PORT_INVALID;
  memset (loc->address, 0, sizeof (loc->address));
}

void set_unspec_xlocator (ddsi_xlocator_t *loc)
{
  loc->conn = NULL;
  set_unspec_locator (&loc->c);
}

int is_unspec_locator (const ddsi_locator_t *loc)
{
  static const ddsi_locator_t zloc = { .kind = 0 };
  return (loc->kind == NN_LOCATOR_KIND_INVALID &&
          loc->port == NN_LOCATOR_PORT_INVALID &&
          memcmp (&zloc.address, loc->address, sizeof (zloc.address)) == 0);
}

int is_unspec_xlocator (const ddsi_xlocator_t *loc)
{
  return is_unspec_locator (&loc->c);
}

#ifdef DDS_HAS_SSM
int addrset_contains_ssm (const struct ddsi_domaingv *gv, const struct addrset *as)
{
  struct addrset_node *n;
  ddsrt_avl_citer_t it;
  LOCK (as);
  for (n = ddsrt_avl_citer_first (&addrset_treedef, &as->mcaddrs, &it); n; n = ddsrt_avl_citer_next (&it))
  {
    if (ddsi_is_ssm_mcaddr (gv, &n->loc.c))
    {
      UNLOCK (as);
      return 1;
    }
  }
  UNLOCK (as);
  return 0;
}

int addrset_any_ssm (const struct ddsi_domaingv *gv, const struct addrset *as, ddsi_xlocator_t *dst)
{
  struct addrset_node *n;
  ddsrt_avl_citer_t it;
  LOCK (as);
  for (n = ddsrt_avl_citer_first (&addrset_treedef, &as->mcaddrs, &it); n; n = ddsrt_avl_citer_next (&it))
  {
    if (ddsi_is_ssm_mcaddr (gv, &n->loc.c))
    {
      *dst = n->loc;
      UNLOCK (as);
      return 1;
    }
  }
  UNLOCK (as);
  return 0;
}

int addrset_any_non_ssm_mc (const struct ddsi_domaingv *gv, const struct addrset *as, ddsi_xlocator_t *dst)
{
  struct addrset_node *n;
  ddsrt_avl_citer_t it;
  LOCK (as);
  for (n = ddsrt_avl_citer_first (&addrset_treedef, &as->mcaddrs, &it); n; n = ddsrt_avl_citer_next (&it))
  {
    if (!ddsi_is_ssm_mcaddr (gv, &n->loc.c))
    {
      *dst = n->loc;
      UNLOCK (as);
      return 1;
    }
  }
  UNLOCK (as);
  return 0;
}
#endif

int addrset_purge (struct addrset *as)
{
  LOCK (as);
  ddsrt_avl_cfree (&addrset_treedef, &as->ucaddrs, ddsrt_free);
  ddsrt_avl_cfree (&addrset_treedef, &as->mcaddrs, ddsrt_free);
  UNLOCK (as);
  return 0;
}

/* Caller holds as's stripe. Split out so the copy_addrset_into_addrset_*
   family can hold both stripes for the whole walk instead of re-acquiring the
   destination's — which deadlocks when it is the same stripe as the source's
   (nano-ros issue 0496). */
static void add_xlocator_to_addrset_locked (const struct ddsi_domaingv *gv, struct addrset *as, const ddsi_xlocator_t *loc)
{
  assert (!is_unspec_locator (&loc->c));
  assert (loc->conn != NULL);
  ddsrt_avl_ipath_t path;
  ddsrt_avl_ctree_t *tree = ddsi_is_mcaddr (gv, &loc->c) ? &as->mcaddrs : &as->ucaddrs;
  if (ddsrt_avl_clookup_ipath (&addrset_treedef, tree, loc, &path) == NULL)
  {
    struct addrset_node *n = ddsrt_malloc (sizeof (*n));
    n->loc = *loc;
    ddsrt_avl_cinsert_ipath (&addrset_treedef, tree, n, &path);
  }
}

static void add_xlocator_to_addrset_impl (const struct ddsi_domaingv *gv, struct addrset *as, const ddsi_xlocator_t *loc)
{
  LOCK (as);
  add_xlocator_to_addrset_locked (gv, as, loc);
  UNLOCK (as);
}

void add_xlocator_to_addrset (const struct ddsi_domaingv *gv, struct addrset *as, const ddsi_xlocator_t *loc)
{
  if (is_unspec_locator (&loc->c))
    return;
  add_xlocator_to_addrset_impl (gv, as, loc);
}

void add_locator_to_addrset (const struct ddsi_domaingv *gv, struct addrset *as, const ddsi_locator_t *loc)
{
  if (is_unspec_locator (loc))
    return;
  if (ddsi_is_mcaddr (gv, loc))
  {
    // multicast: use all transmit connections
    for (int i = 0; i < gv->n_interfaces; i++)
    {
      if (ddsi_factory_supports (gv->xmit_conns[i]->m_factory, loc->kind))
        add_xlocator_to_addrset_impl (gv, as, &(const ddsi_xlocator_t) {
          .conn = gv->xmit_conns[i],
          .c = *loc });
    }
  }
  else
  {
    // unicast: assume the kernel knows how to route it from any connection
    // if it doesn't match a local interface
    for (int i = 0; i < gv->n_interfaces; i++)
    {
      if (!ddsi_factory_supports (gv->xmit_conns[i]->m_factory, loc->kind))
        continue;
      switch (ddsi_is_nearby_address (gv, loc, (size_t) gv->n_interfaces, gv->interfaces, NULL))
      {
        case DNAR_LOCAL:
          add_xlocator_to_addrset_impl (gv, as, &(const ddsi_xlocator_t) {
            .conn = gv->xmit_conns[i],
            .c = *loc });
          return;
        case DNAR_DISTANT:
          break;
      }
    }
    for (int i = 0; i < gv->n_interfaces; i++)
    {
      if (!ddsi_factory_supports (gv->xmit_conns[i]->m_factory, loc->kind))
        continue;
      add_xlocator_to_addrset_impl (gv, as, &(const ddsi_xlocator_t) {
        .conn = gv->xmit_conns[i],
        .c = *loc });
      break;
    }
  }
}

void remove_from_addrset (const struct ddsi_domaingv *gv, struct addrset *as, const ddsi_xlocator_t *loc)
{
  ddsrt_avl_dpath_t path;
  ddsrt_avl_ctree_t *tree = ddsi_is_mcaddr (gv, &loc->c) ? &as->mcaddrs : &as->ucaddrs;
  struct addrset_node *n;
  LOCK (as);
  if ((n = ddsrt_avl_clookup_dpath (&addrset_treedef, tree, loc, &path)) != NULL)
  {
    ddsrt_avl_cdelete_dpath (&addrset_treedef, tree, n, &path);
    ddsrt_free (n);
  }
  UNLOCK (as);
}

void copy_addrset_into_addrset_uc (const struct ddsi_domaingv *gv, struct addrset *as, const struct addrset *asadd)
{
  struct addrset_node *n;
  ddsrt_avl_citer_t it;
  LOCK2 (as, asadd);
  for (n = ddsrt_avl_citer_first (&addrset_treedef, &asadd->ucaddrs, &it); n; n = ddsrt_avl_citer_next (&it))
    add_xlocator_to_addrset_locked (gv, as, &n->loc);
  UNLOCK2 (as, asadd);
}

void copy_addrset_into_addrset_mc (const struct ddsi_domaingv *gv, struct addrset *as, const struct addrset *asadd)
{
  struct addrset_node *n;
  ddsrt_avl_citer_t it;
  LOCK2 (as, asadd);
  for (n = ddsrt_avl_citer_first (&addrset_treedef, &asadd->mcaddrs, &it); n; n = ddsrt_avl_citer_next (&it))
    add_xlocator_to_addrset_locked (gv, as, &n->loc);
  UNLOCK2 (as, asadd);
}

void copy_addrset_into_addrset (const struct ddsi_domaingv *gv, struct addrset *as, const struct addrset *asadd)
{
  copy_addrset_into_addrset_uc (gv, as, asadd);
  copy_addrset_into_addrset_mc (gv, as, asadd);
}

#ifdef DDS_HAS_SSM
void copy_addrset_into_addrset_no_ssm_mc (const struct ddsi_domaingv *gv, struct addrset *as, const struct addrset *asadd)
{
  struct addrset_node *n;
  ddsrt_avl_citer_t it;
  LOCK2 (as, asadd);
  for (n = ddsrt_avl_citer_first (&addrset_treedef, &asadd->mcaddrs, &it); n; n = ddsrt_avl_citer_next (&it))
  {
    if (!ddsi_is_ssm_mcaddr (gv, &n->loc.c))
      add_xlocator_to_addrset_locked (gv, as, &n->loc);
  }
  UNLOCK2 (as, asadd);

}

void copy_addrset_into_addrset_no_ssm (const struct ddsi_domaingv *gv, struct addrset *as, const struct addrset *asadd)
{
  copy_addrset_into_addrset_uc (gv, as, asadd);
  copy_addrset_into_addrset_no_ssm_mc (gv, as, asadd);
}
#endif

size_t addrset_count (const struct addrset *as)
{
  if (as == NULL)
    return 0;
  else
  {
    size_t count;
    LOCK (as);
    count = ddsrt_avl_ccount (&as->ucaddrs) + ddsrt_avl_ccount (&as->mcaddrs);
    UNLOCK (as);
    return count;
  }
}

size_t addrset_count_uc (const struct addrset *as)
{
  if (as == NULL)
    return 0;
  else
  {
    size_t count;
    LOCK (as);
    count = ddsrt_avl_ccount (&as->ucaddrs);
    UNLOCK (as);
    return count;
  }
}

size_t addrset_count_mc (const struct addrset *as)
{
  if (as == NULL)
    return 0;
  else
  {
    size_t count;
    LOCK (as);
    count = ddsrt_avl_ccount (&as->mcaddrs);
    UNLOCK (as);
    return count;
  }
}

int addrset_empty_uc (const struct addrset *as)
{
  int isempty;
  LOCK (as);
  isempty = ddsrt_avl_cis_empty (&as->ucaddrs);
  UNLOCK (as);
  return isempty;
}

int addrset_empty_mc (const struct addrset *as)
{
  int isempty;
  LOCK (as);
  isempty = ddsrt_avl_cis_empty (&as->mcaddrs);
  UNLOCK (as);
  return isempty;
}

int addrset_empty (const struct addrset *as)
{
  int isempty;
  LOCK (as);
  isempty = ddsrt_avl_cis_empty (&as->ucaddrs) && ddsrt_avl_cis_empty (&as->mcaddrs);
  UNLOCK (as);
  return isempty;
}

int addrset_any_uc (const struct addrset *as, ddsi_xlocator_t *dst)
{
  LOCK (as);
  if (ddsrt_avl_cis_empty (&as->ucaddrs))
  {
    UNLOCK (as);
    return 0;
  }
  else
  {
    const struct addrset_node *n = ddsrt_avl_croot_non_empty (&addrset_treedef, &as->ucaddrs);
    *dst = n->loc;
    UNLOCK (as);
    return 1;
  }
}

int addrset_any_mc (const struct addrset *as, ddsi_xlocator_t *dst)
{
  LOCK (as);
  if (ddsrt_avl_cis_empty (&as->mcaddrs))
  {
    UNLOCK (as);
    return 0;
  }
  else
  {
    const struct addrset_node *n = ddsrt_avl_croot_non_empty (&addrset_treedef, &as->mcaddrs);
    *dst = n->loc;
    UNLOCK (as);
    return 1;
  }
}

void addrset_any_uc_else_mc_nofail (const struct addrset *as, ddsi_xlocator_t *dst)
{
  LOCK (as);
  if (!ddsrt_avl_cis_empty (&as->ucaddrs))
  {
    const struct addrset_node *n = ddsrt_avl_croot_non_empty (&addrset_treedef, &as->ucaddrs);
    *dst = n->loc;
  }
  else
  {
    assert (!ddsrt_avl_cis_empty (&as->mcaddrs));
    const struct addrset_node *n = ddsrt_avl_croot_non_empty (&addrset_treedef, &as->mcaddrs);
    *dst = n->loc;
  }
  UNLOCK (as);
}

/* nano-ros issue 0496 — the callback must NOT run with an addrset lock held.
   A callback is arbitrary code and some of them re-enter this layer on the
   same thread: purge_helper (ddsi_proxy_participant.c) deletes a proxy
   participant, and writing that participant's builtin-topic sample formats an
   addrset via addrset_forall (dds_serdata_builtintopic.c). With one mutex per
   addrset that only deadlocked if the callback reached the very same addrset;
   with striped locks any same-stripe pair does it. So snapshot the locators
   under the lock and run the callback afterwards.

   A stack buffer covers the normal case — an addrset holds a handful of
   locators — and the heap is only touched by an unusually large one. */
#define ADDRSET_SNAP_NSTACK 16

struct addrset_snapshot {
  ddsi_xlocator_t *locs;
  size_t n;
  ddsi_xlocator_t stack[ADDRSET_SNAP_NSTACK];
};

static void addrset_snapshot_collect (void *vnode, void *varg)
{
  const struct addrset_node *n = vnode;
  struct addrset_snapshot *s = varg;
  s->locs[s->n++] = n->loc;
}

/* Caller holds as's stripe. `trees` are copied in the order given. */
static void addrset_snapshot_begin (struct addrset_snapshot *s, const ddsrt_avl_ctree_t **trees, size_t ntrees)
{
  size_t total = 0;
  for (size_t i = 0; i < ntrees; i++)
    total += ddsrt_avl_ccount ((ddsrt_avl_ctree_t *) trees[i]);
  s->locs = (total <= ADDRSET_SNAP_NSTACK) ? s->stack : ddsrt_malloc (total * sizeof (*s->locs));
  s->n = 0;
  for (size_t i = 0; i < ntrees; i++)
    ddsrt_avl_cwalk (&addrset_treedef, (ddsrt_avl_ctree_t *) trees[i], addrset_snapshot_collect, s);
  assert (s->n == total);
}

static void addrset_snapshot_end (struct addrset_snapshot *s)
{
  if (s->locs != s->stack)
    ddsrt_free (s->locs);
}

static void addrset_snapshot_apply (const struct addrset_snapshot *s, addrset_forall_fun_t f, void *arg)
{
  for (size_t i = 0; i < s->n; i++)
    f (&s->locs[i], arg);
}

size_t addrset_forall_count (struct addrset *as, addrset_forall_fun_t f, void *arg)
{
  struct addrset_snapshot s;
  const ddsrt_avl_ctree_t *trees[2];
  LOCK (as);
  trees[0] = &as->mcaddrs;
  trees[1] = &as->ucaddrs;
  addrset_snapshot_begin (&s, trees, 2);
  UNLOCK (as);
  addrset_snapshot_apply (&s, f, arg);
  const size_t count = s.n;
  addrset_snapshot_end (&s);
  return count;
}

void addrset_forall (struct addrset *as, addrset_forall_fun_t f, void *arg)
{
  (void) addrset_forall_count (as, f, arg);
}

size_t addrset_forall_uc_else_mc_count (struct addrset *as, addrset_forall_fun_t f, void *arg)
{
  struct addrset_snapshot s;
  const ddsrt_avl_ctree_t *tree;
  LOCK (as);
  tree = !ddsrt_avl_cis_empty (&as->ucaddrs) ? &as->ucaddrs : &as->mcaddrs;
  addrset_snapshot_begin (&s, &tree, 1);
  UNLOCK (as);
  addrset_snapshot_apply (&s, f, arg);
  const size_t count = s.n;
  addrset_snapshot_end (&s);
  return count;
}

size_t addrset_forall_mc_count (struct addrset *as, addrset_forall_fun_t f, void *arg)
{
  struct addrset_snapshot s;
  const ddsrt_avl_ctree_t *tree = &as->mcaddrs;
  LOCK (as);
  addrset_snapshot_begin (&s, &tree, 1);
  UNLOCK (as);
  addrset_snapshot_apply (&s, f, arg);
  const size_t count = s.n;
  addrset_snapshot_end (&s);
  return count;
}

int addrset_forone (struct addrset *as, addrset_forone_fun_t f, void *arg)
{
  addrset_node_t n;
  ddsrt_avl_ctree_t *trees[2];
  ddsrt_avl_citer_t iter;

  trees[0] = &as->mcaddrs;
  trees[1] = &as->ucaddrs;
  for (int i = 0; i < 2; i++)
  {
    n = (addrset_node_t) ddsrt_avl_citer_first (&addrset_treedef, trees[i], &iter);
    while (n)
    {
      if ((f) (&n->loc, arg) > 0)
      {
        return 0;
      }
      n = (addrset_node_t) ddsrt_avl_citer_next (&iter);
    }
  }
  return -1;
}

struct log_addrset_helper_arg
{
  uint32_t tf;
  struct ddsi_domaingv *gv;
};

static void log_addrset_helper (const ddsi_xlocator_t *n, void *varg)
{
  const struct log_addrset_helper_arg *arg = varg;
  const struct ddsi_domaingv *gv = arg->gv;
  char buf[DDSI_LOCSTRLEN];
  if (gv->logconfig.c.mask & arg->tf)
    GVLOG (arg->tf, " %s", ddsi_xlocator_to_string (buf, sizeof(buf), n));
}

void nn_log_addrset (struct ddsi_domaingv *gv, uint32_t tf, const char *prefix, const struct addrset *as)
{
  if (gv->logconfig.c.mask & tf)
  {
    struct log_addrset_helper_arg arg;
    arg.tf = tf;
    arg.gv = gv;
    GVLOG (tf, "%s", prefix);
    addrset_forall ((struct addrset *) as, log_addrset_helper, &arg); /* drop const, we know it is */
  }
}

static int addrset_eq_onesidederr1 (const ddsrt_avl_ctree_t *at, const ddsrt_avl_ctree_t *bt)
{
  /* Just checking the root */
  if (ddsrt_avl_cis_empty (at) && ddsrt_avl_cis_empty (bt)) {
    return 1;
  } else if (ddsrt_avl_cis_singleton (at) && ddsrt_avl_cis_singleton (bt)) {
    const struct addrset_node *a = ddsrt_avl_croot_non_empty (&addrset_treedef, at);
    const struct addrset_node *b = ddsrt_avl_croot_non_empty (&addrset_treedef, bt);
    return compare_xlocators (&a->loc, &b->loc) == 0;
  } else {
    return 0;
  }
}

int addrset_eq_onesidederr (const struct addrset *a, const struct addrset *b)
{
  int iseq;
  if (a == b)
    return 1;
  if (a == NULL || b == NULL)
    return 0;
  LOCK (a);
  if (TRYLOCK (b))
  {
    iseq =
      addrset_eq_onesidederr1 (&a->ucaddrs, &b->ucaddrs) &&
      addrset_eq_onesidederr1 (&a->mcaddrs, &b->mcaddrs);
    UNLOCK (b);
  }
  else
  {
    /* We could try <lock b ; trylock(a)>, in a loop, &c. Or we can
       just decide it isn't worth the bother. Which it isn't because
       it doesn't have to be an exact check on equality. A possible
       improvement would be to use an rwlock. */
    iseq = 0;
  }
  UNLOCK (a);
  return iseq;
}
