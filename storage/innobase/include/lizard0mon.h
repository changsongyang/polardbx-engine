/* Copyright (c) 2018, 2021, Alibaba and/or its affiliates. All rights reserved.
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.
   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL/Apsara GalaxyEngine hereby grant you an
   additional permission to link the program and your derivative works with the
   separately licensed software that they have included with
   MySQL/Apsara GalaxyEngine.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/** @file include/lizard0mon.h
  Lizard monitor metrics.

 Created 2020-06-03 by Jianwei.zhao
 *******************************************************/

#ifndef lizard0mon_h
#define lizard0mon_h

#include "univ.i"

#include "srv0srv.h"

class THD;
struct SHOW_VAR;

struct lizard_var_t {
  /** txn undo rollback segment free list length */
  ulint txn_undo_log_free_list_len;

  /** txn undo log cached for reuse */
  ulint txn_undo_log_cached;

  /** txn undo log segment request count */
  ulint txn_undo_log_request;

  /** txn undo log segment reuse count */
  ulint txn_undo_log_reuse;

  /** txn undo log segment get from rseg free list count */
  ulint txn_undo_log_free_list_get;

  /** txn undo log segment put into rseg free list count */
  ulint txn_undo_log_free_list_put;

  /** txn undo log segment create count */
  ulint txn_undo_log_create;

  /** txn undo log hash count */
  ulint txn_undo_log_hash_element;

  /** txn undo log hash hit count */
  ulint txn_undo_log_hash_hit;

  /** txn undo log hash miss count */
  ulint txn_undo_log_hash_miss;

  ulint txn_undo_lost_page_miss_when_safe;

  ulint txn_undo_lost_magic_number_wrong;

  ulint txn_undo_lost_ext_flag_wrong;

  ulint txn_undo_lost_trx_id_mismatch;

  ulint txn_undo_lookup_by_uba;

  ulint cleanout_page_collect;

  ulint cleanout_record_clean;

  ulint cleanout_cursor_collect;

  ulint cleanout_cursor_restore_failed;

  /*Max Snapshot gcn. */
  ulint snapshot_gcn;

  /*Max commit gcn. */
  ulint commit_gcn;

  /* Max purged gcn, snapshot gcn before that is too old to asof select. */
  ulint purged_gcn;

#ifdef UNIV_DEBUG
  ulint block_tcn_cache_hit;
  ulint block_tcn_cache_miss;
  ulint block_tcn_cache_evict;

  ulint session_tcn_cache_hit;
  ulint session_tcn_cache_miss;
  ulint session_tcn_cache_evict;

  ulint global_tcn_cache_hit;
  ulint global_tcn_cache_miss;
  ulint global_tcn_cache_evict;
#endif
};

struct lizard_stats_t {
  typedef ib_counter_t<ulint, 64> ulint_ctr_64_t;
  typedef ib_counter_t<lsn_t, 1, single_indexer_t> lsn_ctr_1_t;
  typedef ib_counter_t<ulint, 1, single_indexer_t> ulint_ctr_1_t;
  typedef ib_counter_t<lint, 1, single_indexer_t> lint_ctr_1_t;
  typedef ib_counter_t<int64_t, 1, single_indexer_t> int64_ctr_1_t;

  /** txn undo log segment request count */
  ulint_ctr_1_t txn_undo_log_request;

  /** txn undo log segment reuse count */
  ulint_ctr_1_t txn_undo_log_reuse;

  /** txn undo log segment get from rseg free list count */
  ulint_ctr_1_t txn_undo_log_free_list_get;

  /** txn undo log segment put into rseg free list count */
  ulint_ctr_1_t txn_undo_log_free_list_put;

  /** txn undo log segment create count */
  ulint_ctr_1_t txn_undo_log_create;

  /** txn undo log hash count */
  ulint_ctr_1_t txn_undo_log_hash_element;

  /** txn undo log hash hit count */
  ulint_ctr_1_t txn_undo_log_hash_hit;

  /** txn undo log hash miss count */
  ulint_ctr_1_t txn_undo_log_hash_miss;

  /** txn undo lost when missing corresponding pages when cleanout safe mode */
  ulint_ctr_1_t txn_undo_lost_page_miss_when_safe;

  /** txn undo lost because magic number is wrong */
  ulint_ctr_1_t txn_undo_lost_magic_number_wrong;

  /** txn undo lost because ext flag is wrong */
  ulint_ctr_1_t txn_undo_lost_ext_flag_wrong;

  /** txn undo lost because trx_id is mismatch */
  ulint_ctr_1_t txn_undo_lost_trx_id_mismatch;

  /** lookup scn by uba */
  ulint_ctr_1_t txn_undo_lookup_by_uba;

  ulint_ctr_1_t cleanout_page_collect;

  ulint_ctr_1_t cleanout_record_clean;

  ulint_ctr_1_t cleanout_cursor_collect;

  ulint_ctr_1_t cleanout_cursor_restore_failed;

#ifdef UNIV_DEBUG
  ulint_ctr_1_t block_tcn_cache_hit;
  ulint_ctr_1_t block_tcn_cache_miss;
  ulint_ctr_1_t block_tcn_cache_evict;

  ulint_ctr_1_t session_tcn_cache_hit;
  ulint_ctr_1_t session_tcn_cache_miss;
  ulint_ctr_1_t session_tcn_cache_evict;

  ulint_ctr_1_t global_tcn_cache_hit;
  ulint_ctr_1_t global_tcn_cache_miss;
  ulint_ctr_1_t global_tcn_cache_evict;
#endif
};

namespace lizard {

extern lizard_stats_t lizard_stats;

int show_lizard_vars(THD *thd, SHOW_VAR *var, char *buff);

}  // namespace lizard

#ifdef UNIV_DEBUG

#define BLOCK_TCN_CACHE_HIT                         \
  do {                                              \
    lizard::lizard_stats.block_tcn_cache_hit.inc(); \
  } while (0)

#define BLOCK_TCN_CACHE_MISS                         \
  do {                                               \
    lizard::lizard_stats.block_tcn_cache_miss.inc(); \
  } while (0)

#define BLOCK_TCN_CACHE_EVICT                         \
  do {                                                \
    lizard::lizard_stats.block_tcn_cache_evict.inc(); \
  } while (0)

#define SESSION_TCN_CACHE_HIT                        \
  do {                                                \
    lizard::lizard_stats.session_tcn_cache_hit.inc(); \
  } while (0)

#define SESSION_TCN_CACHE_MISS                         \
  do {                                                 \
    lizard::lizard_stats.session_tcn_cache_miss.inc(); \
  } while (0)

#define SESSION_TCN_CACHE_EVICT                         \
  do {                                                  \
    lizard::lizard_stats.session_tcn_cache_evict.inc(); \
  } while (0)

#define GLOBAL_TCN_CACHE_HIT                         \
  do {                                               \
    lizard::lizard_stats.global_tcn_cache_hit.inc(); \
  } while (0)

#define GLOBAL_TCN_CACHE_MISS                         \
  do {                                                \
    lizard::lizard_stats.global_tcn_cache_miss.inc(); \
  } while (0)

#define GLOBAL_TCN_CACHE_EVICT                         \
  do {                                                 \
    lizard::lizard_stats.global_tcn_cache_evict.inc(); \
  } while (0)
#else

#define BLOCK_TCN_CACHE_HIT
#define BLOCK_TCN_CACHE_MISS
#define BLOCK_TCN_CACHE_EVICT
#define SESSION_TCN_CACHE_HIT
#define SESSION_TCN_CACHE_MISS
#define SESSION_TCN_CACHE_EVICT
#define GLOBAL_TCN_CACHE_HIT
#define GLOBAL_TCN_CACHE_MISS
#define GLOBAL_TCN_CACHE_EVICT

#endif



#define LIZARD_MONITOR_INC_TXN_CACHED(NUMBER)                             \
  do {                                                                    \
    if (lizard::lizard_sys != nullptr) {                                  \
      os_atomic_increment_ulint(&lizard::lizard_sys->txn_undo_log_cached, \
                                (NUMBER));                                \
    }                                                                     \
  } while (0)

#define LIZARD_MONITOR_DEC_TXN_CACHED(NUMBER)                             \
  do {                                                                    \
    if (lizard::lizard_sys != nullptr) {                                  \
      os_atomic_decrement_ulint(&lizard::lizard_sys->txn_undo_log_cached, \
                                (NUMBER));                                \
    }                                                                     \
  } while (0)

#endif  // lizard0mon_h

