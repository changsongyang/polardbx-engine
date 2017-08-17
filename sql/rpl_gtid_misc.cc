/* Copyright (c) 2012, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; version 2 of the
   License.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
   02110-1301 USA */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <algorithm>
#include <atomic>

#include "control_events.h"
#include "m_ctype.h"
#include "m_string.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_macros.h"
#include "my_thread.h"
#include "mysql/components/services/mysql_mutex_bits.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/udf_registration_types.h"
#include "sql/rpl_gtid.h"
#include "typelib.h"

#ifdef MYSQL_SERVER
#include <storage/perfschema/pfs_instr_class.h> // gtid_monitoring_getsystime

#include "mysql/thread_type.h"
#include "mysqld_error.h"     // ER_*
#include "sql/binlog.h"
#include "sql/current_thd.h"
#include "sql/rpl_msr.h"
#include "sql/sql_class.h"    // THD
#include "sql/sql_error.h"
#endif // ifdef MYSQL_SERVER

#ifndef MYSQL_SERVER
#include "client/mysqlbinlog.h"
#endif


// Todo: move other global gtid variable declarations here.
Checkable_rwlock *gtid_mode_lock= NULL;
std::atomic<ulong> gtid_mode_counter;

ulong _gtid_mode;
const char *gtid_mode_names[]=
{"OFF", "OFF_PERMISSIVE", "ON_PERMISSIVE", "ON", NullS};
TYPELIB gtid_mode_typelib=
{ array_elements(gtid_mode_names) - 1, "", gtid_mode_names, NULL };


#ifdef MYSQL_SERVER
enum_gtid_mode get_gtid_mode(enum_gtid_mode_lock have_lock)
{
  switch (have_lock)
  {
  case GTID_MODE_LOCK_NONE:
    global_sid_lock->rdlock();
    break;
  case GTID_MODE_LOCK_SID:
    global_sid_lock->assert_some_lock();
    break;
  case GTID_MODE_LOCK_CHANNEL_MAP:
    channel_map.assert_some_lock();
    break;
  case GTID_MODE_LOCK_GTID_MODE:
    gtid_mode_lock->assert_some_lock();

/*
  This lock is currently not used explicitly by any of the places
  that calls get_gtid_mode.  Still it would be valid for a caller to
  use it to protect reads of GTID_MODE, so we keep the code here in
  case it is needed in the future.

  case GTID_MODE_LOCK_LOG:
    mysql_mutex_assert_owner(mysql_bin_log.get_log_lock());
    break;
*/
  }
  enum_gtid_mode ret= (enum_gtid_mode)_gtid_mode;
  if (have_lock == GTID_MODE_LOCK_NONE)
    global_sid_lock->unlock();
  return ret;
}
#endif


ulong _gtid_consistency_mode;
const char *gtid_consistency_mode_names[]=
{"OFF", "ON", "WARN", NullS};
TYPELIB gtid_consistency_mode_typelib=
{ array_elements(gtid_consistency_mode_names) - 1, "", gtid_consistency_mode_names, NULL };


#ifdef MYSQL_SERVER
enum_gtid_consistency_mode get_gtid_consistency_mode()
{
  global_sid_lock->assert_some_lock();
  return (enum_gtid_consistency_mode)_gtid_consistency_mode;
}
#endif


enum_return_status Gtid::parse(Sid_map *sid_map, const char *text)
{
  DBUG_ENTER("Gtid::parse");
  rpl_sid sid;
  const char *s= text;

  SKIP_WHITESPACE();

  // parse sid
  if (sid.parse(s, binary_log::Uuid::TEXT_LENGTH) == 0)
  {
    rpl_sidno sidno_var= sid_map->add_sid(sid);
    if (sidno_var <= 0)
      RETURN_REPORTED_ERROR;
    s += binary_log::Uuid::TEXT_LENGTH;

    SKIP_WHITESPACE();

    // parse colon
    if (*s == ':')
    {
      s++;

      SKIP_WHITESPACE();

      // parse gno
      rpl_gno gno_var= parse_gno(&s);
      if (gno_var > 0)
      {
        SKIP_WHITESPACE();
        if (*s == '\0')
        {
          sidno= sidno_var;
          gno= gno_var;
          RETURN_OK;
        }
        else
          DBUG_PRINT("info", ("expected end of string, found garbage '%.80s' "
                              "at char %d in '%s'",
                              s, (int)(s - text), text));
      }
      else
        DBUG_PRINT("info", ("GNO was zero or invalid (%lld) at char %d in '%s'",
                            gno_var, (int)(s - text), text));
    }
    else
      DBUG_PRINT("info", ("missing colon at char %d in '%s'",
                          (int)(s - text), text));
  }
  else
    DBUG_PRINT("info", ("not a uuid at char %d in '%s'",
                        (int)(s - text), text));
  BINLOG_ERROR(("Malformed GTID specification: %.200s", text),
               (ER_MALFORMED_GTID_SPECIFICATION, MYF(0), text));
  RETURN_REPORTED_ERROR;
}


int Gtid::to_string(const rpl_sid &sid, char *buf) const
{
  DBUG_ENTER("Gtid::to_string");
  char *s= buf + sid.to_string(buf);
  *s= ':';
  s++;
  s+= format_gno(s, gno);
  DBUG_RETURN((int)(s - buf));
}


int Gtid::to_string(const Sid_map *sid_map, char *buf, bool need_lock) const
{
  DBUG_ENTER("Gtid::to_string");
  int ret;
  if (sid_map != NULL)
  {
    Checkable_rwlock *lock= sid_map->get_sid_lock();
    if (lock)
    {
      if (need_lock)
        lock->rdlock();
      else
        lock->assert_some_lock();
    }
    const rpl_sid &sid= sid_map->sidno_to_sid(sidno);
    if (lock && need_lock)
      lock->unlock();
    ret= to_string(sid, buf);
  }
  else
  {
#ifdef DBUG_OFF
    /*
      NULL is only allowed in debug mode, since the sidno does not
      make sense for users but is useful to include in debug
      printouts.  Therefore, we want to ASSERT(0) in non-debug mode.
      Since there is no ASSERT in non-debug mode, we use abort
      instead.
    */
    abort();
#endif
    ret= sprintf(buf, "%d:%lld", sidno, gno);
  }
  DBUG_RETURN(ret);
}


bool Gtid::is_valid(const char *text)
{
  DBUG_ENTER("Gtid::is_valid");
  const char *s= text;
  SKIP_WHITESPACE();
  if (!rpl_sid::is_valid(s, binary_log::Uuid::TEXT_LENGTH))
  {
    DBUG_PRINT("info", ("not a uuid at char %d in '%s'",
                        (int)(s - text), text));
    DBUG_RETURN(false);
  }
  s += binary_log::Uuid::TEXT_LENGTH;
  SKIP_WHITESPACE();
  if (*s != ':')
  {
    DBUG_PRINT("info", ("missing colon at char %d in '%s'",
                        (int)(s - text), text));
    DBUG_RETURN(false);
  }
  s++;
  SKIP_WHITESPACE();
  if (parse_gno(&s) <= 0)
  {
    DBUG_PRINT("info", ("GNO was zero or invalid at char %d in '%s'",
                        (int)(s - text), text));
    DBUG_RETURN(false);
  }
  SKIP_WHITESPACE();
  if (*s != 0)
  {
    DBUG_PRINT("info", ("expected end of string, found garbage '%.80s' "
                        "at char %d in '%s'",
                        s, (int)(s - text), text));
    DBUG_RETURN(false);
  }
  DBUG_RETURN(true);
}


#ifndef DBUG_OFF
void check_return_status(enum_return_status status, const char *action,
                         const char *status_name, int allow_unreported)
{
  if (status != RETURN_STATUS_OK)
  {
    DBUG_ASSERT(allow_unreported || status == RETURN_STATUS_REPORTED_ERROR);
    if (status == RETURN_STATUS_REPORTED_ERROR)
    {
#if defined(MYSQL_SERVER) && !defined(DBUG_OFF)
      THD *thd= current_thd;
      /*
        We create a new system THD with 'SYSTEM_THREAD_COMPRESS_GTID_TABLE'
        when initializing gtid state by fetching gtids during server startup,
        so we can check on it before diagnostic area is active and skip the
        assert in this case. We assert that diagnostic area logged the error
        outside server startup since the assert is realy useful.
     */
      DBUG_ASSERT(thd == NULL ||
                  thd->get_stmt_da()->status() == Diagnostics_area::DA_ERROR ||
                  (thd->get_stmt_da()->status() == Diagnostics_area::DA_EMPTY &&
                   thd->system_thread == SYSTEM_THREAD_COMPRESS_GTID_TABLE));
#endif
    }
    DBUG_PRINT("info", ("%s error %d (%s)", action, status, status_name));
  }
}
#endif // ! DBUG_OFF


#ifdef MYSQL_SERVER
rpl_sidno get_sidno_from_global_sid_map(rpl_sid sid)
{
  DBUG_ENTER("get_sidno_from_global_sid_map(rpl_sid)");

  global_sid_lock->rdlock();
  rpl_sidno sidno= global_sid_map->add_sid(sid);
  global_sid_lock->unlock();

  DBUG_RETURN(sidno);
}

rpl_gno get_last_executed_gno(rpl_sidno sidno)
{
  DBUG_ENTER("get_last_executed_gno(rpl_sidno)");

  global_sid_lock->rdlock();
  rpl_gno gno= gtid_state->get_last_executed_gno(sidno);
  global_sid_lock->unlock();

  DBUG_RETURN(gno);
}

Trx_monitoring_info::Trx_monitoring_info()
{
  is_info_set= false;
}

Trx_monitoring_info::Trx_monitoring_info(const Trx_monitoring_info& info)
{
  if ((is_info_set= info.is_info_set))
  {
    gtid= info.gtid;
    original_commit_timestamp= info.original_commit_timestamp;
    immediate_commit_timestamp= info.immediate_commit_timestamp;
    start_time= info.start_time;
    end_time= info.end_time;
    skipped= info.skipped;
  }
}

void Trx_monitoring_info::clear()
{
  gtid= {0, 0};
  original_commit_timestamp= 0;
  immediate_commit_timestamp= 0;
  start_time= 0;
  end_time= 0;
  skipped= false;
  is_info_set= false;
}

void Trx_monitoring_info::copy_to_ps_table(Sid_map* sid_map,
                                           char *gtid_arg,
                                           uint *gtid_length_arg,
                                           ulonglong *original_commit_ts_arg,
                                           ulonglong *immediate_commit_ts_arg,
                                           ulonglong *start_time_arg)
{
  if (is_info_set)
  {
    // The trx_monitoring_info is populated
    if (gtid.is_empty())
    {
      // The transaction is anonymous
      memcpy(gtid_arg, "ANONYMOUS", 10);
      *gtid_length_arg= 9;
    }
    else
    {
      // The GTID is set
      Checkable_rwlock *sid_lock= sid_map->get_sid_lock();
      sid_lock->rdlock();
      *gtid_length_arg= gtid.to_string(sid_map, gtid_arg);
      sid_lock->unlock();
    }
    *original_commit_ts_arg= original_commit_timestamp;
    *immediate_commit_ts_arg= immediate_commit_timestamp;
    *start_time_arg= start_time/10;
  }
  else
  {
    // This monitoring info is not populated, so let's zero the input
    memcpy(gtid_arg, "", 1);
    *gtid_length_arg= 0;
    *original_commit_ts_arg= 0;
    *immediate_commit_ts_arg= 0;
    *start_time_arg= 0;
  }
}

void Trx_monitoring_info::copy_to_ps_table(Sid_map* sid_map,
                                           char *gtid_arg,
                                           uint *gtid_length_arg,
                                           ulonglong *original_commit_ts_arg,
                                           ulonglong *immediate_commit_ts_arg,
                                           ulonglong *start_time_arg,
                                           ulonglong *end_time_arg)
{
  *end_time_arg= is_info_set ? end_time/10 : 0;
  copy_to_ps_table(sid_map, gtid_arg, gtid_length_arg,
                   original_commit_ts_arg,
                   immediate_commit_ts_arg,
                   start_time_arg);
}

Gtid_monitoring_info::Gtid_monitoring_info(mysql_mutex_t *atomic_mutex_arg)
  : atomic_mutex(atomic_mutex_arg)
{
  processing_trx= new Trx_monitoring_info;
  last_processed_trx= new Trx_monitoring_info;
}

Gtid_monitoring_info::~Gtid_monitoring_info()
{
  delete last_processed_trx;
  delete processing_trx;
}

void Gtid_monitoring_info::atomic_lock()
{
  if (atomic_mutex == NULL)
  {
    bool expected= false;
    while (!atomic_locked.compare_exchange_weak(expected, true))
    {
      /*
        On exchange failures, the atomic_locked value (true) is set
        to the expected variable. It needs to be reset again.
      */
      expected= false;
      /*
        All "atomic" operations on this object are based on copying
        variable contents and setting values. They should not take long.
      */
      my_thread_yield();
    }
#ifndef DBUG_OFF
    DBUG_ASSERT(!is_locked);
    is_locked= true;
#endif
  }
  else
  {
    // If this object is relying on a mutex, just ensure it was acquired.
    mysql_mutex_assert_owner(atomic_mutex)
  }
}

void Gtid_monitoring_info::atomic_unlock()
{
  if (atomic_mutex == NULL)
  {
#ifndef DBUG_OFF
    DBUG_ASSERT(is_locked);
    is_locked= false;
#endif
    atomic_locked= false;
  }
  else
    mysql_mutex_assert_owner(atomic_mutex)
}

void Gtid_monitoring_info::clear()
{
  atomic_lock();
  processing_trx->clear();
  last_processed_trx->clear();
  atomic_unlock();
}

void Gtid_monitoring_info::clear_processing_trx()
{
  atomic_lock();
  processing_trx->clear();
  atomic_unlock();
}

void Gtid_monitoring_info::clear_last_processed_trx()
{
  atomic_lock();
  last_processed_trx->clear();
  atomic_unlock();
}

void Gtid_monitoring_info::start(Gtid gtid_arg,
                                 ulonglong original_ts_arg,
                                 ulonglong immediate_ts_arg,
                                 bool skipped_arg)
{
  /* Collect current timestamp before the atomic operation */
  ulonglong start_time= gtid_monitoring_getsystime();

  atomic_lock();
  processing_trx->gtid= gtid_arg;
  processing_trx->original_commit_timestamp= original_ts_arg;
  processing_trx->immediate_commit_timestamp= immediate_ts_arg;
  processing_trx->start_time= start_time;
  processing_trx->end_time= 0;
  processing_trx->skipped= skipped_arg;
  processing_trx->is_info_set= true;
  atomic_unlock();
}

void Gtid_monitoring_info::finish()
{
  /* Collect current timestamp before the atomic operation */
  ulonglong end_time= gtid_monitoring_getsystime();

  atomic_lock();
  processing_trx->end_time= end_time;
  /*
    We only swap if the transaction was not skipped.

    Notice that only applier thread set the skipped variable to true.
  */
  if (!processing_trx->skipped)
    std::swap(processing_trx,last_processed_trx);

  processing_trx->clear();
  atomic_unlock();
}

void Gtid_monitoring_info::copy_info_to(Trx_monitoring_info *processing_dest,
                                        Trx_monitoring_info *last_processed_dest)
{
  atomic_lock();
  *processing_dest= *processing_trx;
  *last_processed_dest= *last_processed_trx;
  atomic_unlock();
}

void Gtid_monitoring_info::copy_info_to(Gtid_monitoring_info *dest)
{
  copy_info_to(dest->processing_trx, dest->last_processed_trx);
}

bool Gtid_monitoring_info::is_processing_trx_set()
{
  /*
    This function is only called by threads about to update the monitoring
    information. It should be safe to collect this information without
    acquiring locks.
  */
  return processing_trx->is_info_set;
}

const Gtid *Gtid_monitoring_info::get_processing_trx_gtid()
{
  /*
    This function is only called by relay log recovery/queuing.
  */
  DBUG_ASSERT(atomic_mutex != NULL);
  mysql_mutex_assert_owner(atomic_mutex);
  return &processing_trx->gtid;
}
#endif // ifdef MYSQL_SERVER
