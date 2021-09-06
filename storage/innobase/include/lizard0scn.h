/*****************************************************************************

Copyright (c) 2013, 2020, Alibaba and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
lzeusited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the zeusplied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/lizard0scn.h
 Lizard scn number implementation.

 Created 2020-03-23 by Jianwei.zhao
 *******************************************************/

#ifndef lizard0scn_h
#define lizard0scn_h

#include "lizard0scn0types.h"
#include "ut0mutex.h"

#ifdef UNIV_PFS_MUTEX
/* lizard scn mutex PFS key */
extern mysql_pfs_key_t lizard_scn_mutex_key;
#endif

/** The number gap of persist scn number into system tablespace */
#define LIZARD_SCN_NUMBER_MAGIN 8192 

namespace lizard {

/** Invalid scn number was defined as the max value of ulint */
constexpr scn_t SCN_NULL = std::numeric_limits<scn_t>::max();

/** The max of scn number, crash direct if more than SCN_MAX */
constexpr scn_t SCN_MAX = std::numeric_limits<scn_t>::max() - 1;

/** For troubleshooting and readability, we use mutiple SCN FAKE in different
scenarios */
/**------------------------------------------------------------------------*/
/** The minimus and valid scn number */
constexpr scn_t SCN_FAKE = 1;

/** SCN special for temporary table record */
constexpr scn_t SCN_TEMP_TAB_REC = 2;

/** SCN special for undo lost */
constexpr scn_t SCN_UNDO_LOST = 3;

/** SCN special for index */
constexpr scn_t SCN_DICT_REC = 4;

/** Initialized prev scn number in txn header. See the case:
1. If txn undos are unexpectedly removed
2. the mysql run with cleanout_safe_mode again
some prev UBAs might point at such a txn header: in uncommitted status
but if not really the prev UBAs try to find. And lookup by these UBAs
might get a initialized prev scn/utc. We should set them small enough for
visibility. */
constexpr scn_t PREV_SCN_UNDO_LOST = 5;

/** MAX reserved scn NUMBER  */
constexpr scn_t SCN_RESERVERD_MAX = 1024;

/** The scn number for innodb dynamic metadata */
constexpr scn_t SCN_DYNAMIC_METADATA = SCN_MAX;

/** The scn number for innodb log ddl */
constexpr scn_t SCN_LOG_DDL = SCN_MAX;
/**------------------------------------------------------------------------*/

/** Invalid time 1970-01-01 00:00:00 +0000 (UTC) */
constexpr utc_t UTC_NULL = std::numeric_limits<utc_t>::min();

/** Temporary table utc {2020/1/1 00:00:00} */
constexpr utc_t UTC_TEMP_TAB_REC = 1577808000 * 1000000ULL;

/** utc for undo lost:  {2020/1/1 00:00:01} */
constexpr utc_t UTC_UNDO_LOST = 1577808000 * 1000000ULL + 1;

/** Initialized prev utc in txn header */
constexpr utc_t PREV_UTC_UNDO_LOST = 1577808000 * 1000000ULL + 2;

/** The max local time is less than 2038 year */
constexpr utc_t UTC_MAX = std::numeric_limits<std::int32_t>::max() * 1000000ULL;

/** The utc for innodb dynamic metadata */
constexpr utc_t UTC_DYNAMIC_METADATA = UTC_MAX;

/** The utc for innodb log ddl */
constexpr utc_t UTC_LOG_DDL = UTC_MAX;

/* The structure of scn number generation */
class SCN {
 public:
  SCN();
  virtual ~SCN();

  /** Assign the init value by reading from lizard tablespace */
  void init();

  /** Calculate a new scn number
  @return     scn */
  scn_t new_scn();

  /** Calculate a new scn number and consistent UTC time
  @return   <SCN, UTC> */
  commit_scn_t new_commit_scn();

  /** Get m_scn
  @return     m_scn */
  scn_t acquire_scn(bool mutex_hold = false);

  /** lock mutex */
  void lock() {
    ut_ad(m_inited);
    mutex_enter(&m_mutex);
  }

  /** unlock mutex */
  void unlock() {
    ut_ad(m_inited);
    mutex_exit(&m_mutex);
  }

#ifdef UNIV_DEBUG
  /** check if own mutex */
  bool own_lock() {
    ut_ad(m_inited);
    return mutex_own(&m_mutex);
  }
#endif

 private:
  /** Flush the scn number to system tablepace every LIZARD_SCN_NUMBER_MAGIN */
  void flush_scn();

  /** Disable the copy and assign function */
  SCN(const SCN &) = delete;
  SCN(const SCN &&) = delete;
  SCN &operator=(const SCN &) = delete;

 private:
  scn_t m_scn;
  bool m_inited;
  ib_mutex_t m_mutex;
};

/**
  Check the commit scn state

  @param[in]    scn       commit scn
  @return       scn state SCN_STATE_INITIAL, SCN_STATE_ALLOCATED or
                          SCN_STATE_INVALID
*/
enum scn_state_t commit_scn_state(const commit_scn_t &scn);

}  // namespace lizard

#define lizard_sys_scn_mutex_enter()      \
  do {                                    \
    ut_ad(lizard::lizard_sys != nullptr); \
    lizard::lizard_sys->scn.lock();       \
  } while(0)

#define lizard_sys_scn_mutex_exit()       \
  do {                                    \
    ut_ad(lizard::lizard_sys != nullptr); \
    lizard::lizard_sys->scn.unlock();     \
  } while(0)

#ifdef UNIV_DEBUG
#define lizard_sys_scn_mutex_own() lizard::lizard_sys->scn.own_lock()
#endif

/** Commit scn initial value */
#define COMMIT_SCN_NULL \
  { lizard::SCN_NULL, lizard::UTC_NULL }

#if defined UNIV_DEBUG || defined LIZARD_DEBUG

/* Debug validation of commit scn directly */
#define assert_commit_scn_state(scn, state)       \
  do {                                            \
    ut_a(lizard::commit_scn_state(scn) == state); \
  } while (0)

#define assert_commit_scn_initial(scn)               \
  do {                                               \
    assert_commit_scn_state(scn, SCN_STATE_INITIAL); \
  } while (0)

#define assert_commit_scn_allocated(scn)               \
  do {                                                 \
    assert_commit_scn_state(scn, SCN_STATE_ALLOCATED); \
  } while (0)

/* Debug validation of commit scn from trx->scn */
#define assert_trx_scn_state(trx, state)                        \
  do {                                                          \
    ut_a(lizard::commit_scn_state(trx->txn_desc.scn) == state); \
  } while (0)

#define assert_trx_scn_initial(trx)               \
  do {                                            \
    assert_trx_scn_state(trx, SCN_STATE_INITIAL); \
  } while (0)

#define assert_trx_scn_allocated(trx)               \
  do {                                              \
    assert_trx_scn_state(trx, SCN_STATE_ALLOCATED); \
  } while (0)

#define assert_trx_scn(trx)                                                 \
  do {                                                                      \
    if (trx->state == TRX_STATE_PREPARED || trx->state == TRX_STATE_ACTIVE) \
      assert_trx_scn_state(trx, SCN_STATE_INITIAL);                         \
    else if (trx->state == TRX_STATE_COMMITTED_IN_MEMORY)                   \
      assert_trx_scn_state(trx, SCN_STATE_ALLOCATED);                       \
  } while (0)

/* Debug validation of commit scn from undo->scn */
#define assert_undo_scn_state(undo, state)              \
  do {                                                  \
    ut_a(lizard::commit_scn_state(undo->scn) == state); \
  } while (0)

#define assert_undo_scn_initial(undo)               \
  do {                                              \
    assert_undo_scn_state(undo, SCN_STATE_INITIAL); \
  } while (0)

#define assert_undo_scn_allocated(undo)               \
  do {                                                \
    assert_undo_scn_state(undo, SCN_STATE_ALLOCATED); \
  } while (0)

#else

/* Debug validation of commit scn directly */
#define assert_commit_scn_state(scn, state)
#define assert_commit_scn_initial(scn)
#define assert_commit_scn_allocated(scn)

/* Debug validation of commit scn from trx->scn */
#define assert_trx_scn_state(trx, state)
#define assert_trx_scn_initial(trx)
#define assert_trx_scn_allocated(trx)
#define assert_trx_scn(trx)

/* Debug validation of commit scn from undo->scn */
#define assert_undo_scn_state(undo, state)
#define assert_undo_scn_initial(undo)
#define assert_undo_scn_allocated(undo)

#endif /* UNIV_DEBUG || LIZARD_DEBUG */

#endif /* lizard0scn_h define */
