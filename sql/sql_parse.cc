/* Copyright (c) 1999, 2018, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#define  LOG_SUBSYSTEM_TAG "parser"

#include "sql/sql_parse.h"

#include "my_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <thread>
#include <atomic>
#include <utility>

#ifdef HAVE_LSAN_DO_RECOVERABLE_LEAK_CHECK
#include <sanitizer/lsan_interface.h>
#endif

#include "binary_log_types.h"
#include "dur_prop.h"
#include "m_ctype.h"
#include "m_string.h"
#include "my_alloc.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_loglevel.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "my_sys.h"
#include "my_table_map.h"
#include "my_thread_local.h"
#include "my_time.h"
#include "mysql/com_data.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/components/services/log_shared.h"
#include "mysql/components/services/psi_statement_bits.h"
#include "mysql/plugin_audit.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/mysql_rwlock.h"
#include "mysql/psi/mysql_statement.h"
#include "mysql/service_mysql_alloc.h"
#include "mysqld_error.h"
#include "mysys_err.h"        // EE_CAPACITY_EXCEEDED
#include "nullable.h"
#include "pfs_thread_provider.h"
#include "prealloced_array.h"
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h" // acl_authenticate
#include "sql/auth/sql_security_ctx.h"
#include "sql/binlog.h"       // purge_master_logs
#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dd.h"        // dd::get_dictionary
#include "sql/dd/dd_schema.h" // Schema_MDL_locker
#include "sql/dd/dictionary.h" // dd::Dictionary::is_system_view_name
#include "sql/dd/info_schema/table_stats.h"
#include "sql/debug_sync.h"   // DEBUG_SYNC
#include "sql/derror.h"       // ER_THD
#include "sql/discrete_interval.h"
#include "sql/error_handler.h" // Strict_error_handler
#include "sql/events.h"       // Events
#include "sql/field.h"
#include "sql/gis/srid.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_subselect.h"
#include "sql/item_timefunc.h" // Item_func_unix_timestamp
#include "sql/key_spec.h"     // Key_spec
#include "sql/log.h"          // query_logger
#include "sql/log_event.h"    // slave_execute_deferred_events
#include "sql/mdl.h"
#include "sql/mem_root_array.h"
#include "sql/mysqld.h"       // stage_execution_of_init_command
#include "sql/mysqld_thd_manager.h" // Find_thd_with_id
#include "sql/nested_join.h"
#include "sql/opt_hints.h"
#include "sql/opt_trace.h"    // Opt_trace_start
#include "sql/parse_location.h"
#include "sql/parse_tree_node_base.h"
#include "sql/parse_tree_nodes.h"
#include "sql/persisted_variable.h"
#include "sql/protocol.h"
#include "sql/protocol_classic.h"
#include "sql/psi_memory_key.h"
#include "sql/query_options.h"
#include "sql/query_result.h"
#include "sql/resourcegroups/resource_group_basic_types.h"
#include "sql/resourcegroups/resource_group_mgr.h" // Resource_group_mgr::instance
#include "sql/rpl_context.h"
#include "sql/rpl_filter.h"   // rpl_filter
#include "sql/rpl_group_replication.h" // group_replication_start
#include "sql/rpl_gtid.h"
#include "sql/rpl_master.h"   // register_slave
#include "sql/rpl_rli.h"      // mysql_show_relaylog_events
#include "sql/rpl_slave.h"    // change_master_cmd
#include "sql/session_tracker.h"
#include "sql/set_var.h"
#include "sql/sp.h"           // sp_create_routine
#include "sql/sp_cache.h"     // sp_cache_enforce_limit
#include "sql/sp_head.h"      // sp_head
#include "sql/sql_alter.h"
#include "sql/sql_audit.h"    // MYSQL_AUDIT_NOTIFY_CONNECTION_CHANGE_USER
#include "sql/sql_backup_lock.h"  // acquire_shared_mdl_for_backup
#include "sql/sql_base.h"     // find_temporary_table
#include "sql/sql_binlog.h"   // mysql_client_binlog_statement
#include "sql/sql_class.h"
#include "sql/sql_cmd.h"
#include "sql/sql_connect.h"  // decrease_user_connections
#include "sql/sql_const.h"
#include "sql/sql_db.h"       // mysql_change_db
#include "sql/sql_digest.h"
#include "sql/sql_digest_stream.h"
#include "sql/sql_error.h"
#include "sql/sql_handler.h"  // mysql_ha_rm_tables
#include "sql/sql_help.h"     // mysqld_help
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_prepare.h"  // mysql_stmt_execute
#include "sql/sql_profile.h"
#include "sql/sql_query_rewrite.h" // invoke_pre_parse_rewrite_plugins
#include "sql/sql_reload.h"   // handle_reload_request
#include "sql/sql_rename.h"   // mysql_rename_tables
#include "sql/sql_rewrite.h"  // mysql_rewrite_query
#include "sql/sql_select.h"   // handle_query
#include "sql/sql_show.h"     // find_schema_table
#include "sql/sql_table.h"    // mysql_create_table
#include "sql/sql_test.h"     // mysql_print_status
#include "sql/sql_trigger.h"  // add_table_for_trigger
#include "sql/sql_udf.h"
#include "sql/sql_view.h"     // mysql_create_view
#include "sql/srs_fetcher.h"
#include "sql/system_variables.h" // System_status_var
#include "sql/table.h"
#include "sql/table_cache.h"  // table_cache_manager
#include "sql/thd_raii.h"
#include "sql/transaction.h"  // trans_rollback_implicit
#include "sql/transaction_info.h"
#include "sql_string.h"
#include "thr_lock.h"
#include "violite.h"

namespace dd {
class Spatial_reference_system;
}  // namespace dd
namespace resourcegroups {
class Resource_group;
}  // namespace resourcegroups
struct mysql_rwlock_t;

namespace dd {
class Schema;
}  // namespace dd

namespace dd {
class Abstract_table;
}  // namespace dd

using std::max;
using Mysql::Nullable;

/**
  @defgroup Runtime_Environment Runtime Environment
  @{
*/

/* Used in error handling only */
#define SP_COM_STRING(LP) \
  ((LP)->sql_command == SQLCOM_CREATE_SPFUNCTION || \
   (LP)->sql_command == SQLCOM_ALTER_FUNCTION || \
   (LP)->sql_command == SQLCOM_SHOW_CREATE_FUNC || \
   (LP)->sql_command == SQLCOM_DROP_FUNCTION ? \
   "FUNCTION" : "PROCEDURE")

static void sql_kill(THD *thd, my_thread_id id, bool only_kill_query);

const LEX_STRING command_name[]={
  { C_STRING_WITH_LEN("Sleep") },
  { C_STRING_WITH_LEN("Quit") },
  { C_STRING_WITH_LEN("Init DB") },
  { C_STRING_WITH_LEN("Query") },
  { C_STRING_WITH_LEN("Field List") },
  { C_STRING_WITH_LEN("Create DB") },
  { C_STRING_WITH_LEN("Drop DB") },
  { C_STRING_WITH_LEN("Refresh") },
  { C_STRING_WITH_LEN("Shutdown") },
  { C_STRING_WITH_LEN("Statistics") },
  { C_STRING_WITH_LEN("Processlist") },
  { C_STRING_WITH_LEN("Connect") },
  { C_STRING_WITH_LEN("Kill") },
  { C_STRING_WITH_LEN("Debug") },
  { C_STRING_WITH_LEN("Ping") },
  { C_STRING_WITH_LEN("Time") },
  { C_STRING_WITH_LEN("Delayed insert") },
  { C_STRING_WITH_LEN("Change user") },
  { C_STRING_WITH_LEN("Binlog Dump") },
  { C_STRING_WITH_LEN("Table Dump") },
  { C_STRING_WITH_LEN("Connect Out") },
  { C_STRING_WITH_LEN("Register Slave") },
  { C_STRING_WITH_LEN("Prepare") },
  { C_STRING_WITH_LEN("Execute") },
  { C_STRING_WITH_LEN("Long Data") },
  { C_STRING_WITH_LEN("Close stmt") },
  { C_STRING_WITH_LEN("Reset stmt") },
  { C_STRING_WITH_LEN("Set option") },
  { C_STRING_WITH_LEN("Fetch") },
  { C_STRING_WITH_LEN("Daemon") },
  { C_STRING_WITH_LEN("Binlog Dump GTID") },
  { C_STRING_WITH_LEN("Reset Connection") },
  { C_STRING_WITH_LEN("Error") }  // Last command number
};

/**
  Returns true if all tables should be ignored.
*/
bool all_tables_not_ok(THD *thd, TABLE_LIST *tables)
{
  Rpl_filter *rpl_filter= thd->rli_slave->rpl_filter;

  return rpl_filter->is_on() && tables && !thd->sp_runtime_ctx &&
         !rpl_filter->tables_ok(thd->db().str, tables);
}

/**
  Checks whether the event for the given database, db, should
  be ignored or not. This is done by checking whether there are
  active rules in ignore_db or in do_db containers. If there
  are, then check if there is a match, if not then check the
  wild_do rules.
      
  NOTE: This means that when using this function replicate-do-db 
        and replicate-ignore-db take precedence over wild do 
        rules.

  @param thd  Thread handle.
  @param db   Database name used while evaluating the filtering
              rules.
  
*/
inline bool db_stmt_db_ok(THD *thd, char* db)
{
  DBUG_ENTER("db_stmt_db_ok");

  if (!thd->slave_thread)
    DBUG_RETURN(true);

  Rpl_filter* rpl_filter= thd->rli_slave->rpl_filter;

  /*
    No filters exist in ignore/do_db ? Then, just check
    wild_do_table filtering. Otherwise, check the do_db
    rules.
  */
  bool db_ok= (rpl_filter->get_do_db()->is_empty() &&
               rpl_filter->get_ignore_db()->is_empty()) ?
              rpl_filter->db_ok_with_wild_table(db) :
              /*
                We already increased do_db/ignore_db counter by calling
                db_ok(...) in mysql_execute_command(...) when applying
                relay log event for CREATE DATABASE ..., DROP DATABASE
                ... and ALTER DATABASE .... So we do not increase
                do_db/ignore_db counter when calling the db_ok(...) again.
              */
              rpl_filter->db_ok(db, false);

  DBUG_RETURN(db_ok);
}


bool some_non_temp_table_to_be_updated(THD *thd, TABLE_LIST *tables)
{
  for (TABLE_LIST *table= tables; table; table= table->next_global)
  {
    DBUG_ASSERT(table->db && table->table_name);
    /*
      Update on performance_schema and temp tables are allowed
      in readonly mode.
    */
    if (table->updating && !find_temporary_table(thd, table) &&
        !is_perfschema_db(table->db, table->db_length))
      return 1;
  }
  return 0;
}


/**
  Returns whether the command in thd->lex->sql_command should cause an
  implicit commit. An active transaction should be implicitly commited if the
  statement requires so.

  @param thd    Thread handle.
  @param mask   Bitmask used for the SQL command match.

  @retval true This statement shall cause an implicit commit.
  @retval false This statement shall not cause an implicit commit.
*/
bool stmt_causes_implicit_commit(const THD *thd, uint mask)
{
  DBUG_ENTER("stmt_causes_implicit_commit");
  const LEX *lex= thd->lex;

  if ((sql_command_flags[lex->sql_command] & mask) == 0 ||
      thd->is_plugin_fake_ddl())
    DBUG_RETURN(false);

  switch (lex->sql_command) {
  case SQLCOM_DROP_TABLE:
    DBUG_RETURN(!lex->drop_temporary);
  case SQLCOM_ALTER_TABLE:
  case SQLCOM_CREATE_TABLE:
    /* If CREATE TABLE of non-temporary table, do implicit commit */
    DBUG_RETURN((lex->create_info->options & HA_LEX_CREATE_TMP_TABLE) == 0);
  case SQLCOM_SET_OPTION:
    /* Implicitly commit a transaction started by a SET statement */
    DBUG_RETURN(lex->autocommit);
  case SQLCOM_RESET:
    DBUG_RETURN(lex->option_type != OPT_PERSIST);
  default:
    DBUG_RETURN(true);
  }
}


/**
  Mark all commands that somehow changes a table.

  This is used to check number of updates / hour.

  sql_command is actually set to SQLCOM_END sometimes
  so we need the +1 to include it in the array.

  See COMMAND_FLAG_xxx for different type of commands
     2  - query that returns meaningful ROW_COUNT() -
          a number of modified rows
*/

uint sql_command_flags[SQLCOM_END+1];
uint server_command_flags[COM_END+1];

void init_sql_command_flags(void)
{
  /* Initialize the server command flags array. */
  memset(server_command_flags, 0, sizeof(server_command_flags));

  server_command_flags[COM_SLEEP]=               CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_INIT_DB]=             CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_QUERY]=               CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_FIELD_LIST]=          CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_REFRESH]=             CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_STATISTICS]=          CF_SKIP_QUESTIONS;
  server_command_flags[COM_PROCESS_KILL]=        CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_PING]=                CF_SKIP_QUESTIONS;
  server_command_flags[COM_STMT_PREPARE]=        CF_SKIP_QUESTIONS |
                                                 CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_STMT_EXECUTE]=        CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_STMT_SEND_LONG_DATA]= CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_STMT_CLOSE]=          CF_SKIP_QUESTIONS |
                                                 CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_STMT_RESET]=          CF_SKIP_QUESTIONS |
                                                 CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_STMT_FETCH]=          CF_ALLOW_PROTOCOL_PLUGIN;
  server_command_flags[COM_END]=                 CF_ALLOW_PROTOCOL_PLUGIN;

  /* Initialize the sql command flags array. */
  memset(sql_command_flags, 0, sizeof(sql_command_flags));

  /*
    In general, DDL statements do not generate row events and do not go
    through a cache before being written to the binary log. However, the
    CREATE TABLE...SELECT is an exception because it may generate row
    events. For that reason,  the SQLCOM_CREATE_TABLE  which represents
    a CREATE TABLE, including the CREATE TABLE...SELECT, has the
    CF_CAN_GENERATE_ROW_EVENTS flag. The distinction between a regular
    CREATE TABLE and the CREATE TABLE...SELECT is made in other parts of
    the code, in particular in the Query_log_event's constructor.
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]=   CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_INDEX]=   CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_TABLE]=    CF_CHANGES_DATA | CF_WRITE_LOGS_COMMAND |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_TRUNCATE]=       CF_CHANGES_DATA | CF_WRITE_LOGS_COMMAND |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_TABLE]=     CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_LOAD]=           CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS;
  sql_command_flags[SQLCOM_CREATE_DB]=      CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_DB]=        CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_DB]=       CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_RENAME_TABLE]=   CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_INDEX]=     CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_VIEW]=    CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_AUTO_COMMIT_TRANS  |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_VIEW]=      CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_TRIGGER]= CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_TRIGGER]=   CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_EVENT]=   CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_EVENT]=    CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_EVENT]=     CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_IMPORT]=         CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;

  sql_command_flags[SQLCOM_UPDATE]=	    CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  sql_command_flags[SQLCOM_UPDATE_MULTI]=   CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  // This is INSERT VALUES(...), can be VALUES(stored_func()) so we trace it
  sql_command_flags[SQLCOM_INSERT]=	    CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  sql_command_flags[SQLCOM_INSERT_SELECT]=  CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  sql_command_flags[SQLCOM_DELETE]=         CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  sql_command_flags[SQLCOM_DELETE_MULTI]=   CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  sql_command_flags[SQLCOM_REPLACE]=        CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  sql_command_flags[SQLCOM_REPLACE_SELECT]= CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  sql_command_flags[SQLCOM_SELECT]=         CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_HAS_RESULT_SET |
                                            CF_CAN_BE_EXPLAINED;
  // (1) so that subquery is traced when doing "SET @var = (subquery)"
  /*
    @todo SQLCOM_SET_OPTION should have CF_CAN_GENERATE_ROW_EVENTS
    set, because it may invoke a stored function that generates row
    events. /Sven
  */
  sql_command_flags[SQLCOM_SET_OPTION]=     CF_REEXECUTION_FRAGILE |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE; // (1)
  // (1) so that subquery is traced when doing "DO @var := (subquery)"
  sql_command_flags[SQLCOM_DO]=             CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE; // (1)

  sql_command_flags[SQLCOM_SET_PASSWORD]=   CF_CHANGES_DATA |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_NEEDS_AUTOCOMMIT_OFF |
                                            CF_POTENTIAL_ATOMIC_DDL |
                                            CF_DISALLOW_IN_RO_TRANS;

  sql_command_flags[SQLCOM_SHOW_STATUS_PROC]= CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_STATUS]=      CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_DATABASES]=   CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_TRIGGERS]=    CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_EVENTS]=      CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_OPEN_TABLES]= CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_PLUGINS]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_FIELDS]=      CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_KEYS]=        CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_VARIABLES]=   CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_CHARSETS]=    CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_COLLATIONS]=  CF_STATUS_COMMAND |
                                              CF_HAS_RESULT_SET |
                                              CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_BINLOGS]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_SLAVE_HOSTS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_BINLOG_EVENTS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_STORAGE_ENGINES]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PRIVILEGES]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_WARNS]=       CF_STATUS_COMMAND | CF_DIAGNOSTIC_STMT;
  sql_command_flags[SQLCOM_SHOW_ERRORS]=      CF_STATUS_COMMAND | CF_DIAGNOSTIC_STMT;
  sql_command_flags[SQLCOM_SHOW_ENGINE_STATUS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_ENGINE_MUTEX]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_ENGINE_LOGS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PROCESSLIST]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_GRANTS]=      CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_DB]=   CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_MASTER_STAT]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_SLAVE_STAT]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_PROC]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_FUNC]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_TRIGGER]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_STATUS_FUNC]= CF_STATUS_COMMAND |
                                              CF_REEXECUTION_FRAGILE |
                                              CF_HAS_RESULT_SET;
  sql_command_flags[SQLCOM_SHOW_PROC_CODE]=   CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_FUNC_CODE]=   CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_EVENT]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PROFILES]=    CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PROFILE]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_BINLOG_BASE64_EVENT]= CF_STATUS_COMMAND |
                                                 CF_CAN_GENERATE_ROW_EVENTS;

   sql_command_flags[SQLCOM_SHOW_TABLES]=       (CF_STATUS_COMMAND |
                                                 CF_SHOW_TABLE_COMMAND |
                                                 CF_HAS_RESULT_SET |
                                                 CF_REEXECUTION_FRAGILE);
  sql_command_flags[SQLCOM_SHOW_TABLE_STATUS]= (CF_STATUS_COMMAND |
                                                CF_SHOW_TABLE_COMMAND |
                                                CF_HAS_RESULT_SET |
                                                CF_REEXECUTION_FRAGILE);
  /**
    ACL DDLs do not access data-dictionary tables. However, they still
    need to be marked to avoid autocommit. This is necessary because
    code which saves GTID state or slave state in the system tables
    at commit time does statement commit on low-level (see
    System_table_access::close_table()) and thus can pre-maturely commit
    DDL otherwise.
  */
  sql_command_flags[SQLCOM_CREATE_USER]=       CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL  |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_RENAME_USER]=       CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_USER]=         CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_USER]=        CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_GRANT]=             CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_REVOKE]=            CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_REVOKE_ALL]=        CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_USER_DEFAULT_ROLE]=
                                               CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_GRANT_ROLE]=        CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_REVOKE_ROLE]=       CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_ROLE]=         CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_ROLE]=       CF_CHANGES_DATA |
                                               CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL |
                                               CF_ACQUIRE_BACKUP_LOCK;

  sql_command_flags[SQLCOM_OPTIMIZE]=          CF_CHANGES_DATA |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_INSTANCE]=    CF_CHANGES_DATA |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_FUNCTION]=   CF_CHANGES_DATA |
                                               CF_AUTO_COMMIT_TRANS |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_PROCEDURE]=  CF_CHANGES_DATA |
                                               CF_AUTO_COMMIT_TRANS |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_SPFUNCTION]= CF_CHANGES_DATA |
                                               CF_AUTO_COMMIT_TRANS |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_PROCEDURE]=    CF_CHANGES_DATA |
                                               CF_AUTO_COMMIT_TRANS |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_FUNCTION]=     CF_CHANGES_DATA |
                                               CF_AUTO_COMMIT_TRANS |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_PROCEDURE]=   CF_CHANGES_DATA |
                                               CF_AUTO_COMMIT_TRANS |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_FUNCTION]=    CF_CHANGES_DATA |
                                               CF_AUTO_COMMIT_TRANS |
                                               CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_INSTALL_PLUGIN]=    CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_UNINSTALL_PLUGIN]=  CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_INSTALL_COMPONENT]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_UNINSTALL_COMPONENT]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_RESOURCE_GROUP]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_RESOURCE_GROUP]=  CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_RESOURCE_GROUP]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_SET_RESOURCE_GROUP]=    CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;

  sql_command_flags[SQLCOM_CLONE]= CF_AUTO_COMMIT_TRANS |
                                   CF_ALLOW_PROTOCOL_PLUGIN;

  /* Does not change the contents of the Diagnostics Area. */
  sql_command_flags[SQLCOM_GET_DIAGNOSTICS]= CF_DIAGNOSTIC_STMT;

  /*
    (1): without it, in "CALL some_proc((subq))", subquery would not be
    traced.
  */
  sql_command_flags[SQLCOM_CALL]=      CF_REEXECUTION_FRAGILE |
                                       CF_CAN_GENERATE_ROW_EVENTS |
                                       CF_OPTIMIZER_TRACE; // (1)
  sql_command_flags[SQLCOM_EXECUTE]=   CF_CAN_GENERATE_ROW_EVENTS;

  /*
    The following admin table operations are allowed
    on log tables.
  */
  sql_command_flags[SQLCOM_REPAIR]=    CF_WRITE_LOGS_COMMAND |
                                       CF_AUTO_COMMIT_TRANS |
                                       CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_OPTIMIZE]|= CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ANALYZE]=   CF_WRITE_LOGS_COMMAND |
                                       CF_AUTO_COMMIT_TRANS |
                                       CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CHECK]=     CF_WRITE_LOGS_COMMAND |
                                       CF_AUTO_COMMIT_TRANS |
                                       CF_ACQUIRE_BACKUP_LOCK;

  sql_command_flags[SQLCOM_CREATE_USER]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_ROLE]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_USER]|=         CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_ROLE]|=         CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_RENAME_USER]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_USER]|=        CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_RESTART_SERVER]=     CF_AUTO_COMMIT_TRANS |
                                                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_REVOKE]|=            CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_REVOKE_ALL]|=        CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_REVOKE_ROLE]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_GRANT]|=             CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_GRANT_ROLE]|=        CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_USER_DEFAULT_ROLE]|=  CF_AUTO_COMMIT_TRANS;

  sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE]= CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_PRELOAD_KEYS]=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_INSTANCE]|=    CF_AUTO_COMMIT_TRANS;

  sql_command_flags[SQLCOM_FLUSH]=              CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_RESET]=              CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_SERVER]=      CF_AUTO_COMMIT_TRANS |
                                                CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_ALTER_SERVER]=       CF_AUTO_COMMIT_TRANS |
                                                CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_SERVER]=        CF_AUTO_COMMIT_TRANS |
                                                CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CHANGE_MASTER]=      CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CHANGE_REPLICATION_FILTER]=    CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_SLAVE_START]=        CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_SLAVE_STOP]=         CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_TABLESPACE]|=  CF_AUTO_COMMIT_TRANS |
                                                CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_CREATE_SRS]|=        CF_AUTO_COMMIT_TRANS |
                                                CF_ACQUIRE_BACKUP_LOCK;
  sql_command_flags[SQLCOM_DROP_SRS]|=          CF_AUTO_COMMIT_TRANS |
                                                CF_ACQUIRE_BACKUP_LOCK;

  /*
    The following statements can deal with temporary tables,
    so temporary tables should be pre-opened for those statements to
    simplify privilege checking.

    There are other statements that deal with temporary tables and open
    them, but which are not listed here. The thing is that the order of
    pre-opening temporary tables for those statements is somewhat custom.
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DROP_TABLE]|=      CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CREATE_INDEX]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=     CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_TRUNCATE]|=        CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_LOAD]|=            CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DROP_INDEX]|=      CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_UPDATE]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_UPDATE_MULTI]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_INSERT]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_INSERT_SELECT]|=   CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DELETE]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DELETE_MULTI]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_REPLACE]|=         CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_REPLACE_SELECT]|=  CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_SELECT]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_SET_OPTION]|=      CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DO]|=              CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CALL]|=            CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CHECKSUM]|=        CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_ANALYZE]|=         CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CHECK]|=           CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_OPTIMIZE]|=        CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_REPAIR]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_PRELOAD_KEYS]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE]|= CF_PREOPEN_TMP_TABLES;

  /*
    DDL statements that should start with closing opened handlers.

    We use this flag only for statements for which open HANDLERs
    have to be closed before emporary tables are pre-opened.
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]|=    CF_HA_CLOSE;
  sql_command_flags[SQLCOM_DROP_TABLE]|=      CF_HA_CLOSE;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=     CF_HA_CLOSE;
  sql_command_flags[SQLCOM_TRUNCATE]|=        CF_HA_CLOSE;
  sql_command_flags[SQLCOM_REPAIR]|=          CF_HA_CLOSE;
  sql_command_flags[SQLCOM_OPTIMIZE]|=        CF_HA_CLOSE;
  sql_command_flags[SQLCOM_ANALYZE]|=         CF_HA_CLOSE;
  sql_command_flags[SQLCOM_CHECK]|=           CF_HA_CLOSE;
  sql_command_flags[SQLCOM_CREATE_INDEX]|=    CF_HA_CLOSE;
  sql_command_flags[SQLCOM_DROP_INDEX]|=      CF_HA_CLOSE;
  sql_command_flags[SQLCOM_PRELOAD_KEYS]|=    CF_HA_CLOSE;
  sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE]|=  CF_HA_CLOSE;

  /*
    Mark statements that always are disallowed in read-only
    transactions. Note that according to the SQL standard,
    even temporary table DDL should be disallowed.
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_TABLE]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_RENAME_TABLE]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_INDEX]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_INDEX]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_DB]|=        CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_DB]|=          CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_DB]|=         CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_VIEW]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_VIEW]|=        CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_TRIGGER]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_TRIGGER]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_EVENT]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_EVENT]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_EVENT]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_USER]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_ROLE]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_RENAME_USER]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_USER]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_USER]|=        CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_ROLE]|=        CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_SERVER]|=    CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_SERVER]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_SERVER]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_FUNCTION]|=  CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_PROCEDURE]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_SPFUNCTION]|=CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_PROCEDURE]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_FUNCTION]|=    CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_PROCEDURE]|=  CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_FUNCTION]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_TRUNCATE]|=         CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_TABLESPACE]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_REPAIR]|=           CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_OPTIMIZE]|=         CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_GRANT]|=            CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_GRANT_ROLE]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_REVOKE]|=           CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_REVOKE_ALL]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_REVOKE_ROLE]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_INSTALL_PLUGIN]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_UNINSTALL_PLUGIN]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_INSTALL_COMPONENT]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_UNINSTALL_COMPONENT]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_INSTANCE]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_IMPORT]|=           CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_SRS]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_SRS]|=         CF_DISALLOW_IN_RO_TRANS;

  /*
    Mark statements that are allowed to be executed by the plugins.
  */
  sql_command_flags[SQLCOM_SELECT]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_TABLE]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_INDEX]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_UPDATE]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_INSERT]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_INSERT_SELECT]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DELETE]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_TRUNCATE]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_TABLE]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_INDEX]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_DATABASES]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_TABLES]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_FIELDS]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_KEYS]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_VARIABLES]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_STATUS]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_ENGINE_LOGS]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_ENGINE_STATUS]|=      CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_ENGINE_MUTEX]|=       CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_PROCESSLIST]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_MASTER_STAT]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_SLAVE_STAT]|=         CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_GRANTS]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CREATE]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CHARSETS]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_COLLATIONS]|=         CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CREATE_DB]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_TABLE_STATUS]|=       CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_TRIGGERS]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_LOAD]|=                    CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SET_OPTION]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_LOCK_TABLES]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_UNLOCK_TABLES]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_GRANT]|=                   CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CHANGE_DB]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_DB]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_DB]|=                 CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_DB]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_REPAIR]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_REPLACE]|=                 CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_REPLACE_SELECT]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_FUNCTION]|=         CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_FUNCTION]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_REVOKE]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_OPTIMIZE]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CHECK]|=                   CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE]|=      CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_PRELOAD_KEYS]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_FLUSH]|=                   CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_KILL]|=                    CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ANALYZE]|=                 CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ROLLBACK]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ROLLBACK_TO_SAVEPOINT]|=   CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_COMMIT]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SAVEPOINT]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_RELEASE_SAVEPOINT]|=       CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SLAVE_START]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SLAVE_STOP]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_START_GROUP_REPLICATION]|= CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_STOP_GROUP_REPLICATION]|=  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_BEGIN]|=                   CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CHANGE_MASTER]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CHANGE_REPLICATION_FILTER]|= CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_RENAME_TABLE]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_RESET]|=                   CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_PURGE]|=                   CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_PURGE_BEFORE]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_BINLOGS]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_OPEN_TABLES]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_HA_OPEN]|=                 CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_HA_CLOSE]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_HA_READ]|=                 CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_SLAVE_HOSTS]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DELETE_MULTI]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_UPDATE_MULTI]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_BINLOG_EVENTS]|=      CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DO]|=                      CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_WARNS]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_EMPTY_QUERY]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_ERRORS]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_STORAGE_ENGINES]|=    CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_PRIVILEGES]|=         CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_HELP]|=                    CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_USER]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_USER]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_RENAME_USER]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_REVOKE_ALL]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CHECKSUM]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_PROCEDURE]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_SPFUNCTION]|=       CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CALL]|=                    CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_PROCEDURE]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_PROCEDURE]|=         CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_FUNCTION]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CREATE_PROC]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CREATE_FUNC]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_STATUS_PROC]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_STATUS_FUNC]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_PREPARE]|=                 CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_EXECUTE]|=                 CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DEALLOCATE_PREPARE]|=      CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_VIEW]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_VIEW]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_TRIGGER]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_TRIGGER]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_XA_START]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_XA_END]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_XA_PREPARE]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_XA_COMMIT]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_XA_ROLLBACK]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_XA_RECOVER]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_PROC_CODE]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_FUNC_CODE]|=          CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_TABLESPACE]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_BINLOG_BASE64_EVENT]|=     CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_PLUGINS]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_SERVER]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_SERVER]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_SERVER]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_EVENT]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_EVENT]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_EVENT]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CREATE_EVENT]|=       CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_EVENTS]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CREATE_TRIGGER]|=     CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_PROFILE]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_PROFILES]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SIGNAL]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_RESIGNAL]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_RELAYLOG_EVENTS]|=    CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_GET_DIAGNOSTICS]|=         CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_USER]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_EXPLAIN_OTHER]|=           CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SHOW_CREATE_USER]|=        CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SET_PASSWORD]|=            CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_ROLE]|=               CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_ROLE]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_SET_ROLE]|=                CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_GRANT_ROLE]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_REVOKE_ROLE]|=             CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_ALTER_USER_DEFAULT_ROLE]|= CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_IMPORT]|=                  CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_END]|=                     CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_CREATE_SRS]|=              CF_ALLOW_PROTOCOL_PLUGIN;
  sql_command_flags[SQLCOM_DROP_SRS]|=                CF_ALLOW_PROTOCOL_PLUGIN;

  /*
    Mark DDL statements which require that auto-commit mode to be temporarily
    turned off. See sqlcom_needs_autocommit_off() for more details.

    CREATE TABLE and DROP TABLE are not marked as such as they have special
    variants dealing with temporary tables which don't update data-dictionary
    at all and which should be allowed in the middle of transaction.
  */
  sql_command_flags[SQLCOM_CREATE_INDEX]|=     CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=      CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_TRUNCATE]|=         CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_INDEX]|=       CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_CREATE_DB]|=        CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_DB]|=          CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_ALTER_DB]|=         CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_REPAIR]|=           CF_NEEDS_AUTOCOMMIT_OFF;
  sql_command_flags[SQLCOM_OPTIMIZE]|=         CF_NEEDS_AUTOCOMMIT_OFF;
  sql_command_flags[SQLCOM_RENAME_TABLE]|=     CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_CREATE_VIEW]|=      CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_VIEW]|=        CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_ALTER_TABLESPACE]|= CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_CREATE_SPFUNCTION]|= CF_NEEDS_AUTOCOMMIT_OFF |
                                                CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_FUNCTION]|=     CF_NEEDS_AUTOCOMMIT_OFF |
                                                CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_ALTER_FUNCTION]|=    CF_NEEDS_AUTOCOMMIT_OFF |
                                                CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_CREATE_FUNCTION]|=   CF_NEEDS_AUTOCOMMIT_OFF |
                                                CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_CREATE_PROCEDURE]|=  CF_NEEDS_AUTOCOMMIT_OFF |
                                                CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_PROCEDURE]|=    CF_NEEDS_AUTOCOMMIT_OFF |
                                                CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_ALTER_PROCEDURE]|=   CF_NEEDS_AUTOCOMMIT_OFF |
                                                CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_CREATE_TRIGGER]|=   CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_TRIGGER]|=     CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_IMPORT]|=           CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_INSTALL_PLUGIN]|=   CF_NEEDS_AUTOCOMMIT_OFF;
  sql_command_flags[SQLCOM_UNINSTALL_PLUGIN]|= CF_NEEDS_AUTOCOMMIT_OFF;
  sql_command_flags[SQLCOM_CREATE_EVENT]|=     CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_ALTER_EVENT]|=      CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_EVENT]|=       CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_CREATE_SRS]|=       CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
  sql_command_flags[SQLCOM_DROP_SRS]|=         CF_NEEDS_AUTOCOMMIT_OFF |
                                               CF_POTENTIAL_ATOMIC_DDL;
}

bool sqlcom_can_generate_row_events(enum enum_sql_command command)
{
  return (sql_command_flags[command] & CF_CAN_GENERATE_ROW_EVENTS);
}

bool is_update_query(enum enum_sql_command command)
{
  DBUG_ASSERT(command >= 0 && command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_CHANGES_DATA) != 0;
}


bool is_explainable_query(enum enum_sql_command command)
{
  DBUG_ASSERT(command >= 0 && command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_CAN_BE_EXPLAINED) != 0;
}

/**
  Check if a sql command is allowed to write to log tables.
  @param command The SQL command
  @return true if writing is allowed
*/
bool is_log_table_write_query(enum enum_sql_command command)
{
  DBUG_ASSERT(command >= 0 && command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_WRITE_LOGS_COMMAND) != 0;
}


/**
  Check if statement (typically DDL) needs auto-commit mode temporarily
  turned off.

  @note This is necessary to prevent InnoDB from automatically committing
        InnoDB transaction each time data-dictionary tables are closed
        after being updated.
*/
static bool sqlcom_needs_autocommit_off(const LEX *lex)
{
  return (sql_command_flags[lex->sql_command] & CF_NEEDS_AUTOCOMMIT_OFF) ||
          (lex->sql_command == SQLCOM_CREATE_TABLE &&
           ! (lex->create_info->options & HA_LEX_CREATE_TMP_TABLE)) ||
          (lex->sql_command == SQLCOM_DROP_TABLE && ! lex->drop_temporary);
}


void execute_init_command(THD *thd, LEX_STRING *init_command,
                          mysql_rwlock_t *var_lock)
{
  Protocol_classic *protocol= thd->get_protocol_classic();
  Vio* save_vio;
  ulong save_client_capabilities;
  COM_DATA com_data;

  mysql_rwlock_rdlock(var_lock);
  if (!init_command->length)
  {
    mysql_rwlock_unlock(var_lock);
    return;
  }

  /*
    copy the value under a lock, and release the lock.
    init_command has to be executed without a lock held,
    as it may try to change itself
  */
  size_t len= init_command->length;
  char *buf= thd->strmake(init_command->str, len);
  mysql_rwlock_unlock(var_lock);

#if defined(ENABLED_PROFILING)
  thd->profiling->start_new_query();
  thd->profiling->set_query_source(buf, len);
#endif

  THD_STAGE_INFO(thd, stage_execution_of_init_command);
  save_client_capabilities= protocol->get_client_capabilities();
  protocol->add_client_capability(CLIENT_MULTI_QUERIES);
  /*
    We don't need return result of execution to client side.
    To forbid this we should set thd->net.vio to 0.
  */
  save_vio= protocol->get_vio();
  protocol->set_vio(NULL);
  protocol->create_command(&com_data, COM_QUERY, (uchar *) buf, len);
  dispatch_command(thd, &com_data, COM_QUERY);
  protocol->set_client_capabilities(save_client_capabilities);
  protocol->set_vio(save_vio);

#if defined(ENABLED_PROFILING)
  thd->profiling->finish_current_query();
#endif
}


/* This works because items are allocated with sql_alloc() */

void free_items(Item *item)
{
  Item *next;
  DBUG_ENTER("free_items");
  for (; item ; item=next)
  {
    next=item->next;
    item->delete_self();
  }
  DBUG_VOID_RETURN;
}

/**
   This works because items are allocated with sql_alloc().
   @note The function also handles null pointers (empty list).
*/
void cleanup_items(Item *item)
{
  DBUG_ENTER("cleanup_items");  
  for (; item ; item=item->next)
    item->cleanup();
  DBUG_VOID_RETURN;
}


/**
  Read one command from connection and execute it (query or simple command).
  This function is called in loop from thread function.

  For profiling to work, it must never be called recursively.

  @retval
    0  success
  @retval
    1  request of thread shutdown (see dispatch_command() description)
*/

bool do_command(THD *thd)
{
  bool return_value;
  int rc;
  NET *net= NULL;
  enum enum_server_command command;
  COM_DATA com_data;
  DBUG_ENTER("do_command");
  DBUG_ASSERT(thd->is_classic_protocol());

  /*
    indicator of uninitialized lex => normal flow of errors handling
    (see my_message_sql)
  */
  thd->lex->set_current_select(0);

  /*
    XXX: this code is here only to clear possible errors of init_connect. 
    Consider moving to prepare_new_connection_state() instead.
    That requires making sure the DA is cleared before non-parsing statements
    such as COM_QUIT.
  */
  thd->clear_error();				// Clear error message
  thd->get_stmt_da()->reset_diagnostics_area();

  /*
    This thread will do a blocking read from the client which
    will be interrupted when the next command is received from
    the client, the connection is closed or "net_wait_timeout"
    number of seconds has passed.
  */
  net= thd->get_protocol_classic()->get_net();
  my_net_set_read_timeout(net, thd->variables.net_wait_timeout);
  net_new_transaction(net);

  /*
    Synchronization point for testing of KILL_CONNECTION.
    This sync point can wait here, to simulate slow code execution
    between the last test of thd->killed and blocking in read().

    The goal of this test is to verify that a connection does not
    hang, if it is killed at this point of execution.
    (Bug#37780 - main.kill fails randomly)

    Note that the sync point wait itself will be terminated by a
    kill. In this case it consumes a condition broadcast, but does
    not change anything else. The consumed broadcast should not
    matter here, because the read/recv() below doesn't use it.
  */
  DEBUG_SYNC(thd, "before_do_command_net_read");

  /*
    Because of networking layer callbacks in place,
    this call will maintain the following instrumentation:
    - IDLE events
    - SOCKET events
    - STATEMENT events
    - STAGE events
    when reading a new network packet.
    In particular, a new instrumented statement is started.
    See init_net_server_extension()
  */
  thd->m_server_idle= true;
  rc= thd->get_protocol()->get_command(&com_data, &command);
  thd->m_server_idle= false;

  if (rc)
  {
    char desc[VIO_DESCRIPTION_SIZE];
    vio_description(net->vio, desc);
    DBUG_PRINT("info",("Got error %d reading command from socket %s",
                       net->error, desc));
    /* Instrument this broken statement as "statement/com/error" */
    thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                                 com_statement_info[COM_END].m_key);

    /* Check if we can continue without closing the connection */

    /* The error must be set. */
    DBUG_ASSERT(thd->is_error());
    thd->send_statement_status();

    /* Mark the statement completed. */
    MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
    thd->m_statement_psi= NULL;
    thd->m_digest= NULL;

    if (rc < 0)
    {
      return_value= true;                       // We have to close it.
      goto out;
    }
    net->error= 0;
    return_value= false;
    goto out;
  }

  char desc[VIO_DESCRIPTION_SIZE];
  vio_description(net->vio, desc);
  DBUG_PRINT("info",("Command on %s = %d (%s)",
                     desc, command,
                     command_name[command].str));

  DBUG_PRINT("info", ("packet: '%*.s'; command: %d",
             thd->get_protocol_classic()->get_packet_length(),
             thd->get_protocol_classic()->get_raw_packet(), command));
  if (thd->get_protocol_classic()->bad_packet)
    DBUG_ASSERT(0);                // Should be caught earlier

  // Reclaim some memory
  thd->get_protocol_classic()->get_output_packet()->
     shrink(thd->variables.net_buffer_length);
  /* Restore read timeout value */
  my_net_set_read_timeout(net, thd->variables.net_read_timeout);

  return_value= dispatch_command(thd, &com_data, command);
  thd->get_protocol_classic()->get_output_packet()->
    shrink(thd->variables.net_buffer_length);

out:
  /* The statement instrumentation must be closed in all cases. */
  DBUG_ASSERT(thd->m_digest == NULL);
  DBUG_ASSERT(thd->m_statement_psi == NULL);
  DBUG_RETURN(return_value);
}


/**
  @brief Determine if an attempt to update a non-temporary table while the
    read-only option was enabled has been made.

  This is a helper function to mysql_execute_command.

  @note SQLCOM_UPDATE_MULTI is an exception and delt with elsewhere.

  @see mysql_execute_command
  @returns Status code
    @retval true The statement should be denied.
    @retval false The statement isn't updating any relevant tables.
*/
static bool deny_updates_if_read_only_option(THD *thd,
                                             TABLE_LIST *all_tables)
{
  DBUG_ENTER("deny_updates_if_read_only_option");

  if (!check_readonly(thd, false))
    DBUG_RETURN(false);

  LEX *lex = thd->lex;
  if (!(sql_command_flags[lex->sql_command] & CF_CHANGES_DATA))
    DBUG_RETURN(false);

  /* Multi update is an exception and is dealt with later. */
  if (lex->sql_command == SQLCOM_UPDATE_MULTI)
    DBUG_RETURN(false);

  const bool create_temp_tables= 
    (lex->sql_command == SQLCOM_CREATE_TABLE) &&
    (lex->create_info->options & HA_LEX_CREATE_TMP_TABLE);

   const bool create_real_tables=
     (lex->sql_command == SQLCOM_CREATE_TABLE) &&
     !(lex->create_info->options & HA_LEX_CREATE_TMP_TABLE);

  const bool drop_temp_tables= 
    (lex->sql_command == SQLCOM_DROP_TABLE) &&
    lex->drop_temporary;

  const bool update_real_tables=
    ((create_real_tables ||
      some_non_temp_table_to_be_updated(thd, all_tables)) &&
     !(create_temp_tables || drop_temp_tables));

  const bool create_or_drop_databases=
    (lex->sql_command == SQLCOM_CREATE_DB) ||
    (lex->sql_command == SQLCOM_DROP_DB);

  if (update_real_tables || create_or_drop_databases)
  {
      /*
        An attempt was made to modify one or more non-temporary tables.
      */
      DBUG_RETURN(true);
  }


  /* Assuming that only temporary tables are modified. */
  DBUG_RETURN(false);
}


/**
  Check whether max statement time is applicable to statement or not.


  @param  thd   Thread (session) context.

  @return true  if max statement time is applicable to statement
  @return false otherwise.
*/
static inline bool is_timer_applicable_to_statement(THD *thd)
{
  bool timer_value_is_set= (thd->lex->max_execution_time ||
                            thd->variables.max_execution_time);

  /**
    Following conditions are checked,
      - is SELECT statement.
      - timer support is implemented and it is initialized.
      - statement is not made by the slave threads.
      - timer is not set for statement
      - timer out value of is set
      - SELECT statement is not from any stored programs.
  */
  return (thd->lex->sql_command == SQLCOM_SELECT &&
          (have_statement_timeout == SHOW_OPTION_YES) &&
          !thd->slave_thread &&
          !thd->timer && timer_value_is_set &&
          !thd->sp_runtime_ctx);
}


/**
  Perform one connection-level (COM_XXXX) command.

  @param thd             connection handle
  @param command         type of command to perform
  @param com_data        com_data union to store the generated command

  @todo
    set thd->lex->sql_command to SQLCOM_END here.
  @todo
    The following has to be changed to an 8 byte integer

  @retval
    0   ok
  @retval
    1   request of thread shutdown, i. e. if command is
        COM_QUIT
*/
bool dispatch_command(THD *thd, const COM_DATA *com_data,
                      enum enum_server_command command)
{
  bool error= 0;
  Global_THD_manager *thd_manager= Global_THD_manager::get_instance();
  DBUG_ENTER("dispatch_command");
  DBUG_PRINT("info", ("command: %d", command));

  /* SHOW PROFILE instrumentation, begin */
#if defined(ENABLED_PROFILING)
  thd->profiling->start_new_query();
#endif

  /* Performance Schema Interface instrumentation, begin */
  thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                               com_statement_info[command].m_key);

  thd->set_command(command);
  /*
    Commands which always take a long time are logged into
    the slow log only if opt_log_slow_admin_statements is set.
  */
  thd->enable_slow_log= true;
  thd->lex->sql_command= SQLCOM_END; /* to avoid confusing VIEW detectors */
  thd->set_time();
  if (IS_TIME_T_VALID_FOR_TIMESTAMP(thd->query_start_in_secs()) == false)
  {
    /*
      If the time has gone past 2038 we need to shutdown the server. But
      there is possibility of getting invalid time value on some platforms.
      For example, gettimeofday() might return incorrect value on solaris
      platform. Hence validating the current time with 5 iterations before
      initiating the normal server shutdown process because of time getting
      past 2038.
    */
    const int max_tries= 5;
    LogErr(WARNING_LEVEL, ER_CONFIRMING_THE_FUTURE, max_tries);

    int tries= 0;
    while (++tries <= max_tries)
    {
      thd->set_time();
      if (IS_TIME_T_VALID_FOR_TIMESTAMP(thd->query_start_in_secs()) == true)
      {
        LogErr(WARNING_LEVEL, ER_BACK_IN_TIME, tries);
        break;
      }
      LogErr(WARNING_LEVEL, ER_FUTURE_DATE, tries);
    }
    if (tries > max_tries)
    {
      /*
        If the time has got past 2038 we need to shut this server down
        We do this by making sure every command is a shutdown and we
        have enough privileges to shut the server down

        TODO: remove this when we have full 64 bit my_time_t support
      */
      LogErr(ERROR_LEVEL, ER_UNSUPPORTED_DATE);
      ulong master_access= thd->security_context()->master_access();
      thd->security_context()->set_master_access(master_access | SHUTDOWN_ACL);
      error= true;
      kill_mysql();
    }
  }
  thd->set_query_id(next_query_id());
  thd->rewritten_query.mem_free();
  thd_manager->inc_thread_running();

  if (!(server_command_flags[command] & CF_SKIP_QUESTIONS))
    thd->status_var.questions++;

  /**
    Clear the set of flags that are expected to be cleared at the
    beginning of each command.
  */
  thd->server_status&= ~SERVER_STATUS_CLEAR_SET;

  if (thd->get_protocol()->type() == Protocol::PROTOCOL_PLUGIN &&
      !(server_command_flags[command] & CF_ALLOW_PROTOCOL_PLUGIN))
  {
    my_error(ER_PLUGGABLE_PROTOCOL_COMMAND_NOT_SUPPORTED, MYF(0));
    thd->killed= THD::KILL_CONNECTION;
    error= true;
    goto done;
  }

  /**
    Enforce password expiration for all RPC commands, except the
    following:

    COM_QUERY/COM_STMT_PREPARE and COM_STMT_EXECUTE do a more
    fine-grained check later.
    COM_STMT_CLOSE and COM_STMT_SEND_LONG_DATA don't return anything.
    COM_PING only discloses information that the server is running,
       and that's available through other means.
    COM_QUIT should work even for expired statements.
  */
  if (unlikely(thd->security_context()->password_expired() &&
               command != COM_QUERY &&
               command != COM_STMT_CLOSE &&
               command != COM_STMT_SEND_LONG_DATA &&
               command != COM_PING &&
               command != COM_QUIT &&
               command != COM_STMT_PREPARE &&
               command != COM_STMT_EXECUTE))
  {
    my_error(ER_MUST_CHANGE_PASSWORD, MYF(0));
    goto done;
  }

  if (mysql_audit_notify(thd,
                         AUDIT_EVENT(MYSQL_AUDIT_COMMAND_START),
                         command, command_name[command].str))
  {
    goto done;
  }

  switch (command) {
  case COM_INIT_DB:
  {
    LEX_STRING tmp;
    thd->status_var.com_stat[SQLCOM_CHANGE_DB]++;
    thd->convert_string(&tmp, system_charset_info,
                        com_data->com_init_db.db_name,
                        com_data->com_init_db.length, thd->charset());

    LEX_CSTRING tmp_cstr= {tmp.str, tmp.length};
    if (!mysql_change_db(thd, tmp_cstr, false))
    {
      query_logger.general_log_write(thd, command,
                                     thd->db().str, thd->db().length);
      my_ok(thd);
    }
    break;
  }
  case COM_REGISTER_SLAVE:
  {
    // TODO: access of protocol_classic should be removed
    if (!register_slave(thd,
      thd->get_protocol_classic()->get_raw_packet(),
      thd->get_protocol_classic()->get_packet_length()))
      my_ok(thd);
    break;
  }
  case COM_RESET_CONNECTION:
  {
    thd->status_var.com_other++;
    thd->cleanup_connection();
    my_ok(thd);
    break;
  }
  case COM_CHANGE_USER:
  {
    int auth_rc;
    thd->status_var.com_other++;

    thd->cleanup_connection();
    USER_CONN *save_user_connect=
      const_cast<USER_CONN*>(thd->get_user_connect());
    LEX_CSTRING save_db= thd->db();
    Security_context save_security_ctx(*(thd->security_context()));

    auth_rc= acl_authenticate(thd, COM_CHANGE_USER);
    auth_rc|= mysql_audit_notify(thd,
                             AUDIT_EVENT(MYSQL_AUDIT_CONNECTION_CHANGE_USER));
    if (auth_rc)
    {
      *thd->security_context()= save_security_ctx;
      thd->set_user_connect(save_user_connect);
      thd->reset_db(save_db);

      my_error(ER_ACCESS_DENIED_CHANGE_USER_ERROR, MYF(0),
               thd->security_context()->user().str,
               thd->security_context()->host_or_ip().str,
               (thd->password ? ER_THD(thd, ER_YES) : ER_THD(thd, ER_NO)));
      thd->killed= THD::KILL_CONNECTION;
      error=true;
    }
    else
    {
#ifdef HAVE_PSI_THREAD_INTERFACE
      /* we've authenticated new user */
      PSI_THREAD_CALL(notify_session_change_user)(thd->get_psi());
#endif /* HAVE_PSI_THREAD_INTERFACE */

      if (save_user_connect)
        decrease_user_connections(save_user_connect);
      mysql_mutex_lock(&thd->LOCK_thd_data);
      my_free(const_cast<char*>(save_db.str));
      save_db= NULL_CSTR;
      mysql_mutex_unlock(&thd->LOCK_thd_data);
    }
    break;
  }
  case COM_STMT_EXECUTE:
  {
    /* Clear possible warnings from the previous command */
    thd->reset_for_next_command();

    Prepared_statement *stmt= nullptr;
    if (!mysql_stmt_precheck(thd, com_data, command, &stmt))
    {
      PS_PARAM *parameters= com_data->com_stmt_execute.parameters;
      mysqld_stmt_execute(thd, stmt, com_data->com_stmt_execute.has_new_types,
                          com_data->com_stmt_execute.open_cursor, parameters);
    }
    break;
  }
  case COM_STMT_FETCH:
  {
    /* Clear possible warnings from the previous command */
    thd->reset_for_next_command();

    Prepared_statement *stmt= nullptr;
    if (!mysql_stmt_precheck(thd, com_data, command, &stmt))
      mysqld_stmt_fetch(thd, stmt, com_data->com_stmt_fetch.num_rows);

    break;
  }
  case COM_STMT_SEND_LONG_DATA:
  {
    Prepared_statement *stmt;
    thd->get_stmt_da()->disable_status();
    if (!mysql_stmt_precheck(thd, com_data, command, &stmt))
      mysql_stmt_get_longdata(thd, stmt,
                              com_data->com_stmt_send_long_data.param_number,
                              com_data->com_stmt_send_long_data.longdata,
                              com_data->com_stmt_send_long_data.length);
    break;
  }
  case COM_STMT_PREPARE:
  {
    /* Clear possible warnings from the previous command */
    thd->reset_for_next_command();
    Prepared_statement *stmt= nullptr;

    DBUG_EXECUTE_IF("parser_stmt_to_error_log", {
      LogErr(INFORMATION_LEVEL, ER_PARSER_TRACE,
             com_data->com_stmt_prepare.query);
    });
    DBUG_EXECUTE_IF("parser_stmt_to_error_log_with_system_prio", {
      LogErr(SYSTEM_LEVEL, ER_PARSER_TRACE,
             com_data->com_stmt_prepare.query);
    });

    if (!mysql_stmt_precheck(thd, com_data, command, &stmt))
      mysqld_stmt_prepare(thd, com_data->com_stmt_prepare.query,
                          com_data->com_stmt_prepare.length, stmt);
    break;
  }
  case COM_STMT_CLOSE:
  {
    Prepared_statement *stmt= nullptr;
    thd->get_stmt_da()->disable_status();
    if (!mysql_stmt_precheck(thd, com_data, command, &stmt))
      mysqld_stmt_close(thd, stmt);
    break;
  }
  case COM_STMT_RESET:
  {
    /* Clear possible warnings from the previous command */
    thd->reset_for_next_command();

    Prepared_statement *stmt= nullptr;
    if (!mysql_stmt_precheck(thd, com_data, command, &stmt))
      mysqld_stmt_reset(thd, stmt);
    break;
  }
  case COM_QUERY:
  {
    DBUG_ASSERT(thd->m_digest == NULL);
    thd->m_digest= & thd->m_digest_state;
    thd->m_digest->reset(thd->m_token_array, max_digest_length);

    if (alloc_query(thd, com_data->com_query.query,
                    com_data->com_query.length))
      break;					// fatal error is set

    const char *packet_end= thd->query().str + thd->query().length;

    if (opt_general_log_raw)
      query_logger.general_log_write(thd, command, thd->query().str,
                                     thd->query().length);

    DBUG_PRINT("query",("%-.4096s", thd->query().str));

#if defined(ENABLED_PROFILING)
    thd->profiling->set_query_source(thd->query().str, thd->query().length);
#endif

    Parser_state parser_state;
    if (parser_state.init(thd, thd->query().str, thd->query().length))
      break;

    mysql_parse(thd, &parser_state);

    DBUG_EXECUTE_IF("parser_stmt_to_error_log", {
        LogErr(INFORMATION_LEVEL, ER_PARSER_TRACE, thd->query().str);
      });
    DBUG_EXECUTE_IF("parser_stmt_to_error_log_with_system_prio", {
        LogErr(SYSTEM_LEVEL, ER_PARSER_TRACE, thd->query().str);
      });

    while (!thd->killed && (parser_state.m_lip.found_semicolon != NULL) &&
           ! thd->is_error())
    {
      /*
        Multiple queries exits, execute them individually
      */
      const char *beginning_of_next_stmt= parser_state.m_lip.found_semicolon;

      /* Finalize server status flags after executing a statement. */
      thd->update_slow_query_status();
      thd->send_statement_status();

      mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_GENERAL_STATUS),
                         thd->get_stmt_da()->is_error() ?
                         thd->get_stmt_da()->mysql_errno() : 0,
                         command_name[command].str,
                         command_name[command].length);

      size_t length= static_cast<size_t>(packet_end - beginning_of_next_stmt);

      log_slow_statement(thd);

      /* Remove garbage at start of query */
      while (length > 0 && my_isspace(thd->charset(), *beginning_of_next_stmt))
      {
        beginning_of_next_stmt++;
        length--;
      }

/* PSI end */
      MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
      thd->m_statement_psi= NULL;
      thd->m_digest= NULL;

/* SHOW PROFILE end */
#if defined(ENABLED_PROFILING)
      thd->profiling->finish_current_query();
#endif

/* SHOW PROFILE begin */
#if defined(ENABLED_PROFILING)
      thd->profiling->start_new_query("continuing");
      thd->profiling->set_query_source(beginning_of_next_stmt, length);
#endif

/* PSI begin */
      thd->m_digest= & thd->m_digest_state;
      thd->m_digest->reset(thd->m_token_array, max_digest_length);

      thd->m_statement_psi= MYSQL_START_STATEMENT(&thd->m_statement_state,
                                          com_statement_info[command].m_key,
                                          thd->db().str, thd->db().length,
                                          thd->charset(), NULL);
      THD_STAGE_INFO(thd, stage_starting);

      thd->set_query(beginning_of_next_stmt, length);
      thd->set_query_id(next_query_id());
      /*
        Count each statement from the client.
      */
      thd->status_var.questions++;
      thd->set_time(); /* Reset the query start time. */
      parser_state.reset(beginning_of_next_stmt, length);
      /* TODO: set thd->lex->sql_command to SQLCOM_END here */
      mysql_parse(thd, &parser_state);
    }

    /* Need to set error to true for graceful shutdown */
    if((thd->lex->sql_command == SQLCOM_SHUTDOWN) && (thd->get_stmt_da()->is_ok()))
      error= true;

    DBUG_PRINT("info",("query ready"));
    break;
  }
  case COM_FIELD_LIST:				// This isn't actually needed
  {
    char *fields;
    /* Locked closure of all tables */
    TABLE_LIST table_list;
    LEX_STRING table_name;
    LEX_STRING db;
    push_deprecated_warn(thd, "COM_FIELD_LIST",
                         "SHOW COLUMNS FROM statement");
    /*
      SHOW statements should not add the used tables to the list of tables
      used in a transaction.
    */
    MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();

    thd->status_var.com_stat[SQLCOM_SHOW_FIELDS]++;
    if (thd->copy_db_to(&db.str, &db.length))
      break;
    thd->convert_string(&table_name, system_charset_info,
                        (char *) com_data->com_field_list.table_name,
                        com_data->com_field_list.table_name_length,
                        thd->charset());
    Ident_name_check ident_check_status=
      check_table_name(table_name.str, table_name.length);
    if (ident_check_status == Ident_name_check::WRONG)
    {
      /* this is OK due to convert_string() null-terminating the string */
      my_error(ER_WRONG_TABLE_NAME, MYF(0), table_name.str);
      break;
    }
    else if (ident_check_status == Ident_name_check::TOO_LONG)
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), table_name.str);
      break;
    }
    mysql_reset_thd_for_next_command(thd);
    lex_start(thd);
    /* Must be before we init the table list. */
    if (lower_case_table_names && !is_infoschema_db(db.str, db.length))
      table_name.length= my_casedn_str(files_charset_info, table_name.str);
    table_list.init_one_table(db.str, db.length, table_name.str,
                              table_name.length, table_name.str, TL_READ);
    /*
      Init TABLE_LIST members necessary when the undelrying
      table is view.
    */
    table_list.select_lex= thd->lex->select_lex;
    thd->lex->
      select_lex->table_list.link_in_list(&table_list,
                                         &table_list.next_local);
    thd->lex->add_to_query_tables(&table_list);

    if (is_infoschema_db(table_list.db, table_list.db_length))
    {
      ST_SCHEMA_TABLE *schema_table= find_schema_table(thd, table_list.alias);
      if (schema_table)
        table_list.schema_table= schema_table;
    }

    if (!(fields=
        (char *) thd->memdup(com_data->com_field_list.query,
                             com_data->com_field_list.query_length)))
      break;
    // Don't count end \0
    thd->set_query(fields, com_data->com_field_list.query_length - 1);
    query_logger.general_log_print(thd, command, "%s %s",
                                   table_list.table_name, fields);

    if (open_temporary_tables(thd, &table_list))
      break;

    if (check_table_access(thd, SELECT_ACL, &table_list,
                           true, UINT_MAX, false))
      break;

    // See comment in opt_trace_disable_if_no_security_context_access()
    Opt_trace_start ots(thd, &table_list, thd->lex->sql_command, NULL,
                        NULL, 0, NULL, NULL);

    mysqld_list_fields(thd,&table_list,fields);

    thd->lex->unit->cleanup(true);
    /* No need to rollback statement transaction, it's not started. */
    DBUG_ASSERT(thd->get_transaction()->is_empty(Transaction_ctx::STMT));
    close_thread_tables(thd);
    thd->mdl_context.rollback_to_savepoint(mdl_savepoint);

    if (thd->transaction_rollback_request)
    {
      /*
        Transaction rollback was requested since MDL deadlock was
        discovered while trying to open tables. Rollback transaction
        in all storage engines including binary log and release all
        locks.
      */
      trans_rollback_implicit(thd);
      thd->mdl_context.release_transactional_locks();
    }

    thd->cleanup_after_query();
    break;
  }
  case COM_QUIT:
    /* We don't calculate statistics for this command */
    query_logger.general_log_print(thd, command, NullS);
    // Don't give 'abort' message
    // TODO: access of protocol_classic should be removed
    if (thd->is_classic_protocol())
      thd->get_protocol_classic()->get_net()->error= 0;
    thd->get_stmt_da()->disable_status();       // Don't send anything back
    error=true;					// End server
    break;
  case COM_BINLOG_DUMP_GTID:
    // TODO: access of protocol_classic should be removed
    error=
      com_binlog_dump_gtid(thd,
        (char *)thd->get_protocol_classic()->get_raw_packet(),
        thd->get_protocol_classic()->get_packet_length());
    break;
  case COM_BINLOG_DUMP:
    // TODO: access of protocol_classic should be removed
    error=
      com_binlog_dump(thd,
        (char*)thd->get_protocol_classic()->get_raw_packet(),
        thd->get_protocol_classic()->get_packet_length());
    break;
  case COM_REFRESH:
  {
    int not_used;
    push_deprecated_warn(thd, "COM_REFRESH", "FLUSH statement");
    /*
      Initialize thd->lex since it's used in many base functions, such as
      open_tables(). Otherwise, it remains uninitialized and may cause crash
      during execution of COM_REFRESH.
    */
    lex_start(thd);
    
    thd->status_var.com_stat[SQLCOM_FLUSH]++;
    ulong options= (ulong) com_data->com_refresh.options;
    if (trans_commit_implicit(thd))
      break;
    thd->mdl_context.release_transactional_locks();
    if (check_global_access(thd,RELOAD_ACL))
      break;
    query_logger.general_log_print(thd, command, NullS);
#ifndef DBUG_OFF
    bool debug_simulate= false;
    DBUG_EXECUTE_IF("simulate_detached_thread_refresh", debug_simulate= true;);
    if (debug_simulate)
    {
      /*
        Simulate a reload without a attached thread session.
        Provides a environment similar to that of when the
        server receives a SIGHUP signal and reloads caches
        and flushes tables.
      */
      bool res;
      current_thd= nullptr;
      res= handle_reload_request(NULL, options | REFRESH_FAST,
                                 NULL, &not_used);
      current_thd= thd;
      if (res)
        break;
    }
    else
#endif
    if (handle_reload_request(thd, options, (TABLE_LIST*) 0, &not_used))
      break;
    if (trans_commit_implicit(thd))
      break;
    close_thread_tables(thd);
    thd->mdl_context.release_transactional_locks();
    my_ok(thd);
    break;
  }
  case COM_STATISTICS:
  {
    System_status_var current_global_status_var;
    ulong uptime;
    size_t length MY_ATTRIBUTE((unused));
    ulonglong queries_per_second1000;
    char buff[250];
    size_t buff_len= sizeof(buff);

    query_logger.general_log_print(thd, command, NullS);
    thd->status_var.com_stat[SQLCOM_SHOW_STATUS]++;
    mysql_mutex_lock(&LOCK_status);
    calc_sum_of_all_status(&current_global_status_var);
    mysql_mutex_unlock(&LOCK_status);
    if (!(uptime= (ulong) (thd->query_start_in_secs() - server_start_time)))
      queries_per_second1000= 0;
    else
      queries_per_second1000= thd->query_id * 1000LL / uptime;

    length= snprintf(buff, buff_len - 1,
                        "Uptime: %lu  Threads: %d  Questions: %lu  "
                        "Slow queries: %llu  Opens: %llu  Flush tables: %lu  "
                        "Open tables: %u  Queries per second avg: %u.%03u",
                        uptime,
                        (int) thd_manager->get_thd_count(), (ulong) thd->query_id,
                        current_global_status_var.long_query_count,
                        current_global_status_var.opened_tables,
                        refresh_version,
                        table_cache_manager.cached_tables(),
                        (uint) (queries_per_second1000 / 1000),
                        (uint) (queries_per_second1000 % 1000));
    // TODO: access of protocol_classic should be removed.
    // should be rewritten using store functions
    thd->get_protocol_classic()->write((uchar*) buff, length);
    thd->get_protocol()->flush();
    thd->get_stmt_da()->disable_status();
    break;
  }
  case COM_PING:
    thd->status_var.com_other++;
    my_ok(thd);				// Tell client we are alive
    break;
  case COM_PROCESS_INFO:
    thd->status_var.com_stat[SQLCOM_SHOW_PROCESSLIST]++;
    push_deprecated_warn(thd, "COM_PROCESS_INFO",
                         "SHOW PROCESSLIST statement");
    if (!thd->security_context()->priv_user().str[0] &&
        check_global_access(thd, PROCESS_ACL))
      break;
    query_logger.general_log_print(thd, command, NullS);
    mysqld_list_processes(
      thd,
      thd->security_context()->check_access(PROCESS_ACL) ?
      NullS : thd->security_context()->priv_user().str, 0);
    break;
  case COM_PROCESS_KILL:
  {
    push_deprecated_warn(thd, "COM_PROCESS_KILL",
                         "KILL CONNECTION/QUERY statement");
    if (thd_manager->get_thread_id() & (~0xfffffffful))
      my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "thread_id", "mysql_kill()");
    else
    {
    thd->status_var.com_stat[SQLCOM_KILL]++;
      sql_kill(thd, com_data->com_kill.id, false);
    }
    break;
  }
  case COM_SET_OPTION:
  {
    thd->status_var.com_stat[SQLCOM_SET_OPTION]++;

    switch (com_data->com_set_option.opt_command) {
    case (int) MYSQL_OPTION_MULTI_STATEMENTS_ON:
      //TODO: access of protocol_classic should be removed
      thd->get_protocol_classic()->add_client_capability(
          CLIENT_MULTI_STATEMENTS);
      my_eof(thd);
      break;
    case (int) MYSQL_OPTION_MULTI_STATEMENTS_OFF:
      thd->get_protocol_classic()->remove_client_capability(
        CLIENT_MULTI_STATEMENTS);
      my_eof(thd);
      break;
    default:
      my_error(ER_UNKNOWN_COM_ERROR, MYF(0));
      break;
    }
    break;
  }
  case COM_DEBUG:
    thd->status_var.com_other++;
    if (check_global_access(thd, SUPER_ACL))
      break;					/* purecov: inspected */
    mysql_print_status();
    query_logger.general_log_print(thd, command, NullS);
    my_eof(thd);
    break;
  case COM_SLEEP:
  case COM_CONNECT:				// Impossible here
  case COM_TIME:				// Impossible from client
  case COM_DELAYED_INSERT: // INSERT DELAYED has been removed.
  case COM_END:
  default:
    my_error(ER_UNKNOWN_COM_ERROR, MYF(0));
    break;
  }

done:
  DBUG_ASSERT(thd->open_tables == NULL ||
              (thd->locked_tables_mode == LTM_LOCK_TABLES));

  /* Finalize server status flags after executing a command. */
  thd->update_slow_query_status();
  if (thd->killed)
    thd->send_kill_message();
  thd->send_statement_status();
  thd->rpl_thd_ctx.session_gtids_ctx().notify_after_response_packet(thd);

  if (!thd->is_error() && !thd->killed)
    mysql_audit_notify(thd,
                       AUDIT_EVENT(MYSQL_AUDIT_GENERAL_RESULT), 0, NULL, 0);

  mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_GENERAL_STATUS),
                     thd->get_stmt_da()->is_error() ?
                     thd->get_stmt_da()->mysql_errno() : 0,
                     command_name[command].str,
                     command_name[command].length);

  /* command_end is informational only. The plugin cannot abort
     execution of the command at thie point. */
  mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_COMMAND_END), command,
                     command_name[command].str);

  log_slow_statement(thd);

  THD_STAGE_INFO(thd, stage_cleaning_up);

  thd->reset_query();
  thd->set_command(COM_SLEEP);
  thd->proc_info= 0;
  thd->lex->sql_command= SQLCOM_END;

  /* Performance Schema Interface instrumentation, end */
  MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
  thd->m_statement_psi= NULL;
  thd->m_digest= NULL;

  thd_manager->dec_thread_running();

  /* Freeing the memroot will leave the THD::work_part_info invalid. */
  thd->work_part_info= nullptr;

  /*
    If we've allocated a lot of memory (compared to the user's desired preallocation
    size; note that we don't actually preallocate anymore), free it so that one
    big query won't cause us to hold on to a lot of RAM forever. If not, keep the last
    block so that the next query will hopefully be able to run without allocating
    memory from the OS.

    The factor 5 is pretty much arbitrary, but ends up allowing three allocations
    (1 + 1.5 + 1.5²) under the current allocation policy.
  */
  if (thd->mem_root->allocated_size() < 5 * thd->variables.query_prealloc_size)
    thd->mem_root->ClearForReuse();
  else
    thd->mem_root->Clear();

  /* SHOW PROFILE instrumentation, end */
#if defined(ENABLED_PROFILING)
  thd->profiling->finish_current_query();
#endif

  DBUG_RETURN(error);
}

/**
  Shutdown the mysqld server.

  @param  thd        Thread (session) context.
  @param  level      Shutdown level.

  @retval
    true                 success
  @retval
    false                When user has insufficient privilege or unsupported shutdown level

*/

bool shutdown(THD *thd, enum mysql_enum_shutdown_level level)
{
  DBUG_ENTER("shutdown");
  bool res= false;
  thd->lex->no_write_to_binlog= 1;

  if (check_global_access(thd,SHUTDOWN_ACL))
    goto error; /* purecov: inspected */

  if (level == SHUTDOWN_DEFAULT)
    level= SHUTDOWN_WAIT_ALL_BUFFERS; // soon default will be configurable
  else if (level != SHUTDOWN_WAIT_ALL_BUFFERS)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "this shutdown level");
    goto error;;
  }

  my_ok(thd);

  DBUG_PRINT("quit",("Got shutdown command for level %u", level));
  query_logger.general_log_print(thd, COM_QUERY, NullS);
  kill_mysql();
  res= true;

  error:
  DBUG_RETURN(res);
}


/**
  Create a TABLE_LIST object for an INFORMATION_SCHEMA table.

    This function is used in the parser to convert a SHOW or DESCRIBE
    table_name command to a SELECT from INFORMATION_SCHEMA.
    It prepares a SELECT_LEX and a TABLE_LIST object to represent the
    given command as a SELECT parse tree.

  @param thd              thread handle
  @param lex              current lex
  @param table_ident      table alias if it's used
  @param schema_table_idx the type of the INFORMATION_SCHEMA table to be
                          created

  @note
    Due to the way this function works with memory and LEX it cannot
    be used outside the parser (parse tree transformations outside
    the parser break PS and SP).

  @retval
    0                 success
  @retval
    1                 out of memory or SHOW commands are not allowed
                      in this version of the server.
*/

int prepare_schema_table(THD *thd, LEX *lex, Table_ident *table_ident,
                         enum enum_schema_tables schema_table_idx)
{
  SELECT_LEX *schema_select_lex= NULL;
  DBUG_ENTER("prepare_schema_table");

  switch (schema_table_idx) {
  case SCH_TMP_TABLE_COLUMNS:
  case SCH_TMP_TABLE_KEYS:
  {
    DBUG_ASSERT(table_ident);
    TABLE_LIST **query_tables_last= lex->query_tables_last;
    if ((schema_select_lex= lex->new_empty_query_block()) == NULL)
      DBUG_RETURN(1);        /* purecov: inspected */
    if (!schema_select_lex->add_table_to_list(thd, table_ident, 0, 0, TL_READ,
                                              MDL_SHARED_READ))
      DBUG_RETURN(1);
    lex->query_tables_last= query_tables_last;
    break;
  }
  case SCH_PROFILES:
    /* 
      Mark this current profiling record to be discarded.  We don't
      wish to have SHOW commands show up in profiling->
    */
#if defined(ENABLED_PROFILING)
    thd->profiling->discard_current_query();
#endif
    break;
  case SCH_OPTIMIZER_TRACE:
  case SCH_OPEN_TABLES:
  case SCH_ENGINES:
  case SCH_USER_PRIVILEGES:
  case SCH_SCHEMA_PRIVILEGES:
  case SCH_TABLE_PRIVILEGES:
  case SCH_COLUMN_PRIVILEGES:
  default:
    break;
  }
  
  SELECT_LEX *select_lex= lex->current_select();
  if (make_schema_select(thd, select_lex, schema_table_idx))
  {
    DBUG_RETURN(1);
  }
  TABLE_LIST *table_list= select_lex->table_list.first;
  table_list->schema_select_lex= schema_select_lex;
  table_list->schema_table_reformed= 1;
  DBUG_RETURN(0);
}


/**
  Read query from packet and store in thd->query.
  Used in COM_QUERY and COM_STMT_PREPARE.

    Sets the following THD variables:
  - query
  - query_length

  @retval
    false ok
  @retval
    true  error;  In this case thd->fatal_error is set
*/

bool alloc_query(THD *thd, const char *packet, size_t packet_length)
{
  /* Remove garbage at start and end of query */
  while (packet_length > 0 && my_isspace(thd->charset(), packet[0]))
  {
    packet++;
    packet_length--;
  }
  const char *pos= packet + packet_length;     // Point at end null
  while (packet_length > 0 &&
	 (pos[-1] == ';' || my_isspace(thd->charset() ,pos[-1])))
  {
    pos--;
    packet_length--;
  }

  char *query= static_cast<char*>(thd->alloc(packet_length + 1));
  if (!query)
    return true;
  memcpy(query, packet, packet_length);
  query[packet_length]= '\0';

  thd->set_query(query, packet_length);

  /* Reclaim some memory */
  if (thd->is_classic_protocol())
    thd->convert_buffer.shrink(thd->variables.net_buffer_length);

  return false;
}

static
bool sp_process_definer(THD *thd)
{
  DBUG_ENTER("sp_process_definer");

  LEX *lex= thd->lex;

  /*
    If the definer is not specified, this means that CREATE-statement missed
    DEFINER-clause. DEFINER-clause can be missed in two cases:

      - The user submitted a statement w/o the clause. This is a normal
        case, we should assign CURRENT_USER as definer.

      - Our slave received an updated from the master, that does not
        replicate definer for stored rountines. We should also assign
        CURRENT_USER as definer here, but also we should mark this routine
        as NON-SUID. This is essential for the sake of backward
        compatibility.

        The problem is the slave thread is running under "special" user (@),
        that actually does not exist. In the older versions we do not fail
        execution of a stored routine if its definer does not exist and
        continue the execution under the authorization of the invoker
        (BUG#13198). And now if we try to switch to slave-current-user (@),
        we will fail.

        Actually, this leads to the inconsistent state of master and
        slave (different definers, different SUID behaviour), but it seems,
        this is the best we can do.
  */

  if (!lex->definer)
  {
    Prepared_stmt_arena_holder ps_arena_holder(thd);

    lex->definer= create_default_definer(thd);

    /* Error has been already reported. */
    if (lex->definer == NULL)
      DBUG_RETURN(true);

    if (thd->slave_thread && lex->sphead)
      lex->sphead->m_chistics->suid= SP_IS_NOT_SUID;
  }
  else
  {
    /*
      If the specified definer differs from the current user, we
      should check that the current user has a set_user_id privilege
      (in order to create a stored routine under another user one must
       have a set_user_id privilege).
    */
    Security_context *sctx= thd->security_context();
    if ((strcmp(lex->definer->user.str,
                thd->security_context()->priv_user().str) ||
         my_strcasecmp(system_charset_info, lex->definer->host.str,
                       thd->security_context()->priv_host().str)) &&
        !(sctx->check_access(SUPER_ACL) ||
          sctx->has_global_grant(STRING_WITH_LEN("SET_USER_ID")).first))
    {
      my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
               "SUPER or SET_USER_ID");
      DBUG_RETURN(true);
    }
  }

  /* Check that the specified definer exists. Emit a warning if not. */

  if (!is_acl_user(thd, lex->definer->host.str, lex->definer->user.str))
  {
    push_warning_printf(thd,
                        Sql_condition::SL_NOTE,
                        ER_NO_SUCH_USER,
                        ER_THD(thd, ER_NO_SUCH_USER),
                        lex->definer->user.str,
                        lex->definer->host.str);
  }

  DBUG_RETURN(false);
}


/**
  Auxiliary call that opens and locks tables for LOCK TABLES statement
  and initializes the list of locked tables.

  @param thd     Thread context.
  @param tables  List of tables to be locked.

  @return false in case of success, true in case of error.
*/

static bool lock_tables_open_and_lock_tables(THD *thd, TABLE_LIST *tables)
{
  Lock_tables_prelocking_strategy lock_tables_prelocking_strategy;
  MDL_deadlock_and_lock_abort_error_handler deadlock_handler;
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();
  uint counter;
  TABLE_LIST *table;

  thd->in_lock_tables= 1;

retry:

  if (open_tables(thd, &tables, &counter, 0, &lock_tables_prelocking_strategy))
    goto err;

  deadlock_handler.init();
  thd->push_internal_handler(&deadlock_handler);

  for (table= tables; table; table= table->next_global)
  {
    if (!table->is_placeholder())
    {
      if (table->table->s->tmp_table)
      {
        /*
          We allow to change temporary tables even if they were locked for read
          by LOCK TABLES. To avoid a discrepancy between lock acquired at LOCK
          TABLES time and by the statement which is later executed under LOCK
          TABLES we ensure that for temporary tables we always request a write
          lock (such discrepancy can cause problems for the storage engine).
          We don't set TABLE_LIST::lock_type in this case as this might result
          in extra warnings from THD::decide_logging_format() even though
          binary logging is totally irrelevant for LOCK TABLES.
        */
        table->table->reginfo.lock_type= TL_WRITE;
      }
      else if (table->lock_descriptor().type == TL_READ &&
               ! table->prelocking_placeholder &&
               table->table->file->ha_table_flags() & HA_NO_READ_LOCAL_LOCK)
      {
        /*
          In case when LOCK TABLE ... READ LOCAL was issued for table with
          storage engine which doesn't support READ LOCAL option and doesn't
          use THR_LOCK locks we need to upgrade weak SR metadata lock acquired
          in open_tables() to stronger SRO metadata lock.
          This is not needed for tables used through stored routines or
          triggers as we always acquire SRO (or even stronger SNRW) metadata
          lock for them.
        */
        bool result= thd->mdl_context.upgrade_shared_lock(
                                        table->table->mdl_ticket,
                                        MDL_SHARED_READ_ONLY,
                                        thd->variables.lock_wait_timeout);

        if (deadlock_handler.need_reopen())
        {
          /*
            Deadlock occurred during upgrade of metadata lock.
            Let us restart acquring and opening tables for LOCK TABLES.
          */
          thd->pop_internal_handler();
          close_tables_for_reopen(thd, &tables, mdl_savepoint);
          if (open_temporary_tables(thd, tables))
            goto err;
          goto retry;
        }

        if (result)
        {
          thd->pop_internal_handler();
          goto err;
        }
      }
    }
  }

  thd->pop_internal_handler();

  if (lock_tables(thd, tables, counter, 0) ||
      thd->locked_tables_list.init_locked_tables(thd))
    goto err;

  thd->in_lock_tables= 0;

  return false;

err:
  thd->in_lock_tables= 0;

  trans_rollback_stmt(thd);
  /*
    Need to end the current transaction, so the storage engine (InnoDB)
    can free its locks if LOCK TABLES locked some tables before finding
    that it can't lock a table in its list
  */
  trans_rollback(thd);
  /* Close tables and release metadata locks. */
  close_thread_tables(thd);
  DBUG_ASSERT(!thd->locked_tables_mode);
  thd->mdl_context.release_transactional_locks();
  return true;
}


/**
  This is a wrapper for MYSQL_BIN_LOG::gtid_end_transaction. For normal
  statements, the function gtid_end_transaction is called in the commit
  handler. However, if the statement is filtered out or not written to
  the binary log, the commit handler is not invoked. Therefore, this
  wrapper calls gtid_end_transaction in case the current statement is
  committing but was not written to the binary log.
  (The function gtid_end_transaction ensures that gtid-related
  end-of-transaction operations are performed; this includes
  generating an empty transaction and calling
  Gtid_state::update_gtids_impl.)

  @param thd Thread (session) context.
*/

static inline void binlog_gtid_end_transaction(THD *thd)
{
  DBUG_ENTER("binlog_gtid_end_transaction");

  /*
    This performs end-of-transaction actions needed by GTIDs:
    in particular, it generates an empty transaction if
    needed (e.g., if the statement was filtered out).

    It is executed at the end of an implicitly or explicitly
    committing statement.

    In addition, it is executed after CREATE TEMPORARY TABLE
    or DROP TEMPORARY TABLE when they occur outside
    transactional context.  When enforce_gtid_consistency is
    enabled, these statements cannot occur in transactional
    context, and then they behave exactly as implicitly
    committing: they are written to the binary log
    immediately, not wrapped in BEGIN/COMMIT, and cannot be
    rolled back. However, they do not count as implicitly
    committing according to stmt_causes_implicit_commit(), so
    we need to add special cases in the condition below. Hence
    the clauses for SQLCOM_CREATE_TABLE and SQLCOM_DROP_TABLE.

    If enforce_gtid_consistency=off, CREATE TEMPORARY TABLE
    and DROP TEMPORARY TABLE can occur in the middle of a
    transaction.  Then they do not behave as DDL; they are
    written to the binary log inside BEGIN/COMMIT.

    (For base tables, SQLCOM_[CREATE|DROP]_TABLE match both
    the stmt_causes_implicit_commit(...) clause and the
    thd->lex->sql_command == SQLCOM_* clause; for temporary
    tables they match only thd->lex->sql_command == SQLCOM_*.)
  */
  if (thd->lex->sql_command == SQLCOM_COMMIT ||
      thd->lex->sql_command == SQLCOM_XA_PREPARE ||
      thd->lex->sql_command == SQLCOM_XA_COMMIT ||
      thd->lex->sql_command == SQLCOM_XA_ROLLBACK ||
      stmt_causes_implicit_commit(thd, CF_IMPLICIT_COMMIT_END) ||
      ((thd->lex->sql_command == SQLCOM_CREATE_TABLE ||
        thd->lex->sql_command == SQLCOM_DROP_TABLE) &&
       !thd->in_multi_stmt_transaction_mode()))
    (void) mysql_bin_log.gtid_end_transaction(thd);

  DBUG_VOID_RETURN;
}


static inline bool check_if_backup_lock_has_to_be_acquired(LEX *lex)
{
  if ((lex->sql_command == SQLCOM_CREATE_TABLE &&
       (lex->create_info->options & HA_LEX_CREATE_TMP_TABLE)) ||
      (lex->sql_command == SQLCOM_DROP_TABLE &&
       lex->drop_temporary))
    return false;

  return sql_command_flags[lex->sql_command] & CF_ACQUIRE_BACKUP_LOCK;
}


/**
  Execute command saved in thd and lex->sql_command.

  @param thd                       Thread handle
  @param first_level

  @todo
    @todo: this is workaround. right way will be move invalidating in
    the unlock procedure.
    - TODO: use check_change_password()

  @retval
    false       OK
  @retval
    true        Error
*/

int
mysql_execute_command(THD *thd, bool first_level)
{
  int res= false;
  LEX  *const lex= thd->lex;
  /* first SELECT_LEX (have special meaning for many of non-SELECTcommands) */
  SELECT_LEX *const select_lex= lex->select_lex;
  /* first table of first SELECT_LEX */
  TABLE_LIST *const first_table= select_lex->get_table_list();
  /* list of all tables in query */
  TABLE_LIST *all_tables;
  DBUG_ASSERT(select_lex->master_unit() == lex->unit);
  DBUG_ENTER("mysql_execute_command");
  /* EXPLAIN OTHER isn't explainable command, but can have describe flag. */
  DBUG_ASSERT(!lex->is_explain() || is_explainable_query(lex->sql_command) ||
              lex->sql_command == SQLCOM_EXPLAIN_OTHER);

  thd->work_part_info= 0;

  /*
    Each statement or replication event which might produce deadlock
    should handle transaction rollback on its own. So by the start of
    the next statement transaction rollback request should be fulfilled
    already.
  */
  DBUG_ASSERT(! thd->transaction_rollback_request || thd->in_sub_stmt);
  /*
    In many cases first table of main SELECT_LEX have special meaning =>
    check that it is first table in global list and relink it first in
    queries_tables list if it is necessary (we need such relinking only
    for queries with subqueries in select list, in this case tables of
    subqueries will go to global list first)

    all_tables will differ from first_table only if most upper SELECT_LEX
    do not contain tables.

    Because of above in place where should be at least one table in most
    outer SELECT_LEX we have following check:
    DBUG_ASSERT(first_table == all_tables);
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
  */
  lex->first_lists_tables_same();
  /* should be assigned after making first tables same */
  all_tables= lex->query_tables;
  /* set context for commands which do not use setup_tables */
  select_lex->context.resolve_in_table_list_only(select_lex->get_table_list());

  thd->get_stmt_da()->reset_diagnostics_area();
  if ((thd->lex->keep_diagnostics != DA_KEEP_PARSE_ERROR) &&
      (thd->lex->keep_diagnostics != DA_KEEP_DIAGNOSTICS))
  {
    /*
      No parse errors, and it's not a diagnostic statement:
      remove the sql conditions from the DA!
      For diagnostic statements we need to keep the conditions
      around so we can inspec them.
    */
    thd->get_stmt_da()->reset_condition_info(thd);
  }

  if (thd->resource_group_ctx()->m_warn != 0)
  {
    auto res_grp_name=
      thd->resource_group_ctx()->m_switch_resource_group_str;
    switch(thd->resource_group_ctx()->m_warn)
    {
      case WARN_RESOURCE_GROUP_UNSUPPORTED:
      {
        auto res_grp_mgr= resourcegroups::Resource_group_mgr::instance();
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_FEATURE_UNSUPPORTED,
                            ER_THD(thd, ER_FEATURE_UNSUPPORTED),
                            "Resource groups",
                            res_grp_mgr->unsupport_reason());
        break;
      }
      case WARN_RESOURCE_GROUP_UNSUPPORTED_HINT:
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_WARN_UNSUPPORTED_HINT,
                            ER_THD(thd, ER_WARN_UNSUPPORTED_HINT),
                            "Subquery or Stored procedure or Trigger");
        break;
      case WARN_RESOURCE_GROUP_TYPE_MISMATCH:
      {
        ulonglong pfs_thread_id= 0;
        /*
	  Resource group is unsupported with DISABLE_PSI_THREAD.
	  The below #ifdef is required for compilation when DISABLE_PSI_THREAD is
	  enabled.
	*/
#ifdef HAVE_PSI_THREAD_INTERFACE
        ulonglong unused_event_id MY_ATTRIBUTE((unused));
        PSI_THREAD_CALL(get_thread_event_id)(&pfs_thread_id, &unused_event_id);
#endif // HAVE_PSI_THREAD_INTERFACE
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_RESOURCE_GROUP_BIND_FAILED,
                            ER_THD(thd, ER_RESOURCE_GROUP_BIND_FAILED),
                            res_grp_name, pfs_thread_id,
                            "System resource group can't be bound"
                            " with a session thread");
        break;
      }
      case WARN_RESOURCE_GROUP_NOT_EXISTS:
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_RESOURCE_GROUP_NOT_EXISTS,
                            ER_THD(thd, ER_RESOURCE_GROUP_NOT_EXISTS),
                            res_grp_name);
        break;
      case WARN_RESOURCE_GROUP_ACCESS_DENIED:
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_SPECIFIC_ACCESS_DENIED_ERROR,
                            ER_THD(thd, ER_SPECIFIC_ACCESS_DENIED_ERROR),
                            "SUPER OR RESOURCE_GROUP_ADMIN OR "
                            "RESOURCE_GROUP_USER");
    }
    thd->resource_group_ctx()->m_warn= 0;
    res_grp_name[0]= '\0';
  }

  if (unlikely(thd->slave_thread))
  {
    bool need_increase_counter= !(lex->sql_command == SQLCOM_XA_START ||
                                  lex->sql_command == SQLCOM_XA_END ||
                                  lex->sql_command == SQLCOM_XA_COMMIT ||
                                  lex->sql_command == SQLCOM_XA_ROLLBACK);
    // Database filters.
    if (lex->sql_command != SQLCOM_BEGIN &&
        lex->sql_command != SQLCOM_COMMIT &&
        lex->sql_command != SQLCOM_SAVEPOINT &&
        lex->sql_command != SQLCOM_ROLLBACK &&
        lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT &&
        !thd->rli_slave->rpl_filter->db_ok(thd->db().str,
                                           need_increase_counter))
    {
      binlog_gtid_end_transaction(thd);
      DBUG_RETURN(0);
    }

    if (lex->sql_command == SQLCOM_DROP_TRIGGER)
    {
      /*
        When dropping a trigger, we need to load its table name
        before checking slave filter rules.
      */
      TABLE_LIST *trigger_table= nullptr;
      (void)get_table_for_trigger(thd, lex->spname->m_db,
                                  lex->spname->m_name, true, &trigger_table);
      if (trigger_table != nullptr)
      {
        lex->add_to_query_tables(trigger_table);
        all_tables= trigger_table;
      }
      else
      {
        /*
          If table name cannot be loaded,
          it means the trigger does not exists possibly because
          CREATE TRIGGER was previously skipped for this trigger
          according to slave filtering rules.
          Returning success without producing any errors in this case.
        */
        binlog_gtid_end_transaction(thd);
        DBUG_RETURN(0);
      }

      // force searching in slave.cc:tables_ok()
      all_tables->updating= 1;
    }

    /*
      For fix of BUG#37051, the master stores the table map for update
      in the Query_log_event, and the value is assigned to
      thd->variables.table_map_for_update before executing the update
      query.

      If thd->variables.table_map_for_update is set, then we are
      replicating from a new master, we can use this value to apply
      filter rules without opening all the tables. However If
      thd->variables.table_map_for_update is not set, then we are
      replicating from an old master, so we just skip this and
      continue with the old method. And of course, the bug would still
      exist for old masters.
    */
    if (lex->sql_command == SQLCOM_UPDATE_MULTI &&
        thd->table_map_for_update)
    {
      table_map table_map_for_update= thd->table_map_for_update;
      uint nr= 0;
      TABLE_LIST *table;
      for (table=all_tables; table; table=table->next_global, nr++)
      {
        if (table_map_for_update & ((table_map)1 << nr))
          table->updating= true;
        else
          table->updating= false;
      }

      if (all_tables_not_ok(thd, all_tables))
      {
        /* we warn the slave SQL thread */
        my_error(ER_SLAVE_IGNORED_TABLE, MYF(0));
        binlog_gtid_end_transaction(thd);
        DBUG_RETURN(0);
      }
      
      for (table=all_tables; table; table=table->next_global)
        table->updating= true;
    }
    
    /*
      Check if statement should be skipped because of slave filtering
      rules

      Exceptions are:
      - UPDATE MULTI: For this statement, we want to check the filtering
        rules later in the code
      - SET: we always execute it (Not that many SET commands exists in
        the binary log anyway -- only 4.1 masters write SET statements,
	in 5.0 there are no SET statements in the binary log)
      - DROP TEMPORARY TABLE IF EXISTS: we always execute it (otherwise we
        have stale files on slave caused by exclusion of one tmp table).
    */
    if (!(lex->sql_command == SQLCOM_UPDATE_MULTI) &&
	!(lex->sql_command == SQLCOM_SET_OPTION) &&
	!(lex->sql_command == SQLCOM_DROP_TABLE &&
          lex->drop_temporary && lex->drop_if_exists) &&
        all_tables_not_ok(thd, all_tables))
    {
      /* we warn the slave SQL thread */
      my_error(ER_SLAVE_IGNORED_TABLE, MYF(0));
      binlog_gtid_end_transaction(thd);
      DBUG_RETURN(0);
    }
    /* 
       Execute deferred events first
    */
    if (slave_execute_deferred_events(thd))
      DBUG_RETURN(-1);
  }
  else
  {
    /*
      When option readonly is set deny operations which change non-temporary
      tables. Except for the replication thread and the 'super' users.
    */
    if (deny_updates_if_read_only_option(thd, all_tables))
    {
      err_readonly(thd);
      DBUG_RETURN(-1);
    }
  } /* endif unlikely slave */

  thd->status_var.com_stat[lex->sql_command]++;

  Opt_trace_start ots(thd, all_tables, lex->sql_command, &lex->var_list,
                      thd->query().str, thd->query().length, NULL,
                      thd->variables.character_set_client);

  Opt_trace_object trace_command(&thd->opt_trace);
  Opt_trace_array trace_command_steps(&thd->opt_trace, "steps");

  DBUG_ASSERT(thd->get_transaction()->cannot_safely_rollback(
      Transaction_ctx::STMT) == false);

  switch (gtid_pre_statement_checks(thd))
  {
  case GTID_STATEMENT_EXECUTE:
    break;
  case GTID_STATEMENT_CANCEL:
    DBUG_RETURN(-1);
  case GTID_STATEMENT_SKIP:
    my_ok(thd);
    binlog_gtid_end_transaction(thd);
    DBUG_RETURN(0);
  }

  if (check_if_backup_lock_has_to_be_acquired(lex) &&
      acquire_shared_backup_lock(thd, thd->variables.lock_wait_timeout))
    DBUG_RETURN(1);

  /*
    End a active transaction so that this command will have it's
    own transaction and will also sync the binary log. If a DDL is
    not run in it's own transaction it may simply never appear on
    the slave in case the outside transaction rolls back.
  */
  if (stmt_causes_implicit_commit(thd, CF_IMPLICIT_COMMIT_BEGIN))
  {
    /*
      Note that this should never happen inside of stored functions
      or triggers as all such statements prohibited there.
    */
    DBUG_ASSERT(! thd->in_sub_stmt);
    /* Statement transaction still should not be started. */
    DBUG_ASSERT(thd->get_transaction()->is_empty(Transaction_ctx::STMT));

    /*
      Implicit commit is not allowed with an active XA transaction.
      In this case we should not release metadata locks as the XA transaction
      will not be rolled back. Therefore we simply return here.
    */
    if (trans_check_state(thd))
      DBUG_RETURN(-1);

    /* Commit the normal transaction if one is active. */
    if (trans_commit_implicit(thd))
      DBUG_RETURN(-1);
    /* Release metadata locks acquired in this transaction. */
    thd->mdl_context.release_transactional_locks();
  }

  DEBUG_SYNC(thd, "after_implicit_pre_commit");

  if (gtid_pre_statement_post_implicit_commit_checks(thd))
    DBUG_RETURN(-1);

  if (mysql_audit_notify(thd, first_level ?
                              MYSQL_AUDIT_QUERY_START :
                              MYSQL_AUDIT_QUERY_NESTED_START,
                              first_level ?
                              "MYSQL_AUDIT_QUERY_START" :
                              "MYSQL_AUDIT_QUERY_NESTED_START"))
  {
    DBUG_RETURN(1);
  }

#ifndef DBUG_OFF
  if (lex->sql_command != SQLCOM_SET_OPTION)
    DEBUG_SYNC(thd,"before_execute_sql_command");
#endif

  /*
    For statements which need this, prevent InnoDB from automatically
    committing InnoDB transaction each time data-dictionary tables are
    closed after being updated.
  */
  Disable_autocommit_guard
    autocommit_guard(sqlcom_needs_autocommit_off(lex) &&
                     !thd->is_plugin_fake_ddl() ? thd : NULL);

  /*
    Check if we are in a read-only transaction and we're trying to
    execute a statement which should always be disallowed in such cases.

    Note that this check is done after any implicit commits.
  */
  if (thd->tx_read_only &&
      (sql_command_flags[lex->sql_command] & CF_DISALLOW_IN_RO_TRANS))
  {
    my_error(ER_CANT_EXECUTE_IN_READ_ONLY_TRANSACTION, MYF(0));
    goto error;
  }

  /*
    Close tables open by HANDLERs before executing DDL statement
    which is going to affect those tables.

    This should happen before temporary tables are pre-opened as
    otherwise we will get errors about attempt to re-open tables
    if table to be changed is open through HANDLER.

    Note that even although this is done before any privilege
    checks there is no security problem here as closing open
    HANDLER doesn't require any privileges anyway.
  */
  if (sql_command_flags[lex->sql_command] & CF_HA_CLOSE)
    mysql_ha_rm_tables(thd, all_tables);

  /*
    Check that the command is allowed on the PROTOCOL_PLUGIN
  */
  if (thd->get_protocol()->type() == Protocol::PROTOCOL_PLUGIN &&
      !(sql_command_flags[lex->sql_command] & CF_ALLOW_PROTOCOL_PLUGIN))
  {
    my_error(ER_PLUGGABLE_PROTOCOL_COMMAND_NOT_SUPPORTED, MYF(0));
    goto error;
  }

  /*
    Pre-open temporary tables to simplify privilege checking
    for statements which need this.
  */
  if (sql_command_flags[lex->sql_command] & CF_PREOPEN_TMP_TABLES)
  {
    if (open_temporary_tables(thd, all_tables))
      goto error;
  }

  // Save original info for EXPLAIN FOR CONNECTION
  if (!thd->in_sub_stmt)
    thd->query_plan.set_query_plan(lex->sql_command, lex,
                                   !thd->stmt_arena->is_conventional());

  /* Update system variables specified in SET_VAR hints. */
  if (lex->opt_hints_global && lex->opt_hints_global->sys_var_hint)
    lex->opt_hints_global->sys_var_hint->update_vars(thd);

  switch (lex->sql_command) {

  case SQLCOM_SHOW_STATUS:
  {
    System_status_var old_status_var= thd->status_var;
    thd->initial_status_var= &old_status_var;

    if (!(res= show_precheck(thd, lex, true)))
      res= execute_show(thd, all_tables);

    /* Don't log SHOW STATUS commands to slow query log */
    thd->server_status&= ~(SERVER_QUERY_NO_INDEX_USED |
                           SERVER_QUERY_NO_GOOD_INDEX_USED);
    /*
      restore status variables, as we don't want 'show status' to cause
      changes
    */
    mysql_mutex_lock(&LOCK_status);
    add_diff_to_status(&global_status_var, &thd->status_var,
                       &old_status_var);
    thd->status_var= old_status_var;
    thd->initial_status_var= NULL;
    mysql_mutex_unlock(&LOCK_status);
    break;
  }
  case SQLCOM_SHOW_EVENTS:
  case SQLCOM_SHOW_STATUS_PROC:
  case SQLCOM_SHOW_STATUS_FUNC:
  case SQLCOM_SHOW_DATABASES:
  case SQLCOM_SHOW_TRIGGERS:
  case SQLCOM_SHOW_TABLE_STATUS:
  case SQLCOM_SHOW_OPEN_TABLES:
  case SQLCOM_SHOW_PLUGINS:
  case SQLCOM_SHOW_VARIABLES:
  case SQLCOM_SHOW_CHARSETS:
  case SQLCOM_SHOW_COLLATIONS:
  case SQLCOM_SHOW_STORAGE_ENGINES:
  case SQLCOM_SHOW_PROFILE:
  {
    DBUG_EXECUTE_IF("use_attachable_trx",
                    thd->begin_attachable_ro_transaction(););

    thd->clear_current_query_costs();

    res= show_precheck(thd, lex, true);

    if (!res)
      res= execute_show(thd, all_tables);

    thd->save_current_query_costs();

    DBUG_EXECUTE_IF("use_attachable_trx",
                    thd->end_attachable_transaction(););

    break;
  }
  case SQLCOM_PREPARE:
  {
    mysql_sql_stmt_prepare(thd);
    break;
  }
  case SQLCOM_EXECUTE:
  {
    mysql_sql_stmt_execute(thd);
    break;
  }
  case SQLCOM_DEALLOCATE_PREPARE:
  {
    mysql_sql_stmt_close(thd);
    break;
  }

  case SQLCOM_EMPTY_QUERY:
    my_ok(thd);
    break;

  case SQLCOM_HELP:
    res= mysqld_help(thd,lex->help_arg);
    break;

  case SQLCOM_PURGE:
  {
    Security_context *sctx= thd->security_context();
    if (!sctx->check_access(SUPER_ACL) &&
        !sctx->has_global_grant(STRING_WITH_LEN("BINLOG_ADMIN")).first)
    {
      my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
               "SUPER or BINLOG_ADMIN");
      goto error;
    }
    /* PURGE MASTER LOGS TO 'file' */
    res = purge_master_logs(thd, lex->to_log);
    break;
  }
  case SQLCOM_PURGE_BEFORE:
  {
    Item *it;
    Security_context *sctx= thd->security_context();
    if (!sctx->check_access(SUPER_ACL) &&
        !sctx->has_global_grant(STRING_WITH_LEN("BINLOG_ADMIN")).first)
    {
      my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
               "SUPER or BINLOG_ADMIN");
      goto error;
    }
    /* PURGE MASTER LOGS BEFORE 'data' */
    it= lex->purge_value_list.head();
    if ((!it->fixed && it->fix_fields(lex->thd, &it)) ||
        it->check_cols(1))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "PURGE LOGS BEFORE");
      goto error;
    }
    it= new Item_func_unix_timestamp(it);
    /*
      it is OK only emulate fix_fieds, because we need only
      value of constant
    */
    it->quick_fix_field();
    time_t purge_time= static_cast<time_t>(it->val_int());
    if (thd->is_error())
      goto error;
    res = purge_master_logs_before_date(thd, purge_time);
    break;
  }
  case SQLCOM_SHOW_WARNS:
  {
    res= mysqld_show_warnings(thd, (ulong)
			      ((1L << (uint) Sql_condition::SL_NOTE) |
			       (1L << (uint) Sql_condition::SL_WARNING) |
			       (1L << (uint) Sql_condition::SL_ERROR)
			       ));
    break;
  }
  case SQLCOM_SHOW_ERRORS:
  {
    res= mysqld_show_warnings(thd, (ulong)
			      (1L << (uint) Sql_condition::SL_ERROR));
    break;
  }
  case SQLCOM_SHOW_PROFILES:
  {
#if defined(ENABLED_PROFILING)
    thd->profiling->discard_current_query();
    res= thd->profiling->show_profiles();
    if (res)
      goto error;
#else
    my_error(ER_FEATURE_DISABLED, MYF(0), "SHOW PROFILES", "enable-profiling");
    goto error;
#endif
    break;
  }
  case SQLCOM_SHOW_SLAVE_HOSTS:
  {
    if (check_global_access(thd, REPL_SLAVE_ACL))
      goto error;
    res= show_slave_hosts(thd);
    break;
  }
  case SQLCOM_SHOW_RELAYLOG_EVENTS:
  {
    if (check_global_access(thd, REPL_SLAVE_ACL))
      goto error;
    res = mysql_show_relaylog_events(thd);
    break;
  }
  case SQLCOM_SHOW_BINLOG_EVENTS:
  {
    if (check_global_access(thd, REPL_SLAVE_ACL))
      goto error;
    res = mysql_show_binlog_events(thd);
    break;
  }
  case SQLCOM_CHANGE_MASTER:
  {
    Security_context *sctx= thd->security_context();
    if (!sctx->check_access(SUPER_ACL) &&
        !sctx->has_global_grant(STRING_WITH_LEN("REPLICATION_SLAVE_ADMIN")).first)
    {
      my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "SUPER or REPLICATION_SLAVE_ADMIN");
      goto error;
    }
    res= change_master_cmd(thd);
    break;
  }
  case SQLCOM_SHOW_SLAVE_STAT:
  {
    /* Accept one of two privileges */
    if (check_global_access(thd, SUPER_ACL | REPL_CLIENT_ACL))
      goto error;
    res= show_slave_status_cmd(thd);
    break;
  }
  case SQLCOM_SHOW_MASTER_STAT:
  {
    /* Accept one of two privileges */
    if (check_global_access(thd, SUPER_ACL | REPL_CLIENT_ACL))
      goto error;
    res = show_master_status(thd);
    break;
  }
  case SQLCOM_SHOW_ENGINE_STATUS:
    {
      if (check_global_access(thd, PROCESS_ACL))
        goto error;
      res = ha_show_status(thd, lex->create_info->db_type, HA_ENGINE_STATUS);
      break;
    }
  case SQLCOM_SHOW_ENGINE_MUTEX:
    {
      if (check_global_access(thd, PROCESS_ACL))
        goto error;
      res = ha_show_status(thd, lex->create_info->db_type, HA_ENGINE_MUTEX);
      break;
    }
  case SQLCOM_START_GROUP_REPLICATION:
  {
    Security_context *sctx= thd->security_context();
    if (!sctx->check_access(SUPER_ACL) &&
        !sctx->has_global_grant(STRING_WITH_LEN("GROUP_REPLICATION_ADMIN")).first)
    {
      my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "SUPER or GROUP_REPLICATION_ADMIN");
      goto error;
    }

    /*
      If the client thread has locked tables, a deadlock is possible.
      Assume that
      - the client thread does LOCK TABLE t READ.
      - then the client thread does START GROUP_REPLICATION.
           -try to make the server in super ready only mode
           -acquire MDL lock ownership which will be waiting for
            LOCK on table t to be released.
      To prevent that, refuse START GROUP_REPLICATION if the
      client thread has locked tables
    */
    if (thd->locked_tables_mode ||
        thd->in_active_multi_stmt_transaction() || thd->in_sub_stmt)
    {
      my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
      goto error;
    }

    char *error_message= NULL;
    res= group_replication_start(&error_message);

    //To reduce server dependency, server errors are not used here
    switch (res)
    {
      case 1: //GROUP_REPLICATION_CONFIGURATION_ERROR
        my_error(ER_GROUP_REPLICATION_CONFIGURATION, MYF(0));
        goto error;
      case 2: //GROUP_REPLICATION_ALREADY_RUNNING
        my_error(ER_GROUP_REPLICATION_RUNNING, MYF(0));
        goto error;
      case 3: //GROUP_REPLICATION_REPLICATION_APPLIER_INIT_ERROR
        my_error(ER_GROUP_REPLICATION_APPLIER_INIT_ERROR, MYF(0));
        goto error;
      case 4: //GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR
        my_error(ER_GROUP_REPLICATION_COMMUNICATION_LAYER_SESSION_ERROR, MYF(0));
        goto error;
      case 5: //GROUP_REPLICATION_COMMUNICATION_LAYER_JOIN_ERROR
        my_error(ER_GROUP_REPLICATION_COMMUNICATION_LAYER_JOIN_ERROR, MYF(0));
        goto error;
      case 7: //GROUP_REPLICATION_MAX_GROUP_SIZE
        my_error(ER_GROUP_REPLICATION_MAX_GROUP_SIZE, MYF(0));
        goto error;
      case 8: //GROUP_REPLICATION_COMMAND_FAILURE
        if (error_message == NULL)
        {
          my_error(ER_GROUP_REPLICATION_COMMAND_FAILURE, MYF(0),
                   "START GROUP_REPLICATION",
                   "Please check error log for additional details.");
        }
        else
        {
          my_error(ER_GROUP_REPLICATION_COMMAND_FAILURE, MYF(0),
                   "START GROUP_REPLICATION", error_message);
          my_free(error_message);
        }
        goto error;
    }
    my_ok(thd);
    res= 0;
    break;
  }

  case SQLCOM_STOP_GROUP_REPLICATION:
  {
    Security_context *sctx= thd->security_context();
    if (!sctx->check_access(SUPER_ACL) &&
        !sctx->has_global_grant(STRING_WITH_LEN("GROUP_REPLICATION_ADMIN")).first)
    {
      my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "SUPER or GROUP_REPLICATION_ADMIN");
      goto error;
    }

    /*
      Please see explanation @SQLCOM_SLAVE_STOP case
      to know the reason for thd->locked_tables_mode in
      the below if condition.
    */
    if (thd->locked_tables_mode ||
        thd->in_active_multi_stmt_transaction() || thd->in_sub_stmt)
    {
      my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
      goto error;
    }

    char *error_message= NULL;
    res= group_replication_stop(&error_message);
    if (res == 1) //GROUP_REPLICATION_CONFIGURATION_ERROR
    {
      my_error(ER_GROUP_REPLICATION_CONFIGURATION, MYF(0));
      goto error;
    }
    if (res == 6) //GROUP_REPLICATION_APPLIER_THREAD_TIMEOUT
    {
      my_error(ER_GROUP_REPLICATION_STOP_APPLIER_THREAD_TIMEOUT, MYF(0));
      goto error;
    }
    if (res == 8) //GROUP_REPLICATION_COMMAND_FAILURE
    {
      if (error_message == NULL)
      {
        my_error(ER_GROUP_REPLICATION_COMMAND_FAILURE, MYF(0),
                 "STOP GROUP_REPLICATION",
                 "Please check error log for additonal details.");
      }
      else
      {
        my_error(ER_GROUP_REPLICATION_COMMAND_FAILURE, MYF(0),
                 "STOP GROUP_REPLICATION", error_message);
        my_free(error_message);
      }
      goto error;
    }
    my_ok(thd);
    res= 0;
    break;
  }

  case SQLCOM_SLAVE_START:
  {
    res= start_slave_cmd(thd);
    break;
  }
  case SQLCOM_SLAVE_STOP:
  {
  /*
    If the client thread has locked tables, a deadlock is possible.
    Assume that
    - the client thread does LOCK TABLE t READ.
    - then the master updates t.
    - then the SQL slave thread wants to update t,
      so it waits for the client thread because t is locked by it.
    - then the client thread does SLAVE STOP.
      SLAVE STOP waits for the SQL slave thread to terminate its
      update t, which waits for the client thread because t is locked by it.
    To prevent that, refuse SLAVE STOP if the
    client thread has locked tables
  */
  if (thd->locked_tables_mode ||
      thd->in_active_multi_stmt_transaction() || thd->global_read_lock.is_acquired())
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    goto error;
  }

  res= stop_slave_cmd(thd);
  break;
  }
  case SQLCOM_RENAME_TABLE:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    TABLE_LIST *table;
    for (table= first_table; table; table= table->next_local->next_local)
    {
      if (check_access(thd, ALTER_ACL | DROP_ACL, table->db,
                       &table->grant.privilege,
                       &table->grant.m_internal,
                       0, 0) ||
          check_access(thd, INSERT_ACL | CREATE_ACL, table->next_local->db,
                       &table->next_local->grant.privilege,
                       &table->next_local->grant.m_internal,
                       0, 0))
	goto error;
      TABLE_LIST old_list, new_list;
      /*
        we do not need initialize old_list and new_list because we will
        come table[0] and table->next[0] there
      */
      old_list= table[0];
      new_list= table->next_local[0];
      /*
        It's not clear what the above assignments actually want to
        accomplish. What we do know is that they do *not* want to copy the MDL
        requests, so we overwrite them with uninitialized request.
      */
      old_list.mdl_request= MDL_request();
      new_list.mdl_request= MDL_request();

      if (check_grant(thd, ALTER_ACL | DROP_ACL, &old_list, false, 1, false) ||
         (!test_all_bits(table->next_local->grant.privilege,
                         INSERT_ACL | CREATE_ACL) &&
          check_grant(thd, INSERT_ACL | CREATE_ACL, &new_list, false, 1,
                      false)))
        goto error;
    }

    if (mysql_rename_tables(thd, first_table))
      goto error;
    break;
  }
  case SQLCOM_SHOW_BINLOGS:
    {
      if (check_global_access(thd, SUPER_ACL | REPL_CLIENT_ACL))
	goto error;
      res = show_binlogs(thd);
      break;
    }
  case SQLCOM_SHOW_CREATE:
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    {
     /*
        Access check:
        SHOW CREATE TABLE require any privileges on the table level (ie
        effecting all columns in the table).
        SHOW CREATE VIEW require the SHOW_VIEW and SELECT ACLs on the table
        level.
        NOTE: SHOW_VIEW ACL is checked when the view is created.
      */

      DBUG_PRINT("debug", ("lex->only_view: %d, table: %s.%s",
                           lex->only_view,
                           first_table->db, first_table->table_name));
      if (lex->only_view)
      {
        if (check_table_access(thd, SELECT_ACL, first_table, false, 1, false))
        {
          DBUG_PRINT("debug", ("check_table_access failed"));
          my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                   "SHOW", thd->security_context()->priv_user().str,
                   thd->security_context()->host_or_ip().str,
                   first_table->alias);
          goto error;
        }
        DBUG_PRINT("debug", ("check_table_access succeeded"));

        /* Ignore temporary tables if this is "SHOW CREATE VIEW" */
        first_table->open_type= OT_BASE_ONLY;

      }
      else
      {
        /*
          Temporary tables should be opened for SHOW CREATE TABLE, but not
          for SHOW CREATE VIEW.
        */
        if (open_temporary_tables(thd, all_tables))
          goto error;

        /*
          The fact that check_some_access() returned false does not mean that
          access is granted. We need to check if first_table->grant.privilege
          contains any table-specific privilege.
        */
        DBUG_PRINT("debug", ("first_table->grant.privilege: %lx",
                             first_table->grant.privilege));
        if (check_some_access(thd, SHOW_CREATE_TABLE_ACLS, first_table) ||
            (first_table->grant.privilege & SHOW_CREATE_TABLE_ACLS) == 0)
        {
          my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                   "SHOW", thd->security_context()->priv_user().str,
                   thd->security_context()->host_or_ip().str,
                   first_table->alias);
          goto error;
        }
      }

      /* Access is granted. Execute the command.  */
      res= mysqld_show_create(thd, first_table);
      break;
    }
  case SQLCOM_CHECKSUM:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    if (check_table_access(thd, SELECT_ACL, all_tables,
                           false, UINT_MAX, false))
      goto error; /* purecov: inspected */

    res = mysql_checksum_table(thd, first_table, &lex->check_opt);
    break;
  }
  case SQLCOM_REPLACE:
  case SQLCOM_INSERT:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_CREATE_INDEX:
  case SQLCOM_DROP_INDEX:
  case SQLCOM_ASSIGN_TO_KEYCACHE:
  case SQLCOM_PRELOAD_KEYS:
  case SQLCOM_LOAD:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    DBUG_ASSERT(lex->m_sql_cmd != NULL);
    res= lex->m_sql_cmd->execute(thd);
    break;
  }
  case SQLCOM_DROP_TABLE:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    if (!lex->drop_temporary)
    {
      if (check_table_access(thd, DROP_ACL, all_tables, false, UINT_MAX, false))
	goto error;				/* purecov: inspected */
    }
    /* DDL and binlog write order are protected by metadata locks. */
    res= mysql_rm_table(thd, first_table, lex->drop_if_exists,
			lex->drop_temporary);
    /* when dropping temporary tables if @@session_track_state_change is ON then
       send the boolean tracker in the OK packet */
    if(!res && lex->drop_temporary)
    {
      if (thd->session_tracker.get_tracker(SESSION_STATE_CHANGE_TRACKER)->is_enabled())
        thd->session_tracker.get_tracker(SESSION_STATE_CHANGE_TRACKER)->mark_as_changed(thd, NULL);
    }
  }
  break;
  case SQLCOM_SHOW_PROCESSLIST:
    if (!thd->security_context()->priv_user().str[0] &&
        check_global_access(thd,PROCESS_ACL))
      break;
    mysqld_list_processes(
      thd,
      (thd->security_context()->check_access(PROCESS_ACL) ?
         NullS :
        thd->security_context()->priv_user().str),
      lex->verbose);
    break;
  case SQLCOM_SHOW_ENGINE_LOGS:
    {
      if (check_access(thd, FILE_ACL, any_db, NULL, NULL, 0, 0))
	goto error;
      res= ha_show_status(thd, lex->create_info->db_type, HA_ENGINE_LOGS);
      break;
    }
  case SQLCOM_CHANGE_DB:
  {
    const LEX_CSTRING db_str= { select_lex->db,
                                strlen(select_lex->db) };

    if (!mysql_change_db(thd, db_str, false))
      my_ok(thd);

    break;
  }

  case SQLCOM_SET_OPTION:
  {
    List<set_var_base> *lex_var_list= &lex->var_list;

    if (check_table_access(thd, SELECT_ACL, all_tables, false, UINT_MAX, false))
      goto error;
    if (open_tables_for_query(thd, all_tables, false))
      goto error;
    if (!(res= sql_set_variables(thd, lex_var_list, true)))
      my_ok(thd);
    else
    {
      /*
        We encountered some sort of error, but no message was sent.
        Send something semi-generic here since we don't know which
        assignment in the list caused the error.
      */
      if (!thd->is_error())
        my_error(ER_WRONG_ARGUMENTS,MYF(0),"SET");
      goto error;
    }

    break;
  }
  case SQLCOM_SET_PASSWORD:
  {
    List<set_var_base> *lex_var_list= &lex->var_list;

    DBUG_ASSERT(lex_var_list->elements == 1);
    DBUG_ASSERT(all_tables == NULL);

    if (!(res= sql_set_variables(thd, lex_var_list, false)))
    {
      my_ok(thd);
    }
    else
    {
      // We encountered some sort of error, but no message was sent.
      if (!thd->is_error())
        my_error(ER_WRONG_ARGUMENTS,MYF(0),"SET PASSWORD");
      goto error;
    }

    break;
  }

  case SQLCOM_UNLOCK_TABLES:
    /*
      It is critical for mysqldump --single-transaction --master-data that
      UNLOCK TABLES does not implicitely commit a connection which has only
      done FLUSH TABLES WITH READ LOCK + BEGIN. If this assumption becomes
      false, mysqldump will not work.
    */
    if (thd->variables.option_bits & OPTION_TABLE_LOCK)
    {
      /*
        Can we commit safely? If not, return to avoid releasing
        transactional metadata locks.
      */
      if (trans_check_state(thd))
        DBUG_RETURN(-1);
      res= trans_commit_implicit(thd);
      thd->locked_tables_list.unlock_locked_tables(thd);
      thd->mdl_context.release_transactional_locks();
      thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
    }
    if (thd->global_read_lock.is_acquired())
      thd->global_read_lock.unlock_global_read_lock(thd);
    if (res)
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_LOCK_TABLES:
    /*
      Can we commit safely? If not, return to avoid releasing
      transactional metadata locks.
    */
    if (trans_check_state(thd))
      DBUG_RETURN(-1);
    /* We must end the transaction first, regardless of anything */
    res= trans_commit_implicit(thd);
    thd->locked_tables_list.unlock_locked_tables(thd);
    /* Release transactional metadata locks. */
    thd->mdl_context.release_transactional_locks();
    if (res)
      goto error;

    /*
      Here we have to pre-open temporary tables for LOCK TABLES.

      CF_PREOPEN_TMP_TABLES is not set for this SQL statement simply
      because LOCK TABLES calls close_thread_tables() as a first thing
      (it's called from unlock_locked_tables() above). So even if
      CF_PREOPEN_TMP_TABLES was set and the tables would be pre-opened
      in a usual way, they would have been closed.
    */

    if (open_temporary_tables(thd, all_tables))
      goto error;

    if (lock_tables_precheck(thd, all_tables))
      goto error;

    thd->variables.option_bits|= OPTION_TABLE_LOCK;

    res= lock_tables_open_and_lock_tables(thd, all_tables);

    if (res)
    {
      thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
    }
    else
    {
      my_ok(thd);
    }
    break;

  case SQLCOM_IMPORT:
    res= lex->m_sql_cmd->execute(thd);
    break;
  case SQLCOM_CREATE_DB:
  {
    const char* alias;
    if (!(alias=thd->strmake(lex->name.str, lex->name.length)) ||
        (check_and_convert_db_name(&lex->name, false) != Ident_name_check::OK))
      break;
    /*
      If in a slave thread :
      CREATE DATABASE DB was certainly not preceded by USE DB.
      For that reason, db_ok() in sql/slave.cc did not check the
      do_db/ignore_db. And as this query involves no tables, tables_ok()
      above was not called. So we have to check rules again here.
    */
    if (!db_stmt_db_ok(thd, lex->name.str))
    {
      my_error(ER_SLAVE_IGNORED_TABLE, MYF(0));
      break;
    }
    if (check_access(thd, CREATE_ACL, lex->name.str, NULL, NULL, 1, 0))
      break;
    /*
      As mysql_create_db() may modify HA_CREATE_INFO structure passed to
      it, we need to use a copy of LEX::create_info to make execution
      prepared statement- safe.
    */
    HA_CREATE_INFO create_info(*lex->create_info);
    res= mysql_create_db(thd, (lower_case_table_names == 2 ? alias :
                               lex->name.str), &create_info);
    break;
  }
  case SQLCOM_DROP_DB:
  {
    if (check_and_convert_db_name(&lex->name, false) != Ident_name_check::OK)
      break;
    /*
      If in a slave thread :
      DROP DATABASE DB may not be preceded by USE DB.
      For that reason, maybe db_ok() in sql/slave.cc did not check the 
      do_db/ignore_db. And as this query involves no tables, tables_ok()
      above was not called. So we have to check rules again here.
    */
    if (!db_stmt_db_ok(thd, lex->name.str))
    {
      my_error(ER_SLAVE_IGNORED_TABLE, MYF(0));
      break;
    }
    if (check_access(thd, DROP_ACL, lex->name.str, NULL, NULL, 1, 0))
      break;
    res= mysql_rm_db(thd, to_lex_cstring(lex->name), lex->drop_if_exists);
    break;
  }
  case SQLCOM_ALTER_DB:
  {
    if (check_and_convert_db_name(&lex->name, false) != Ident_name_check::OK)
      break;
    /*
      If in a slave thread :
      ALTER DATABASE DB may not be preceded by USE DB.
      For that reason, maybe db_ok() in sql/slave.cc did not check the
      do_db/ignore_db. And as this query involves no tables, tables_ok()
      above was not called. So we have to check rules again here.
    */
    if (!db_stmt_db_ok(thd, lex->name.str))
    {
      my_error(ER_SLAVE_IGNORED_TABLE, MYF(0));
      break;
    }
    if (check_access(thd, ALTER_ACL, lex->name.str, NULL, NULL, 1, 0))
      break;
    /*
      As mysql_alter_db() may modify HA_CREATE_INFO structure passed to
      it, we need to use a copy of LEX::create_info to make execution
      prepared statement- safe.
    */
    HA_CREATE_INFO create_info(*lex->create_info);
    res= mysql_alter_db(thd, lex->name.str, &create_info);
    break;
  }
  case SQLCOM_SHOW_CREATE_DB:
  {
    DBUG_EXECUTE_IF("4x_server_emul",
                    my_error(ER_UNKNOWN_ERROR, MYF(0)); goto error;);
    if (check_and_convert_db_name(&lex->name, true) != Ident_name_check::OK)
      break;
    res= mysqld_show_create_db(thd, lex->name.str, lex->create_info);
    break;
  }
  case SQLCOM_CREATE_EVENT:
  case SQLCOM_ALTER_EVENT:
  do
  {
    DBUG_ASSERT(lex->event_parse_data);
    if (lex->table_or_sp_used())
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), "Usage of subqueries or stored "
               "function calls as part of this statement");
      break;
    }

    res= sp_process_definer(thd);
    if (res)
      break;

    switch (lex->sql_command) {
    case SQLCOM_CREATE_EVENT:
    {
      bool if_not_exists= (lex->create_info->options &
                           HA_LEX_CREATE_IF_NOT_EXISTS);
      res= Events::create_event(thd, lex->event_parse_data, if_not_exists);
      break;
    }
    case SQLCOM_ALTER_EVENT:
    {
      LEX_STRING db_lex_str= NULL_STR;
      if (lex->spname)
      {
        db_lex_str.str= const_cast<char*>(lex->spname->m_db.str);
        db_lex_str.length= lex->spname->m_db.length;
      }

      res= Events::update_event(thd, lex->event_parse_data,
                                lex->spname ? &db_lex_str : NULL,
                                lex->spname ? &lex->spname->m_name : NULL);
      break;
    }
    default:
      DBUG_ASSERT(0);
    }
    DBUG_PRINT("info",("DDL error code=%d", res));
    if (!res && !thd->killed)
      my_ok(thd);

  } while (0);
  /* Don't do it, if we are inside a SP */
  if (!thd->sp_runtime_ctx)
  {
    sp_head::destroy(lex->sphead);
    lex->sphead= NULL;
  }
  /* lex->unit->cleanup() is called outside, no need to call it here */
  break;
  case SQLCOM_SHOW_CREATE_EVENT:
  {
    LEX_STRING db_lex_str= {const_cast<char*>(lex->spname->m_db.str),
                              lex->spname->m_db.length};
    res= Events::show_create_event(thd, db_lex_str,
                                   lex->spname->m_name);
    break;
  }
  case SQLCOM_DROP_EVENT:
  {
    LEX_STRING db_lex_str= {const_cast<char*>(lex->spname->m_db.str),
                              lex->spname->m_db.length};
    if (!(res= Events::drop_event(thd,
                                  db_lex_str, lex->spname->m_name,
                                  lex->drop_if_exists)))
        my_ok(thd);
    break;
  }
  case SQLCOM_CREATE_FUNCTION:                  // UDF function
  {
    if (check_access(thd, INSERT_ACL, "mysql", NULL, NULL, 1, 0))
      break;
    if (!(res = mysql_create_function(thd, &lex->udf)))
      my_ok(thd);
    break;
  }
  case SQLCOM_CREATE_USER:
  {
    if (check_access(thd, INSERT_ACL, "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;
    /* Conditionally writes to binlog */
    HA_CREATE_INFO create_info(*lex->create_info);
    if (!(res = mysql_create_user(thd, lex->users_list, create_info.options & HA_LEX_CREATE_IF_NOT_EXISTS, false)))
      my_ok(thd);
    break;
  }
  case SQLCOM_DROP_USER:
  {
    if (check_access(thd, DELETE_ACL, "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;
    /* Conditionally writes to binlog */
    if (!(res = mysql_drop_user(thd, lex->users_list, lex->drop_if_exists)))
      my_ok(thd);

    break;
  }
  case SQLCOM_RENAME_USER:
  {
    if (check_access(thd, UPDATE_ACL, "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;
    /* Conditionally writes to binlog */
    if (!(res= mysql_rename_user(thd, lex->users_list)))
      my_ok(thd);
    break;
  }
  case SQLCOM_REVOKE_ALL:
  {
    if (check_access(thd, UPDATE_ACL, "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;

    /* Replicate current user as grantor */
    thd->binlog_invoker();

    /* Conditionally writes to binlog */
    if (!(res = mysql_revoke_all(thd, lex->users_list)))
      my_ok(thd);
    break;
  }
  case SQLCOM_REVOKE:
  case SQLCOM_GRANT:
  {
    /*
      Skip access check if we're granting a proxy 
    */
    if (lex->type != TYPE_ENUM_PROXY)
    {
      /*
        If there are static grants in the GRANT statement or there are no
        dynamic privileges we perform check_access on GRANT_OPTION based on 
        static global privilege level and set the DA accordingly.
      */
      if (lex->grant > 0 || lex->dynamic_privileges.elements == 0)
      {
        /*
          check_access sets DA error message based on GRANT arguments.
        */
        if (check_access(thd, lex->grant | lex->grant_tot_col | GRANT_ACL,
                         first_table ?  first_table->db : select_lex->db,
                         first_table ? &first_table->grant.privilege : NULL,
                         first_table ? &first_table->grant.m_internal : NULL,
                         first_table ? 0 : 1, 0))
        {      
          goto error;
        }
      }
      /*
        ..else we still call check_access to load internal structures, but defer
        checking of global dynamic GRANT_OPTION to mysql_grant.
        We still ignore checks if this was a grant of a proxy.
      */
      else
      {
        /*
          check_access will load grant.privilege and grant.m_internal with values
          which are used later during column privilege checking.
          The return value isn't interesting as we'll check for dynamic global
          privileges later.
        */
        check_access(thd, lex->grant | lex->grant_tot_col | GRANT_ACL,
                     first_table ?  first_table->db : select_lex->db,
                     first_table ? &first_table->grant.privilege : NULL,
                     first_table ? &first_table->grant.m_internal : NULL,
                     first_table ? 0 : 1, 1);
      }
    }

    /* Replicate current user as grantor */
    thd->binlog_invoker();

    if (thd->security_context()->user().str)            // If not replication
    {
      LEX_USER *user, *tmp_user;
      bool first_user= true;

      List_iterator <LEX_USER> user_list(lex->users_list);
      while ((tmp_user= user_list++))
      {
        if (!(user= get_current_user(thd, tmp_user)))
          goto error;
        if (specialflag & SPECIAL_NO_RESOLVE &&
            hostname_requires_resolving(user->host.str))
          push_warning(thd, Sql_condition::SL_WARNING,
                       ER_WARN_HOSTNAME_WONT_WORK,
                       ER_THD(thd, ER_WARN_HOSTNAME_WONT_WORK));
        // Are we trying to change a password of another user
        DBUG_ASSERT(user->host.str != 0);

        /*
          GRANT/REVOKE PROXY has the target user as a first entry in the list. 
         */
        if (lex->type == TYPE_ENUM_PROXY && first_user)
        {
          first_user= false;
          if (acl_check_proxy_grant_access (thd, user->host.str, user->user.str,
                                        lex->grant & GRANT_ACL))
            goto error;
        }
        else if (is_acl_user(thd, user->host.str, user->user.str) &&
                 user->auth.str &&
                 check_change_password(thd, user->host.str, user->user.str))
          goto error;
      }
    }
    if (first_table)
    {
      if (lex->type == TYPE_ENUM_PROCEDURE ||
          lex->type == TYPE_ENUM_FUNCTION)
      {
        uint grants= lex->all_privileges 
		   ? (PROC_ACLS & ~GRANT_ACL) | (lex->grant & GRANT_ACL)
		   : lex->grant;
        if (check_grant_routine(thd, grants | GRANT_ACL, all_tables,
                                lex->type == TYPE_ENUM_PROCEDURE, 0))
	  goto error;
        /* Conditionally writes to binlog */
        res= mysql_routine_grant(thd, all_tables,
                                 lex->type == TYPE_ENUM_PROCEDURE, 
                                 lex->users_list, grants,
                                 lex->sql_command == SQLCOM_REVOKE, true);
        if (!res)
          my_ok(thd);
      }
      else
      {
	if (check_grant(thd,(lex->grant | lex->grant_tot_col | GRANT_ACL),
                        all_tables, false, UINT_MAX, false))
	  goto error;
        if (lex->dynamic_privileges.elements > 0)
        {
          my_error(ER_ILLEGAL_PRIVILEGE_LEVEL, MYF(0), all_tables->table_name);
          goto error;
        }
        /* Conditionally writes to binlog */
        res= mysql_table_grant(thd, all_tables, lex->users_list,
			       lex->columns, lex->grant,
			       lex->sql_command == SQLCOM_REVOKE);
      }
    }
    else
    {
      if (lex->columns.elements || (lex->type && lex->type != TYPE_ENUM_PROXY))
      {
	my_error(ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0));
        goto error;
      }
      else
      {
        /* Conditionally writes to binlog */
        res = mysql_grant(thd, select_lex->db, lex->users_list, lex->grant,
                          lex->sql_command == SQLCOM_REVOKE,
                          lex->type == TYPE_ENUM_PROXY,
                          lex->dynamic_privileges,
                          lex->all_privileges);
      }
    }
    break;
  }
  case SQLCOM_RESET:
    /*
      RESET commands are never written to the binary log, so we have to
      initialize this variable because RESET shares the same code as FLUSH
    */
    lex->no_write_to_binlog= 1;
    if ((lex->type & REFRESH_PERSIST) && (lex->option_type == OPT_PERSIST))
    {
      Persisted_variables_cache *pv= Persisted_variables_cache::get_instance();
      if (pv)
        if (pv->reset_persisted_variables(thd, lex->name.str,
            lex->drop_if_exists))
          goto error;
      my_ok(thd);
      break;
    }
    // Fall through.
  case SQLCOM_FLUSH:
  {
    int write_to_binlog;
    if (check_global_access(thd,RELOAD_ACL))
      goto error;

    if (first_table && lex->type & REFRESH_READ_LOCK)
    {
      /* Check table-level privileges. */
      if (check_table_access(thd, LOCK_TABLES_ACL | SELECT_ACL, all_tables,
                             false, UINT_MAX, false))
        goto error;
      if (flush_tables_with_read_lock(thd, all_tables))
        goto error;
      my_ok(thd);
      break;
    }
    else if (first_table && lex->type & REFRESH_FOR_EXPORT)
    {
      /* Check table-level privileges. */
      if (check_table_access(thd, LOCK_TABLES_ACL | SELECT_ACL, all_tables,
                             false, UINT_MAX, false))
        goto error;
      if (flush_tables_for_export(thd, all_tables))
        goto error;
      my_ok(thd);
      break;
    }

    /*
      handle_reload_request() will tell us if we are allowed to write to the
      binlog or not.
    */
    if (!handle_reload_request(thd, lex->type, first_table, &write_to_binlog))
    {
      /*
        We WANT to write and we CAN write.
        ! we write after unlocking the table.
      */
      /*
        Presumably, RESET and binlog writing doesn't require synchronization
      */

      if (write_to_binlog > 0)  // we should write
      { 
        if (!lex->no_write_to_binlog)
          res= write_bin_log(thd, false, thd->query().str, thd->query().length);
      } else if (write_to_binlog < 0) 
      {
        /* 
           We should not write, but rather report error because 
           handle_reload_request binlog interactions failed 
         */
        res= 1;
      } 

      if (!res)
        my_ok(thd);
    } 
    
    break;
  }
  case SQLCOM_KILL:
  {
    Item *it= lex->kill_value_list.head();

    if (lex->table_or_sp_used())
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), "Usage of subqueries or stored "
               "function calls as part of this statement");
      goto error;
    }

    if ((!it->fixed && it->fix_fields(lex->thd, &it)) || it->check_cols(1))
    {
      my_error(ER_SET_CONSTANTS_ONLY, MYF(0));
      goto error;
    }

    my_thread_id thread_id= static_cast<my_thread_id>(it->val_int());
    if (thd->is_error())
      goto error;

    sql_kill(thd, thread_id, lex->type & ONLY_KILL_QUERY);
    break;
  }
  case SQLCOM_SHOW_PRIVILEGES:
  {
    mysqld_show_privileges(thd);
    break;
  }
  case SQLCOM_SHOW_CREATE_USER:
  {
    LEX_USER *show_user= get_current_user(thd, lex->grant_user);
    if (!(strcmp(thd->security_context()->priv_user().str, show_user->user.str) ||
         my_strcasecmp(system_charset_info, show_user->host.str,
                              thd->security_context()->priv_host().str)) ||
        !check_access(thd, SELECT_ACL, "mysql", NULL, NULL, 1, 0))
      res= mysql_show_create_user(thd, show_user);
    break;
  }
  case SQLCOM_BEGIN:
    if (trans_begin(thd, lex->start_transaction_opt))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_COMMIT:
  {
    DBUG_ASSERT(thd->lock == NULL ||
                thd->locked_tables_mode == LTM_LOCK_TABLES);
    bool tx_chain= (lex->tx_chain == TVL_YES ||
                    (thd->variables.completion_type == 1 &&
                     lex->tx_chain != TVL_NO));
    bool tx_release= (lex->tx_release == TVL_YES ||
                      (thd->variables.completion_type == 2 &&
                       lex->tx_release != TVL_NO));
    if (trans_commit(thd))
      goto error;
    thd->mdl_context.release_transactional_locks();
    /* Begin transaction with the same isolation level. */
    if (tx_chain)
    {
      if (trans_begin(thd))
      goto error;
    }
    else
    {
      /* Reset the isolation level and access mode if no chaining transaction.*/
      trans_reset_one_shot_chistics(thd);
    }
    /* Disconnect the current client connection. */
    if (tx_release)
      thd->killed= THD::KILL_CONNECTION;
    my_ok(thd);
    break;
  }
  case SQLCOM_ROLLBACK:
  {
    DBUG_ASSERT(thd->lock == NULL ||
                thd->locked_tables_mode == LTM_LOCK_TABLES);
    bool tx_chain= (lex->tx_chain == TVL_YES ||
                    (thd->variables.completion_type == 1 &&
                     lex->tx_chain != TVL_NO));
    bool tx_release= (lex->tx_release == TVL_YES ||
                      (thd->variables.completion_type == 2 &&
                       lex->tx_release != TVL_NO));
    if (trans_rollback(thd))
      goto error;
    thd->mdl_context.release_transactional_locks();
    /* Begin transaction with the same isolation level. */
    if (tx_chain)
    {
      if (trans_begin(thd))
        goto error;
    }
    else
    {
      /* Reset the isolation level and access mode if no chaining transaction.*/
      trans_reset_one_shot_chistics(thd);
    }
    /* Disconnect the current client connection. */
    if (tx_release)
      thd->killed= THD::KILL_CONNECTION;
    my_ok(thd);
    break;
  }
  case SQLCOM_RELEASE_SAVEPOINT:
    if (trans_release_savepoint(thd, lex->ident))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_ROLLBACK_TO_SAVEPOINT:
    if (trans_rollback_to_savepoint(thd, lex->ident))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_SAVEPOINT:
    if (trans_savepoint(thd, lex->ident))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_CREATE_PROCEDURE:
  case SQLCOM_CREATE_SPFUNCTION:
  {
    uint namelen;
    char *name;

    DBUG_ASSERT(lex->sphead != 0);
    DBUG_ASSERT(lex->sphead->m_db.str); /* Must be initialized in the parser */
    /*
      Verify that the database name is allowed, optionally
      lowercase it.
    */
    if (check_and_convert_db_name(&lex->sphead->m_db, false) !=
        Ident_name_check::OK)
      goto error;

    if (check_access(thd, CREATE_PROC_ACL, lex->sphead->m_db.str,
                     NULL, NULL, 0, 0))
      goto error;

    name= lex->sphead->name(&namelen);
    if (lex->sphead->m_type == enum_sp_type::FUNCTION)
    {
      udf_func *udf = find_udf(name, namelen);

      if (udf)
      {
        my_error(ER_UDF_EXISTS, MYF(0), name);
        goto error;
      }
    }

    if (sp_process_definer(thd))
      goto error;

    /*
      Record the CURRENT_USER in binlog. The CURRENT_USER is used on slave to
      grant default privileges when sp_automatic_privileges variable is set.
    */
    thd->binlog_invoker();

    if (! (res= sp_create_routine(thd, lex->sphead, thd->lex->definer)))
    {
      /* only add privileges if really neccessary */

      Security_context security_context;
      bool restore_backup_context= false;
      Security_context *backup= NULL;
      /*
        We're going to issue an implicit GRANT statement so we close all
        open tables. We have to keep metadata locks as this ensures that
        this statement is atomic against concurent FLUSH TABLES WITH READ
        LOCK. Deadlocks which can arise due to fact that this implicit
        statement takes metadata locks should be detected by a deadlock
        detector in MDL subsystem and reported as errors.

        No need to commit/rollback statement transaction, it's not started.

        TODO: Long-term we should either ensure that implicit GRANT statement
              is written into binary log as a separate statement or make both
              creation of routine and implicit GRANT parts of one fully atomic
              statement.
      */
      DBUG_ASSERT(thd->get_transaction()->is_empty(Transaction_ctx::STMT));
      close_thread_tables(thd);
      /*
        Check if invoker exists on slave, then use invoker privilege to
        insert routine privileges to mysql.procs_priv. If invoker is not
        available then consider using definer.

        Check if the definer exists on slave,
        then use definer privilege to insert routine privileges to mysql.procs_priv.

        For current user of SQL thread has GLOBAL_ACL privilege,
        which doesn't any check routine privileges,
        so no routine privilege record  will insert into mysql.procs_priv.
      */

      if (thd->slave_thread)
      {
        LEX_CSTRING current_user;
        LEX_CSTRING current_host;
        if (thd->has_invoker())
        {
          current_host= thd->get_invoker_host();
          current_user= thd->get_invoker_user();
        }
        else
        {
          current_host= lex->definer->host;
          current_user= lex->definer->user;
        }
        if (is_acl_user(thd, current_host.str, current_user.str))
        {
          security_context.change_security_context(thd,
                                                   current_user,
                                                   current_host,
                                                   &thd->lex->sphead->m_db,
                                                   &backup);
          restore_backup_context= true;
        }
      }

      if (sp_automatic_privileges && !opt_noacl &&
          check_routine_access(thd, DEFAULT_CREATE_PROC_ACLS,
                               lex->sphead->m_db.str, name,
                               lex->sql_command == SQLCOM_CREATE_PROCEDURE, 1))
      {
        if (sp_grant_privileges(thd, lex->sphead->m_db.str, name,
                                lex->sql_command == SQLCOM_CREATE_PROCEDURE))
          push_warning(thd, Sql_condition::SL_WARNING,
                       ER_PROC_AUTO_GRANT_FAIL,
                       ER_THD(thd, ER_PROC_AUTO_GRANT_FAIL));
        thd->clear_error();
      }

      /*
        Restore current user with GLOBAL_ACL privilege of SQL thread
      */
      if (restore_backup_context)
      {
        DBUG_ASSERT(thd->slave_thread == 1);
        thd->security_context()->restore_security_context(thd, backup);
      }
      my_ok(thd);
    }
    break; /* break super switch */
  } /* end case group bracket */

  case SQLCOM_ALTER_PROCEDURE:
  case SQLCOM_ALTER_FUNCTION:
    {
      if (check_routine_access(thd, ALTER_PROC_ACL, lex->spname->m_db.str,
                               lex->spname->m_name.str,
                               lex->sql_command == SQLCOM_ALTER_PROCEDURE,
                               false))
        goto error;

      enum_sp_type sp_type= (lex->sql_command == SQLCOM_ALTER_PROCEDURE) ?
                            enum_sp_type::PROCEDURE : enum_sp_type::FUNCTION;
      /*
        Note that if you implement the capability of ALTER FUNCTION to
        alter the body of the function, this command should be made to
        follow the restrictions that log-bin-trust-function-creators=0
        already puts on CREATE FUNCTION.
      */
      /* Conditionally writes to binlog */
      res= sp_update_routine(thd, sp_type, lex->spname, &lex->sp_chistics);
      if (res || thd->killed)
        goto error;

      my_ok(thd);
      break;
    }
  case SQLCOM_DROP_PROCEDURE:
  case SQLCOM_DROP_FUNCTION:
    {
      if (lex->sql_command == SQLCOM_DROP_FUNCTION &&
          ! lex->spname->m_explicit_name)
      {
        /* DROP FUNCTION <non qualified name> */
        udf_func *udf = find_udf(lex->spname->m_name.str,
                                 lex->spname->m_name.length);
        if (udf)
        {
          if (check_access(thd, DELETE_ACL, "mysql", NULL, NULL, 1, 0))
            goto error;

          if (!(res = mysql_drop_function(thd, &lex->spname->m_name)))
          {
            my_ok(thd);
            break;
          }
          my_error(ER_SP_DROP_FAILED, MYF(0),
                   "FUNCTION (UDF)", lex->spname->m_name.str);
          goto error;
        }

        if (lex->spname->m_db.str == NULL)
        {
          if (lex->drop_if_exists)
          {
            push_warning_printf(thd, Sql_condition::SL_NOTE,
                                ER_SP_DOES_NOT_EXIST,
                                ER_THD(thd, ER_SP_DOES_NOT_EXIST),
                                "FUNCTION (UDF)", lex->spname->m_name.str);
            res= false;
            my_ok(thd);
            break;
          }
          my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
                   "FUNCTION (UDF)", lex->spname->m_name.str);
          goto error;
        }
        /* Fall thought to test for a stored function */
      }

      const char *db= lex->spname->m_db.str;
      char *name= lex->spname->m_name.str;

      if (check_routine_access(thd, ALTER_PROC_ACL, db, name,
                               lex->sql_command == SQLCOM_DROP_PROCEDURE,
                               false))
        goto error;

      enum_sp_type sp_type= (lex->sql_command == SQLCOM_DROP_PROCEDURE) ?
                            enum_sp_type::PROCEDURE : enum_sp_type::FUNCTION;

      /* Conditionally writes to binlog */
      enum_sp_return_code sp_result= sp_drop_routine(thd, sp_type,
                                                     lex->spname);

      /*
        We're going to issue an implicit REVOKE statement so we close all
        open tables. We have to keep metadata locks as this ensures that
        this statement is atomic against concurent FLUSH TABLES WITH READ
        LOCK. Deadlocks which can arise due to fact that this implicit
        statement takes metadata locks should be detected by a deadlock
        detector in MDL subsystem and reported as errors.

        No need to commit/rollback statement transaction, it's not started.

        TODO: Long-term we should either ensure that implicit REVOKE statement
              is written into binary log as a separate statement or make both
              dropping of routine and implicit REVOKE parts of one fully atomic
              statement.
      */
      DBUG_ASSERT(thd->get_transaction()->is_empty(Transaction_ctx::STMT));
      close_thread_tables(thd);

      if (sp_result != SP_DOES_NOT_EXISTS &&
          sp_automatic_privileges && !opt_noacl &&
          sp_revoke_privileges(thd, db, name,
                               lex->sql_command == SQLCOM_DROP_PROCEDURE))
      {
        push_warning(thd, Sql_condition::SL_WARNING,
                     ER_PROC_AUTO_REVOKE_FAIL,
                     ER_THD(thd, ER_PROC_AUTO_REVOKE_FAIL));
        /* If this happens, an error should have been reported. */
        goto error;
      }

      res= sp_result;
      switch (sp_result) {
      case SP_OK:
        my_ok(thd);
        break;
      case SP_DOES_NOT_EXISTS:
        if (lex->drop_if_exists)
        {
          res= write_bin_log(thd, true, thd->query().str, thd->query().length);
          push_warning_printf(thd, Sql_condition::SL_NOTE,
                              ER_SP_DOES_NOT_EXIST,
                              ER_THD(thd, ER_SP_DOES_NOT_EXIST),
                              SP_COM_STRING(lex), lex->spname->m_qname.str);
          if (!res)
            my_ok(thd);
          break;
        }
        my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
                 SP_COM_STRING(lex), lex->spname->m_qname.str);
        goto error;
      default:
        my_error(ER_SP_DROP_FAILED, MYF(0),
                 SP_COM_STRING(lex), lex->spname->m_qname.str);
        goto error;
      }
      break;
    }
  case SQLCOM_SHOW_CREATE_PROC:
    {
      if (sp_show_create_routine(thd, enum_sp_type::PROCEDURE, lex->spname))
        goto error;
      break;
    }
  case SQLCOM_SHOW_CREATE_FUNC:
    {
      if (sp_show_create_routine(thd, enum_sp_type::FUNCTION, lex->spname))
	goto error;
      break;
    }
  case SQLCOM_SHOW_PROC_CODE:
  case SQLCOM_SHOW_FUNC_CODE:
    {
#ifndef DBUG_OFF
      sp_head *sp;
      enum_sp_type sp_type= (lex->sql_command == SQLCOM_SHOW_PROC_CODE) ?
                            enum_sp_type::PROCEDURE : enum_sp_type::FUNCTION;

      if (sp_cache_routine(thd, sp_type, lex->spname, false, &sp))
        goto error;
      if (!sp || sp->show_routine_code(thd))
      {
        /* We don't distinguish between errors for now */
        my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
                 SP_COM_STRING(lex), lex->spname->m_name.str);
        goto error;
      }
      break;
#else
      my_error(ER_FEATURE_DISABLED, MYF(0),
               "SHOW PROCEDURE|FUNCTION CODE", "--with-debug");
      goto error;
#endif // ifndef DBUG_OFF
    }
  case SQLCOM_SHOW_CREATE_TRIGGER:
    {
      if (lex->spname->m_name.length > NAME_LEN)
      {
        my_error(ER_TOO_LONG_IDENT, MYF(0), lex->spname->m_name.str);
        goto error;
      }

      if (show_create_trigger(thd, lex->spname))
        goto error; /* Error has been already logged. */

      break;
    }
  case SQLCOM_CREATE_VIEW:
    {
      /*
        Note: SQLCOM_CREATE_VIEW also handles 'ALTER VIEW' commands
        as specified through the thd->lex->create_view_mode flag.
      */
      res= mysql_create_view(thd, first_table, thd->lex->create_view_mode);
      break;
    }
  case SQLCOM_DROP_VIEW:
    {
      if (check_table_access(thd, DROP_ACL, all_tables, false, UINT_MAX, false))
        goto error;
      /* Conditionally writes to binlog. */
      res= mysql_drop_view(thd, first_table);
      break;
    }
  case SQLCOM_CREATE_TRIGGER:
  case SQLCOM_DROP_TRIGGER:
  {
    /* Conditionally writes to binlog. */
    DBUG_ASSERT(lex->m_sql_cmd != nullptr);
    static_cast<Sql_cmd_ddl_trigger_common*>(lex->m_sql_cmd)->set_table(
      all_tables);

    res= lex->m_sql_cmd->execute(thd);
    break;
  }
  case SQLCOM_BINLOG_BASE64_EVENT:
  {
    mysql_client_binlog_statement(thd);
    break;
  }
  case SQLCOM_ANALYZE:
  case SQLCOM_CHECK:
  case SQLCOM_OPTIMIZE:
  case SQLCOM_REPAIR:
  case SQLCOM_TRUNCATE:
  case SQLCOM_ALTER_TABLE:
  case SQLCOM_HA_OPEN:
  case SQLCOM_HA_READ:
  case SQLCOM_HA_CLOSE:
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    /* fall through */
  case SQLCOM_CREATE_SERVER:
  case SQLCOM_CREATE_RESOURCE_GROUP:
  case SQLCOM_ALTER_SERVER:
  case SQLCOM_ALTER_RESOURCE_GROUP:
  case SQLCOM_DROP_RESOURCE_GROUP:
  case SQLCOM_DROP_SERVER:
  case SQLCOM_SET_RESOURCE_GROUP:
  case SQLCOM_SIGNAL:
  case SQLCOM_RESIGNAL:
  case SQLCOM_GET_DIAGNOSTICS:
  case SQLCOM_CHANGE_REPLICATION_FILTER:
  case SQLCOM_XA_START:
  case SQLCOM_XA_END:
  case SQLCOM_XA_PREPARE:
  case SQLCOM_XA_COMMIT:
  case SQLCOM_XA_ROLLBACK:
  case SQLCOM_XA_RECOVER:
  case SQLCOM_INSTALL_PLUGIN:
  case SQLCOM_UNINSTALL_PLUGIN:
  case SQLCOM_INSTALL_COMPONENT:
  case SQLCOM_UNINSTALL_COMPONENT:
  case SQLCOM_SHUTDOWN:
  case SQLCOM_ALTER_INSTANCE:
  case SQLCOM_SELECT:
  case SQLCOM_DO:
  case SQLCOM_CALL:
  case SQLCOM_CREATE_ROLE:
  case SQLCOM_DROP_ROLE:
  case SQLCOM_SET_ROLE:
  case SQLCOM_GRANT_ROLE:
  case SQLCOM_REVOKE_ROLE:
  case SQLCOM_ALTER_USER_DEFAULT_ROLE:
  case SQLCOM_SHOW_GRANTS:
  case SQLCOM_SHOW_FIELDS:
  case SQLCOM_SHOW_KEYS:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_CLONE:
  case SQLCOM_LOCK_INSTANCE:
  case SQLCOM_UNLOCK_INSTANCE:
  case SQLCOM_ALTER_TABLESPACE:
  case SQLCOM_EXPLAIN_OTHER:
  case SQLCOM_RESTART_SERVER:
  case SQLCOM_CREATE_SRS:
  case SQLCOM_DROP_SRS:

    DBUG_ASSERT(lex->m_sql_cmd != nullptr);
    res= lex->m_sql_cmd->execute(thd);
    break;

  case SQLCOM_ALTER_USER:
  {
    LEX_USER *user, *tmp_user;
    bool changing_own_password= false;
    bool own_password_expired= thd->security_context()->password_expired();
    bool check_permission= true;

    List_iterator <LEX_USER> user_list(lex->users_list);
    while ((tmp_user= user_list++))
    {
      bool update_password_only= false;
      bool is_self= false;

      /* If it is an empty lex_user update it with current user */
      if (!tmp_user->host.str && !tmp_user->user.str)
      {
        /* set user information as of the current user */
        DBUG_ASSERT(thd->security_context()->priv_host().str);
        tmp_user->host.str= (char *) thd->security_context()->priv_host().str;
        tmp_user->host.length= strlen(thd->security_context()->priv_host().str);
        DBUG_ASSERT(thd->security_context()->user().str);
        tmp_user->user.str= (char *) thd->security_context()->user().str;
        tmp_user->user.length= strlen(thd->security_context()->user().str);
      }
      if (!(user= get_current_user(thd, tmp_user)))
        goto error;

      /* copy password expire attributes to individual lex user */
      user->alter_status= thd->lex->alter_password;

      if (user->uses_identified_by_clause &&
          !thd->lex->mqh.specified_limits &&
          !user->alter_status.update_account_locked_column &&
          !user->alter_status.update_password_expired_column &&
          !user->alter_status.expire_after_days &&
          user->alter_status.use_default_password_lifetime &&
          (thd->lex->ssl_type == SSL_TYPE_NOT_SPECIFIED))
        update_password_only= true;

      is_self= !strcmp(thd->security_context()->user().length ?
                       thd->security_context()->user().str : "",
                       user->user.str) &&
               !my_strcasecmp(&my_charset_latin1, user->host.str,
                              thd->security_context()->priv_host().str);
      /*
        if user executes ALTER statement to change password only
        for himself then skip access check.
      */
      if (update_password_only && is_self)
      {
        changing_own_password= true;
        continue;
      }
      else if (check_permission)
      {
        if (check_access(thd, UPDATE_ACL, "mysql", NULL, NULL, 1, 1) &&
            check_global_access(thd, CREATE_USER_ACL))
          goto error;

        check_permission= false;
      }

      if (is_self &&
          (user->uses_identified_by_clause ||
           user->uses_identified_with_clause ||
           user->uses_authentication_string_clause))
      {
        changing_own_password= true;
        break;
      }

      if (update_password_only &&
          likely((get_server_state() == SERVER_OPERATING)) &&
          !strcmp(thd->security_context()->priv_user().str,""))
      {
        my_error(ER_PASSWORD_ANONYMOUS_USER, MYF(0));
        goto error;
      }
    }

    if (unlikely(own_password_expired && !changing_own_password))
    {
      my_error(ER_MUST_CHANGE_PASSWORD, MYF(0));
      goto error;
    }

    /* Conditionally writes to binlog */
    if (!(res = mysql_alter_user(thd, lex->users_list, lex->drop_if_exists)))
      my_ok(thd);
    break;
  }
  default:
    DBUG_ASSERT(0);                             /* Impossible */
    my_ok(thd);
    break;
  }
  goto finish;

error:
  res= true;

finish:
  /* Restore system variables which were changed by SET_VAR hint. */
  if (lex->opt_hints_global && lex->opt_hints_global->sys_var_hint)
    lex->opt_hints_global->sys_var_hint->restore_vars(thd);

  THD_STAGE_INFO(thd, stage_query_end);

  if (!res)
    lex->set_exec_started();

  // Cleanup EXPLAIN info
  if (!thd->in_sub_stmt)
  {
    if (is_explainable_query(lex->sql_command))
    {
      DEBUG_SYNC(thd, "before_reset_query_plan");
      /*
        We want EXPLAIN CONNECTION to work until the explained statement ends,
        thus it is only now that we may fully clean up any unit of this statement.
      */
      lex->unit->assert_not_fully_clean();
    }
    thd->query_plan.set_query_plan(SQLCOM_END, NULL, false);
  }

  DBUG_ASSERT(!thd->in_active_multi_stmt_transaction() ||
               thd->in_multi_stmt_transaction_mode());

  if (! thd->in_sub_stmt)
  {
    mysql_audit_notify(thd,
                       first_level ? MYSQL_AUDIT_QUERY_STATUS_END :
                                     MYSQL_AUDIT_QUERY_NESTED_STATUS_END,
                       first_level ? "MYSQL_AUDIT_QUERY_STATUS_END" :
                                     "MYSQL_AUDIT_QUERY_NESTED_STATUS_END");

    /* report error issued during command execution */
    if (thd->killed)
      thd->send_kill_message();
    if (thd->is_error() || (thd->variables.option_bits & OPTION_MASTER_SQL_ERROR))
      trans_rollback_stmt(thd);
    else
    {
      /* If commit fails, we should be able to reset the OK status. */
      thd->get_stmt_da()->set_overwrite_status(true);
      trans_commit_stmt(thd);
      thd->get_stmt_da()->set_overwrite_status(false);
    }
    if (thd->killed == THD::KILL_QUERY ||
        thd->killed == THD::KILL_TIMEOUT)
    {
      thd->killed= THD::NOT_KILLED;
    }
  }

  lex->unit->cleanup(true);
  /* Free tables */
  THD_STAGE_INFO(thd, stage_closing_tables);
  close_thread_tables(thd);

#ifndef DBUG_OFF
  if (lex->sql_command != SQLCOM_SET_OPTION && ! thd->in_sub_stmt)
    DEBUG_SYNC(thd, "execute_command_after_close_tables");
#endif

  if (! thd->in_sub_stmt && thd->transaction_rollback_request)
  {
    /*
      We are not in sub-statement and transaction rollback was requested by
      one of storage engines (e.g. due to deadlock). Rollback transaction in
      all storage engines including binary log.
    */
    trans_rollback_implicit(thd);
    thd->mdl_context.release_transactional_locks();
  }
  else if (stmt_causes_implicit_commit(thd, CF_IMPLICIT_COMMIT_END))
  {
    /* No transaction control allowed in sub-statements. */
    DBUG_ASSERT(! thd->in_sub_stmt);
    /* If commit fails, we should be able to reset the OK status. */
    thd->get_stmt_da()->set_overwrite_status(true);
    /* Commit the normal transaction if one is active. */
    trans_commit_implicit(thd);
    thd->get_stmt_da()->set_overwrite_status(false);
    thd->mdl_context.release_transactional_locks();
  }
  else if (! thd->in_sub_stmt && ! thd->in_multi_stmt_transaction_mode())
  {
    /*
      - If inside a multi-statement transaction,
      defer the release of metadata locks until the current
      transaction is either committed or rolled back. This prevents
      other statements from modifying the table for the entire
      duration of this transaction.  This provides commit ordering
      and guarantees serializability across multiple transactions.
      - If in autocommit mode, or outside a transactional context,
      automatically release metadata locks of the current statement.
    */
    thd->mdl_context.release_transactional_locks();
  }
  else if (! thd->in_sub_stmt)
  {
    thd->mdl_context.release_statement_locks();
  }

  if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
  {
    ((Transaction_state_tracker *)
     thd->session_tracker.get_tracker(TRANSACTION_INFO_TRACKER))
      ->add_trx_state_from_thd(thd);
  }

#ifdef HAVE_LSAN_DO_RECOVERABLE_LEAK_CHECK
  // Get incremental leak reports, for easier leak hunting.
  // ./mtr --mem --mysqld='-T 4096' --sanitize main.1st
  // Don't waste time calling leak sanitizer during bootstrap.
  if (!opt_initialize && (test_flags & TEST_DO_QUICK_LEAK_CHECK))
  {
    int have_leaks= __lsan_do_recoverable_leak_check();
    if (have_leaks > 0)
    {
      fprintf(stderr, "LSAN found leaks for Query: %*s\n",
              static_cast<int>(thd->query().length), thd->query().str);
      fflush(stderr);
    }
  }
#endif

#if defined(VALGRIND_DO_QUICK_LEAK_CHECK)
  // Get incremental leak reports, for easier leak hunting.
  // ./mtr --mem --mysqld='-T 4096' --valgrind-mysqld main.1st
  // Note that with multiple connections, the report below may be misleading.
  if (test_flags & TEST_DO_QUICK_LEAK_CHECK)
  {
    static unsigned long total_leaked_bytes= 0;
    unsigned long leaked= 0;
    unsigned long dubious MY_ATTRIBUTE((unused));
    unsigned long reachable MY_ATTRIBUTE((unused));
    unsigned long suppressed MY_ATTRIBUTE((unused));
    /*
      We could possibly use VALGRIND_DO_CHANGED_LEAK_CHECK here,
      but that is a fairly new addition to the Valgrind api.
      Note: we dont want to check 'reachable' until we have done shutdown,
      and that is handled by the final report anyways.
      We print some extra information, to tell mtr to ignore this report.
    */
    LogErr(INFORMATION_LEVEL, ER_VALGRIND_DO_QUICK_LEAK_CHECK);
    VALGRIND_DO_QUICK_LEAK_CHECK;
    VALGRIND_COUNT_LEAKS(leaked, dubious, reachable, suppressed);
    if (leaked > total_leaked_bytes)
    {
      LogErr(ERROR_LEVEL, ER_VALGRIND_COUNT_LEAKS,
             leaked - total_leaked_bytes,
             static_cast<int>(thd->query().length), thd->query().str);
    }
    total_leaked_bytes= leaked;
  }
#endif

  if (!(res || thd->is_error()))
    binlog_gtid_end_transaction(thd);
  DBUG_RETURN(res || thd->is_error());
}

/**
  Do special checking for SHOW statements.

  @param thd              Thread context.
  @param lex              LEX for SHOW statement.
  @param lock             If true, lock metadata for schema objects

  @returns false if check is successful, true if error
*/

bool show_precheck(THD *thd, LEX *lex, bool lock)
{
  bool new_dd_show= false;

  TABLE_LIST *const tables= lex->query_tables;

  switch (lex->sql_command)
  {
    // For below show commands, perform check_show_access() call
    case SQLCOM_SHOW_DATABASES:
    case SQLCOM_SHOW_EVENTS:
      new_dd_show= true;
      break;

    case SQLCOM_SHOW_TABLES:
    case SQLCOM_SHOW_TABLE_STATUS:
    case SQLCOM_SHOW_TRIGGERS:
    {
      new_dd_show= true;

      if (!lock)
        break;

      LEX_STRING lex_str_db;
      if (make_lex_string_root(thd->mem_root, &lex_str_db,
                               lex->select_lex->db,
                               strlen(lex->select_lex->db), false) == nullptr)
        return true;

      // Acquire IX MDL lock on schema name.
      MDL_request mdl_request;
      MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA,
                       lex_str_db.str, "",
                       MDL_INTENTION_EXCLUSIVE,
                       MDL_TRANSACTION);
      if (thd->mdl_context.acquire_lock(&mdl_request,
                                        thd->variables.lock_wait_timeout))
        return true;

      // Stop if given database does not exist.
      bool exists= false;
      if (dd::schema_exists(thd, lex_str_db.str, &exists))
        return true;

      if (!exists)
      {
        my_error(ER_BAD_DB_ERROR, MYF(0), lex->select_lex->db);
        return true;
      }

      break;
    }
    case SQLCOM_SHOW_FIELDS:
    case SQLCOM_SHOW_KEYS:
      {
        new_dd_show= true;

        if (!lock)
          break;

        enum enum_schema_tables schema_table_idx;
        if (tables->schema_table)
        {
          schema_table_idx= get_schema_table_idx(tables->schema_table);
          if (schema_table_idx == SCH_TMP_TABLE_COLUMNS ||
              schema_table_idx == SCH_TMP_TABLE_KEYS)
            break;
        }

        bool can_deadlock= thd->mdl_context.has_locks();
        TABLE_LIST *dst_table= tables->schema_select_lex->table_list.first;
        if (try_acquire_high_prio_shared_mdl_lock(thd, dst_table, can_deadlock))
        {
          /*
            Some error occured (most probably we have been killed while
            waiting for conflicting locks to go away), let the caller to
            handle the situation.
          */
          return true;
        }

        if (dst_table->mdl_request.ticket == nullptr)
        {
          /*
            We are in situation when we have encountered conflicting metadata
            lock and deadlocks can occur due to waiting for it to go away.
            So instead of waiting skip this table with an appropriate warning.
          */
          DBUG_ASSERT(can_deadlock);
          my_error(ER_WARN_I_S_SKIPPED_TABLE, MYF(0),
                   dst_table->db, dst_table->table_name);
          return true;
        }

        // Stop if given database does not exist.
        dd::Schema_MDL_locker mdl_handler(thd);
        dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
        const dd::Schema *schema= nullptr;
        if (mdl_handler.ensure_locked(dst_table->db) ||
            thd->dd_client()->acquire(dst_table->db, &schema))
          return true;

        if (schema == nullptr)
        {
          my_error(ER_BAD_DB_ERROR, MYF(0), dst_table->db);
          return true;
        }

        const dd::Abstract_table *at= nullptr;
        if (thd->dd_client()->acquire(dst_table->db,
                                      dst_table->table_name,
                                      &at))
          return true;

        if (at == nullptr)
        {
          my_error(ER_NO_SUCH_TABLE, MYF(0),
                   dst_table->db,
                   dst_table->table_name);
          return true;
        }
        break;
      }
    default:
      break;
  }

  if (tables != NULL)
  {
    if (check_table_access(thd, SELECT_ACL, tables, false, UINT_MAX, false))
      return true;

    if ((tables->schema_table_reformed || new_dd_show) &&
        check_show_access(thd, tables))
      return true;
  }
  return false;
}


bool execute_show(THD *thd, TABLE_LIST *all_tables)
{
  DBUG_ENTER("execute_show");
  LEX	*lex= thd->lex;
  bool statement_timer_armed= false;
  bool res;

  /* assign global limit variable if limit is not given */
  {
    SELECT_LEX *param= lex->unit->global_parameters();
    if (!param->explicit_limit)
      param->select_limit=
        new Item_int((ulonglong) thd->variables.select_limit);
  }

  //check if timer is applicable to statement, if applicable then set timer.
  if (is_timer_applicable_to_statement(thd))
    statement_timer_armed= set_statement_timer(thd);

  if (!(res= open_tables_for_query(thd, all_tables, false)))
  {
    if (lex->is_explain())
    {
      /*
        We always use Query_result_send for EXPLAIN, even if it's an EXPLAIN
        for SELECT ... INTO OUTFILE: a user application should be able
        to prepend EXPLAIN to any query and receive output for it,
        even if the query itself redirects the output.
      */
      Query_result *const result= new (*THR_MALLOC) Query_result_send(thd);
      if (!result)
        DBUG_RETURN(true); /* purecov: inspected */
      res= handle_query(thd, lex, result, 0, 0);
    }
    else
    {
      Query_result *result= lex->result;
      if (!result && !(result= new (*THR_MALLOC) Query_result_send(thd)))
        DBUG_RETURN(true);                            /* purecov: inspected */
      Query_result *save_result= result;
      res= handle_query(thd, lex, result, 0, 0);
      if (save_result != lex->result)
        destroy(save_result);
    }
  }

  if (statement_timer_armed && thd->timer)
    reset_statement_timer(thd);

  DBUG_RETURN(res);
}


#define MY_YACC_INIT 1000			// Start with big alloc
#define MY_YACC_MAX  32000			// Because of 'short'

bool my_yyoverflow(short **yyss, YYSTYPE **yyvs, YYLTYPE **yyls, ulong *yystacksize)
{
  Yacc_state *state= & current_thd->m_parser_state->m_yacc;
  ulong old_info=0;
  DBUG_ASSERT(state);
  if ((uint) *yystacksize >= MY_YACC_MAX)
    return 1;
  if (!state->yacc_yyvs)
    old_info= *yystacksize;
  *yystacksize= set_zone((*yystacksize)*2,MY_YACC_INIT,MY_YACC_MAX);
  if (!(state->yacc_yyvs= (uchar*)
        my_realloc(key_memory_bison_stack,
                   state->yacc_yyvs,
                   *yystacksize*sizeof(**yyvs),
                   MYF(MY_ALLOW_ZERO_PTR | MY_FREE_ON_ERROR))) ||
      !(state->yacc_yyss= (uchar*)
        my_realloc(key_memory_bison_stack,
                   state->yacc_yyss,
                   *yystacksize*sizeof(**yyss),
                   MYF(MY_ALLOW_ZERO_PTR | MY_FREE_ON_ERROR))) ||
      !(state->yacc_yyls= (uchar*)
        my_realloc(key_memory_bison_stack,
                   state->yacc_yyls,
                   *yystacksize*sizeof(**yyls),
                   MYF(MY_ALLOW_ZERO_PTR | MY_FREE_ON_ERROR))))
    return 1;
  if (old_info)
  {
    /*
      Only copy the old stack on the first call to my_yyoverflow(),
      when replacing a static stack (YYINITDEPTH) by a dynamic stack.
      For subsequent calls, my_realloc already did preserve the old stack.
    */
    memcpy(state->yacc_yyss, *yyss, old_info*sizeof(**yyss));
    memcpy(state->yacc_yyvs, *yyvs, old_info*sizeof(**yyvs));
    memcpy(state->yacc_yyls, *yyls, old_info*sizeof(**yyls));
  }
  *yyss= (short*) state->yacc_yyss;
  *yyvs= (YYSTYPE*) state->yacc_yyvs;
  *yyls= (YYLTYPE*) state->yacc_yyls;
  return 0;
}


/**
  Reset the part of THD responsible for the state of command
  processing.

  This needs to be called before execution of every statement
  (prepared or conventional).  It is not called by substatements of
  routines.

  @todo Remove mysql_reset_thd_for_next_command and only use the
  member function.

  @todo Call it after we use THD for queries, not before.
*/
void mysql_reset_thd_for_next_command(THD *thd)
{
  thd->reset_for_next_command();
}

void THD::reset_for_next_command()
{
  // TODO: Why on earth is this here?! We should probably fix this
  // function and move it to the proper file. /Matz
  THD *thd= this;
  DBUG_ENTER("mysql_reset_thd_for_next_command");
  DBUG_ASSERT(!thd->sp_runtime_ctx); /* not for substatements of routines */
  DBUG_ASSERT(! thd->in_sub_stmt);
  thd->free_list= 0;
  /*
    Those two lines below are theoretically unneeded as
    THD::cleanup_after_query() should take care of this already.
  */
  thd->auto_inc_intervals_in_cur_stmt_for_binlog.empty();
  thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt= 0;

  thd->query_start_usec_used= false;
  thd->is_fatal_error= thd->time_zone_used= 0;
  /*
    Clear the status flag that are expected to be cleared at the
    beginning of each SQL statement.
  */
  thd->server_status&= ~SERVER_STATUS_CLEAR_SET;
  /*
    If in autocommit mode and not in a transaction, reset flag
    that identifies if a transaction has done some operations
    that cannot be safely rolled back.

    If the flag is set an warning message is printed out in
    ha_rollback_trans() saying that some tables couldn't be
    rolled back.
  */
  if (!thd->in_multi_stmt_transaction_mode())
  {
    thd->get_transaction()->reset_unsafe_rollback_flags(
        Transaction_ctx::SESSION);
  }
  DBUG_ASSERT(thd->security_context()== &thd->m_main_security_ctx);
  thd->thread_specific_used= false;

  if (opt_bin_log)
  {
    thd->user_var_events.clear();
    thd->user_var_events_alloc= thd->mem_root;
  }
  thd->clear_error();
  thd->get_stmt_da()->reset_diagnostics_area();
  thd->get_stmt_da()->reset_statement_cond_count();

  thd->rand_used= 0;
  thd->m_sent_row_count= thd->m_examined_row_count= 0;

  thd->reset_current_stmt_binlog_format_row();
  thd->binlog_unsafe_warning_flags= 0;
  thd->binlog_need_explicit_defaults_ts= false;

  thd->commit_error= THD::CE_NONE;
  thd->durability_property= HA_REGULAR_DURABILITY;
  thd->set_trans_pos(NULL, 0);

  thd->derived_tables_processing= false;
  thd->parsing_system_view= false;

  // Need explicit setting, else demand all privileges to a table.
  thd->want_privilege= ~NO_ACCESS;

  thd->reset_skip_readonly_check();

  DBUG_PRINT("debug",
             ("is_current_stmt_binlog_format_row(): %d",
              thd->is_current_stmt_binlog_format_row()));

  /*
    In case we're processing multiple statements we need to checkout a new
    acl access map here as the global acl version might have increased due to
    a grant/revoke or flush.
  */
  thd->security_context()->checkout_access_maps();
#ifndef DBUG_OFF
  thd->set_tmp_table_seq_id(1);
#endif

  DBUG_VOID_RETURN;
}


/**
  Create a select to return the same output as 'SELECT @@var_name'.

  Used for SHOW COUNT(*) [ WARNINGS | ERROR].

  This will crash with a core dump if the variable doesn't exists.

  @param pc                     Current parse context
  @param var_name		Variable name

  @returns false if success, true if error

  @todo - replace this function with one that generates a PT_select node
          and performs MAKE_CMD on it.
*/

bool create_select_for_variable(Parse_context *pc, const char *var_name)
{
  LEX_STRING tmp, null_lex_string;
  char buff[MAX_SYS_VAR_LENGTH*2+4+8];
  DBUG_ENTER("create_select_for_variable");

  THD *thd= pc->thd;
  LEX *lex= thd->lex;
  lex->sql_command= SQLCOM_SELECT;
  tmp.str= (char*) var_name;
  tmp.length=strlen(var_name);
  memset(&null_lex_string, 0, sizeof(null_lex_string));
  /*
    We set the name of Item to @@session.var_name because that then is used
    as the column name in the output.
  */
  Item *var= get_system_var(pc, OPT_SESSION, tmp, null_lex_string);
  if (var == NULL)
    DBUG_RETURN(true);                      /* purecov: inspected */

  char *end= strxmov(buff, "@@session.", var_name, NullS);
  var->item_name.copy(buff, end - buff);
  add_item_to_list(thd, var);

  DBUG_RETURN(false);
}


/*
  When you modify mysql_parse(), you may need to mofify
  mysql_test_parse_for_slave() in this same file.
*/

/**
  Parse a query.

  @param thd          Current session.
  @param parser_state Parser state.
*/

void mysql_parse(THD *thd, Parser_state *parser_state)
{
  DBUG_ENTER("mysql_parse");
  DBUG_PRINT("mysql_parse", ("query: '%s'", thd->query().str));

  DBUG_EXECUTE_IF("parser_debug", turn_parser_debug_on(););

  mysql_reset_thd_for_next_command(thd);
  lex_start(thd);

  thd->m_parser_state= parser_state;
  invoke_pre_parse_rewrite_plugins(thd);
  thd->m_parser_state= NULL;

  enable_digest_if_any_plugin_needs_it(thd, parser_state);

  LEX *lex= thd->lex;
  const char *found_semicolon= nullptr;

  bool err= thd->get_stmt_da()->is_error();

  if (!err)
  {
    err= parse_sql(thd, parser_state, NULL);
    if (!err)
      err= invoke_post_parse_rewrite_plugins(thd, false);

    found_semicolon= parser_state->m_lip.found_semicolon;
  }

  if (!err)
  {
    /*
      Rewrite the query for logging and for the Performance Schema statement
      tables. Raw logging happened earlier.

      Sub-routines of mysql_rewrite_query() should try to only rewrite when
      necessary (e.g. not do password obfuscation when query contains no
      password).

      If rewriting does not happen here, thd->rewritten_query is still empty
      from being reset in alloc_query().
    */
    mysql_rewrite_query(thd);

    if (thd->rewritten_query.length())
    {
      lex->safe_to_cache_query= false; // see comments below

      MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi,
                               thd->rewritten_query.c_ptr_safe(),
                               thd->rewritten_query.length());
    }
    else
    {
      MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi,
                               thd->query().str,
                               thd->query().length);
    }

    if (!(opt_general_log_raw || thd->slave_thread))
    {
      if (thd->rewritten_query.length())
        query_logger.general_log_write(thd, COM_QUERY,
                                       thd->rewritten_query.c_ptr_safe(),
                                       thd->rewritten_query.length());
      else
      {
        size_t qlen= found_semicolon
          ? (found_semicolon - thd->query().str)
          : thd->query().length;
        
        query_logger.general_log_write(thd, COM_QUERY,
                                       thd->query().str, qlen);
      }
    }
  }

  if (!err)
  {
    thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                                 sql_statement_info[thd->lex->sql_command].m_key);

    if (mqh_used && thd->get_user_connect() &&
        check_mqh(thd, lex->sql_command))
    {
      if (thd->is_classic_protocol())
        thd->get_protocol_classic()->get_net()->error = 0;
    }
    else
    {
      if (! thd->is_error())
      {
        /*
          Binlog logs a string starting from thd->query and having length
          thd->query_length; so we set thd->query_length correctly (to not
          log several statements in one event, when we executed only first).
          We set it to not see the ';' (otherwise it would get into binlog
          and Query_log_event::print() would give ';;' output).
          This also helps display only the current query in SHOW
          PROCESSLIST.
        */
        if (found_semicolon && (ulong) (found_semicolon - thd->query().str))
          thd->set_query(thd->query().str,
                         static_cast<size_t>(found_semicolon -
                                             thd->query().str - 1));
        /* Actually execute the query */
        if (found_semicolon)
        {
          lex->safe_to_cache_query= 0;
          thd->server_status|= SERVER_MORE_RESULTS_EXISTS;
        }
        lex->set_trg_event_type_for_tables();

        int error MY_ATTRIBUTE((unused));
        if (unlikely(thd->security_context()->password_expired() &&
                     lex->sql_command != SQLCOM_SET_PASSWORD &&
                     lex->sql_command != SQLCOM_SET_OPTION &&
                     lex->sql_command != SQLCOM_ALTER_USER))
        {
          my_error(ER_MUST_CHANGE_PASSWORD, MYF(0));
          error= 1;
        }
        else
        {
          resourcegroups::Resource_group *src_res_grp= nullptr;
          resourcegroups::Resource_group *dest_res_grp= nullptr;
          MDL_ticket *ticket= nullptr;
          MDL_ticket *cur_ticket= nullptr;
          auto mgr_ptr= resourcegroups::Resource_group_mgr::instance();
          bool switched= mgr_ptr->switch_resource_group_if_needed(
            thd, &src_res_grp, &dest_res_grp, &ticket, &cur_ticket);

          error= mysql_execute_command(thd, true);

          if (switched)
            mgr_ptr->restore_original_resource_group(thd, src_res_grp,
                                                     dest_res_grp);
          thd->resource_group_ctx()->m_switch_resource_group_str[0]= '\0';
          if (ticket != nullptr)
            mgr_ptr->release_shared_mdl_for_resource_group(thd, ticket);
          if (cur_ticket != nullptr)
            mgr_ptr->release_shared_mdl_for_resource_group(thd, cur_ticket);
        }
      }
    }
  }
  else
  {
    /*
      Log the failed raw query in the Performance Schema. This statement did not
      parse, so there is no way to tell if it may contain a password of not.

      The tradeoff is:
        a) If we do log the query, a user typing by accident a broken query
           containing a password will have the password exposed. This is very
           unlikely, and this behavior can be documented. Remediation is to use
           a new password when retyping the corrected query.

        b) If we do not log the query, finding broken queries in the client
           application will be much more difficult. This is much more likely.

      Considering that broken queries can typically be generated by attempts at
      SQL injection, finding the source of the SQL injection is critical, so the
      design choice is to log the query text of broken queries (a).
    */
    MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi,
                             thd->query().str,
                             thd->query().length);

    /* Instrument this broken statement as "statement/sql/error" */
    thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                                 sql_statement_info[SQLCOM_END].m_key);

    DBUG_ASSERT(thd->is_error());
    DBUG_PRINT("info",("Command aborted. Fatal_error: %d",
      		 thd->is_fatal_error));
  }

  THD_STAGE_INFO(thd, stage_freeing_items);
  sp_cache_enforce_limit(thd->sp_proc_cache, stored_program_cache_size);
  sp_cache_enforce_limit(thd->sp_func_cache, stored_program_cache_size);
  thd->end_statement();
  thd->cleanup_after_query();
  DBUG_ASSERT(thd->change_list.is_empty());

  DBUG_VOID_RETURN;
}


/**
  Usable by the replication SQL thread only: just parse a query to know if it
  can be ignored because of replicate-*-table rules.

  @retval
    0	cannot be ignored
  @retval
    1	can be ignored
*/

bool mysql_test_parse_for_slave(THD *thd)
{
  LEX *lex= thd->lex;
  bool ignorable= false;
  sql_digest_state *parent_digest= thd->m_digest;
  PSI_statement_locker *parent_locker= thd->m_statement_psi;
  DBUG_ENTER("mysql_test_parse_for_slave");

  DBUG_ASSERT(thd->slave_thread);

  Parser_state parser_state;
  if (parser_state.init(thd, thd->query().str, thd->query().length) == 0)
  {
    lex_start(thd);
    mysql_reset_thd_for_next_command(thd);

    thd->m_digest= NULL;
    thd->m_statement_psi= NULL;
    if (parse_sql(thd, & parser_state, NULL) == 0)
    {
      if (all_tables_not_ok(thd, lex->select_lex->table_list.first))
        ignorable= true;
      else if (lex->sql_command != SQLCOM_BEGIN &&
               lex->sql_command != SQLCOM_COMMIT &&
               lex->sql_command != SQLCOM_SAVEPOINT &&
               lex->sql_command != SQLCOM_ROLLBACK &&
               lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT &&
               !thd->rli_slave->rpl_filter->db_ok(thd->db().str))
        ignorable= true;
    }
    thd->m_digest= parent_digest;
    thd->m_statement_psi= parent_locker;
    thd->end_statement();
  }
  thd->cleanup_after_query();
  DBUG_RETURN(ignorable);
}


/**
  Store field definition for create.

  @param thd                    The thread handler.
  @param field_name             The field name.
  @param type                   The type of the field.
  @param length                 The length of the field or NULL.
  @param decimals               The length of a decimal part or NULL.
  @param type_modifier          Type modifiers & constraint flags of the field.
  @param default_value          The default value or NULL.
  @param on_update_value        The ON UPDATE expression or NULL.
  @param comment                The comment.
  @param change                 The old column name (if renaming) or NULL.
  @param interval_list          The list of ENUM/SET values or NULL.
  @param cs                     The character set of the field.
  @param uint_geom_type         The GIS type of the field.
  @param gcol_info              The generated column data or NULL.
  @param opt_after              The name of the field to add after or
                                the @see first_keyword pointer to insert first.
  @param srid                   The SRID for this column (only relevant if this
                                is a geometry column).

  @return
    Return 0 if ok
*/

bool Alter_info::add_field(THD *thd,
                           const LEX_STRING *field_name,
                           enum_field_types type,
                           const char *length, const char *decimals,
                           uint type_modifier,
                           Item *default_value, Item *on_update_value,
                           LEX_STRING *comment,
                           const char *change,
                           List<String> *interval_list, const CHARSET_INFO *cs,
                           uint uint_geom_type,
                           Generated_column *gcol_info,
                           const char *opt_after, Nullable<gis::srid_t> srid)
{
  Create_field *new_field;
  uint8 datetime_precision= decimals ? atoi(decimals) : 0;
  DBUG_ENTER("add_field_to_list");

  LEX_CSTRING field_name_cstr= {field_name->str, field_name->length};

  if (check_string_char_length(field_name_cstr, "", NAME_CHAR_LEN,
                               system_charset_info, 1))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), field_name->str); /* purecov: inspected */
    DBUG_RETURN(1);				/* purecov: inspected */
  }
  if (type_modifier & PRI_KEY_FLAG)
  {
    List<Key_part_spec> key_parts;
    auto key_part_spec= new (*THR_MALLOC) Key_part_spec(field_name_cstr, 0, ORDER_ASC);
    if (key_part_spec == NULL || key_parts.push_back(key_part_spec))
      DBUG_RETURN(true);
    Key_spec *key= new (*THR_MALLOC) Key_spec(thd->mem_root,
                                              KEYTYPE_PRIMARY,
                                              NULL_CSTR,
                                              &default_key_create_info,
                                              false, true, key_parts);
    if (key == NULL || key_list.push_back(key))
      DBUG_RETURN(true);
  }
  if (type_modifier & (UNIQUE_FLAG | UNIQUE_KEY_FLAG))
  {
    List<Key_part_spec> key_parts;
    auto key_part_spec= new (*THR_MALLOC) Key_part_spec(field_name_cstr, 0, ORDER_ASC);
    if (key_part_spec == NULL || key_parts.push_back(key_part_spec))
      DBUG_RETURN(true);
    Key_spec *key= new (*THR_MALLOC) Key_spec(thd->mem_root,
                                              KEYTYPE_UNIQUE,
                                              NULL_CSTR,
                                              &default_key_create_info,
                                              false, true, key_parts);
    if (key == NULL || key_list.push_back(key))
      DBUG_RETURN(true);
  }

  if (default_value)
  {
    /* 
      Default value should be literal => basic constants =>
      no need fix_fields()

      We allow only CURRENT_TIMESTAMP as function default for the TIMESTAMP or
      DATETIME types.
    */
    if (default_value->type() == Item::FUNC_ITEM && 
        (static_cast<Item_func*>(default_value)->functype() !=
         Item_func::NOW_FUNC ||
         (!real_type_with_now_as_default(type)) ||
         default_value->decimals != datetime_precision))
    {
      my_error(ER_INVALID_DEFAULT, MYF(0), field_name->str);
      DBUG_RETURN(1);
    }
    else if (default_value->type() == Item::NULL_ITEM)
    {
      default_value= 0;
      if ((type_modifier & (NOT_NULL_FLAG | AUTO_INCREMENT_FLAG)) ==
	  NOT_NULL_FLAG)
      {
	my_error(ER_INVALID_DEFAULT, MYF(0), field_name->str);
	DBUG_RETURN(1);
      }
    }
    else if (type_modifier & AUTO_INCREMENT_FLAG)
    {
      my_error(ER_INVALID_DEFAULT, MYF(0), field_name->str);
      DBUG_RETURN(1);
    }
  }

  if (on_update_value &&
      (!real_type_with_now_on_update(type) ||
       on_update_value->decimals != datetime_precision))
  {
    my_error(ER_INVALID_ON_UPDATE, MYF(0), field_name->str);
    DBUG_RETURN(1);
  }

  // If the SRID is specified on a non-geometric column, return an error
  if (type != MYSQL_TYPE_GEOMETRY && srid.has_value())
  {
    my_error(ER_WRONG_USAGE, MYF(0), "SRID", "non-geometry column");
    DBUG_RETURN(true);
  }

  // Check if the spatial reference system exists
  if (srid.has_value() && srid.value() != 0)
  {
    Srs_fetcher fetcher(thd);
    const dd::Spatial_reference_system *srs= nullptr;
    dd::cache::Dictionary_client::Auto_releaser m_releaser(thd->dd_client());
    if (fetcher.acquire(srid.value(), &srs))
    {
      // An error has already been raised
      DBUG_RETURN(true); /* purecov: deadcode */
    }

    if (srs == nullptr)
    {
      my_error(ER_SRS_NOT_FOUND, MYF(0), srid.value());
      DBUG_RETURN(true);
    }
  }

  if (!(new_field= new (*THR_MALLOC) Create_field()) ||
      new_field->init(thd, field_name->str, type, length, decimals, type_modifier,
                      default_value, on_update_value, comment, change,
                      interval_list, cs, uint_geom_type, gcol_info, srid))
    DBUG_RETURN(1);

  create_list.push_back(new_field);
  if (opt_after != NULL)
  {
    flags |= Alter_info::ALTER_COLUMN_ORDER;
    new_field->after=(char*) (opt_after);
  }
  DBUG_RETURN(0);
}


/**
  save order by and tables in own lists.
*/

void add_to_list(SQL_I_List<ORDER> &list, ORDER *order)
{
  DBUG_ENTER("add_to_list");
  order->item= &order->item_ptr;
  order->used_alias= false;
  order->used=0;
  list.link_in_list(order, &order->next);
  DBUG_VOID_RETURN;
}


extern int MYSQLparse(class THD *thd); // from sql_yacc.cc


/**
  Produces a PT_subquery object from a subquery's text.
  @param thd      Thread handler
  @param text     Subquery's text
  @param text_offset Offset in bytes of 'text' in the original statement
  @param[out] node Produced PT_subquery object

  @returns true if error
 */
static bool reparse_common_table_expr(THD *thd, const LEX_STRING &text,
                                      uint text_offset,
                                      PT_subquery **node)
{
  Common_table_expr_parser_state parser_state;
  parser_state.init(thd, text.str, text.length);

  Parser_state *old= thd->m_parser_state;
  thd->m_parser_state= &parser_state;

  /*
    Re-parsing a CTE creates Item_param-s and Item_sp_local-s which are
    special, as they do not exist in the original query: thus they should not
    exist from the points of view of logging.
    This is achieved like this:
    - for SP local vars: their pos_in_query is set to 0
    - for PS parameters: they are not added to LEX::param_list and thus not to
    Prepared_statement::param_array.
    They still need a value, which they get like this:
    - for SP local vars: through the ordinary look-up of SP local
    variables' values by name of the variable.
    - for PS parameters: first the first-parsed, 'non-special' Item-params,
    which are in param_array, get their value bound from user-supplied data,
    then they propagate their value to their 'special' clones (@see
    Item_param::m_clones).
  */
  parser_state.m_lip.stmt_prepare_mode= old->m_lip.stmt_prepare_mode;
  parser_state.m_lip.multi_statements= false; // A safety measure.
  parser_state.m_lip.m_digest= NULL;

  // This is saved and restored by caller:
  thd->lex->reparse_common_table_expr_at= text_offset;

  /*
    As this function is called during parsing only, it can and should use the
    current Query_arena, character_set_client, etc.
    It intentionally uses MYSQLparse() directly without the parse_sql()
    wrapper: because it's building a node of the statement currently being
    parsed at the upper call site.
  */
  bool mysql_parse_status= MYSQLparse(thd) != 0;
  thd->m_parser_state= old;
  if (mysql_parse_status)
    return true;                                /* purecov: inspected */

  *node= parser_state.result;
  return false;
}


bool PT_common_table_expr::make_subquery_node(THD *thd, PT_subquery **node)
{
  if (m_postparse.references.size() >= 2)
  {
    // m_subq_node was already attached elsewhere, make new node:
    return reparse_common_table_expr(thd, m_subq_text, m_subq_text_offset,
                                     node);
  }
  *node = m_subq_node;
  return false;
}


/**
   Tries to match an identifier to the CTEs in scope; if matched, it
   modifies *table_name, *tl', and the matched with-list-element.

   @param          thd      Thread handler
   @param[out]     table_name Identifier
   @param[in,out]  tl       TABLE_LIST for the identifier
   @param          pc       Current parsing context, if available
   @param[out]     found    Is set to true if found.

   @returns true if error (OOM).
*/
bool
SELECT_LEX::find_common_table_expr(THD *thd, Table_ident *table_name,
                                   TABLE_LIST *tl, Parse_context *pc,
                                   bool *found)
{
  *found= false;
  if (!pc)
    return false;

  PT_with_clause *wc;
  PT_common_table_expr *cte= nullptr;
  SELECT_LEX *select= this;
  SELECT_LEX_UNIT *unit;
  do
  {
    DBUG_ASSERT(select->first_execution);
    unit= select->master_unit();
    if (!(wc= unit->m_with_clause))
      continue;
    if (wc->lookup(tl, &cte))
      return true;
    /*
      If no match in the WITH clause of 'select', maybe this is a subquery, so
      look up in the outer query's WITH clause:
    */
  } while (cte == nullptr && (select= unit->outer_select()));

  if (cte == nullptr)
    return false;
  *found= true;

  const auto save_reparse_cte= thd->lex->reparse_common_table_expr_at;
  PT_subquery *node;
  if (tl->is_recursive_reference())
  {
    LEX_STRING dummy_subq= {C_STRING_WITH_LEN("(select 0)")};
    if (reparse_common_table_expr(thd, dummy_subq, 0, &node))
      return true;                              /* purecov: inspected */
  }
  else if (cte->make_subquery_node(thd, &node))
    return true;             /* purecov: inspected */
  // We imitate derived tables as much as possible.
  DBUG_ASSERT(parsing_place == CTX_NONE && linkage != GLOBAL_OPTIONS_TYPE);
  parsing_place= CTX_DERIVED;
  node->m_is_derived_table= true;
  auto wc_save= wc->enter_parsing_definition(tl);

  DBUG_ASSERT(thd->lex->will_contextualize);
  if (node->contextualize(pc))
    return true;

  wc->leave_parsing_definition(wc_save);
  parsing_place= CTX_NONE;
  /*
    Prepared statement's parameters and SP local variables are spotted as
    'made during re-parsing' by node->contextualize(), which is why we
    ran that call _before_ restoring lex->reparse_common_table_expr_at.
  */
  thd->lex->reparse_common_table_expr_at= save_reparse_cte;
  tl->is_alias= true;
  SELECT_LEX_UNIT *node_unit= node->value()->master_unit();
  *table_name= Table_ident(node_unit);
  if (tl->is_recursive_reference())
    recursive_dummy_unit= node_unit;
  DBUG_ASSERT(table_name->is_derived_table());
  tl->db= const_cast<char*>(table_name->db.str);
  tl->db_length= table_name->db.length;
  tl->save_name_temporary();
  return false;
}


bool PT_with_clause::lookup(TABLE_LIST *tl, PT_common_table_expr **found)
{
  *found= nullptr;
  DBUG_ASSERT(tl->select_lex != nullptr);
  /*
    If right_bound!=NULL, it means we are currently parsing the
    definition of CTE 'right_bound' and this definition contains
    'tl'.
  */
  const Common_table_expr *right_bound= m_most_inner_in_parsing ?
    m_most_inner_in_parsing->common_table_expr() : nullptr;
  bool in_self= false;
  for (auto el : m_list.elements())
  {
    // Search for a CTE named like 'tl', in this list, from left to right.
    if (el->is(right_bound))
    {
      /*
        We meet right_bound.
        If not RECURSIVE:
        we must stop the search in this WITH clause;
        indeed right_bound must not reference itself or any CTE defined after it
        in the WITH list (forward references are forbidden, preventing any
        cycle).
        If RECURSIVE:
        If right_bound matches 'tl', it is a recursive reference.
      */
      if (!m_recursive)
        break;                                // Prevent forward reference.
      in_self= true;                          // Accept a recursive reference.
    }
    bool match;
    if (el->match_table_ref(tl, in_self, &match))
      return true;
    if (!match)
    {
      if (in_self)
        break;                                  // Prevent forward reference.
      continue;
    }
    if (in_self && tl->select_lex->outer_select() !=
        m_most_inner_in_parsing->select_lex)
    {
      /*
        SQL2011 says a recursive CTE cannot contain a subquery
        referencing the CTE, except if this subquery is a derived table
        like:
        WITH RECURSIVE qn AS (non-rec-SELECT UNION ALL
        SELECT * FROM (SELECT * FROM qn) AS dt)
        However, we don't allow this, as:
        - it simplifies detection and substitution correct recursive
        references (they're all on "level 0" of the UNION)
        - it's not limiting the user so much (in most cases, he can just
        merge his DT up manually, as the DT cannot contain aggregation).
        - Oracle bans it:
        with qn (a) as (
        select 123 from dual
        union all
        select 1+a from (select * from qn) where a<130) select * from qn
        ORA-32042: recursive WITH clause must reference itself directly in one of the
        UNION ALL branches.

        The above if() works because, when we parse such example query, we
        first resolve the 'qn' reference in the top query, making it a derived
        table:

        select * from (
           select 123 from dual
           union all
           select 1+a from (select * from qn) where a<130) qn(a);
                                                           ^most_inner_in_parsing
        Then we contextualize that derived table (containing the union);
        when we contextualize the recursive query block of the union, the
        inner 'qn' is recognized as a recursive reference, and its
        select_lex->outer_select() is _not_ the select_lex of
        most_inner_in_parsing, which indicates that the inner 'qn' is placed
        too deep.
      */
      my_error(ER_CTE_RECURSIVE_REQUIRES_SINGLE_REFERENCE,
               MYF(0), el->name().str);
      return true;
    }
    *found= el;
    break;
  }
  return false;
}


bool PT_common_table_expr::match_table_ref(TABLE_LIST *tl, bool in_self,
                                           bool *found)
{
  *found= false;
  if (tl->table_name_length == m_name.length &&
      /*
        memcmp() is fine even if lower_case_table_names==1, as CTE names
        have been lowercased in the ctor.
      */
      !memcmp(tl->table_name, m_name.str, m_name.length))
  {
    *found= true;
    // 'tl' is a reference to CTE 'el'.
    if (in_self)
    {
      m_postparse.recursive= true;
      if (tl->set_recursive_reference())
      {
        my_error(ER_CTE_RECURSIVE_REQUIRES_SINGLE_REFERENCE,
                 MYF(0), name().str);
        return true;
      }
    }
    else
    {
      if (m_postparse.references.push_back(tl))
        return true;                            /* purecov: inspected */
      tl->set_common_table_expr(&m_postparse);
      if (m_column_names.size())
        tl->set_derived_column_names(&m_column_names);
    }
  }
  return false;
}


/**
  Add a table to list of used tables.

  @param thd      Current session.
  @param table_name	Table to add
  @param alias		alias for table (or null if no alias)
  @param table_options	A set of the following bits:
                         - TL_OPTION_UPDATING : Table will be updated
                         - TL_OPTION_FORCE_INDEX : Force usage of index
                         - TL_OPTION_ALIAS : an alias in multi table DELETE
  @param lock_type	How table should be locked
  @param mdl_type       Type of metadata lock to acquire on the table.
  @param index_hints_arg
  @param partition_names
  @param option
  @param pc             Current parsing context, if available.

  @return Pointer to TABLE_LIST element added to the total table list
  @retval
      0		Error
*/

TABLE_LIST *SELECT_LEX::add_table_to_list(THD *thd,
                                          Table_ident *table_name,
                                          const char *alias,
                                          ulong table_options,
                                          thr_lock_type lock_type,
                                          enum_mdl_type mdl_type,
                                          List<Index_hint> *index_hints_arg,
                                          List<String> *partition_names,
                                          LEX_STRING *option,
                                          Parse_context *pc)
{
  TABLE_LIST *previous_table_ref= NULL; /* The table preceding the current one. */
  LEX *lex= thd->lex;
  DBUG_ENTER("add_table_to_list");

  DBUG_ASSERT(table_name != nullptr);
  // A derived table has no table name, only an alias.
  if (!(table_options & TL_OPTION_ALIAS) && !table_name->is_derived_table())
  {
    Ident_name_check ident_check_status=
      check_table_name(table_name->table.str, table_name->table.length);
    if (ident_check_status == Ident_name_check::WRONG)
    {
      my_error(ER_WRONG_TABLE_NAME, MYF(0), table_name->table.str);
      DBUG_RETURN(0);
    }
    else if (ident_check_status == Ident_name_check::TOO_LONG)
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), table_name->table.str);
      DBUG_RETURN(0);
    }
  }
  LEX_STRING db= to_lex_string(table_name->db);
  if (!table_name->is_derived_table() && !table_name->is_table_function() &&
      table_name->db.str &&
      (check_and_convert_db_name(&db, false) != Ident_name_check::OK))
    DBUG_RETURN(0);

  const char *alias_str= alias ? alias : table_name->table.str;
  if (!alias)					/* Alias is case sensitive */
  {
    if (table_name->sel)
    {
      my_error(ER_DERIVED_MUST_HAVE_ALIAS, MYF(0));
      DBUG_RETURN(0);
    }
    if (!(alias_str= (char*) thd->memdup(alias_str,table_name->table.length+1)))
      DBUG_RETURN(0);
  }

  TABLE_LIST *ptr= new (thd->mem_root) TABLE_LIST;
  if (ptr == nullptr)
    DBUG_RETURN(nullptr); /* purecov: inspected */

  if (lower_case_table_names && table_name->table.length)
    table_name->table.length= my_casedn_str(files_charset_info,
                                       const_cast<char*>(table_name->table.str));

  ptr->select_lex= this;
  ptr->table_name= const_cast<char*>(table_name->table.str);
  ptr->table_name_length= table_name->table.length;
  ptr->alias= const_cast<char*>(alias_str);
  ptr->is_alias= alias != nullptr;
  ptr->table_function= table_name->table_function;
  if (table_name->table_function)
  {
    table_func_count++;
    ptr->derived_key_list.empty();
  }

  if (table_name->db.str)
  {
    ptr->is_fqtn= true;
    ptr->db= const_cast<char*>(table_name->db.str);
    ptr->db_length= table_name->db.length;
  }
  else
  {
    bool found_cte;
    if (find_common_table_expr(thd, table_name, ptr, pc, &found_cte))
      DBUG_RETURN(0);
    if (!found_cte &&
        lex->copy_db_to((char**)&ptr->db, &ptr->db_length))
      DBUG_RETURN(0);
  }

  ptr->set_tableno(0);
  ptr->set_lock({lock_type, THR_DEFAULT});
  ptr->updating= (table_options & TL_OPTION_UPDATING);
  /* TODO: remove TL_OPTION_FORCE_INDEX as it looks like it's not used */
  ptr->force_index= (table_options & TL_OPTION_FORCE_INDEX);
  ptr->ignore_leaves= (table_options & TL_OPTION_IGNORE_LEAVES);
  ptr->set_derived_unit(table_name->sel);
  if (!ptr->is_derived() && !ptr->is_table_function() &&
      is_infoschema_db(ptr->db, ptr->db_length))
  {
    dd::info_schema::convert_table_name_case(
                       const_cast<char*>(ptr->db),
                       const_cast<char*>(ptr->table_name));

    bool hidden_system_view= false;
    ptr->is_system_view=
      dd::get_dictionary()->is_system_view_name(ptr->db, ptr->table_name,
                                                &hidden_system_view);

    ST_SCHEMA_TABLE *schema_table;
    if (ptr->updating &&
        /* Special cases which are processed by commands itself */
        lex->sql_command != SQLCOM_CHECK &&
        lex->sql_command != SQLCOM_CHECKSUM &&
        !(lex->sql_command == SQLCOM_CREATE_VIEW && ptr->is_system_view))
    {
      my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
               thd->security_context()->priv_user().str,
               thd->security_context()->priv_host().str,
               INFORMATION_SCHEMA_NAME.str);
      DBUG_RETURN(0);
    }
    if (ptr->is_system_view)
    {
      if (thd->lex->sql_command != SQLCOM_CREATE_VIEW)
      {
        /*
          Stop users from using hidden system views, unless
          it is used by SHOW commands.
        */
        if (thd->lex->select_lex && hidden_system_view &&
            !(thd->lex->select_lex->active_options() &
              OPTION_SELECT_FOR_SHOW))
        {
          my_error(ER_NO_SYSTEM_VIEW_ACCESS, MYF(0), ptr->table_name);
          DBUG_RETURN(0);
        }
      }
    }
    else
    {
      schema_table= find_schema_table(thd, ptr->table_name);
      /*
        Report an error
          if hidden schema table name is used in the statement other than
          SHOW statement OR
          if unknown schema table is used in the statement other than
          SHOW CREATE VIEW statement.
        Invalid view warning is reported for SHOW CREATE VIEW statement in
        the table open stage.
      */
      if ((!schema_table &&
           !(thd->query_plan.get_command() == SQLCOM_SHOW_CREATE &&
             thd->query_plan.get_lex()->only_view)) ||
          (schema_table && schema_table->hidden &&
           (sql_command_flags[lex->sql_command] & CF_STATUS_COMMAND) == 0))
      {
        my_error(ER_UNKNOWN_TABLE, MYF(0),
                 ptr->table_name, INFORMATION_SCHEMA_NAME.str);
        DBUG_RETURN(0);
      }

      if (schema_table)
      {
        ptr->schema_table_name= const_cast<char*>(ptr->table_name);
        ptr->schema_table= schema_table;
      }
    }
  }

  ptr->cacheable_table= 1;
  ptr->index_hints= index_hints_arg;
  ptr->option= option ? option->str : 0;
  /* check that used name is unique */
  if (lock_type != TL_IGNORE)
  {
    TABLE_LIST *first_table= table_list.first;
    if (lex->sql_command == SQLCOM_CREATE_VIEW)
      first_table= first_table ? first_table->next_local : NULL;
    for (TABLE_LIST *tables= first_table ;
	 tables ;
	 tables=tables->next_local)
    {
      if (!my_strcasecmp(table_alias_charset, alias_str, tables->alias) &&
	  !strcmp(ptr->db, tables->db))
      {
	my_error(ER_NONUNIQ_TABLE, MYF(0), alias_str); /* purecov: tested */
	DBUG_RETURN(0);				/* purecov: tested */
      }
    }
  }
  /* Store the table reference preceding the current one. */
  if (table_list.elements > 0)
  {
    /*
      table_list.next points to the last inserted TABLE_LIST->next_local'
      element
      We don't use the offsetof() macro here to avoid warnings from gcc
    */
    previous_table_ref= (TABLE_LIST*) ((char*) table_list.next -
                                       ((char*) &(ptr->next_local) -
                                        (char*) ptr));
    /*
      Set next_name_resolution_table of the previous table reference to point
      to the current table reference. In effect the list
      TABLE_LIST::next_name_resolution_table coincides with
      TABLE_LIST::next_local. Later this may be changed in
      store_top_level_join_columns() for NATURAL/USING joins.
    */
    previous_table_ref->next_name_resolution_table= ptr;
  }

  /*
    Link the current table reference in a local list (list for current select).
    Notice that as a side effect here we set the next_local field of the
    previous table reference to 'ptr'. Here we also add one element to the
    list 'table_list'.
  */
  table_list.link_in_list(ptr, &ptr->next_local);
  ptr->next_name_resolution_table= NULL;
  ptr->partition_names= partition_names;
  /* Link table in global list (all used tables) */
  lex->add_to_query_tables(ptr);

  // Pure table aliases do not need to be locked:
  if (!(table_options & TL_OPTION_ALIAS))
  {
    MDL_REQUEST_INIT(& ptr->mdl_request,
                     MDL_key::TABLE, ptr->db, ptr->table_name, mdl_type,
                     MDL_TRANSACTION);
  }
  if (table_name->is_derived_table())
  {
    ptr->derived_key_list.empty();
    derived_table_count++;
    ptr->save_name_temporary();
  }

  // Check access to DD tables. We must allow CHECK and ALTER TABLE
  // for the DDSE tables, since this is expected by the upgrade
  // client. We must also allow DDL access for the initialize thread,
  // since this thread is creating the I_S views.
  // Note that at this point, the mdl request for CREATE TABLE is still
  // MDL_SHARED, so we must explicitly check for SQLCOM_CREATE_TABLE.
  const dd::Dictionary *dictionary= dd::get_dictionary();
  if (dictionary && !dictionary->is_dd_table_access_allowed(
             thd->is_dd_system_thread() || thd->is_initialize_system_thread(),
             (ptr->mdl_request.is_ddl_or_lock_tables_lock_request() ||
              (lex->sql_command == SQLCOM_CREATE_TABLE &&
               ptr == lex->query_tables)) &&
              lex->sql_command != SQLCOM_CHECK &&
              lex->sql_command != SQLCOM_ALTER_TABLE,
             ptr->db, ptr->db_length, ptr->table_name))
  {
    // We must allow creation of the system views even for non-system
    // threads since this is expected by the mysql_upgrade utility.
    if (!(lex->sql_command == SQLCOM_CREATE_VIEW &&
          dd::get_dictionary()->is_system_view_name(
                                  lex->query_tables->db,
                                  lex->query_tables->table_name)))
    {
      my_error(ER_NO_SYSTEM_TABLE_ACCESS, MYF(0),
               ER_THD(thd, dictionary->table_type_error_code(ptr->db,
                                                             ptr->table_name)),
               ptr->db, ptr->table_name);
      // Take error handler into account to see if we should return.
      if (thd->is_error())
        DBUG_RETURN(nullptr);
    }
  }

  DBUG_RETURN(ptr);
}


/**
  Initialize a new table list for a nested join.

    The function initializes a structure of the TABLE_LIST type
    for a nested join. It sets up its nested join list as empty.
    The created structure is added to the front of the current
    join list in the SELECT_LEX object. Then the function
    changes the current nest level for joins to refer to the newly
    created empty list after having saved the info on the old level
    in the initialized structure.

  @param thd         current thread

  @retval
    0   if success
  @retval
    1   otherwise
*/

bool SELECT_LEX::init_nested_join(THD *thd)
{
  DBUG_ENTER("init_nested_join");

  TABLE_LIST *const ptr=
    TABLE_LIST::new_nested_join(thd->mem_root, "(nested_join)",
                                embedding, join_list, this);
  if (ptr == NULL)
    DBUG_RETURN(true);

  join_list->push_front(ptr);
  embedding= ptr;
  join_list= &ptr->nested_join->join_list;

  DBUG_RETURN(false);
}


/**
  End a nested join table list.

    The function returns to the previous join nest level.
    If the current level contains only one member, the function
    moves it one level up, eliminating the nest.

  @return
    - Pointer to TABLE_LIST element added to the total table list, if success
    - 0, otherwise
*/

TABLE_LIST *SELECT_LEX::end_nested_join()
{
  TABLE_LIST *ptr;
  NESTED_JOIN *nested_join;
  DBUG_ENTER("end_nested_join");

  DBUG_ASSERT(embedding);
  ptr= embedding;
  join_list= ptr->join_list;
  embedding= ptr->embedding;
  nested_join= ptr->nested_join;
  if (nested_join->join_list.elements == 1)
  {
    TABLE_LIST *embedded= nested_join->join_list.head();
    join_list->pop();
    embedded->join_list= join_list;
    embedded->embedding= embedding;
    if (join_list->push_front(embedded))
      DBUG_RETURN(NULL);
    ptr= embedded;
  }
  else if (nested_join->join_list.elements == 0)
  {
    join_list->pop();
    ptr= 0;                                     // return value
  }
  DBUG_RETURN(ptr);
}


/**
  Nest last join operations.

  The function nest last table_cnt join operations as if they were
  the components of a cross join operation.

  @param thd         current thread
  @param table_cnt   2 for regular joins: t1 JOIN t2.
                     N for the MySQL join-like extension: (t1, t2, ... tN).

  @return Pointer to TABLE_LIST element created for the new nested join
  @retval
    0  Error
*/

TABLE_LIST *SELECT_LEX::nest_last_join(THD *thd, size_t table_cnt)
{
  DBUG_ENTER("nest_last_join");

  TABLE_LIST *const ptr=
    TABLE_LIST::new_nested_join(thd->mem_root, "(nest_last_join)",
                                embedding, join_list, this);
  if (ptr == NULL)
    DBUG_RETURN(NULL);

  List<TABLE_LIST> *const embedded_list= &ptr->nested_join->join_list;

  for (uint i=0; i < table_cnt; i++)
  {
    TABLE_LIST *table= join_list->pop();
    table->join_list= embedded_list;
    table->embedding= ptr;
    embedded_list->push_back(table);
    if (table->natural_join)
      ptr->is_natural_join= true;
  }
  if (join_list->push_front(ptr))
    DBUG_RETURN(NULL);

  DBUG_RETURN(ptr);
}


/**
  Add a table to the current join list.

    The function puts a table in front of the current join list
    of SELECT_LEX object.
    Thus, joined tables are put into this list in the reverse order
    (the most outer join operation follows first).

  @param table       The table to add.

  @returns false if success, true if error (OOM).
*/

bool SELECT_LEX::add_joined_table(TABLE_LIST *table)
{
  DBUG_ENTER("add_joined_table");
  if (join_list->push_front(table))
    DBUG_RETURN(true);
  table->join_list= join_list;
  table->embedding= embedding;
  DBUG_RETURN(false);
}


/**
  Convert a right join into equivalent left join.

    The function takes the current join list t[0],t[1] ... and
    effectively converts it into the list t[1],t[0] ...
    Although the outer_join flag for the new nested table contains
    JOIN_TYPE_RIGHT, it will be handled as the inner table of a left join
    operation.

  EXAMPLES
  @verbatim
    SELECT * FROM t1 RIGHT JOIN t2 ON on_expr =>
      SELECT * FROM t2 LEFT JOIN t1 ON on_expr

    SELECT * FROM t1,t2 RIGHT JOIN t3 ON on_expr =>
      SELECT * FROM t1,t3 LEFT JOIN t2 ON on_expr

    SELECT * FROM t1,t2 RIGHT JOIN (t3,t4) ON on_expr =>
      SELECT * FROM t1,(t3,t4) LEFT JOIN t2 ON on_expr

    SELECT * FROM t1 LEFT JOIN t2 ON on_expr1 RIGHT JOIN t3  ON on_expr2 =>
      SELECT * FROM t3 LEFT JOIN (t1 LEFT JOIN t2 ON on_expr2) ON on_expr1
   @endverbatim

  @return
    - Pointer to the table representing the inner table, if success
    - 0, otherwise
*/

TABLE_LIST *SELECT_LEX::convert_right_join()
{
  TABLE_LIST *tab2= join_list->pop();
  TABLE_LIST *tab1= join_list->pop();
  DBUG_ENTER("convert_right_join");

  if (join_list->push_front(tab2) || join_list->push_front(tab1))
    DBUG_RETURN(NULL);
  tab1->outer_join|= JOIN_TYPE_RIGHT;

  DBUG_RETURN(tab1);
}


void SELECT_LEX::set_lock_for_table(const Lock_descriptor &descriptor,
                                    TABLE_LIST *table)
{
  thr_lock_type lock_type= descriptor.type;
  bool for_update= lock_type >= TL_READ_NO_INSERT;
  enum_mdl_type mdl_type= mdl_type_for_dml(lock_type);
  DBUG_ENTER("set_lock_for_table");
  DBUG_PRINT("enter", ("lock_type: %d  for_update: %d", lock_type,
                       for_update));
  table->set_lock(descriptor);
  table->updating=  for_update;
  table->mdl_request.set_type(mdl_type);

  DBUG_VOID_RETURN;
}


/**
  Set lock for all tables in current query block.

  @param lock_type Lock to set for tables.

  @note
    If the lock is a write lock, then tables->updating is set to true.
    This is to get tables_ok to know that the table is being updated by the
    query.
    Sets the type of metadata lock to request according to lock_type.
*/
void SELECT_LEX::set_lock_for_tables(thr_lock_type lock_type)
{
  DBUG_ENTER("set_lock_for_tables");
  DBUG_PRINT("enter", ("lock_type: %d  for_update: %d", lock_type,
                       lock_type >= TL_READ_NO_INSERT));
  for (TABLE_LIST *table= table_list.first; table; table= table->next_local)
    set_lock_for_table({ lock_type, THR_WAIT }, table);
  DBUG_VOID_RETURN;
}


/**
  Create a fake SELECT_LEX for a unit.

    The method create a fake SELECT_LEX object for a unit.
    This object is created for any union construct containing a union
    operation and also for any single select union construct of the form
    @verbatim
    (SELECT ... ORDER BY order_list [LIMIT n]) ORDER BY ... 
    @endverbatim
    or of the form
    @verbatim
    (SELECT ... ORDER BY LIMIT n) ORDER BY ...
    @endverbatim
  
  @param thd_arg       thread handle

  @note
    The object is used to retrieve rows from the temporary table
    where the result on the union is obtained.

  @retval
    1     on failure to create the object
  @retval
    0     on success
*/

bool SELECT_LEX_UNIT::add_fake_select_lex(THD *thd_arg)
{
  SELECT_LEX *first_sl= first_select();
  DBUG_ENTER("add_fake_select_lex");
  DBUG_ASSERT(!fake_select_lex);
  DBUG_ASSERT(thd_arg == thd);

  if (!(fake_select_lex= thd_arg->lex->new_empty_query_block()))
    DBUG_RETURN(true);       /* purecov: inspected */
  fake_select_lex->include_standalone(this, &fake_select_lex);
  fake_select_lex->select_number= INT_MAX;
  fake_select_lex->linkage= GLOBAL_OPTIONS_TYPE;
  fake_select_lex->select_limit= 0;

  fake_select_lex->set_context(first_sl->context.outer_context);

  /* allow item list resolving in fake select for ORDER BY */
  fake_select_lex->context.resolve_in_select_list= true;

  if (!is_union())
  {
    /* 
      This works only for 
      (SELECT ... ORDER BY list [LIMIT n]) ORDER BY order_list [LIMIT m],
      (SELECT ... LIMIT n) ORDER BY order_list [LIMIT m]
      just before the parser starts processing order_list
    */ 
    fake_select_lex->no_table_names_allowed= 1;
  }
  thd->lex->pop_context();
  DBUG_RETURN(false);
}


/**
  Push a new name resolution context for a JOIN ... ON clause to the
  context stack of a query block.

    Create a new name resolution context for a JOIN ... ON clause,
    set the first and last leaves of the list of table references
    to be used for name resolution, and push the newly created
    context to the stack of contexts of the query.

  @param pc        current parse context
  @param left_op   left  operand of the JOIN
  @param right_op  rigth operand of the JOIN

  @todo Research if we should set the "outer_context" member of the new ON
  context.

  @retval
    false  if all is OK
  @retval
    true   if a memory allocation error occured
*/

bool
push_new_name_resolution_context(Parse_context *pc,
                                 TABLE_LIST *left_op, TABLE_LIST *right_op)
{
  THD *thd= pc->thd;
  Name_resolution_context *on_context;
  if (!(on_context= new (thd->mem_root) Name_resolution_context))
    return true;
  on_context->init();
  on_context->first_name_resolution_table=
    left_op->first_leaf_for_name_resolution();
  on_context->last_name_resolution_table=
    right_op->last_leaf_for_name_resolution();
  on_context->select_lex= pc->select;
  on_context->next_context= pc->select->first_context;
  pc->select->first_context= on_context;

  return thd->lex->push_context(on_context);
}


/**
  Add an ON condition to the second operand of a JOIN ... ON.

    Add an ON condition to the right operand of a JOIN ... ON clause.

  @param b     the second operand of a JOIN ... ON
  @param expr  the condition to be added to the ON clause
*/

void add_join_on(TABLE_LIST *b, Item *expr)
{
  if (expr)
  {
    b->set_join_cond_optim((Item*)1); // m_join_cond_optim is not ready
    if (!b->join_cond())
      b->set_join_cond(expr);
    else
    {
      /*
        If called from the parser, this happens if you have both a
        right and left join. If called later, it happens if we add more
        than one condition to the ON clause.
      */
      b->set_join_cond(new Item_cond_and(b->join_cond(), expr));
    }
    b->join_cond()->top_level_item();
  }
}


const CHARSET_INFO *get_bin_collation(const CHARSET_INFO *cs)
{
  const CHARSET_INFO *ret= get_charset_by_csname(cs->csname,
                                                 MY_CS_BINSORT, MYF(0));
  if (ret)
    return ret;

  char tmp[65];
  strmake(strmake(tmp, cs->csname, sizeof(tmp) - 4), STRING_WITH_LEN("_bin"));
  my_error(ER_UNKNOWN_COLLATION, MYF(0), tmp);
  return NULL;
}


/**
  kill on thread.

  @param thd			Thread class
  @param id			Thread id
  @param only_kill_query        Should it kill the query or the connection

  @note
    This is written such that we have a short lock on LOCK_thd_list
*/


static uint kill_one_thread(THD *thd, my_thread_id id, bool only_kill_query)
{
  THD *tmp= NULL;
  uint error=ER_NO_SUCH_THREAD;
  Find_thd_with_id find_thd_with_id(id);

  DBUG_ENTER("kill_one_thread");
  DBUG_PRINT("enter", ("id=%u only_kill=%d", id, only_kill_query));
  tmp= Global_THD_manager::get_instance()->find_thd(&find_thd_with_id);
  Security_context *sctx= thd->security_context();
  if (tmp)
  {
    /*
      If we're SUPER, we can KILL anything, including system-threads.
      No further checks.

      KILLer: thd->m_security_ctx->user could in theory be NULL while
      we're still in "unauthenticated" state. This is a theoretical
      case (the code suggests this could happen, so we play it safe).

      KILLee: tmp->m_security_ctx->user will be NULL for system threads.
      We need to check so Jane Random User doesn't crash the server
      when trying to kill a) system threads or b) unauthenticated users'
      threads (Bug#43748).

      If user of both killer and killee are non-NULL, proceed with
      slayage if both are string-equal.
    */

    if (sctx->check_access(SUPER_ACL) ||
        sctx->has_global_grant(STRING_WITH_LEN("CONNECTION_ADMIN")).first ||
        thd->security_context()->user_matches(tmp->security_context()))
    {
      /* process the kill only if thread is not already undergoing any kill
         connection.
      */
      if (tmp->killed != THD::KILL_CONNECTION)
      {
        tmp->awake(only_kill_query ? THD::KILL_QUERY : THD::KILL_CONNECTION);
      }
      error= 0;
    }
    else
      error=ER_KILL_DENIED_ERROR;
    mysql_mutex_unlock(&tmp->LOCK_thd_data);
  }
  DEBUG_SYNC(thd, "kill_thd_end");
  DBUG_PRINT("exit", ("%d", error));
  DBUG_RETURN(error);
}


/*
  kills a thread and sends response

  SYNOPSIS
    sql_kill()
    thd			Thread class
    id			Thread id
    only_kill_query     Should it kill the query or the connection
*/

static
void sql_kill(THD *thd, my_thread_id id, bool only_kill_query)
{
  uint error;
  if (!(error= kill_one_thread(thd, id, only_kill_query)))
  {
    if (! thd->killed)
      my_ok(thd);
  }
  else
    my_error(error, MYF(0), id);
}

/**
  This class implements callback function used by killall_non_super_threads
  to kill all threads that do not have the SUPER privilege
*/

class Kill_non_super_conn : public Do_THD_Impl
{
private:
  /* THD of connected client. */
  THD *m_client_thd;

public:
  Kill_non_super_conn(THD *thd) :
	    m_client_thd(thd)
  {
    DBUG_ASSERT(m_client_thd->security_context()->check_access(SUPER_ACL) ||
      m_client_thd->security_context()->
        has_global_grant(STRING_WITH_LEN("CONNECTION_ADMIN")).first);
  }

  virtual void operator()(THD *thd_to_kill)
  {
    mysql_mutex_lock(&thd_to_kill->LOCK_thd_data);

    Security_context *sctx= thd_to_kill->security_context();
    /* Kill only if non-privileged thread and non slave thread.
       If an account has not yet been assigned to the security context of the
       thread we cannot tell if the account is super user or not. In this case
       we cannot kill that thread. In offline mode, after the account is
       assigned to this thread and it turns out it is not privileged user
       thread, the authentication for this thread will fail and the thread will
       be terminated.
    */
    if (sctx->has_account_assigned() &&
        !(sctx->check_access(SUPER_ACL) ||
          sctx->has_global_grant(STRING_WITH_LEN("CONNECTION_ADMIN")).first) &&
	thd_to_kill->killed != THD::KILL_CONNECTION &&
	!thd_to_kill->slave_thread)
      thd_to_kill->awake(THD::KILL_CONNECTION);

    mysql_mutex_unlock(&thd_to_kill->LOCK_thd_data);
  }
};

/*
  kills all the threads that do not have the
  SUPER privilege.

  SYNOPSIS
    killall_non_super_threads()
    thd                 Thread class
*/

void killall_non_super_threads(THD *thd)
{
  Kill_non_super_conn kill_non_super_conn(thd);
  Global_THD_manager *thd_manager= Global_THD_manager::get_instance();
  thd_manager->do_for_all_thd(&kill_non_super_conn);
}


/**
  prepares the index and data directory path.

  @param thd                    Thread handle
  @param data_file_name         Pathname for data directory
  @param index_file_name        Pathname for index directory
  @param table_name             Table name to be appended to the pathname specified

  @return false                 success
  @return true                  An error occurred
*/

bool prepare_index_and_data_dir_path(THD *thd, const char **data_file_name,
                                     const char **index_file_name,
                                     const char *table_name)
{
  int ret_val;
  const char *file_name;
  const char *directory_type;

  /*
    If a data directory path is passed, check if the path exists and append
    table_name to it.
  */
  if (data_file_name &&
      (ret_val= append_file_to_dir(thd, data_file_name, table_name)))
  {
    file_name= *data_file_name;
    directory_type= "DATA DIRECTORY";
    goto err;
  }

  /*
    If an index directory path is passed, check if the path exists and append
    table_name to it.
  */
  if (index_file_name &&
      (ret_val= append_file_to_dir(thd, index_file_name, table_name)))
  {
    file_name= *index_file_name;
    directory_type= "INDEX DIRECTORY";
    goto err;
  }

  return false;
err:
  if (ret_val == ER_PATH_LENGTH)
    my_error(ER_PATH_LENGTH, MYF(0), directory_type);
  if (ret_val == ER_WRONG_VALUE)
    my_error(ER_WRONG_VALUE, MYF(0), "path", file_name);
  return true;
}


/** If pointer is not a null pointer, append filename to it. */

int append_file_to_dir(THD *thd, const char **filename_ptr,
                       const char *table_name)
{
  char buff[FN_REFLEN],*ptr, *end;
  if (!*filename_ptr)
    return 0;					// nothing to do

  /* Check that the filename is not too long and it's a hard path */
  if (strlen(*filename_ptr) + strlen(table_name) >= FN_REFLEN - 1)
    return ER_PATH_LENGTH;

  if (!test_if_hard_path(*filename_ptr))
    return ER_WRONG_VALUE;

  /* Fix is using unix filename format on dos */
  my_stpcpy(buff,*filename_ptr);
  end=convert_dirname(buff, *filename_ptr, NullS);
  if (!(ptr= (char*) thd->alloc((size_t) (end-buff) + strlen(table_name)+1)))
    return ER_OUTOFMEMORY;                     // End of memory
  *filename_ptr=ptr;
  strxmov(ptr,buff,table_name,NullS);
  return 0;
}


Comp_creator *comp_eq_creator(bool invert)
{
  return invert?(Comp_creator *)&ne_creator:(Comp_creator *)&eq_creator;
}

Comp_creator *comp_equal_creator(bool invert MY_ATTRIBUTE((unused)))
{
  DBUG_ASSERT(!invert); // Function never called with true.
  return &equal_creator;
}


Comp_creator *comp_ge_creator(bool invert)
{
  return invert?(Comp_creator *)&lt_creator:(Comp_creator *)&ge_creator;
}


Comp_creator *comp_gt_creator(bool invert)
{
  return invert?(Comp_creator *)&le_creator:(Comp_creator *)&gt_creator;
}


Comp_creator *comp_le_creator(bool invert)
{
  return invert?(Comp_creator *)&gt_creator:(Comp_creator *)&le_creator;
}


Comp_creator *comp_lt_creator(bool invert)
{
  return invert?(Comp_creator *)&ge_creator:(Comp_creator *)&lt_creator;
}


Comp_creator *comp_ne_creator(bool invert)
{
  return invert?(Comp_creator *)&eq_creator:(Comp_creator *)&ne_creator;
}


/**
  Construct ALL/ANY/SOME subquery Item.

  @param left_expr   pointer to left expression
  @param cmp         compare function creator
  @param all         true if we create ALL subquery
  @param select_lex  pointer on parsed subquery structure

  @return
    constructed Item (or 0 if out of memory)
*/
Item * all_any_subquery_creator(Item *left_expr,
				chooser_compare_func_creator cmp,
				bool all,
				SELECT_LEX *select_lex)
{
  if ((cmp == &comp_eq_creator) && !all)       //  = ANY <=> IN
    return new Item_in_subselect(left_expr, select_lex);

  if ((cmp == &comp_ne_creator) && all)        // <> ALL <=> NOT IN
    return new Item_func_not(new Item_in_subselect(left_expr, select_lex));

  Item_allany_subselect *it=
    new Item_allany_subselect(left_expr, cmp, select_lex, all);
  if (all)
    return it->upper_item= new Item_func_not_all(it);	/* ALL */

  return it->upper_item= new Item_func_nop_all(it);      /* ANY/SOME */
}


/**
   Set proper open mode and table type for element representing target table
   of CREATE TABLE statement, also adjust statement table list if necessary.
*/

void create_table_set_open_action_and_adjust_tables(LEX *lex)
{
  TABLE_LIST *create_table= lex->query_tables;

  if (lex->create_info->options & HA_LEX_CREATE_TMP_TABLE)
    create_table->open_type= OT_TEMPORARY_ONLY;
  else
    create_table->open_type= OT_BASE_ONLY;

  if (!lex->select_lex->item_list.elements)
  {
    /*
      Avoid opening and locking target table for ordinary CREATE TABLE
      or CREATE TABLE LIKE for write (unlike in CREATE ... SELECT we
      won't do any insertions in it anyway). Not doing this causes
      problems when running CREATE TABLE IF NOT EXISTS for already
      existing log table.
    */
    create_table->set_lock({TL_READ, THR_DEFAULT});
  }
}


/**
  negate given expression.

  @param pc   current parse context
  @param expr expression for negation

  @return
    negated expression
*/

Item *negate_expression(Parse_context *pc, Item *expr)
{
  Item *negated;
  if (expr->type() == Item::FUNC_ITEM &&
      ((Item_func *) expr)->functype() == Item_func::NOT_FUNC)
  {
    /* it is NOT(NOT( ... )) */
    Item *arg= ((Item_func *) expr)->arguments()[0];
    enum_parsing_context place= pc->select->parsing_place;
    if (arg->is_bool_func() || place == CTX_WHERE || place == CTX_HAVING)
      return arg;
    /*
      if it is not boolean function then we have to emulate value of
      not(not(a)), it will be a != 0
    */
    return new Item_func_ne(arg, new Item_int_0());
  }

  if ((negated= expr->neg_transformer(pc->thd)) != 0)
    return negated;
  return new Item_func_not(expr);
}

/**
  Set the specified definer to the default value, which is the
  current user in the thread.
 
  @param[in]  thd       thread handler
  @param[out] definer   definer
*/
 
void get_default_definer(THD *thd, LEX_USER *definer)
{
  const Security_context *sctx= thd->security_context();

  definer->user.str= (char *) sctx->priv_user().str;
  definer->user.length= strlen(definer->user.str);

  definer->host.str= (char *) sctx->priv_host().str;
  definer->host.length= strlen(definer->host.str);

  definer->plugin= EMPTY_CSTR;
  definer->auth= NULL_CSTR;
  definer->uses_identified_with_clause= false;
  definer->uses_identified_by_clause= false;
  definer->uses_authentication_string_clause= false;
  definer->alter_status.update_password_expired_column= false;
  definer->alter_status.use_default_password_lifetime= true;
  definer->alter_status.expire_after_days= 0;
  definer->alter_status.update_account_locked_column= false;
  definer->alter_status.account_locked= false;
}


/**
  Create default definer for the specified THD.

  @param[in] thd         thread handler

  @return
    - On success, return a valid pointer to the created and initialized
    LEX_USER, which contains definer information.
    - On error, return 0.
*/

LEX_USER *create_default_definer(THD *thd)
{
  LEX_USER *definer;

  if (! (definer= (LEX_USER*) thd->alloc(sizeof(LEX_USER))))
    return 0;

  thd->get_definer(definer);

  return definer;
}


/**
  Retuns information about user or current user.

  @param[in] thd          thread handler
  @param[in] user         user

  @return
    - On success, return a valid pointer to initialized
    LEX_USER, which contains user information.
    - On error, return 0.
*/

LEX_USER *get_current_user(THD *thd, LEX_USER *user)
{
  if (!user || !user->user.str)  // current_user
  {
    LEX_USER *default_definer= create_default_definer(thd);
    if (default_definer)
    {
      /*
        Inherit parser semantics from the statement in which the user parameter
        was used.
        This is needed because a LEX_USER is both used as a component in an
        AST and as a specifier for a particular user in the ACL subsystem.
      */
      default_definer->uses_authentication_string_clause=
        user->uses_authentication_string_clause;
      default_definer->uses_identified_by_clause=
        user->uses_identified_by_clause;
      default_definer->uses_identified_with_clause=
        user->uses_identified_with_clause;
      default_definer->plugin.str= user->plugin.str;
      default_definer->plugin.length= user->plugin.length;
      default_definer->auth.str= user->auth.str;
      default_definer->auth.length= user->auth.length;
      default_definer->alter_status= user->alter_status;

      return default_definer;
    }
  }

  return user;
}


/**
  Check that byte length of a string does not exceed some limit.

  @param str         string to be checked
  @param err_msg     error message to be displayed if the string is too long
  @param max_byte_length  max length

  @retval
    false   the passed string is not longer than max_length
  @retval
    true    the passed string is longer than max_length

  NOTE
    The function is not used in existing code but can be useful later?
*/

static bool check_string_byte_length(const LEX_CSTRING &str,
                                     const char *err_msg,
                                     size_t max_byte_length)
{
  if (str.length <= max_byte_length)
    return false;

  my_error(ER_WRONG_STRING_LENGTH, MYF(0), str.str, err_msg, max_byte_length);

  return true;
}


/*
  Check that char length of a string does not exceed some limit.

  SYNOPSIS
  check_string_char_length()
      str              string to be checked
      err_msg          error message to be displayed if the string is too long
      max_char_length  max length in symbols
      cs               string charset

  RETURN
    false   the passed string is not longer than max_char_length
    true    the passed string is longer than max_char_length
*/


bool check_string_char_length(const LEX_CSTRING &str, const char *err_msg,
                              size_t max_char_length, const CHARSET_INFO *cs,
                              bool no_error)
{
  int well_formed_error;
  size_t res= cs->cset->well_formed_len(cs, str.str, str.str + str.length,
                                        max_char_length, &well_formed_error);

  if (!well_formed_error &&  str.length == res)
    return false;

  if (!no_error)
  {
    ErrConvString err(str.str, str.length, cs);
    my_error(ER_WRONG_STRING_LENGTH, MYF(0), err.ptr(), err_msg, max_char_length);
  }
  return true;
}


/*
  Check if path does not contain mysql data home directory
  SYNOPSIS
    test_if_data_home_dir()
    dir                     directory
    conv_home_dir           converted data home directory
    home_dir_len            converted data home directory length

  RETURN VALUES
    0	ok
    1	error  
*/
int test_if_data_home_dir(const char *dir)
{
  char path[FN_REFLEN];
  size_t dir_len;
  DBUG_ENTER("test_if_data_home_dir");

  if (!dir)
    DBUG_RETURN(0);

  (void) fn_format(path, dir, "", "",
                   (MY_RETURN_REAL_PATH|MY_RESOLVE_SYMLINKS));
  dir_len= strlen(path);
  if (mysql_unpacked_real_data_home_len<= dir_len)
  {
    if (dir_len > mysql_unpacked_real_data_home_len &&
        path[mysql_unpacked_real_data_home_len] != FN_LIBCHAR)
      DBUG_RETURN(0);

    if (lower_case_file_system)
    {
      if (!my_strnncoll(default_charset_info, (const uchar*) path,
                        mysql_unpacked_real_data_home_len,
                        (const uchar*) mysql_unpacked_real_data_home,
                        mysql_unpacked_real_data_home_len))
        DBUG_RETURN(1);
    }
    else if (!memcmp(path, mysql_unpacked_real_data_home,
                     mysql_unpacked_real_data_home_len))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/**
  Check that host name string is valid.

  @param[in] str string to be checked

  @return             Operation status
    @retval  false    host name is ok
    @retval  true     host name string is longer than max_length or
                      has invalid symbols
*/

bool check_host_name(const LEX_CSTRING &str)
{
  const char *name= str.str;
  const char *end= str.str + str.length;
  if (check_string_byte_length(str, ER_THD(current_thd, ER_HOSTNAME),
                               HOSTNAME_LENGTH))
    return true;

  while (name != end)
  {
    if (*name == '@')
    {
      my_printf_error(ER_UNKNOWN_ERROR, 
                      "Malformed hostname (illegal symbol: '%c')", MYF(0),
                      *name);
      return true;
    }
    name++;
  }
  return false;
}


class Parser_oom_handler : public Internal_error_handler
{
public:
  Parser_oom_handler()
    : m_has_errors(false), m_is_mem_error(false)
  {}
  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char*,
                                Sql_condition::enum_severity_level *level,
                                const char*)
  {
    if (*level == Sql_condition::SL_ERROR)
    {
      m_has_errors= true;
      /* Out of memory error is reported only once. Return as handled */
      if (m_is_mem_error && sql_errno == EE_CAPACITY_EXCEEDED)
        return true;
      if (sql_errno == EE_CAPACITY_EXCEEDED)
      {
        m_is_mem_error= true;
        my_error(ER_CAPACITY_EXCEEDED, MYF(0),
                 static_cast<ulonglong>(thd->variables.parser_max_mem_size),
                 "parser_max_mem_size",
                 ER_THD(thd, ER_CAPACITY_EXCEEDED_IN_PARSER));
        return true;
      }
    }
    return false;
  }
private:
  bool m_has_errors;
  bool m_is_mem_error;
};


/**
  This is a wrapper of MYSQLparse(). All the code should call parse_sql()
  instead of MYSQLparse().

  As a by product of parsing, the parser can also generate a query digest.
  To compute a digest, invoke this function as follows.

  @verbatim
    THD *thd = ...;
    const char *query_text = ...;
    uint query_length = ...;
    Object_creation_ctx *ctx = ...;
    bool rc;

    Parser_state parser_state;
    if (parser_state.init(thd, query_text, query_length)
    {
      ... handle error
    }

    parser_state.m_input.m_compute_digest= true;
    
    rc= parse_sql(the, &parser_state, ctx);
    if (! rc)
    {
      unsigned char md5[MD5_HASH_SIZE];
      char digest_text[1024];
      bool truncated;
      const sql_digest_storage *digest= & thd->m_digest->m_digest_storage;

      compute_digest_md5(digest, & md5[0]);
      compute_digest_text(digest, & digest_text[0], sizeof(digest_text), & truncated);
    }
  @endverbatim

  @param thd Thread context.
  @param parser_state Parser state.
  @param creation_ctx Object creation context.

  @return Error status.
    @retval false on success.
    @retval true on parsing error.
*/

bool parse_sql(THD *thd,
               Parser_state *parser_state,
               Object_creation_ctx *creation_ctx)
{
  DBUG_ENTER("parse_sql");
  bool ret_value;
  DBUG_ASSERT(thd->m_parser_state == NULL);
  // TODO fix to allow parsing gcol exprs after main query.
//  DBUG_ASSERT(thd->lex->m_sql_cmd == NULL);

  /* Backup creation context. */

  Object_creation_ctx *backup_ctx= NULL;

  if (creation_ctx)
    backup_ctx= creation_ctx->set_n_backup(thd);

  /* Set parser state. */

  thd->m_parser_state= parser_state;

  parser_state->m_digest_psi= NULL;
  parser_state->m_lip.m_digest= NULL;

  if (thd->m_digest != NULL)
  {
    /* Start Digest */
    parser_state->m_digest_psi= MYSQL_DIGEST_START(thd->m_statement_psi);

    if (parser_state->m_input.m_compute_digest ||
       (parser_state->m_digest_psi != NULL))
    {
      /*
        If either:
        - the caller wants to compute a digest
        - the performance schema wants to compute a digest
        set the digest listener in the lexer.
      */
      parser_state->m_lip.m_digest= thd->m_digest;
      parser_state->m_lip.m_digest->m_digest_storage.m_charset_number= thd->charset()->number;
    }
  }

  /* Parse the query. */

  /*
    Use a temporary DA while parsing. We don't know until after parsing
    whether the current command is a diagnostic statement, in which case
    we'll need to have the previous DA around to answer questions about it.
  */
  Diagnostics_area *parser_da= thd->get_parser_da();
  Diagnostics_area *da=        thd->get_stmt_da();

  Parser_oom_handler poomh;
  // Note that we may be called recursively here, on INFORMATION_SCHEMA queries.

  set_memroot_max_capacity(thd->mem_root, thd->variables.parser_max_mem_size);
  set_memroot_error_reporting(thd->mem_root, true);
  thd->push_internal_handler(&poomh);

  thd->push_diagnostics_area(parser_da, false);

  bool mysql_parse_status= MYSQLparse(thd) != 0;

  thd->pop_internal_handler();
  set_memroot_max_capacity(thd->mem_root, 0);
  set_memroot_error_reporting(thd->mem_root, false);
  /*
    Unwind diagnostics area.

    If any issues occurred during parsing, they will become
    the sole conditions for the current statement.

    Otherwise, if we have a diagnostic statement on our hands,
    we'll preserve the previous diagnostics area here so we
    can answer questions about it.  This specifically means
    that repeatedly asking about a DA won't clear it.

    Otherwise, it's a regular command with no issues during
    parsing, so we'll just clear the DA in preparation for
    the processing of this command.
  */

  if (parser_da->current_statement_cond_count() != 0)
  {
    /*
      Error/warning during parsing: top DA should contain parse error(s)!  Any
      pre-existing conditions will be replaced. The exception is diagnostics
      statements, in which case we wish to keep the errors so they can be sent
      to the client.
    */
    if (thd->lex->sql_command != SQLCOM_SHOW_WARNS &&
        thd->lex->sql_command != SQLCOM_GET_DIAGNOSTICS)
      da->reset_condition_info(thd);

    /*
      We need to put any errors in the DA as well as the condition list.
    */
    if (parser_da->is_error() && !da->is_error())
    {
      da->set_error_status(parser_da->mysql_errno(),
                           parser_da->message_text(),
                           parser_da->returned_sqlstate());
    }

    da->copy_sql_conditions_from_da(thd, parser_da);

    parser_da->reset_diagnostics_area();
    parser_da->reset_condition_info(thd);

    /*
      Do not clear the condition list when starting execution as it
      now contains not the results of the previous executions, but
      a non-zero number of errors/warnings thrown during parsing!
    */
    thd->lex->keep_diagnostics= DA_KEEP_PARSE_ERROR;
  }

  thd->pop_diagnostics_area();

  /*
    Check that if MYSQLparse() failed either thd->is_error() is set, or an
    internal error handler is set.

    The assert will not catch a situation where parsing fails without an
    error reported if an error handler exists. The problem is that the
    error handler might have intercepted the error, so thd->is_error() is
    not set. However, there is no way to be 100% sure here (the error
    handler might be for other errors than parsing one).
  */

  DBUG_ASSERT(!mysql_parse_status ||
              (mysql_parse_status && thd->is_error()) ||
              (mysql_parse_status && thd->get_internal_handler()));

  /* Reset parser state. */

  thd->m_parser_state= NULL;

  /* Restore creation context. */

  if (creation_ctx)
    creation_ctx->restore_env(thd, backup_ctx);

  /* That's it. */

  ret_value= mysql_parse_status || thd->is_fatal_error;

  if ((ret_value == 0) &&
      (parser_state->m_digest_psi != NULL))
  {
    /*
      On parsing success, record the digest in the performance schema.
    */
    DBUG_ASSERT(thd->m_digest != NULL);
    MYSQL_DIGEST_END(parser_state->m_digest_psi,
                     & thd->m_digest->m_digest_storage);
  }

  DBUG_RETURN(ret_value);
}

/**
  @} (end of group Runtime_Environment)
*/



/**
  Check and merge "CHARACTER SET cs [ COLLATE cl ]" clause

  @param cs character set pointer.
  @param cl collation pointer.

  Check if collation "cl" is applicable to character set "cs".

  If "cl" is NULL (e.g. when COLLATE clause is not specified),
  then simply "cs" is returned.
  
  @return Error status.
    @retval NULL, if "cl" is not applicable to "cs".
    @retval pointer to merged CHARSET_INFO on success.
*/


const CHARSET_INFO*
merge_charset_and_collation(const CHARSET_INFO *cs, const CHARSET_INFO *cl)
{
  if (cl)
  {
    if (!my_charset_same(cs, cl))
    {
      my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0), cl->name, cs->csname);
      return NULL;
    }
    return cl;
  }
  return cs;
}


bool merge_sp_var_charset_and_collation(const CHARSET_INFO **to,
                                        const CHARSET_INFO *cs,
                                        const CHARSET_INFO *cl)
{
  if (cs)
  {
    *to= merge_charset_and_collation(cs, cl);
    return *to == NULL;
  }

  if (cl)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
        "COLLATE with no CHARACTER SET in SP parameters, RETURNS, DECLARE");
    return true;
  }

  *to= NULL;
  return false;
}
