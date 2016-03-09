/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/*
  Atomic rename of table;  RENAME TABLE t1 to t2, tmp to t1 [,...]
*/

#include "sql_rename.h"

#include "log.h"              // query_logger
#include "mysqld.h"           // lower_case_table_names
#include "sql_base.h"         // tdc_remove_table,
                              // lock_table_names,
#include "sql_cache.h"        // query_cache
#include "sql_class.h"        // THD
#include "sql_handler.h"      // mysql_ha_rm_tables
#include "sql_table.h"        // write_bin_log,
                              // build_table_filename
#include "sql_trigger.h"      // change_trigger_table_name
#include "sql_view.h"         // mysql_rename_view

#include "dd/dd_table.h"      // dd::table_exists
#include "dd/cache/dictionary_client.h"// dd::cache::Dictionary_client
#include "dd/types/abstract_table.h" // dd::Abstract_table


static TABLE_LIST *rename_tables(THD *thd, TABLE_LIST *table_list,
				 bool skip_error, bool *int_commit_done);

static TABLE_LIST *reverse_table_list(TABLE_LIST *table_list);

/*
  Every two entries in the table_list form a pair of original name and
  the new name.
*/

bool mysql_rename_tables(THD *thd, TABLE_LIST *table_list, bool silent)
{
  bool error= 1;
  bool binlog_error= 0;
  TABLE_LIST *ren_table= 0;
  int to_table;
  char *rename_log_table[2]= {NULL, NULL};
  bool int_commit_done= false;
  DBUG_ENTER("mysql_rename_tables");

  /*
    Avoid problems with a rename on a table that we have locked or
    if the user is trying to to do this in a transcation context
  */

  if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction())
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(1);
  }

  mysql_ha_rm_tables(thd, table_list);

  if (query_logger.is_log_table_enabled(QUERY_LOG_GENERAL) ||
      query_logger.is_log_table_enabled(QUERY_LOG_SLOW))
  {

    /*
      Rules for rename of a log table:

      IF   1. Log tables are enabled
      AND  2. Rename operates on the log table and nothing is being
              renamed to the log table.
      DO   3. Throw an error message.
      ELSE 4. Perform rename.
    */

    for (to_table= 0, ren_table= table_list; ren_table;
         to_table= 1 - to_table, ren_table= ren_table->next_local)
    {
      int log_table_rename= 0;

      if ((log_table_rename= query_logger.check_if_log_table(ren_table, true)))
      {
        /*
          as we use log_table_rename as an array index, we need it to start
          with 0, while QUERY_LOG_SLOW == 1 and QUERY_LOG_GENERAL == 2.
          So, we shift the value to start with 0;
        */
        log_table_rename--;
        if (rename_log_table[log_table_rename])
        {
          if (to_table)
            rename_log_table[log_table_rename]= NULL;
          else
          {
            /*
              Two renames of "log_table TO" w/o rename "TO log_table" in
              between.
            */
            my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0), ren_table->table_name,
                     ren_table->table_name);
            goto err;
          }
        }
        else
        {
          if (to_table)
          {
            /*
              Attempt to rename a table TO log_table w/o renaming
              log_table TO some table.
            */
            my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0), ren_table->table_name,
                     ren_table->table_name);
            goto err;
          }
          else
          {
            /* save the name of the log table to report an error */
            rename_log_table[log_table_rename]=
              const_cast<char*>(ren_table->table_name);
          }
        }
      }
    }
    if (rename_log_table[0] || rename_log_table[1])
    {
      if (rename_log_table[0])
        my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0), rename_log_table[0],
                 rename_log_table[0]);
      else
        my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0), rename_log_table[1],
                 rename_log_table[1]);
      goto err;
    }
  }

  if (lock_table_names(thd, table_list, 0, thd->variables.lock_wait_timeout, 0))
    goto err;

  for (ren_table= table_list; ren_table; ren_table= ren_table->next_local)
    tdc_remove_table(thd, TDC_RT_REMOVE_ALL, ren_table->db,
                     ren_table->table_name, FALSE);

  error=0;
  /*
    An exclusive lock on table names is satisfactory to ensure
    no other thread accesses this table.
  */
  if ((ren_table= rename_tables(thd, table_list, 0, &int_commit_done)))
  {
    /* Rename didn't succeed;  rename back the tables in reverse order */
    TABLE_LIST *table;

#ifdef WORKAROUND_TO_BE_REMOVED_BY_WL7016_AND_WL7896
    if (int_commit_done)
    {
#else
      int_commit_done= true;
#endif
      /* Reverse the table list */
      table_list= reverse_table_list(table_list);

      /* Find the last renamed table */
      for (table= table_list;
	   table->next_local != ren_table ;
	   table= table->next_local->next_local) ;
      table= table->next_local->next_local;		// Skip error table
      /* Revert to old names */
      rename_tables(thd, table, 1, &int_commit_done);

      /* Revert the table list (for prepared statements) */
      table_list= reverse_table_list(table_list);
#ifdef WORKAROUND_TO_BE_REMOVED_BY_WL7016_AND_WL7896
    }
#endif

    error= 1;
  }

  if (!silent && !error)
  {
    binlog_error= write_bin_log(thd, true,
                                thd->query().str, thd->query().length,
                                !int_commit_done);
    if (!binlog_error)
      my_ok(thd);
  }

  if (!error)
    query_cache.invalidate(thd, table_list, FALSE);

err:
  DBUG_RETURN(error || binlog_error);
}


/*
  reverse table list

  SYNOPSIS
    reverse_table_list()
    table_list pointer to table _list

  RETURN
    pointer to new (reversed) list
*/
static TABLE_LIST *reverse_table_list(TABLE_LIST *table_list)
{
  TABLE_LIST *prev= 0;

  while (table_list)
  {
    TABLE_LIST *next= table_list->next_local;
    table_list->next_local= prev;
    prev= table_list;
    table_list= next;
  }
  return (prev);
}


/**
  Rename a single table or a view.

  @param[in]      thd               Thread handle.
  @param[in]      ren_table         A table/view to be renamed.
  @param[in]      new_db            The database to which the
                                    table to be moved to.
  @param[in]      new_table_name    The new table/view name.
  @param[in]      new_table_alias   The new table/view alias.
  @param[in]      skip_error        Whether to skip errors.
  @param[in/out]  int_commit_done   Whether intermediate commits
                                    were done.

  @return False on success, True if rename failed.
*/

static bool
do_rename(THD *thd, TABLE_LIST *ren_table,
          const char *new_db, const char *new_table_name,
          const char *new_table_alias, bool skip_error,
          bool *int_commit_done)
{
  const char *new_alias= new_table_name;
  const char *old_alias= ren_table->table_name;

  DBUG_ENTER("do_rename");

  if (lower_case_table_names == 2)
  {
    old_alias= ren_table->alias;
    new_alias= new_table_alias;
  }
  DBUG_ASSERT(new_alias);

  // Fail if the target table already exists
  const dd::Abstract_table *new_table;
  if (thd->dd_client()->acquire_uncached_uncommitted<dd::Abstract_table>(new_db,
                          new_alias, &new_table))
    DBUG_RETURN(true);                         // This error cannot be skipped
  if (new_table)
  {
    delete new_table;
    my_error(ER_TABLE_EXISTS_ERROR, MYF(0), new_alias);
    DBUG_RETURN(true);                         // This error cannot be skipped
  }

  // Get the table type of the old table, and fail if it does not exist
  std::unique_ptr<dd::Abstract_table> old_table_def;
  if (!(old_table_def=
          dd::acquire_uncached_uncommitted_table<dd::Abstract_table>(thd,
                ren_table->db, old_alias)))
    DBUG_RETURN(!skip_error);

  // So here we know the source table exists and the target table does
  // not exist. Next is to act based on the table type.
  switch (old_table_def->type())
  {
  case dd::enum_table_type::BASE_TABLE:
    {
      handlerton *hton= NULL;
      // If the engine is not found, my_error() has already been called
      if (dd::table_storage_engine(thd, ren_table, &hton))
        DBUG_RETURN(!skip_error);

      /*
        Commit changes to data-dictionary immediately after renaming
        table in storage negine if SE doesn't support atomic DDL or
        there were intermediate commits already. In the latter case
        the whole statement is not crash-safe anyway and clean-up is
        simpler this way.
      */
      const bool do_commit= *int_commit_done ||
                            (hton->flags & HTON_SUPPORTS_ATOMIC_DDL);

      // If renaming fails, my_error() has already been called
      if (mysql_rename_table(thd, hton, ren_table->db, old_alias, new_db,
                             new_alias, (do_commit ? 0 : NO_DD_COMMIT)))
        DBUG_RETURN(!skip_error);

      *int_commit_done|= do_commit;

#ifndef WORKAROUND_TO_BE_REMOVED_BY_WL7896
      // If we fail to update the triggers appropriately, we revert the
      // changes done and report an error.
      if (change_trigger_table_name(thd, ren_table->db, old_alias,
                                         ren_table->table_name,
                                         new_db, new_alias))
      {
        (void) mysql_rename_table(thd, hton, new_db, new_alias,
                                  ren_table->db, old_alias, NO_FK_CHECKS);
        DBUG_RETURN(!skip_error);
      }
#endif
      break;
    }
  case dd::enum_table_type::SYSTEM_VIEW: // Fall through
  case dd::enum_table_type::USER_VIEW:
    {
      // Changing the schema of a view is not allowed.
      if (strcmp(ren_table->db, new_db))
      {
        my_error(ER_FORBID_SCHEMA_CHANGE, MYF(0), ren_table->db, new_db);
        DBUG_RETURN(!skip_error);
      }
      else if (mysql_rename_view(thd, new_db, new_alias, ren_table,
                                 *int_commit_done))
        DBUG_RETURN(!skip_error);
      break;
    }
  default:
    DBUG_ASSERT(false); /* purecov: deadcode */
  }

  // Now, we know that rename succeeded, and can log the schema access
  thd->add_to_binlog_accessed_dbs(ren_table->db);
  thd->add_to_binlog_accessed_dbs(new_db);

  DBUG_RETURN(false);
}
/*
  Rename all tables in list;
  Return pointer to wrong entry if something goes
  wrong.  Note that the table_list may be empty!
*/

/**
  Rename all tables/views in the list.

  @param[in]      thd               Thread handle.
  @param[in]      table_list        List of tables to rename.
  @param[in]      skip_error        Whether to skip errors.
  @param[in/out]  int_commit_done   Whether intermediate commits
                                    were done.

  @note
    Take a table/view name from and odd list element and rename it to a
    the name taken from list element+1. Note that the table_list may be
    empty.

  @return 0 - on success, pointer to problematic entry if something
          goes wrong.
*/

static TABLE_LIST *
rename_tables(THD *thd, TABLE_LIST *table_list, bool skip_error,
              bool *int_commit_done)
{
  TABLE_LIST *ren_table, *new_table;

  DBUG_ENTER("rename_tables");

  for (ren_table= table_list; ren_table; ren_table= new_table->next_local)
  {
    new_table= ren_table->next_local;
    if (do_rename(thd, ren_table, new_table->db, new_table->table_name,
                  new_table->alias, skip_error, int_commit_done))
      DBUG_RETURN(ren_table);
  }
  DBUG_RETURN(0);
}
