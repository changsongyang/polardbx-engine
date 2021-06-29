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

#ifndef SQL_RANGE_OPTIMIZER_ROWID_ORDERED_RETRIEVAL_PLAN_H_
#define SQL_RANGE_OPTIMIZER_ROWID_ORDERED_RETRIEVAL_PLAN_H_

#include <sys/types.h>

#include "my_base.h"
#include "my_bitmap.h"
#include "sql/handler.h"
#include "sql/range_optimizer/table_read_plan.h"

class Opt_trace_object;
class RANGE_OPT_PARAM;
class QUICK_SELECT_I;
class SEL_ROOT;
class SEL_TREE;
struct MEM_ROOT;

struct ROR_SCAN_INFO {
  uint idx;         ///< # of used key in param->keys
  uint keynr;       ///< # of used key in table
  ha_rows records;  ///< estimate of # records this scan will return

  /** Set of intervals over key fields that will be used for row retrieval. */
  SEL_ROOT *sel_root;

  /** Fields used in the query and covered by this ROR scan. */
  MY_BITMAP covered_fields;
  /**
    Fields used in the query that are a) covered by this ROR scan and
    b) not already covered by ROR scans ordered earlier in the merge
    sequence.
  */
  MY_BITMAP covered_fields_remaining;
  /** Number of fields in covered_fields_remaining (caching of
   * bitmap_bits_set()) */
  uint num_covered_fields_remaining;

  /**
    Cost of reading all index records with values in sel_arg intervals set
    (assuming there is no need to access full table records)
  */
  Cost_estimate index_read_cost;
};

/* Plan for QUICK_ROR_INTERSECT_SELECT scan. */

class TRP_ROR_INTERSECT : public TABLE_READ_PLAN {
 public:
  TRP_ROR_INTERSECT(TABLE *table_arg, bool forced_by_hint_arg,
                    KEY_PART *const *key_arg, const uint *real_keynr_arg)
      : TABLE_READ_PLAN(table_arg, MAX_KEY, forced_by_hint_arg),
        key(key_arg),
        real_keynr(real_keynr_arg) {}

  QUICK_SELECT_I *make_quick(bool retrieve_full_rows,
                             MEM_ROOT *return_mem_root) override;

  /* Array of pointers to ROR range scans used in this intersection */
  ROR_SCAN_INFO **first_scan;
  ROR_SCAN_INFO **last_scan; /* End of the above array */
  ROR_SCAN_INFO *cpk_scan;   /* Clustered PK scan, if there is one */
  bool is_covering;          /* true if no row retrieval phase is necessary */
  Cost_estimate index_scan_cost; /* SUM(cost(index_scan)) */

  void trace_basic_info(THD *thd, const RANGE_OPT_PARAM *param,
                        Opt_trace_object *trace_object) const override;

 private:
  KEY_PART *const *key;
  const uint *real_keynr;
};

/*
  Plan for QUICK_ROR_UNION_SELECT scan.
  QUICK_ROR_UNION_SELECT always retrieves full rows, so retrieve_full_rows
  is ignored by make_quick.
*/

class TRP_ROR_UNION : public TABLE_READ_PLAN {
 public:
  TRP_ROR_UNION(TABLE *table_arg, bool forced_by_hint_arg)
      : TABLE_READ_PLAN(table_arg, MAX_KEY, forced_by_hint_arg) {}
  QUICK_SELECT_I *make_quick(bool retrieve_full_rows,
                             MEM_ROOT *return_mem_root) override;
  TABLE_READ_PLAN **first_ror; /* array of ptrs to plans for merged scans */
  TABLE_READ_PLAN **last_ror;  /* end of the above array */

  void trace_basic_info(THD *thd, const RANGE_OPT_PARAM *param,
                        Opt_trace_object *trace_object) const override;
};

TRP_ROR_INTERSECT *get_best_ror_intersect(
    THD *thd, const RANGE_OPT_PARAM *param, TABLE *table,
    bool index_merge_intersect_allowed, enum_order order_direction,
    SEL_TREE *tree, const MY_BITMAP *needed_fields,
    const Cost_estimate *cost_est, bool force_index_merge_result);

#endif  // SQL_RANGE_OPTIMIZER_ROWID_ORDERED_RETRIEVAL_PLAN_H_
