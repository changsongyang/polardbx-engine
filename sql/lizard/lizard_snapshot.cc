/*****************************************************************************

Copyright (c) 2013, 2023, Alibaba and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

#include "lizard_iface.h"

#include "sql/lizard/lizard_snapshot.h"

#include "sql/parse_tree_node_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "sql/sql_lex.h"
#include "sql/item.h"
#include "sql/item_func.h"
#include "sql/item_timefunc.h"

namespace lizard {

/* Cast text or decimal to integer */
class Item_typecast_cnum : public Item_int_func {
 public:
  Item_typecast_cnum(Item *a) : Item_int_func(a) {}

  enum Functype functype() const { return TYPECAST_FUNC; }
  uint decimal_precision() const { return args[0]->decimal_precision(); }

  bool resolve_type(THD *) {
    fix_char_length(std::min<uint32>(args[0]->max_char_length(),
                                     MY_INT64_NUM_DECIMAL_DIGITS));
    return reject_geometry_args(arg_count, args, this);
  }

  longlong val_int() {
    longlong value;
    int error;
    size_t length;
    const char *end;
    const CHARSET_INFO *cs;
    char buff[MAX_FIELD_WIDTH], *start;
    String tmp(buff, sizeof(buff), &my_charset_bin), *res;
    Item_result result_type = args[0]->cast_to_int_type();

    if (result_type == DECIMAL_RESULT) {
      value = args[0]->val_int();
      null_value = args[0]->null_value;
      return value;
    }

    DBUG_ASSERT(result_type == STRING_RESULT);

    /* null value */
    if (!(res = args[0]->val_str(&tmp))) {
      null_value = 1;
      goto fail;
    }

    null_value = 0;
    start = res->ptr();
    length = res->length();
    cs = res->charset();
    end = start + length;

    value = cs->cset->strtoll10(cs, start, &end, &error);

    if (error > 0 || end != start + length) goto fail;

    return value;

  fail:
    my_error(ER_AS_OF_BAD_SCN_TYPE, MYF(0));
    return 0;
  }
};

class Item_typecast_scn : public Item_typecast_cnum {
 public:
  Item_typecast_scn(Item *a) : Item_typecast_cnum(a) {}

  const char *func_name() const { return "cast_as_scn"; }

  void print(const THD *thd, String *str, enum_query_type query_type) const {
    str->append(STRING_WITH_LEN("cast("));
    args[0]->print(thd, str, query_type);
    str->append(STRING_WITH_LEN(" as scn)"));
  }
};

class Item_typecast_gcn : public Item_typecast_cnum {
 public:
  Item_typecast_gcn(Item *a) : Item_typecast_cnum(a) {}

  const char *func_name() const { return "cast_as_gcn"; }

  void print(const THD *thd, String *str, enum_query_type query_type) const {
    str->append(STRING_WITH_LEN("cast("));
    args[0]->print(thd, str, query_type);
    str->append(STRING_WITH_LEN(" as gcn)"));
  }
};


static bool is_string_item(Item *item) {
  switch (item->data_type()) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      return true;
    default:
      return false;
  }
}

static bool is_decimal_zero(Item *item) {
  switch (item->data_type()) {
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      return (item->decimals == 0);
    default:
      return false;
  }
}

static bool fix_and_check_const(THD *thd, Item **item) {
  if (!(*item)->fixed && (*item)->fix_fields(thd, item)) return true;

  if (!(*item)->const_for_execution()) {
    my_error(ER_AS_OF_EXPR_NOT_CONSTANT, MYF(0));
    return true;
  }

  return false;
}

static bool try_cast_to_datetime(THD *thd, Item **item) {
  /* We only try to cast string item to datetime.
  If it cannot cast, errors will be raised when evaluating */
  if (!is_string_item(*item)) return true;

  Item *cast = new Item_typecast_datetime(*item);
  if (!cast) return true;

  cast->fix_fields(thd, item);
  thd->change_item_tree(item, cast);
  return false;
}

static bool try_cast_to_scn(THD *thd, Item **item) {
  /* We only try to cast string or decimal item to scn.
  If it cannot cast, errors will be raised when evaluating */
  if (!is_string_item(*item) && !is_decimal_zero(*item)) return true;

  Item *cast = new Item_typecast_scn(*item);
  if (!cast) return true;

  cast->fix_fields(thd, item);
  thd->change_item_tree(item, cast);
  return false;
}

static bool try_cast_to_gcn(THD *thd, Item **item) {
  /* We only try to cast string or decimal item to scn.
  If it cannot cast, errors will be raised when evaluating */
  if (!is_string_item(*item) && !is_decimal_zero(*item)) return true;

  Item *cast = new Item_typecast_gcn(*item);
  if (!cast) return true;

  cast->fix_fields(thd, item);
  thd->change_item_tree(item, cast);
  return false;
}


/**
  Itemize the snapshot item and hook onto TABLE_LIST.

  My_error if failure.

  @retval	true	Failure
  @retval	false	Success
*/
bool Snapshot_hint::itemize(Parse_context *pc, TABLE_LIST *owner) {
  DBUG_ASSERT(pc && pc->thd && pc->thd->lex);
  DBUG_ASSERT(owner);

  SELECT_LEX *select = owner->select_lex;
  DBUG_ASSERT(select);

  if (select->select_number == 1 && pc->thd->lex->is_update_stmt) {
    my_error(ER_AS_OF_NOT_SELECT, MYF(0));
    return true;
  }

  if (m_item->itemize(pc, &m_item)) return true;

  owner->snapshot_hint = this;

  pc->thd->lex->safe_to_cache_query = false;

  return false;
}

/**
  Fix fields

  My_error if failure.

  @retval	true	Failure
  @retval	false	Success
 */
bool Snapshot_time_hint::fix_fields(THD *thd) {
  DBUG_ASSERT(m_item);

  if (fix_and_check_const(thd, &m_item)) return true;

  if (!m_item->is_temporal_with_date_and_time()) {
    if (try_cast_to_datetime(thd, &m_item)) {
      my_error(ER_AS_OF_BAD_TIMESTAMP_TYPE, MYF(0));
      return true;
    }
  }
  return false;
}

/** Calculate second from hint item. */
bool Snapshot_time_hint::val_int(uint64_t *value) {
  int unused;
  struct timeval tm;

  if (m_item->get_timeval(&tm, &unused)) {
    my_error(ER_AS_OF_BAD_TIMESTAMP_TYPE, MYF(0));
    return true;
  }

  if (current_thd->is_error()) return true;

  *value = tm.tv_sec;
  return false;
}

/**
  Evoke the table vision.
  My_error if failure.

  @retval	true	Failure
  @retval	false	Success
 */
int Snapshot_hint::evoke_vision(TABLE *table, my_scn_t scn, THD *thd) {
  uint64_t value;
  bool error;
  if ((error = val_int(&value))) {
    return error;
  }

  Snapshot_vision *vision = table->table_snapshot.get(type());
  vision->store_int(value);

  if (type() == AS_OF_GCN) {
    (dynamic_cast<Snapshot_gcn_vision *>(vision))->store_current_scn(scn);
  }

  return table->table_snapshot.activate(vision, thd);
}

/**
  Fix fields

  My_error if failure.

  @retval	true	Failure
  @retval	false	Success
 */
bool Snapshot_scn_hint::fix_fields(THD *thd) {
  if (fix_and_check_const(thd, &m_item)) return true;

  if (!is_integer_type(m_item->data_type())) {
    if (try_cast_to_scn(thd, &m_item)) {
      my_error(ER_AS_OF_BAD_SCN_TYPE, MYF(0));
      return true;
    }
  }

  return false;
}

bool Snapshot_scn_hint::val_int(uint64_t *value) {
  uint64_t val = m_item->val_uint();

  if (current_thd->is_error()) return true;

  *value = val;
  return false;
}

/**
  Fix fields

  My_error if failure.

  @retval	true	Failure
  @retval	false	Success
 */

bool Snapshot_gcn_hint::fix_fields(THD *thd) {
  if (fix_and_check_const(thd, &m_item)) return true;

  if (!is_integer_type(m_item->data_type())) {
    if (try_cast_to_gcn(thd, &m_item)) {
      my_error(ER_AS_OF_BAD_SCN_TYPE, MYF(0));
      return true;
    }
  }

  return false;
}

bool Snapshot_gcn_hint::val_int(uint64_t *value) {
  uint64_t val = m_item->val_uint();

  if (current_thd->is_error()) return true;

  *value = val;
  return false;
}

void Snapshot_gcn_vision::after_activate() {
  gcs_set_snapshot_gcn_if_bigger(val_int());
}

/*
  Reset snapshot, increase the snapshot table count.
*/
void init_table_snapshot(TABLE *table, THD *thd) {
  DBUG_ASSERT(thd && thd->lex);

  table->table_snapshot.release_vision();

  if (table->pos_in_table_list->snapshot_hint)
    thd->lex->table_snap_expr_count_to_evaluate++;
}

/*
  Evaluate table snapshot expressions.

  @return true   If some error occurs.
*/
bool evaluate_snapshot(THD *thd, const LEX *lex) {
  int error;
  DBUG_ASSERT(thd->lex->table_snap_expr_count_to_evaluate >= 0);

  /* Cases that need not do evaluating */
  if ((thd->lex->table_snap_expr_count_to_evaluate == 0) || /* No snapshot */
      (lex->is_explain() && !lex->is_explain_analyze))      /* Not analyze */
    return false;

  DBUG_ASSERT(thd->open_tables);

  my_scn_t current_scn = MYSQL_SCN_NULL;
  ha_acquire_scn(&current_scn);

  for (TABLE *table = thd->open_tables; table; table = table->next) {
    DBUG_ASSERT(table->pos_in_table_list);
    DBUG_ASSERT(!table->table_snapshot.is_activated());

    Snapshot_hint *hint = table->pos_in_table_list->snapshot_hint;
    if (hint) {
      error = hint->evoke_vision(table, current_scn, thd);
      if (error) {
        table->file->print_error(error, 0);
        return true;
      }
    }
  }

  return false;
}

/**
Simulate asof syntax by adding Item onto Table_snapshot.
@param[in]        thd         current context
@param[in/out]    snapshot    ASOF attributes
*/
void simulate_snapshot_clause(THD *thd, TABLE_LIST *all_tables) {
  Item *item = nullptr;
  ulonglong gcn = thd->variables.innodb_snapshot_gcn;

  /**
   * The value is max gcn + 1 if innodb_current_snapshot_gcn is setted.
   * It will make sure lastest record can be seen by current gcn.
   */
  if (gcn == MYSQL_GCN_NULL && thd->variables.innodb_current_snapshot_gcn) {
    ha_acquire_gcn((uint64 *)&gcn);
  }

  if (gcn != MYSQL_GCN_NULL) {
    TABLE_LIST *table;
    for (table = all_tables; table; table = table->next_global) {
      if (table->snapshot_hint == nullptr) {
        item = new (thd->mem_root) Item_int(gcn);
        Snapshot_hint *hint = new (thd->mem_root) Snapshot_gcn_hint(item);
        table->snapshot_hint = hint;
      }
    }
  }
}

int Table_snapshot::exchange_timestamp_vision_to_scn_vision(
    Snapshot_vision **vision, THD *thd) {
  int error = 0;

  my_utc_t utc_second = (*vision)->val_int();
  my_scn_t scn;

  error = convert_timestamp_to_scn(thd, utc_second, &scn);

  if (!error) {
    /** Change the vision. */
    *vision = get(AS_OF_SCN);

    (*vision)->store_int(scn);
  }

  return error;
}

}  // namespace lizard

bool TABLE_LIST::process_snapshot_hint(THD *thd) {
  if (snapshot_hint) {
    return snapshot_hint->fix_fields(thd);
  }
  return false;
}

