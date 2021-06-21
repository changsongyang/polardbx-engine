/* Copyright (c) 2000, 2021, Oracle and/or its affiliates.

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

#include "sql/range_optimizer/group_min_max.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <new>

#include "my_dbug.h"
#include "my_sys.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "prealloced_array.h"
#include "sql/handler.h"
#include "sql/item_sum.h"
#include "sql/psi_memory_key.h"
#include "sql/range_optimizer/internal.h"
#include "sql/range_optimizer/range_scan.h"
#include "sql/range_optimizer/tree.h"
#include "sql/sql_class.h"
#include "sql/sql_list.h"
#include "sql/sql_optimizer.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/thr_malloc.h"

void QUICK_GROUP_MIN_MAX_SELECT::add_info_string(String *str) {
  str->append(STRING_WITH_LEN("index_for_group_by("));
  str->append(index_info->name);
  str->append(')');
}

/*
  Construct new quick select for group queries with min/max.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::QUICK_GROUP_MIN_MAX_SELECT()
    table             The table being accessed
    join              Descriptor of the current query
    have_min          true if the query selects a MIN function
    have_max          true if the query selects a MAX function
    min_max_arg_part  The only argument field of all MIN/MAX functions
    group_prefix_len  Length of all key parts in the group prefix
    prefix_key_parts  All key parts in the group prefix
    index_info        The index chosen for data access
    use_index         The id of index_info
    read_cost         Cost of this access method
    records           Number of records returned
    key_infix_len     Length of the key infix appended to the group prefix
    key_infix         Infix of constants from equality predicates
    parent_alloc      Memory pool for this and quick_prefix_query_block data
    is_index_scan     get the next different key not by jumping on it via
                      index read, but by scanning until the end of the
                      rows with equal key value.

  RETURN
    None
*/

QUICK_GROUP_MIN_MAX_SELECT::QUICK_GROUP_MIN_MAX_SELECT(
    TABLE *table, JOIN *join_arg, bool have_min_arg, bool have_max_arg,
    bool have_agg_distinct_arg, KEY_PART_INFO *min_max_arg_part_arg,
    uint group_prefix_len_arg, uint group_key_parts_arg,
    uint used_key_parts_arg, KEY *index_info_arg, uint use_index,
    const Cost_estimate *read_cost_arg, ha_rows records_arg,
    uint key_infix_len_arg, MEM_ROOT *parent_alloc, bool is_index_scan_arg)
    : join(join_arg),
      index_info(index_info_arg),
      group_prefix_len(group_prefix_len_arg),
      group_key_parts(group_key_parts_arg),
      have_min(have_min_arg),
      have_max(have_max_arg),
      have_agg_distinct(have_agg_distinct_arg),
      seen_first_key(false),
      min_max_arg_part(min_max_arg_part_arg),
      key_infix_len(key_infix_len_arg),
      min_max_ranges(PSI_INSTRUMENT_ME),
      key_infix_ranges(PSI_INSTRUMENT_ME),
      is_index_scan(is_index_scan_arg),
      alloc(key_memory_quick_group_min_max_select_root,
            join->thd->variables.range_alloc_block_size) {
  head = table;
  index = use_index;
  record = head->record[0];
  tmp_record = head->record[1];
  cost_est = *read_cost_arg;
  records = records_arg;
  used_key_parts = used_key_parts_arg;
  real_key_parts = used_key_parts_arg;
  key_infix_parts = used_key_parts_arg - group_key_parts_arg;
  real_prefix_len = group_prefix_len + key_infix_len;
  group_prefix = nullptr;
  min_max_arg_len = min_max_arg_part ? min_max_arg_part->store_length : 0;
  min_max_keypart_asc =
      min_max_arg_part ? !(min_max_arg_part->key_part_flag & HA_REVERSE_SORT)
                       : false;
  memset(cur_infix_range_position, 0, sizeof(cur_infix_range_position));

  /*
    We can't have parent_alloc set as the init function can't handle this case
    yet.
  */
  assert(!parent_alloc);
  join->thd->mem_root = &alloc;
}

/*
  Do post-constructor initialization.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::init()

  DESCRIPTION
    The method performs initialization that cannot be done in the constructor
    such as memory allocations that may fail. It allocates memory for the
    group prefix and inifix buffers, and for the lists of MIN/MAX item to be
    updated during execution.

  RETURN
    0      OK
    other  Error code
*/

int QUICK_GROUP_MIN_MAX_SELECT::init() {
  if (group_prefix) /* Already initialized. */
    return 0;

  if (!(last_prefix = (uchar *)alloc.Alloc(group_prefix_len))) return 1;
  /*
    We may use group_prefix to store keys with all select fields, so allocate
    enough space for it.
  */
  if (!(group_prefix = (uchar *)alloc.Alloc(real_prefix_len + min_max_arg_len)))
    return 1;

  if (key_infix_len > 0) {
    /*
      The memory location pointed to by key_infix will be deleted soon, so
      allocate a new buffer and copy the key_infix into it.
    */
    for (uint i = 0; i < key_infix_parts; i++) {
      Quick_ranges *tmp = new (std::nothrow) Quick_ranges(PSI_INSTRUMENT_ME);
      key_infix_ranges.push_back(tmp);
    }
  }

  if (min_max_arg_part) {
    if (have_min) {
      if (!(min_functions = new (*THR_MALLOC) List<Item_sum>)) return 1;
    } else
      min_functions = nullptr;
    if (have_max) {
      if (!(max_functions = new (*THR_MALLOC) List<Item_sum>)) return 1;
    } else
      max_functions = nullptr;

    Item_sum *min_max_item;
    Item_sum **func_ptr = join->sum_funcs;
    while ((min_max_item = *(func_ptr++))) {
      if (have_min && (min_max_item->sum_func() == Item_sum::MIN_FUNC))
        min_functions->push_back(min_max_item);
      else if (have_max && (min_max_item->sum_func() == Item_sum::MAX_FUNC))
        max_functions->push_back(min_max_item);
    }
  }

  return 0;
}

QUICK_GROUP_MIN_MAX_SELECT::~QUICK_GROUP_MIN_MAX_SELECT() {
  DBUG_TRACE;
  if (head->file->inited)
    /*
      We may have used this object for index access during
      create_sort_index() and then switched to rnd access for the rest
      of execution. Since we don't do cleanup until now, we must call
      ha_*_end() for whatever is the current access method.
    */
    head->file->ha_index_or_rnd_end();

  for (uint i = 0; i < key_infix_parts; i++) delete key_infix_ranges[i];
  delete quick_prefix_query_block;
}

/*
   Construct a new QUICK_RANGE object from a SEL_ARG object, and
   add it to the either min_max_ranges or key_infix_ranges array.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::add_range()
    sel_range  Range object from which QUICK_RANGE object is created
    idx        Index of the keypart for which ranges are being added.
               It is negative for ranges on min/max keypart.

  NOTES
    If sel_arg is an infinite range, e.g. (x < 5 or x > 4),
    then skip it and do not construct a quick range.

  RETURN
    false on success
    true  otherwise
*/

bool QUICK_GROUP_MIN_MAX_SELECT::add_range(SEL_ARG *sel_range, int idx) {
  QUICK_RANGE *range;
  uint range_flag = sel_range->min_flag | sel_range->max_flag;

  /* Skip (-inf,+inf) ranges, e.g. (x < 5 or x > 4). */
  if ((range_flag & NO_MIN_RANGE) && (range_flag & NO_MAX_RANGE)) return false;

  // -1 in the second argument to this function indicates that the incoming
  // range should be added min_max_ranges. Else use the index passed to add the
  // range to correct infix keypart ranges array.
  Quick_ranges *range_array = nullptr;
  uint key_length = 0;
  if (idx < 0) {
    range_array = &min_max_ranges;
    key_length = min_max_arg_len;
  } else {
    assert((uint)idx < MAX_REF_PARTS);
    KEY_PART_INFO *key_infix_part =
        index_info->key_part + group_key_parts + idx;
    range_array = key_infix_ranges[idx];
    key_length = key_infix_part->store_length;
  }

  if (!(sel_range->min_flag & NO_MIN_RANGE) &&
      !(sel_range->max_flag & NO_MAX_RANGE)) {
    if (sel_range->maybe_null() && sel_range->min_value[0] &&
        sel_range->max_value[0])
      range_flag |= NULL_RANGE; /* IS NULL condition */
    /*
      Do not perform comparison if one of the argiment is NULL value.
    */
    else if (!sel_range->min_value[0] && !sel_range->max_value[0] &&
             memcmp(sel_range->min_value, sel_range->max_value, key_length) ==
                 0)
      range_flag |= EQ_RANGE; /* equality condition */
  }
  range = new (*THR_MALLOC) QUICK_RANGE(
      sel_range->min_value, key_length, make_keypart_map(sel_range->part),
      sel_range->max_value, key_length, make_keypart_map(sel_range->part),
      range_flag, HA_READ_INVALID);
  if (!range) return true;
  if (range_array->push_back(range)) return true;
  return false;
}

/*
  Opens the ranges if there are more conditions in quick_prefix_query_block than
  the ones used for jumping through the prefixes.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::adjust_prefix_ranges()

  NOTES
    quick_prefix_query_block is made over the conditions on the whole key.
    It defines a number of ranges of length x.
    However when jumping through the prefixes we use only the the first
    few most significant keyparts in the range key. However if there
    are more keyparts to follow the ones we are using we must make the
    condition on the key inclusive (because x < "ab" means
    x[0] < 'a' OR (x[0] == 'a' AND x[1] < 'b').
    To achive the above we must turn off the NEAR_MIN/NEAR_MAX
*/
void QUICK_GROUP_MIN_MAX_SELECT::adjust_prefix_ranges() {
  if (quick_prefix_query_block &&
      group_prefix_len < quick_prefix_query_block->max_used_key_length) {
    for (size_t ix = 0; ix < quick_prefix_query_block->ranges.size(); ++ix) {
      QUICK_RANGE *range = quick_prefix_query_block->ranges[ix];
      range->flag &= ~(NEAR_MIN | NEAR_MAX);
    }
  }
}

/*
  Determine the total number and length of the keys that will be used for
  index lookup.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::update_key_stat()

  DESCRIPTION
    The total length of the keys used for index lookup depends on whether
    there are any predicates referencing the min/max argument, and/or if
    the min/max argument field can be NULL.
    This function does an optimistic analysis whether the search key might
    be extended by a constant for the min/max keypart. It is 'optimistic'
    because during actual execution it may happen that a particular range
    is skipped, and then a shorter key will be used. However this is data
    dependent and can't be easily estimated here.

  RETURN
    None
*/

void QUICK_GROUP_MIN_MAX_SELECT::update_key_stat() {
  max_used_key_length = real_prefix_len;
  if (min_max_ranges.size() > 0) {
    if (have_min) { /* Check if the right-most range has a lower boundary. */
      QUICK_RANGE *rightmost_range = min_max_ranges[min_max_ranges.size() - 1];
      if (!(rightmost_range->flag & NO_MIN_RANGE)) {
        max_used_key_length += min_max_arg_len;
        used_key_parts++;
        return;
      }
    }
    if (have_max) { /* Check if the left-most range has an upper boundary. */
      QUICK_RANGE *leftmost_range = min_max_ranges[0];
      if (!(leftmost_range->flag & NO_MAX_RANGE)) {
        max_used_key_length += min_max_arg_len;
        used_key_parts++;
        return;
      }
    }
  } else if (have_min && min_max_arg_part &&
             min_max_arg_part->field->is_nullable()) {
    /*
      If a MIN argument value is NULL, we can quickly determine
      that we're in the beginning of the next group, because NULLs
      are always < any other value. This allows us to quickly
      determine the end of the current group and jump to the next
      group (see next_min()) and thus effectively increases the
      usable key length(see next_min()).
    */
    max_used_key_length += min_max_arg_len;
    used_key_parts++;
  }
}

/*
  Initialize a quick group min/max select for key retrieval.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::reset()

  DESCRIPTION
    Initialize the index chosen for access and find and store the prefix
    of the last group. The method is expensive since it performs disk access.

  RETURN
    0      OK
    other  Error code
*/

int QUICK_GROUP_MIN_MAX_SELECT::reset(void) {
  int result;
  DBUG_TRACE;

  seen_first_key = false;
  head->set_keyread(true); /* We need only the key attributes */
  /*
    Request ordered index access as usage of ::index_last(),
    ::index_first() within QUICK_GROUP_MIN_MAX_SELECT depends on it.
  */
  if (head->file->inited) head->file->ha_index_or_rnd_end();
  if ((result = head->file->ha_index_init(index, true))) {
    head->file->print_error(result, MYF(0));
    return result;
  }
  if (quick_prefix_query_block && quick_prefix_query_block->reset()) return 1;

  result = head->file->ha_index_last(record);
  if (result != 0) {
    if (result == HA_ERR_END_OF_FILE)
      return 0;
    else
      return result;
  }

  /* Save the prefix of the last group. */
  key_copy(last_prefix, record, index_info, group_prefix_len);

  return 0;
}

/*
  Get the next key containing the MIN and/or MAX key for the next group.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::get_next()

  DESCRIPTION
    The method finds the next subsequent group of records that satisfies the
    query conditions and finds the keys that contain the MIN/MAX values for
    the key part referenced by the MIN/MAX function(s). Once a group and its
    MIN/MAX values are found, store these values in the Item_sum objects for
    the MIN/MAX functions. The rest of the values in the result row are stored
    in the Item_field::result_field of each select field. If the query does
    not contain MIN and/or MAX functions, then the function only finds the
    group prefix, which is a query answer itself.

  NOTES
    If both MIN and MAX are computed, then we use the fact that if there is
    no MIN key, there can't be a MAX key as well, so we can skip looking
    for a MAX key in this case.

  RETURN
    0                  on success
    HA_ERR_END_OF_FILE if returned all keys
    other              if some error occurred
*/

int QUICK_GROUP_MIN_MAX_SELECT::get_next() {
  int result;
  int is_last_prefix = 0;

  DBUG_TRACE;

  /*
    Loop until a group is found that satisfies all query conditions or the last
    group is reached.
  */
  do {
    result = next_prefix();
    /*
      Check if this is the last group prefix. Notice that at this point
      this->record contains the current prefix in record format.
    */
    if (!result) {
      is_last_prefix =
          key_cmp(index_info->key_part, last_prefix, group_prefix_len);
      assert(is_last_prefix <= 0);
    } else {
      if (result == HA_ERR_KEY_NOT_FOUND) continue;
      break;
    }

    // Reset current infix range and min/max function as a new group is
    // starting.
    reset_group();
    // TRUE if at least one group satisfied the prefix and infix condition is
    // found.
    bool found_result = false;
    // Reset MIN/MAX value only for the first infix range.
    bool reset_min_value = true;
    bool reset_max_value = true;
    while (!append_next_infix()) {
      assert(!result || !is_index_access_error(result));
      if (have_min || have_max) {
        if (min_max_keypart_asc) {
          if (have_min) {
            if (!(result = next_min()))
              update_min_result(&reset_min_value);
            else {
              DBUG_EXECUTE_IF("bug30769515_QUERY_INTERRUPTED",
                              result = HA_ERR_QUERY_INTERRUPTED;);
              if (is_index_access_error(result)) return result;
              continue;  // Record is not found, no reason to call next_max()
            }
          }
          if (have_max) {
            if (!(result = next_max()))
              update_max_result(&reset_max_value);
            else if (is_index_access_error(result))
              return result;
          }
        } else {
          // Call next_max() first and then next_min() if
          // MIN/MAX key part is descending.
          if (have_max) {
            if (!(result = next_max()))
              update_max_result(&reset_max_value);
            else {
              DBUG_EXECUTE_IF("bug30769515_QUERY_INTERRUPTED",
                              result = HA_ERR_QUERY_INTERRUPTED;);
              if (is_index_access_error(result)) return result;
              continue;  // Record is not found, no reason to call next_min()
            }
          }
          if (have_min) {
            if (!(result = next_min()))
              update_min_result(&reset_min_value);
            else if (is_index_access_error(result))
              return result;
          }
        }
        if (!result) found_result = true;
      } else if (key_infix_len > 0) {
        /*
          If this is just a GROUP BY or DISTINCT without MIN or MAX and there
          are equality predicates for the key parts after the group, find the
          first sub-group with the extended prefix. There is no need to iterate
          through the whole group to accumulate the MIN/MAX and returning just
          the one distinct record is enough.
        */
        if (!(result = head->file->ha_index_read_map(
                  record, group_prefix, make_prev_keypart_map(real_key_parts),
                  HA_READ_KEY_EXACT)) ||
            is_index_access_error(result))
          return result;
      }
    }
    if (seen_all_infix_ranges && found_result) return 0;
  } while (!is_index_access_error(result) && is_last_prefix != 0);

  if (result == HA_ERR_KEY_NOT_FOUND) result = HA_ERR_END_OF_FILE;

  return result;
}

/*
  Retrieve the minimal key in the next group.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::next_min()

  DESCRIPTION
    Find the minimal key within this group such that the key satisfies the query
    conditions and NULL semantics. The found key is loaded into this->record.

  IMPLEMENTATION
    Depending on the values of min_max_ranges.elements, key_infix_len, and
    whether there is a  NULL in the MIN field, this function may directly
    return without any data access. In this case we use the key loaded into
    this->record by the call to this->next_prefix() just before this call.

  RETURN
    0                    on success
    HA_ERR_KEY_NOT_FOUND if no MIN key was found that fulfills all conditions.
    HA_ERR_END_OF_FILE   - "" -
    other                if some error occurred
*/

int QUICK_GROUP_MIN_MAX_SELECT::next_min() {
  int result = 0;
  DBUG_TRACE;

  /* Find the MIN key using the eventually extended group prefix. */
  if (min_max_ranges.size() > 0) {
    uchar key_buf[MAX_KEY_LENGTH];
    key_copy(key_buf, record, index_info, max_used_key_length);
    result = next_min_in_range();
    if (result) key_restore(record, key_buf, index_info, max_used_key_length);
  } else {
    /*
      Apply the constant equality conditions to the non-group select fields.
      There is no reason to call handler method if MIN/MAX key part is
      ascending since  MIN/MAX field points to min value after
      next_prefix() call.
    */
    if (key_infix_len > 0 || !min_max_keypart_asc) {
      if ((result = head->file->ha_index_read_map(
               record, group_prefix, make_prev_keypart_map(real_key_parts),
               min_max_keypart_asc ? HA_READ_KEY_EXACT : HA_READ_PREFIX_LAST)))
        return result;
    }

    /*
      If the min/max argument field is NULL, skip subsequent rows in the same
      group with NULL in it. Notice that:
      - if the first row in a group doesn't have a NULL in the field, no row
      in the same group has (because NULL < any other value),
      - min_max_arg_part->field->ptr points to some place in 'record'.
    */
    if (min_max_arg_part && min_max_arg_part->field->is_null()) {
      uchar key_buf[MAX_KEY_LENGTH];

      /* Find the first subsequent record without NULL in the MIN/MAX field. */
      key_copy(key_buf, record, index_info, max_used_key_length);
      result = head->file->ha_index_read_map(
          record, key_buf, make_keypart_map(real_key_parts),
          min_max_keypart_asc ? HA_READ_AFTER_KEY : HA_READ_BEFORE_KEY);
      /*
        Check if the new record belongs to the current group by comparing its
        prefix with the group's prefix. If it is from the next group, then the
        whole group has NULLs in the MIN/MAX field, so use the first record in
        the group as a result.
        TODO:
        It is possible to reuse this new record as the result candidate for
        the next call to next_min(), and to save one lookup in the next call.
        For this add a new member 'this->next_group_prefix'.
      */
      if (!result) {
        if (key_cmp(index_info->key_part, group_prefix, real_prefix_len))
          key_restore(record, key_buf, index_info, 0);
      } else if (result == HA_ERR_KEY_NOT_FOUND || result == HA_ERR_END_OF_FILE)
        result = 0; /* There is a result in any case. */
    }
  }

  /*
    If the MIN attribute is non-nullable, this->record already contains the
    MIN key in the group, so just return.
  */
  return result;
}

/*
  Retrieve the maximal key in the next group.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::next_max()

  DESCRIPTION
    Lookup the maximal key of the group, and store it into this->record.

  RETURN
    0                    on success
    HA_ERR_KEY_NOT_FOUND if no MAX key was found that fulfills all conditions.
    HA_ERR_END_OF_FILE	 - "" -
    other                if some error occurred
*/

int QUICK_GROUP_MIN_MAX_SELECT::next_max() {
  int result = 0;

  DBUG_TRACE;

  /* Get the last key in the (possibly extended) group. */
  if (min_max_ranges.size() > 0) {
    uchar key_buf[MAX_KEY_LENGTH];
    key_copy(key_buf, record, index_info, max_used_key_length);
    result = next_max_in_range();
    if (result) key_restore(record, key_buf, index_info, max_used_key_length);
  } else {
    /*
      There is no reason to call handler method if MIN/MAX key part is
      descending since  MIN/MAX field points to max value after
      next_prefix() call.
    */
    if (key_infix_len > 0 || min_max_keypart_asc)
      result = head->file->ha_index_read_map(
          record, group_prefix, make_prev_keypart_map(real_key_parts),
          min_max_keypart_asc ? HA_READ_PREFIX_LAST : HA_READ_KEY_EXACT);
  }
  return result;
}

/*
  Determine the prefix of the next group.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::next_prefix()

  DESCRIPTION
    Determine the prefix of the next group that satisfies the query conditions.
    If there is a range condition referencing the group attributes, use a
    QUICK_RANGE_SELECT object to retrieve the *first* key that satisfies the
    condition. The prefix is stored in this->group_prefix. The first key of
    the found group is stored in this->record, on which relies this->next_min().

  RETURN
    0                    on success
    HA_ERR_KEY_NOT_FOUND if there is no key with the formed prefix
    HA_ERR_END_OF_FILE   if there are no more keys
    other                if some error occurred
*/
int QUICK_GROUP_MIN_MAX_SELECT::next_prefix() {
  int result;
  DBUG_TRACE;

  if (quick_prefix_query_block) {
    uchar *cur_prefix = seen_first_key ? group_prefix : nullptr;
    if ((result = quick_prefix_query_block->get_next_prefix(
             group_prefix_len, group_key_parts, cur_prefix)))
      return result;
    seen_first_key = true;
  } else {
    if (!seen_first_key) {
      result = head->file->ha_index_first(record);
      if (result) return result;
      seen_first_key = true;
    } else {
      /* Load the first key in this group into record. */
      result = index_next_different(is_index_scan, head->file,
                                    index_info->key_part, record, group_prefix,
                                    group_prefix_len, group_key_parts);
      if (result) return result;
    }
  }

  /* Save the prefix of this group for subsequent calls. */
  key_copy(group_prefix, record, index_info, group_prefix_len);
  return 0;
}

/*
  Determine and append the next infix.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::append_next_infix()

  DESCRIPTION
    Appends the next infix onto this->group_prefix based on the current
    position stored in cur_infix_range_position

  RETURN
    true                 No next infix exists
    false                on success
*/

bool QUICK_GROUP_MIN_MAX_SELECT::append_next_infix() {
  if (seen_all_infix_ranges) return true;

  if (key_infix_len > 0) {
    uchar *key_ptr = group_prefix + group_prefix_len;
    // For each infix keypart, get the participating range for
    // the next row retrieval. cur_key_infix_position determines
    // which range needs to be used.
    for (uint i = 0; i < key_infix_parts; i++) {
      QUICK_RANGE *cur_range = nullptr;
      assert(key_infix_ranges[i]->size() > 0);

      Quick_ranges *infix_range_array = key_infix_ranges[i];
      cur_range = infix_range_array->at(cur_infix_range_position[i]);
      memcpy(key_ptr, cur_range->min_key, cur_range->min_length);
      key_ptr += cur_range->min_length;
    }

    // cur_infix_range_position is updated with the next infix range
    // position.
    for (int i = key_infix_parts - 1; i >= 0; i--) {
      cur_infix_range_position[i]++;
      if (cur_infix_range_position[i] == key_infix_ranges[i]->size()) {
        // All the ranges for infix keypart "i" is done
        cur_infix_range_position[i] = 0;
        if (i == 0) seen_all_infix_ranges = true;
      } else
        break;
    }
  } else
    seen_all_infix_ranges = true;

  return false;
}

/*
  Reset all the variables that need to be updated for the new group.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::reset_group()

  DESCRIPTION
    It is called before a new group is processed.

  RETURN
    None
*/

void QUICK_GROUP_MIN_MAX_SELECT::reset_group() {
  // Reset the current infix range before a new group is processed.
  seen_all_infix_ranges = false;
  memset(cur_infix_range_position, 0, sizeof(cur_infix_range_position));

  if (have_min) {
    for (Item_sum &min_func : *min_functions) {
      min_func.aggregator_clear();
    }
  }

  if (have_max) {
    for (Item_sum &max_func : *max_functions) {
      max_func.aggregator_clear();
    }
  }
}

/**
  Function returns search mode that needs to be used to read
  the next record. It takes the type of the range, the
  key part's order (ascending or descending) and if the
  range is on MIN function or a MAX function to get the
  right search mode.
  For "MIN" functon:
   - ASC keypart
   We need to
    1. Read the first key that matches the range
       a) if a minimum value is not specified in the condition
       b) if it is a equality or is NULL condition
    2. Read the first key after a range value if range is like "a > 10"
    3. Read the key that matches the condition or any key after
       the range value for any other condition
   - DESC keypart
   We need to
    4. Read the last value for the key prefix if there is no minimum
       range specified.
    5. Read the first key that matches the range if it is a equality
       condition.
    6. Read the first key before a range value if range is like "a > 10"
    7. Read the key that matches the prefix or any key before for any
       other condition
  For MAX function:
   - ASC keypart
   We need to
    8. Read the last value for the key prefix if there is no maximum
       range specified
    9. Read the first key that matches the range if it is a equality
       condition
   10. Read the first key before a range value if range is like "a < 10"
   11. Read the key that matches the condition or any key before
       the range value for any other condition
   - DESC keypart
   We need to
   12. Read the first key that matches the range
       a) if a minimum value is not specified in the condition
       b) if it is a equality
   13. Read the first key after a range value if range is like "a < 10"
   14. Read the key that matches the prefix or any key after for
       any other condition


  @param cur_range         pointer to QUICK_RANGE.
  @param is_asc            TRUE if key part is ascending,
                           FALSE otherwise.
  @param is_min            TRUE if the range is on MIN function.
                           FALSE for MAX function.
  @return search mode
*/

static ha_rkey_function get_search_mode(QUICK_RANGE *cur_range, bool is_asc,
                                        bool is_min) {
  // If MIN function
  if (is_min) {
    if (is_asc) {                          // key part is ascending
      if (cur_range->flag & NO_MIN_RANGE)  // 1a
        return HA_READ_KEY_EXACT;
      else
        return (cur_range->flag & (EQ_RANGE | NULL_RANGE))  // 1b
                   ? HA_READ_KEY_EXACT
                   : (cur_range->flag & NEAR_MIN) ? HA_READ_AFTER_KEY     // 2
                                                  : HA_READ_KEY_OR_NEXT;  // 3
    } else {  // key parts is descending
      if (cur_range->flag & NO_MIN_RANGE)
        return HA_READ_PREFIX_LAST;  // 4
      else
        return (cur_range->flag & EQ_RANGE)
                   ? HA_READ_KEY_EXACT  // 5
                   : (cur_range->flag & NEAR_MIN)
                         ? HA_READ_BEFORE_KEY            // 6
                         : HA_READ_PREFIX_LAST_OR_PREV;  // 7
    }
  }

  // If max function
  if (is_asc) {  // key parts is ascending
    if (cur_range->flag & NO_MAX_RANGE)
      return HA_READ_PREFIX_LAST;  // 8
    else
      return (cur_range->flag & EQ_RANGE)
                 ? HA_READ_KEY_EXACT  // 9
                 : (cur_range->flag & NEAR_MAX)
                       ? HA_READ_BEFORE_KEY            // 10
                       : HA_READ_PREFIX_LAST_OR_PREV;  // 11
  } else {  // key parts is descending
    if (cur_range->flag & NO_MAX_RANGE)
      return HA_READ_KEY_EXACT;  // 12a
    else
      return (cur_range->flag & EQ_RANGE)
                 ? HA_READ_KEY_EXACT                                    // 12b
                 : (cur_range->flag & NEAR_MAX) ? HA_READ_AFTER_KEY     // 13
                                                : HA_READ_KEY_OR_NEXT;  // 14
  }
}

/*
  Find the minimal key in a group that satisfies some range conditions for the
  min/max argument field.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::next_min_in_range()

  DESCRIPTION
    Given the sequence of ranges min_max_ranges, find the minimal key that is
    in the left-most possible range. If there is no such key, then the current
    group does not have a MIN key that satisfies the WHERE clause. If a key is
    found, its value is stored in this->record.

  RETURN
    0                    on success
    HA_ERR_KEY_NOT_FOUND if there is no key with the given prefix in any of
                         the ranges
    HA_ERR_END_OF_FILE   - "" -
    other                if some error
*/

int QUICK_GROUP_MIN_MAX_SELECT::next_min_in_range() {
  ha_rkey_function search_mode;
  key_part_map keypart_map;
  bool found_null = false;
  int result = HA_ERR_KEY_NOT_FOUND;

  assert(min_max_ranges.size() > 0);

  /* Search from the left-most range to the right. */
  for (Quick_ranges::const_iterator it = min_max_ranges.begin();
       it != min_max_ranges.end(); ++it) {
    QUICK_RANGE *cur_range = *it;
    /*
      If the current value for the min/max argument is bigger than the right
      boundary of cur_range, there is no need to check this range.
    */
    if (it != min_max_ranges.begin() && !(cur_range->flag & NO_MAX_RANGE) &&
        (key_cmp(min_max_arg_part, (const uchar *)cur_range->max_key,
                 min_max_arg_len) == (min_max_keypart_asc ? 1 : -1)) &&
        !result)
      continue;

    if (cur_range->flag & NO_MIN_RANGE) {
      keypart_map = make_prev_keypart_map(real_key_parts);
      search_mode = get_search_mode(cur_range, min_max_keypart_asc, true);
    } else {
      /* Extend the search key with the lower boundary for this range. */
      memcpy(group_prefix + real_prefix_len, cur_range->min_key,
             cur_range->min_length);
      keypart_map = make_keypart_map(real_key_parts);
      search_mode = get_search_mode(cur_range, min_max_keypart_asc, true);
    }

    result = head->file->ha_index_read_map(record, group_prefix, keypart_map,
                                           search_mode);
    if (result) {
      if ((result == HA_ERR_KEY_NOT_FOUND || result == HA_ERR_END_OF_FILE) &&
          (cur_range->flag & (EQ_RANGE | NULL_RANGE)))
        continue; /* Check the next range. */

      /*
        In all other cases (HA_ERR_*, HA_READ_KEY_EXACT with NO_MIN_RANGE,
        HA_READ_AFTER_KEY, HA_READ_KEY_OR_NEXT) if the lookup failed for this
        range, it can't succeed for any other subsequent range.
      */
      break;
    }

    /* A key was found. */
    if (cur_range->flag & EQ_RANGE)
      break; /* No need to perform the checks below for equal keys. */

    if (min_max_keypart_asc && cur_range->flag & NULL_RANGE) {
      /*
        Remember this key, and continue looking for a non-NULL key that
        satisfies some other condition.
      */
      memcpy(tmp_record, record, head->s->rec_buff_length);
      found_null = true;
      continue;
    }

    /* Check if record belongs to the current group. */
    if (key_cmp(index_info->key_part, group_prefix, real_prefix_len)) {
      result = HA_ERR_KEY_NOT_FOUND;
      continue;
    }

    /* If there is an upper limit, check if the found key is in the range. */
    if (!(cur_range->flag & NO_MAX_RANGE)) {
      /* Compose the MAX key for the range. */
      uchar *max_key = (uchar *)my_alloca(real_prefix_len + min_max_arg_len);
      memcpy(max_key, group_prefix, real_prefix_len);
      memcpy(max_key + real_prefix_len, cur_range->max_key,
             cur_range->max_length);
      /* Compare the found key with max_key. */
      int cmp_res = key_cmp(index_info->key_part, max_key,
                            real_prefix_len + min_max_arg_len);
      /*
        The key is outside of the range if:
        the interval is open and the key is equal to the maximum boundry
        or
        the key is greater than the maximum
      */
      if (((cur_range->flag & NEAR_MAX) && cmp_res == 0) ||
          (min_max_keypart_asc ? (cmp_res > 0) : (cmp_res < 0))) {
        result = HA_ERR_KEY_NOT_FOUND;
        continue;
      }
    }
    /* If we got to this point, the current key qualifies as MIN. */
    assert(result == 0);
    break;
  }
  /*
    If there was a key with NULL in the MIN/MAX field, and there was no other
    key without NULL from the same group that satisfies some other condition,
    then use the key with the NULL.
  */
  if (found_null && result) {
    memcpy(record, tmp_record, head->s->rec_buff_length);
    result = 0;
  }
  return result;
}

/*
  Find the maximal key in a group that satisfies some range conditions for the
  min/max argument field.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::next_max_in_range()

  DESCRIPTION
    Given the sequence of ranges min_max_ranges, find the maximal key that is
    in the right-most possible range. If there is no such key, then the current
    group does not have a MAX key that satisfies the WHERE clause. If a key is
    found, its value is stored in this->record.

  RETURN
    0                    on success
    HA_ERR_KEY_NOT_FOUND if there is no key with the given prefix in any of
                         the ranges
    HA_ERR_END_OF_FILE   - "" -
    other                if some error
*/

int QUICK_GROUP_MIN_MAX_SELECT::next_max_in_range() {
  ha_rkey_function search_mode;
  key_part_map keypart_map;
  int result = HA_ERR_KEY_NOT_FOUND;

  assert(min_max_ranges.size() > 0);

  /* Search from the right-most range to the left. */
  for (Quick_ranges::const_iterator it = min_max_ranges.end();
       it != min_max_ranges.begin(); --it) {
    QUICK_RANGE *cur_range = *(it - 1);
    /*
      If the current value for the min/max argument is smaller than the left
      boundary of cur_range, there is no need to check this range.
    */
    if (it != min_max_ranges.end() && !(cur_range->flag & NO_MIN_RANGE) &&
        (key_cmp(min_max_arg_part, (const uchar *)cur_range->min_key,
                 min_max_arg_len) == (min_max_keypart_asc ? -1 : 1)) &&
        !result)
      continue;

    if (cur_range->flag & NO_MAX_RANGE) {
      keypart_map = make_prev_keypart_map(real_key_parts);
      search_mode = get_search_mode(cur_range, min_max_keypart_asc, false);
    } else {
      /* Extend the search key with the upper boundary for this range. */
      memcpy(group_prefix + real_prefix_len, cur_range->max_key,
             cur_range->max_length);
      keypart_map = make_keypart_map(real_key_parts);
      search_mode = get_search_mode(cur_range, min_max_keypart_asc, false);
    }

    result = head->file->ha_index_read_map(record, group_prefix, keypart_map,
                                           search_mode);

    if (result) {
      if ((result == HA_ERR_KEY_NOT_FOUND || result == HA_ERR_END_OF_FILE) &&
          (cur_range->flag & EQ_RANGE))
        continue; /* Check the next range. */

      /*
        In no key was found with this upper bound, there certainly are no keys
        in the ranges to the left.
      */
      return result;
    }
    /* A key was found. */
    if (cur_range->flag & EQ_RANGE)
      return 0; /* No need to perform the checks below for equal keys. */

    /* Check if record belongs to the current group. */
    if (key_cmp(index_info->key_part, group_prefix, real_prefix_len)) {
      result = HA_ERR_KEY_NOT_FOUND;
      continue;  // Row not found
    }

    /* If there is a lower limit, check if the found key is in the range. */
    if (!(cur_range->flag & NO_MIN_RANGE)) {
      /* Compose the MIN key for the range. */
      uchar *min_key = (uchar *)my_alloca(real_prefix_len + min_max_arg_len);
      memcpy(min_key, group_prefix, real_prefix_len);
      memcpy(min_key + real_prefix_len, cur_range->min_key,
             cur_range->min_length);
      /* Compare the found key with min_key. */
      int cmp_res = key_cmp(index_info->key_part, min_key,
                            real_prefix_len + min_max_arg_len);
      /*
        The key is outside of the range if:
        the interval is open and the key is equal to the minimum boundry
        or
        the key is less than the minimum
      */
      if (((cur_range->flag & NEAR_MIN) && cmp_res == 0) ||
          (min_max_keypart_asc ? (cmp_res < 0) : (cmp_res > 0))) {
        result = HA_ERR_KEY_NOT_FOUND;
        continue;
      }
    }
    /* If we got to this point, the current key qualifies as MAX. */
    return result;
  }
  return HA_ERR_KEY_NOT_FOUND;
}

/*
  Update all MIN function results with the newly found value.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::update_min_result()

  DESCRIPTION
    The method iterates through all MIN functions and updates the result value
    of each function by calling Item_sum::aggregator_add(), which in turn picks
    the new result value from this->head->record[0], previously updated by
    next_min(). The updated value is stored in a member variable of each of the
    Item_sum objects, depending on the value type.

  IMPLEMENTATION
    The update must be done separately for MIN and MAX, immediately after
    next_min() was called and before next_max() is called, because both MIN and
    MAX take their result value from the same buffer this->head->record[0]
    (i.e.  this->record).

  @param  reset   IN/OUT reset MIN value if TRUE.

  RETURN
    None
*/

void QUICK_GROUP_MIN_MAX_SELECT::update_min_result(bool *reset) {
  for (Item_sum &min_func : *min_functions) {
    if (*reset) {
      min_func.aggregator_clear();
      *reset = false;
    }
    min_func.aggregator_add();
  }
}

/*
  Update all MAX function results with the newly found value.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::update_max_result()

  DESCRIPTION
    The method iterates through all MAX functions and updates the result value
    of each function by calling Item_sum::aggregator_add(), which in turn picks
    the new result value from this->head->record[0], previously updated by
    next_max(). The updated value is stored in a member variable of each of the
    Item_sum objects, depending on the value type.

  IMPLEMENTATION
    The update must be done separately for MIN and MAX, immediately after
    next_max() was called, because both MIN and MAX take their result value
    from the same buffer this->head->record[0] (i.e.  this->record).

  @param  reset   IN/OUT reset MAX value if TRUE.

  RETURN
    None
*/

void QUICK_GROUP_MIN_MAX_SELECT::update_max_result(bool *reset) {
  for (Item_sum &max_func : *max_functions) {
    if (*reset) {
      max_func.aggregator_clear();
      *reset = false;
    }
    max_func.aggregator_add();
  }
}

/*
  Append comma-separated list of keys this quick select uses to key_names;
  append comma-separated list of corresponding used lengths to used_lengths.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::add_keys_and_lengths()
    key_names    [out] Names of used indexes
    used_lengths [out] Corresponding lengths of the index names

  DESCRIPTION
    This method is used by select_describe to extract the names of the
    indexes used by a quick select.

*/

void QUICK_GROUP_MIN_MAX_SELECT::add_keys_and_lengths(String *key_names,
                                                      String *used_lengths) {
  char buf[64];
  size_t length;
  key_names->append(index_info->name);
  length = longlong10_to_str(max_used_key_length, buf, 10) - buf;
  used_lengths->append(buf, length);
}

#ifndef NDEBUG
/*
  Print quick select information to DBUG_FILE.

  SYNOPSIS
    QUICK_GROUP_MIN_MAX_SELECT::dbug_dump()
    indent  Indentation offset
    verbose If true show more detailed output.

  DESCRIPTION
    Print the contents of this quick select to DBUG_FILE. The method also
    calls dbug_dump() for the used quick select if any.

  IMPLEMENTATION
    Caller is responsible for locking DBUG_FILE before this call and unlocking
    it afterwards.

  RETURN
    None
*/

void QUICK_GROUP_MIN_MAX_SELECT::dbug_dump(int indent, bool verbose) {
  fprintf(DBUG_FILE,
          "%*squick_group_min_max_query_block: index %s (%d), length: %d\n",
          indent, "", index_info->name, index, max_used_key_length);
  if (key_infix_len > 0) {
    fprintf(DBUG_FILE, "%*susing key_infix with length %d:\n", indent, "",
            key_infix_len);
  }
  if (quick_prefix_query_block) {
    fprintf(DBUG_FILE, "%*susing quick_range_query_block:\n", indent, "");
    quick_prefix_query_block->dbug_dump(indent + 2, verbose);
  }
  if (min_max_ranges.size() > 0) {
    fprintf(DBUG_FILE, "%*susing %d quick_ranges for MIN/MAX:\n", indent, "",
            static_cast<int>(min_max_ranges.size()));
  }
}
#endif
