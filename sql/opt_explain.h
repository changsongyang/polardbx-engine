/* Copyright (c) 2011, 2017, Oracle and/or its affiliates. All rights reserved.

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


#ifndef OPT_EXPLAIN_INCLUDED
#define OPT_EXPLAIN_INCLUDED

/**
  @file sql/opt_explain.h
  EXPLAIN @<command@>.

Single table UPDATE/DELETE commands are explained by the 
explain_single_table_modification() function.

A query expression (complete SELECT query possibly including
subqueries and unions), INSERT...SELECT and multitable UPDATE/DELETE
commands are explained like this:

(1) explain_query_expression()

Is the entry point. Forwards the job to explain_unit().

(2) explain_unit()

Is for a SELECT_LEX_UNIT, prepares, optimizes, explains one JOIN for
each "top-level" SELECT_LEXs of the unit (like: all SELECTs of a
UNION; but not subqueries), and one JOIN for the fake SELECT_LEX of
UNION); each JOIN explain (JOIN::exec()) calls explain_query_specification()

(3) explain_query_specification()

Is for a single SELECT_LEX (fake or not). It needs a prepared and
optimized JOIN, for which it builds the EXPLAIN rows. But it also
launches the EXPLAIN process for "inner units" (==subqueries of this
SELECT_LEX), by calling explain_unit() for each of them. 
*/

#include "my_base.h"
#include "sql/opt_explain_format.h"
#include "sql/parse_tree_node_base.h"
#include "sql/query_result.h"            // Query_result_send
#include "sql/sql_lex.h"
#include "sys/types.h"

class Item;
class QEP_TAB;
class THD;
struct TABLE;
template <class T> class List;

extern const char *join_type_str[];

/** Table modification plan for JOIN-less statements (update/delete) */
class Modification_plan
{
public:
  THD *const thd;           ///< Owning thread
  const enum_mod_type mod_type;///< Modification type - MT_INSERT/MT_UPDATE/etc
  TABLE *table;             ///< Table to modify

  QEP_TAB *tab;             ///< QUICK access method + WHERE clause
  uint key;                 ///< Key to use
  ha_rows limit;            ///< Limit
  bool need_tmp_table;      ///< Whether tmp table needs to be used
  bool need_sort;           ///< Whether to use filesort
  bool used_key_is_modified;///< Whether the key used to scan is modified
  const char *message;      ///< Arbitrary message
  bool zero_result;         ///< TRUE <=> plan will not be executed
  ha_rows examined_rows;    ///< # of rows expected to be examined in the table

  Modification_plan(THD *thd_arg,
                    enum_mod_type mt, QEP_TAB *qep_tab,
                    uint key_arg, ha_rows limit_arg, bool need_tmp_table_arg,
                    bool need_sort_arg, bool used_key_is_modified_arg,
                    ha_rows rows);

  Modification_plan(THD *thd_arg,
                    enum_mod_type mt, TABLE *table_arg,
                    const char *message_arg, bool zero_result_arg,
                    ha_rows rows);

  ~Modification_plan();

private:
  void register_in_thd();
};


/**
  EXPLAIN functionality for Query_result_insert, Query_result_update and
  Query_result_delete.

  This class objects substitute Query_result_insert, Query_result_update and
  Query_result_delete data interceptor objects to implement EXPLAIN for INSERT,
  REPLACE and multi-table UPDATE and DELETE queries.
  Query_result_explain class object initializes tables like Query_result_insert,
  Query_result_update or Query_result_delete data interceptor do, but it
  suppresses table data modification by the underlying interceptor object.
  Thus, we can use Query_result_explain object in the context of EXPLAIN INSERT/
  REPLACE/UPDATE/DELETE query like we use Query_result_send in the context of
  EXPLAIN SELECT command:
  1) in presence of lex->describe flag, pass Query_result_explain object to
     execution function,
  2) it calls prepare(), optimize() and start_execution() functions
     to mark modified tables etc.
*/

class Query_result_explain final : public Query_result_send
{
protected:
  /**
    Pointer to underlying Query_result_insert, Query_result_update or
    Query_result_delete object.
  */
  Query_result *interceptor;

public:
  Query_result_explain(THD *thd, SELECT_LEX_UNIT *unit_arg,
                       Query_result *interceptor_arg)
  : Query_result_send(thd), interceptor(interceptor_arg)
  { unit= unit_arg; }

protected:
  bool prepare(List<Item> &list, SELECT_LEX_UNIT *u) override
  {
    return Query_result_send::prepare(list, u) || interceptor->prepare(list, u);
  }

  bool start_execution(void) override
  {
    return Query_result_send::start_execution() ||
           interceptor->start_execution();
  }

  bool optimize() override
  {
    return Query_result_send::optimize() || interceptor->optimize();
  }

  void cleanup() override
  {
    Query_result_send::cleanup();
    interceptor->cleanup();
  }
};


bool explain_no_table(THD *thd, SELECT_LEX *select_lex, const char *message,
                      enum_parsing_context ctx);
bool explain_single_table_modification(THD *ethd,
                                       const Modification_plan *plan,
                                       SELECT_LEX *select);
bool explain_query(THD *thd, SELECT_LEX_UNIT *unit);
bool explain_query_specification(THD *ethd, SELECT_LEX *select_lex,
                                 enum_parsing_context ctx);
void mysql_explain_other(THD *thd);

#endif /* OPT_EXPLAIN_INCLUDED */
