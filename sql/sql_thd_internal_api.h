/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_THD_INTERNAL_API_INCLUDED
#define SQL_THD_INTERNAL_API_INCLUDED

/*
  This file defines THD-related API calls that are meant for internal
  usage (e.g. InnoDB, Thread Pool) only. There are therefore no stabilty
  guarantees.
*/

#include "my_global.h"
#include "my_thread.h"
#include "mysql/psi/psi.h"
#include "dur_prop.h"      // durability_properties
#include "handler.h"       // enum_tx_isolation

class partition_info;
class THD;
typedef struct charset_info_st CHARSET_INFO;

/**
  Set up various THD data for a new connection

  @param              thd            THD object
  @param              stack_start    Start of stack for connection
  @param              bound          True if bound to a physical thread.
  @param              psi_key        Instrumentation key for the thread.
*/
int thd_init(THD *thd, char *stack_start, bool bound, PSI_thread_key psi_key);

/**
  Create a THD and do proper initialization of it.

  @param enable_plugins     Should dynamic plugin support be enabled?
  @param background_thread  Is this a background thread?
  @param bound              True if bound to a physical thread.
  @param psi_key            Instrumentation key for the thread.

  @note Dynamic plugin support is only possible for THDs that
        are created after the server has initialized properly.
  @note THDs for background threads are currently not added to
        the global THD list. So they will e.g. not be visible in
        SHOW PROCESSLIST and the server will not wait for them to
        terminate during shutdown.
*/
THD *create_thd(bool enable_plugins, bool background_thread, bool bound, PSI_thread_key psi_key);

/**
  Cleanup the THD object, remove it from the global list of THDs
  and delete it.

  @param    thd   pointer to THD object.
*/
void destroy_thd(THD *thd);

/**
  Set thread stack in THD object

  @param thd              Thread object
  @param stack_start      Start of stack to set in THD object
*/
void thd_set_thread_stack(THD *thd, const char *stack_start);

/**
  Returns the partition_info working copy.
  Used to see if a table should be created with partitioning.

  @param thd thread context

  @return Pointer to the working copy of partition_info or NULL.
*/
partition_info* thd_get_work_part_info(THD* thd);

enum_tx_isolation thd_get_trx_isolation(const THD* thd);

const CHARSET_INFO *thd_charset(THD *thd);

/**
  Get the current query string for the thread.

  @param thd   The MySQL internal thread pointer

  @return query string and length. May be non-null-terminated.

  @note This function is not thread safe and should only be called
        from the thread owning thd. @see thd_query_safe().
*/
LEX_CSTRING thd_query_unsafe(THD *thd);

/**
  Get the current query string for the thread.

  @param thd     The MySQL internal thread pointer
  @param buf     Buffer where the query string will be copied
  @param buflen  Length of the buffer

  @return Length of the query

  @note This function is thread safe as the query string is
        accessed under mutex protection and the string is copied
        into the provided buffer. @see thd_query_unsafe().
*/
size_t thd_query_safe(THD *thd, char *buf, size_t buflen);

/**
  Check if a user thread is a replication slave thread
  @param thd user thread
  @retval 0 the user thread is not a replication slave thread
  @retval 1 the user thread is a replication slave thread
*/
int thd_slave_thread(const THD *thd);

/**
  Check if a user thread is running a non-transactional update
  @param thd user thread
  @retval 0 the user thread is not running a non-transactional update
  @retval 1 the user thread is running a non-transactional update
*/
int thd_non_transactional_update(const THD *thd);

/**
  Get the user thread's binary logging format
  @param thd user thread
  @return Value to be used as index into the binlog_format_names array
*/
int thd_binlog_format(const THD *thd);

/**
  Check if binary logging is filtered for thread's current db.
  @param thd Thread handle
  @retval 1 the query is not filtered, 0 otherwise.
*/
bool thd_binlog_filter_ok(const THD *thd);

/**
  Check if the query may generate row changes which may end up in the binary.
  @param thd Thread handle
  @retval 1 the query may generate row changes, 0 otherwise.
*/
bool thd_sqlcom_can_generate_row_events(const THD *thd);

/**
  Gets information on the durability property requested by a thread.
  @param thd Thread handle
  @return a durability property.
*/
durability_properties thd_get_durability_property(const THD *thd);

/**
  Get the auto_increment_offset auto_increment_increment.
  @param thd Thread object
  @param off auto_increment_offset
  @param inc auto_increment_increment
*/
void thd_get_autoinc(const THD *thd, ulong* off, ulong* inc);

/**
  Is strict sql_mode set.
  Needed by InnoDB.
  @param thd	Thread object
  @return True if sql_mode has strict mode (all or trans).
    @retval true  sql_mode has strict mode (all or trans).
    @retval false sql_mode has not strict mode (all or trans).
*/
bool thd_is_strict_mode(const THD *thd);

#endif // SQL_THD_INTERNAL_API_INCLUDED
