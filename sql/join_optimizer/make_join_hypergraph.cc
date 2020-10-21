/* Copyright (c) 2020, 2021, Oracle and/or its affiliates.

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

#include "sql/join_optimizer/make_join_hypergraph.h"

#include <assert.h>
#include <stddef.h>
#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "limits.h"
#include "mem_root_deque.h"
#include "my_alloc.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "my_table_map.h"
#include "mysqld_error.h"
#include "sql/current_thd.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/join_optimizer/estimate_selectivity.h"
#include "sql/join_optimizer/hypergraph.h"
#include "sql/join_optimizer/print_utils.h"
#include "sql/join_optimizer/relational_expression.h"
#include "sql/join_optimizer/subgraph_enumeration.h"
#include "sql/nested_join.h"
#include "sql/sql_class.h"
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "template_utils.h"

using hypergraph::Hyperedge;
using hypergraph::Hypergraph;
using hypergraph::NodeMap;
using std::array;
using std::string;
using std::vector;

namespace {

RelationalExpression *MakeRelationalExpressionFromJoinList(
    THD *thd, const mem_root_deque<TABLE_LIST *> &join_list);

RelationalExpression *MakeRelationalExpression(THD *thd, const TABLE_LIST *tl) {
  if (tl->nested_join == nullptr) {
    // A single table.
    RelationalExpression *ret = new (thd->mem_root) RelationalExpression(thd);
    ret->type = RelationalExpression::TABLE;
    ret->table = tl;
    ret->tables_in_subtree = tl->map();
    ret->join_conditions_pushable_to_this.init(thd->mem_root);
    return ret;
  } else {
    // A join or multijoin.
    return MakeRelationalExpressionFromJoinList(thd,
                                                tl->nested_join->join_list);
  }
}

/**
  Convert the Query_block's join lists into a RelationalExpression,
  ie., a join tree with tables at the leaves.
 */
RelationalExpression *MakeRelationalExpressionFromJoinList(
    THD *thd, const mem_root_deque<TABLE_LIST *> &join_list) {
  assert(!join_list.empty());
  RelationalExpression *ret = nullptr;
  for (auto it = join_list.rbegin(); it != join_list.rend();
       ++it) {  // The list goes backwards.
    const TABLE_LIST *tl = *it;
    if (ret == nullptr) {
      // The first table in the list.
      ret = MakeRelationalExpression(thd, tl);
      continue;
    }

    RelationalExpression *join = new (thd->mem_root) RelationalExpression(thd);
    join->left = ret;
    if (tl->is_sj_or_aj_nest()) {
      join->right =
          MakeRelationalExpressionFromJoinList(thd, tl->nested_join->join_list);
      join->type = tl->is_sj_nest() ? RelationalExpression::SEMIJOIN
                                    : RelationalExpression::ANTIJOIN;
    } else {
      join->right = MakeRelationalExpression(thd, tl);
      join->type = tl->outer_join ? RelationalExpression::LEFT_JOIN
                                  : RelationalExpression::INNER_JOIN;
    }
    if (tl->is_aj_nest()) {
      assert(tl->join_cond() != nullptr);
    }
    if (tl->join_cond() != nullptr) {
      ExtractConditions(tl->join_cond(), &join->join_conditions);
    }
    join->tables_in_subtree =
        join->left->tables_in_subtree | join->right->tables_in_subtree;
    ret = join;
  }
  return ret;
}

string PrintRelationalExpression(RelationalExpression *expr, int level) {
  string result;
  for (int i = 0; i < level * 2; ++i) result += ' ';

  switch (expr->type) {
    case RelationalExpression::TABLE:
      result += StringPrintf("* %s\n", expr->table->alias);
      // Do not try to descend further.
      return result;
    case RelationalExpression::INNER_JOIN:
      result += "* Inner join";
      break;
    case RelationalExpression::LEFT_JOIN:
      result += "* Left join";
      break;
    case RelationalExpression::SEMIJOIN:
      result += "* Semijoin";
      break;
    case RelationalExpression::ANTIJOIN:
      result += "* Antijoin";
      break;
    case RelationalExpression::FULL_OUTER_JOIN:
      result += "* Full outer join";
      break;
  }
  if (expr->equijoin_conditions.empty() && expr->join_conditions.empty()) {
    result += " (no join conditions)";
  } else if (!expr->equijoin_conditions.empty()) {
    result += StringPrintf(" (equijoin condition = %s)",
                           ItemsToString(expr->equijoin_conditions).c_str());
  } else if (!expr->join_conditions.empty()) {
    result += StringPrintf(" (extra join condition = %s)",
                           ItemsToString(expr->join_conditions).c_str());
  } else {
    result += StringPrintf(" (equijoin condition = %s, extra = %s)",
                           ItemsToString(expr->equijoin_conditions).c_str(),
                           ItemsToString(expr->join_conditions).c_str());
  }
  result += '\n';

  result += PrintRelationalExpression(expr->left, level + 1);
  result += PrintRelationalExpression(expr->right, level + 1);
  return result;
}

// Returns whether the join condition for “expr” is null-rejecting (also known
// as strong or strict) on the given relations; that is, if it is guaranteed to
// return FALSE or NULL if _all_ tables in “tables” consist only of NULL values.
// (This means that adding tables in “tables” which are not part of any of the
// predicates is legal, and has no effect on the result.)
//
// A typical example of a null-rejecting condition would be a simple equality,
// e.g. t1.x = t2.x, which would reject NULLs on t1 and t2.
bool IsNullRejecting(const RelationalExpression &expr, table_map tables) {
  for (Item *cond : expr.join_conditions) {
    if (Overlaps(tables, cond->not_null_tables())) {
      return true;
    }
  }
  for (Item *cond : expr.equijoin_conditions) {
    if (Overlaps(tables, cond->not_null_tables())) {
      return true;
    }
  }
  return false;
}

// Returns true if (t1 <a> t2) <b> t3 === t1 <a> (t2 <b> t3).
//
// Note that this is not symmetric; e.g.
//
//   (t1 JOIN t2) LEFT JOIN t3 === t1 JOIN (t2 LEFT JOIN t3)
//
// but
//
//   (t1 LEFT JOIN t2) JOIN t3 != t1 LEFT JOIN (t2 JOIN t3)
//
// Note that this does not check that the rewrite would be _syntatically_ valid,
// i.e., that <b> does not refer to tables from t1. That is the job of the SES
// (syntactic eligibility set), which forms the base of the hyperedge
// representing the join, and not conflict rules -- if <b> refers to t1, the
// edge will include t1 no matter what we return here. This also goes for
// l-asscom and r-asscom below.
//
// When generating conflict rules, we call this function in a generalized sense:
//
//  1. t1, t2 and t3 could be join expressions, not just single tables.
//  2. <a> may not be a direct descendant of <b>, but further down the tree.
//  3. <b> may be below <a> in the tree, instead of the other way round.
//
// Due to #1 and #2, we need to take care when checking for null-rejecting
// conditions. Specifically, when the tables say we should check whether a
// condition mentioning (t2,t3) is null-rejecting on t2, we need to check the
// left arm of <b> instead of the right arm of <a>, as the condition might
// refer to a table that is not even part of <a> (ie., the “t2” in the condition
// is not the same “t2” as is under <a>). Otherwise, we might be rejecting
// valid plans. An example (where LJmn is LEFT JOIN with a null-rejecting
// predicate between tables m and n):
//
//   ((t1 LJ12 t2) LJ23 t3) LJ34 t4
//
// At some point, we will be called with <a> = LJ12 and <b> = LJ34.
// If we check whether LJ34 is null-rejecting on t2 (a.right), instead of
// checking wheher it is null-rejecting on {t1,t2,t3} (b.left), we will
// erroneously create a conflict rule {t2} → {t1}, since we believe the
// LJ34 predicate is not null-rejecting on its left side.
bool OperatorsAreAssociative(const RelationalExpression &a,
                             const RelationalExpression &b) {
  // Table 2 from [Moe13]; which operator pairs are associative.

  if ((a.type == RelationalExpression::LEFT_JOIN ||
       a.type == RelationalExpression::FULL_OUTER_JOIN) &&
      b.type == RelationalExpression::LEFT_JOIN) {
    // True if and only if the second join predicate rejects NULLs
    // on all tables in e2.
    return IsNullRejecting(b, b.left->tables_in_subtree);
  }

  if (a.type == RelationalExpression::FULL_OUTER_JOIN &&
      b.type == RelationalExpression::FULL_OUTER_JOIN) {
    // True if and only if both join predicates rejects NULLs
    // on all tables in e2.
    return IsNullRejecting(a, a.right->tables_in_subtree) &&
           IsNullRejecting(b, b.left->tables_in_subtree);
  }

  // For the operations we support, it can be collapsed into this simple
  // condition. (Cartesian products and inner joins are treated the same.)
  return a.type == RelationalExpression::INNER_JOIN &&
         b.type != RelationalExpression::FULL_OUTER_JOIN;
}

// Returns true if (t1 <a> t2) <b> t3 === (t1 <b> t3) <a> t2,
// ie., the order of right-applying <a> and <b> don't matter.
//
// This is a symmetric property. The name comes from the fact that
// associativity and commutativity together would imply l-asscom;
// however, the converse is not true, so this is a more lenient property.
//
// See comments on OperatorsAreAssociative().
bool OperatorsAreLeftAsscom(const RelationalExpression &a,
                            const RelationalExpression &b) {
  // Table 3 from [Moe13]; which operator pairs are l-asscom.
  // (Cartesian products and inner joins are treated the same.)
  if (a.type == RelationalExpression::LEFT_JOIN) {
    if (b.type == RelationalExpression::FULL_OUTER_JOIN) {
      return IsNullRejecting(a, a.left->tables_in_subtree);
    } else {
      return true;
    }
  }
  if (a.type == RelationalExpression::FULL_OUTER_JOIN) {
    if (b.type == RelationalExpression::LEFT_JOIN) {
      return IsNullRejecting(b, b.right->tables_in_subtree);
    }
    if (b.type == RelationalExpression::FULL_OUTER_JOIN) {
      return IsNullRejecting(a, a.left->tables_in_subtree) &&
             IsNullRejecting(b, b.left->tables_in_subtree);
    }
    return false;
  }
  return b.type != RelationalExpression::FULL_OUTER_JOIN;
}

// Returns true if e1 <a> (e2 <b> e3) === e2 <b> (e1 <a> e3),
// ie., the order of left-applying <a> and <b> don't matter.
// Similar to OperatorsAreLeftAsscom().
bool OperatorsAreRightAsscom(const RelationalExpression &a,
                             const RelationalExpression &b) {
  // Table 3 from [Moe13]; which operator pairs are r-asscom.
  // (Cartesian products and inner joins are treated the same.)
  if (a.type == RelationalExpression::FULL_OUTER_JOIN &&
      b.type == RelationalExpression::FULL_OUTER_JOIN) {
    return IsNullRejecting(a, a.right->tables_in_subtree) &&
           IsNullRejecting(b, b.right->tables_in_subtree);
  }
  return a.type == RelationalExpression::INNER_JOIN &&
         b.type == RelationalExpression::INNER_JOIN;
}

/**
  Try to push down the condition “cond” down in the join tree given by “expr”,
  as far as possible. cond is either a join condition on expr
  (is_join_condition_for_expr=true), or a filter which is applied at some point
  after expr (...=false).

  Returns false if cond was pushed down and stored as a join condition on some
  lower place than it started, ie., the caller no longer needs to worry about
  it.

  Since PushDownAsMuchAsPossible() only calls us for join conditions, there are
  only two ways we can push down something onto a single table (which naturally
  has no concept of “join condition”). Neither of them affect the return
  condition. These are:

  1. Sargable join conditions.

  Equijoin conditions can often be pushed down into indexes; e.g. t1.x = t2.x
  could be pushed down into an index on t1.x. When we have pushed such a
  condition all the way down onto the t1/t2 join, we are ostensibly done
  (and would return true), but before that, we push down the condition down
  onto both sides if possible. (E.g.: If the join was a left join, we could
  push it down to t2, but not to t1.) When we hit a table in such a push,
  we store the conditions in “join_conditions_pushable_to_this“ for the table
  to signal that it should be investigated when we consider the table during
  join optimization. This push happens with parameter_tables set to a bitmap
  of the table(s) on the other side of the join, e.g. the push to t1 happens
  with t2 in the bitmap. A push with nonzero parameter_tables is not subject
  to being left as a join condition as would usually be the case; if it is
  not pushable all the way down to a table, it is simply discarded.

  2. Partial pushdown.

  In addition to regular pushdown, PushDownCondition() will do partial pushdown
  if appropriate. Some expressions cannot be fully pushed down, but we can
  push down necessary-but-not-sufficient conditions to get earlier filtering.
  (This is a performance win for e.g. hash join and the left side of a
  nested loop join, but not for the right side of a nested loop join. Note that
  we currently do not compensate for the errors in selectivity estimation
  this may incur.) An example would be

    (t1.x = 1 AND t2.y=2) OR (t1.x = 3 AND t2.y=4);

  we could push down the conditions (t1.x = 1 OR t1.x = 3) to t1 and similarly
  for t2, but we could not delete the original condition. If we get all the way
  down to a table, we store the condition in “table_filters”. These are
  conditions that can be evaluated directly on the given table, without any
  concern for what is joined in before (ie., TES = SES).
 */
bool PushDownCondition(Item *cond, RelationalExpression *expr,
                       bool is_join_condition_for_expr,
                       table_map parameter_tables,
                       Mem_root_array<Item *> *table_filters) {
  if (expr->type == RelationalExpression::TABLE) {
    if (parameter_tables == 0) {
      table_filters->push_back(cond);
    } else {
      expr->join_conditions_pushable_to_this.push_back(cond);
    }
    return true;
  }

  assert(
      !Overlaps(expr->left->tables_in_subtree, expr->right->tables_in_subtree));

  table_map used_tables =
      cond->used_tables() & ~(OUTER_REF_TABLE_BIT | INNER_TABLE_BIT);

  // See if we can push down into the left side, ie., it only touches
  // tables on the left side of the join.
  //
  // If the condition is a filter, we can do this for all join types
  // except FULL OUTER JOIN, which we don't support yet. If it's a join
  // condition for this join, we cannot push it for outer joins and
  // antijoins, since that would remove rows that should otherwise
  // be output (as NULL-complemented ones in the case if outer joins).
  const bool can_push_into_left =
      (expr->type == RelationalExpression::INNER_JOIN ||
       expr->type == RelationalExpression::SEMIJOIN ||
       !is_join_condition_for_expr);
  if (IsSubset(used_tables, expr->left->tables_in_subtree | parameter_tables)) {
    if (!can_push_into_left) {
      return true;
    }
    return PushDownCondition(cond, expr->left,
                             /*is_join_condition_for_expr=*/false,
                             parameter_tables, table_filters);
  }

  // See if we can push down into the right side. For inner joins,
  // we can always do this, assuming the condition refers to the right
  // side only. For outer joins and antijoins, we cannot push conditions
  // _through_ them; that is, we can push them if they come directly from said
  // node's join condition, but not otherwise. (This is, incidentally, the exact
  // opposite condition from pushing into the left side.)
  //
  // Normally, this also goes for semijoins, except that MySQL's semijoin
  // rewriting causes conditions to appear higher up in the tree that we
  // _must_ push back down and through them for correctness. Thus, we have
  // no choice but to just trust that these conditions are pushable.
  // (The user cannot cannot specify semijoins directly, so all such conditions
  // come from ourselves.)
  const bool can_push_into_right =
      (expr->type == RelationalExpression::INNER_JOIN ||
       expr->type == RelationalExpression::SEMIJOIN ||
       is_join_condition_for_expr);
  if (IsSubset(used_tables,
               expr->right->tables_in_subtree | parameter_tables)) {
    if (!can_push_into_right) {
      return true;
    }
    return PushDownCondition(cond, expr->right,
                             /*is_join_condition_for_expr=*/false,
                             parameter_tables, table_filters);
  }

  // It's not a subset of left, it's not a subset of right, so it's a
  // filter that must either stay after this join, or it can be promoted
  // to a join condition for it.

  // Try partial pushdown into the left side (see function comment).
  if (can_push_into_left) {
    Item *partial_cond = make_cond_for_table(
        current_thd, cond, expr->left->tables_in_subtree, /*used_table=*/0,
        /*exclude_expensive_cond=*/true);
    if (partial_cond != nullptr) {
      PushDownCondition(partial_cond, expr->left,
                        /*is_join_condition_for_expr=*/false, parameter_tables,
                        table_filters);
    }
  }

  // Then the right side, if it's allowed.
  if (can_push_into_right) {
    Item *partial_cond = make_cond_for_table(
        current_thd, cond, expr->right->tables_in_subtree, /*used_table=*/0,
        /*exclude_expensive_cond=*/true);
    if (partial_cond != nullptr) {
      PushDownCondition(partial_cond, expr->right,
                        /*is_join_condition_for_expr=*/false, parameter_tables,
                        table_filters);
    }
  }

  // Push join conditions further down each side to see if they are sargable
  // (see the function comment).
  if (can_push_into_left) {
    table_map left_tables = cond->used_tables() & expr->left->tables_in_subtree;
    if (left_tables == 0) {
      // Degenerate condition, so add everything just to be safe.
      left_tables = expr->left->tables_in_subtree;
    }
    PushDownCondition(cond, expr->left,
                      /*is_join_condition_for_expr=*/false,
                      parameter_tables | left_tables, table_filters);
  }
  if (can_push_into_right) {
    table_map right_tables =
        cond->used_tables() & expr->right->tables_in_subtree;
    if (right_tables == 0) {
      // Degenerate condition, so add everything just to be safe.
      right_tables = expr->right->tables_in_subtree;
    }
    PushDownCondition(cond, expr->right,
                      /*is_join_condition_for_expr=*/false,
                      parameter_tables | right_tables, table_filters);
  }

  if (parameter_tables != 0) {
    // If this is pushdown for a sargable condition, we need to stop
    // here, or we'd add extra join conditions. The return value
    // doesn't matter much.
    return false;
  }

  // Now that any partial pushdown has been done, see if we can promote
  // the original filter to a join condition.
  if (is_join_condition_for_expr) {
    // We were already a join condition on this join, so there's nothing to do.
    return true;
  }

  // We cannot promote filters to join conditions for outer joins
  // and antijoins, but we can on inner joins and semijoins.
  if (expr->type == RelationalExpression::LEFT_JOIN ||
      expr->type == RelationalExpression::ANTIJOIN) {
    return true;
  }

  // Promote the filter to a join condition on this join.
  // If it's an equijoin condition, MakeHashJoinConditions() will convert it to
  // one (in expr->equijoin_conditions) when it runs later.
  assert(expr->equijoin_conditions.empty());
  expr->join_conditions.push_back(cond);
  return false;
}

/**
  Push down as many of the conditions in “conditions” as we can, into the join
  tree under “expr”. The parts that could not be pushed are returned.

  The conditions are nominally taken to be from higher up the tree than “expr”
  (e.g., WHERE conditions, or join conditions from a higher join), unless
  is_join_condition_for_expr is true, in which case they are taken to be
  posted as join conditions posted on “expr” itself. This causes them to be
  returned as remaining if “expr” is indeed their final lowest place
  in the tree (otherwise, they might get lost).
 */
Mem_root_array<Item *> PushDownAsMuchAsPossible(
    THD *thd, Mem_root_array<Item *> conditions, RelationalExpression *expr,
    bool is_join_condition_for_expr, Mem_root_array<Item *> *table_filters) {
  Mem_root_array<Item *> remaining_parts(thd->mem_root);
  for (Item *item : conditions) {
    if (IsSingleBitSet(item->used_tables() & ~PSEUDO_TABLE_BITS)) {
      // Only push down join conditions, not filters; they will stay in WHERE,
      // as we handle them separately in FoundSingleNode() and
      // FoundSubgraphPair().
      remaining_parts.push_back(item);
    } else {
      if (PushDownCondition(item, expr, is_join_condition_for_expr,
                            /*parameter_tables=*/0, table_filters)) {
        // Pushdown failed.
        remaining_parts.push_back(item);
      }
    }
  }

  return remaining_parts;
}

/**
  For each condition posted as a join condition on “expr”, try to push
  all of them further down the tree, as far as we can; then recurse to
  the child nodes, if any.

  This is needed because the initial optimization steps (before the join
  optimizer) try to hoist join conditions as far _up_ the tree as possible,
  normally all the way up to the WHERE, but could be stopped by outer joins and
  antijoins. E.g. assume what the user wrote was

     a LEFT JOIN (B JOIN C on b.x=c.x)

  This would be pulled up to

     a LEFT JOIN (B JOIN C) ON b.x=c.x

  ie., a pushable join condition posted on the LEFT JOIN, that could not go into
  the WHERE. When this function is called on the said join, it will push the
  join condition down again.
 */
void PushDownJoinConditions(THD *thd, RelationalExpression *expr,
                            Mem_root_array<Item *> *table_filters) {
  if (expr->type == RelationalExpression::TABLE) {
    return;
  }
  assert(expr->equijoin_conditions
             .empty());  // MakeHashJoinConditions() has not run yet.
  if (!expr->join_conditions.empty()) {
    expr->join_conditions = PushDownAsMuchAsPossible(
        thd, std::move(expr->join_conditions), expr,
        /*is_join_condition_for_expr=*/true, table_filters);
  }
  PushDownJoinConditions(thd, expr->left, table_filters);
  PushDownJoinConditions(thd, expr->right, table_filters);
}

/**
  Find constant expressions in join conditions, and add caches around them.
  Also add cast nodes if there are incompatible arguments in comparisons.

  Similar to work done in JOIN::finalize_table_conditions() in the old
  optimizer. Non-join predicates are done near the end in MakeJoinHypergraph().
 */
bool CanonicalizeJoinConditions(THD *thd, RelationalExpression *expr) {
  if (expr->type == RelationalExpression::TABLE) {
    return false;
  }
  assert(expr->equijoin_conditions
             .empty());  // MakeHashJoinConditions() has not run yet.
  for (Item *&condition : expr->join_conditions) {
    condition->walk(&Item::cast_incompatible_args, enum_walk::POSTFIX, nullptr);

    cache_const_expr_arg cache_arg;
    cache_const_expr_arg *analyzer_arg = &cache_arg;
    condition = condition->compile(
        &Item::cache_const_expr_analyzer, pointer_cast<uchar **>(&analyzer_arg),
        &Item::cache_const_expr_transformer, pointer_cast<uchar *>(&cache_arg));
    if (condition == nullptr) {
      return true;
    }
  }
  return CanonicalizeJoinConditions(thd, expr->left) ||
         CanonicalizeJoinConditions(thd, expr->right);
}

/**
  For all join conditions on “expr”, go through and figure out which ones are
  equijoin conditions, ie., suitable for hash join. An equijoin condition for us
  is one that is an equality comparison (=) and pulls in relations from both
  sides of the tree (so is not degenerate, and pushed as far down as possible).
  We also demand that it does not use row comparison, as our hash join
  implementation currently does not support that. Any condition that is found to
  be an equijoin condition is moved from expr->join_conditions to
  expr->equijoin_conditions.

  The function recurses down the join tree.
 */
void MakeHashJoinConditions(THD *thd, RelationalExpression *expr) {
  if (expr->type == RelationalExpression::TABLE) {
    return;
  }
  if (!expr->join_conditions.empty()) {
    assert(expr->equijoin_conditions.empty());
    Mem_root_array<Item *> extra_conditions(thd->mem_root);

    for (Item *item : expr->join_conditions) {
      // See if this is a (non-degenerate) equijoin condition.
      if ((item->used_tables() & expr->left->tables_in_subtree) &&
          (item->used_tables() & expr->right->tables_in_subtree) &&
          (item->type() == Item::FUNC_ITEM ||
           item->type() == Item::COND_ITEM)) {
        Item_func *func_item = down_cast<Item_func *>(item);
        if (func_item->contains_only_equi_join_condition()) {
          Item_func_eq *join_condition = down_cast<Item_func_eq *>(func_item);
          // Join conditions with items that returns row values (subqueries or
          // row value expression) are set up with multiple child comparators,
          // one for each column in the row. As long as the row contains only
          // one column, use it as a join condition. If it has more than one
          // column, attach it as an extra condition. Note that join
          // conditions that does not return row values are not set up with
          // any child comparators, meaning that get_child_comparator_count()
          // will return 0.
          if (join_condition->get_comparator()->get_child_comparator_count() <
              2) {
            expr->equijoin_conditions.push_back(
                down_cast<Item_func_eq *>(func_item));
            continue;
          }
        }
      }
      // It was not.
      extra_conditions.push_back(item);
    }

    expr->join_conditions = std::move(extra_conditions);
  }
  MakeHashJoinConditions(thd, expr->left);
  MakeHashJoinConditions(thd, expr->right);
}

void FindConditionsUsedTables(THD *thd, RelationalExpression *expr) {
  if (expr->type == RelationalExpression::TABLE) {
    return;
  }
  assert(expr->equijoin_conditions
             .empty());  // MakeHashJoinConditions() has not run yet.
  for (Item *item : expr->join_conditions) {
    expr->conditions_used_tables |= item->used_tables();
  }
  FindConditionsUsedTables(thd, expr->left);
  FindConditionsUsedTables(thd, expr->right);
}

/**
  Convert multi-equalities to simple equalities. This is a hack until we get
  real handling of multi-equalities (in which case it would be done much later,
  after the join order has been determined); however, note that
  remove_eq_conds() also does some constant conversion/folding work that is
  important for correctness in general.
 */
bool ConcretizeMultipleEquals(THD *thd, Mem_root_array<Item *> *conditions) {
  for (auto it = conditions->begin(); it != conditions->end();) {
    Item::cond_result res;
    if (remove_eq_conds(thd, *it, &*it, &res)) {
      return true;
    }

    if (res == Item::COND_TRUE) {
      it = conditions->erase(it);
    } else if (res == Item::COND_FALSE) {
      conditions->clear();
      conditions->push_back(new Item_int(0));
      return false;
    } else {
      ++it;
    }
  }
  return false;
}

/**
  Convert all multi-equalities in join conditions under “expr” into simple
  equalities. See ConcretizeMultipleEquals() for more information.
 */
bool ConcretizeAllMultipleEquals(THD *thd, RelationalExpression *expr,
                                 Mem_root_array<Item *> *where_conditions) {
  if (expr->type == RelationalExpression::TABLE) {
    return false;
  }
  assert(expr->equijoin_conditions
             .empty());  // MakeHashJoinConditions() has not run yet.
  if (ConcretizeMultipleEquals(thd, &expr->join_conditions)) {
    return true;
  }
  PushDownJoinConditions(thd, expr->left, where_conditions);
  PushDownJoinConditions(thd, expr->right, where_conditions);
  return false;
}

string PrintJoinList(const mem_root_deque<TABLE_LIST *> &join_list, int level) {
  string str;
  const char *join_types[] = {"inner", "left", "right"};
  std::vector<TABLE_LIST *> list(join_list.begin(), join_list.end());
  for (TABLE_LIST *tbl : list) {
    for (int i = 0; i < level * 2; ++i) str += ' ';
    if (tbl->join_cond() != nullptr) {
      str += StringPrintf("* %s %s  join_type=%s\n", tbl->alias,
                          ItemToString(tbl->join_cond()).c_str(),
                          join_types[tbl->outer_join]);
    } else {
      str += StringPrintf("* %s  join_type=%s\n", tbl->alias,
                          join_types[tbl->outer_join]);
    }
    if (tbl->nested_join != nullptr) {
      str += PrintJoinList(tbl->nested_join->join_list, level + 1);
    }
  }
  return str;
}

/**
  For a condition with the SES (Syntactic Eligibility Set) “used_tables”,
  find all relations in or under “expr” that are part of the condition's TES
  (Total Eligibility Set). The SES contains all relations that are directly
  referenced by the predicate; the TES contains all relations that are needed
  to be available before the predicate can be evaluated.

  The TES always contains at least SES, but may be bigger. For instance,
  given the join tree (a LEFT JOIN b), a condition such as b.x IS NULL
  would have a SES of {b}, but a TES of {a,b}, since joining in a could
  synthesize NULLs from b. However, given (a JOIN b) (ie., an inner join
  instead of an outer join), the TES would be {b}, identical to the SES.

  NOTE: The terms SES and TES are often used about join conditions;
  the use here is for general conditions beyond just those.

  NOTE: This returns a table_map, which is later converted to a NodeMap.
 */
table_map FindTESForCondition(table_map used_tables,
                              RelationalExpression *expr) {
  if (expr->type == RelationalExpression::TABLE) {
    // We're at the bottom of an inner join stack; nothing to see here.
    // (We could just as well return 0, but this at least makes sure the
    // SES is included in the TES.)
    return used_tables;
  } else if (expr->type == RelationalExpression::LEFT_JOIN ||
             expr->type == RelationalExpression::ANTIJOIN) {
    table_map tes = used_tables;
    if (Overlaps(used_tables, expr->left->tables_in_subtree)) {
      tes |= FindTESForCondition(used_tables, expr->left);
    }
    if (Overlaps(used_tables, expr->right->tables_in_subtree)) {
      tes |= FindTESForCondition(used_tables, expr->right);

      // The predicate needs a table from the right-hand side, but this join can
      // cause that table to become NULL, so we need to delay until the join has
      // happened. Notwithstanding any reordering on the left side, the join
      // cannot happen until all the join condition's used tables are in place,
      // so for non-degenerate conditions, that is a neccessary and sufficient
      // condition for the predicate to be applied.
      for (Item *condition : expr->equijoin_conditions) {
        tes |= condition->used_tables();
      }
      for (Item *condition : expr->join_conditions) {
        tes |= condition->used_tables();
      }

      // If all conditions were degenerate (and not left-degenerate, ie.,
      // referenced the left-hand side only), simply add all tables from the
      // left-hand side as required, so that it will not be pushed into the
      // right-hand side in any case.
      if (!Overlaps(tes, expr->left->tables_in_subtree)) {
        tes |= expr->left->tables_in_subtree;
      }
    }
    return tes;
  } else {
    table_map tes = used_tables;
    if (Overlaps(used_tables, expr->left->tables_in_subtree)) {
      tes |= FindTESForCondition(used_tables, expr->left);
    }
    if (Overlaps(used_tables, expr->right->tables_in_subtree)) {
      tes |= FindTESForCondition(used_tables, expr->right);
    }
    return tes;
  }
}

/**
  For the given hypergraph, make a textual representation in the form
  of a dotty graph. You can save this to a file and then use Graphviz
  to render this it a graphical representation of the hypergraph for
  easier debugging, e.g. like this:

    dot -Tps graph.dot > graph.ps
    display graph.ps

  See also Dbug_table_list_dumper.
 */
string PrintDottyHypergraph(const JoinHypergraph &graph) {
  string digraph;
  digraph =
      StringPrintf("digraph G {  # %zu edges\n", graph.graph.edges.size() / 2);
  for (size_t edge_idx = 0; edge_idx < graph.graph.edges.size();
       edge_idx += 2) {
    const Hyperedge &e = graph.graph.edges[edge_idx];
    const RelationalExpression *expr = graph.edges[edge_idx / 2].expr;
    string label = GenerateExpressionLabel(expr);

    // Add conflict rules to the label.
    for (const ConflictRule &rule : expr->conflict_rules) {
      label += " [conflict rule: {";
      bool first = true;
      for (int node_idx : BitsSetIn(rule.needed_to_activate_rule)) {
        if (!first) {
          label += ",";
        }
        label += graph.nodes[node_idx].table->alias;
        first = false;
      }
      label += "} -> {";
      first = true;
      for (int node_idx : BitsSetIn(rule.required_nodes)) {
        if (!first) {
          label += ",";
        }
        label += graph.nodes[node_idx].table->alias;
        first = false;
      }
      label += "}]";
    }

    // Output the edge.
    if (IsSingleBitSet(e.left) && IsSingleBitSet(e.right)) {
      // Simple edge.
      int left_node = FindLowestBitSet(e.left);
      int right_node = FindLowestBitSet(e.right);
      digraph += StringPrintf(
          "  %s -> %s [label=\"%s\"]\n", graph.nodes[left_node].table->alias,
          graph.nodes[right_node].table->alias, label.c_str());
    } else {
      // Hyperedge; draw it as a tiny “virtual node”.
      digraph += StringPrintf(
          "  e%zu [shape=circle,width=.001,height=.001,label=\"\"]\n",
          edge_idx);

      // Print the label only once.
      string left_label, right_label;
      if (IsSingleBitSet(e.right) && !IsSingleBitSet(e.left)) {
        right_label = label;
      } else {
        left_label = label;
      }

      // Left side of the edge.
      for (int left_node : BitsSetIn(e.left)) {
        digraph += StringPrintf("  %s -> e%zu [arrowhead=none,label=\"%s\"]\n",
                                graph.nodes[left_node].table->alias, edge_idx,
                                left_label.c_str());
        left_label = "";
      }

      // Right side of the edge.
      for (int right_node : BitsSetIn(e.right)) {
        digraph += StringPrintf("  e%zu -> %s [label=\"%s\"]\n", edge_idx,
                                graph.nodes[right_node].table->alias,
                                right_label.c_str());
        right_label = "";
      }
    }
  }
  digraph += "}\n";
  return digraph;
}

NodeMap IntersectIfNotDegenerate(NodeMap used_nodes, NodeMap available_nodes) {
  if (!Overlaps(used_nodes, available_nodes)) {
    // Degenerate case.
    return available_nodes;
  } else {
    return used_nodes & available_nodes;
  }
}

/**
  When we have the conflict rules, we want to fold them into the hyperedge
  we are about to create. This works by growing the TES (Total Eligibility
  Set), the set of tables that needs to be present before we can do the
  join; the TES will eventually be split into two and made into a hyperedge.

  The TES must obviously include the SES (Syntactic Eligibility Set),
  every table mentioned in the join condition. And if anything on the left
  side of a conflict rule overlaps with the TES, that conflict rule would
  always be active, and we can safely include the right side into the TES.
  Similarly, if the TES is a superset of what's on the right side of a conflict
  rule, that rule will never prevent anything (since we never see a subgraph
  unless we have everything touched by its hyperedge, ie., the TES), so it
  can be removed. We iterate over all the conflict rules until they are all
  gone or the TES has stopped growing; then we create our hyperedge by
  splitting the TES.
 */
NodeMap AbsorbConflictRulesIntoTES(
    NodeMap total_eligibility_set,
    Mem_root_array<ConflictRule> *conflict_rules) {
  NodeMap prev_total_eligibility_set;
  do {
    prev_total_eligibility_set = total_eligibility_set;
    for (const ConflictRule &rule : *conflict_rules) {
      if (Overlaps(rule.needed_to_activate_rule, total_eligibility_set)) {
        // This conflict rule will always be active, so we can add its right
        // side to the TES unconditionally. (The rule is now obsolete and
        // will be removed below.)
        total_eligibility_set |= rule.required_nodes;
      }
    }
    auto new_end = std::remove_if(
        conflict_rules->begin(), conflict_rules->end(),
        [total_eligibility_set](const ConflictRule &rule) {
          // If the right side of the conflict rule is
          // already part of the TES, it is obsolete
          // and can be removed. It will be dealt with
          // as a hyperedge.
          return IsSubset(rule.required_nodes, total_eligibility_set);
        });
    conflict_rules->erase(new_end, conflict_rules->end());
  } while (total_eligibility_set != prev_total_eligibility_set &&
           !conflict_rules->empty());
  return total_eligibility_set;
}

/**
  For the join operator in “expr”, build a hyperedge that encapsulates its
  reordering conditions as completely as possible. The conditions given by
  the hyperedge are necessary and usually sufficient; for the cases where
  they are not sufficient, we leave conflict rules on “expr” (see below).

  This function is almost verbatim the CD-C algorithm from “On the correct and
  complete enumeration of the core search space” by Moerkotte et al [Moe13].
  It works by the concept of conflict rules (CRs); if a CR A → B, for relation
  sets A and B, is attached on a given join, then if _any_ table from A is
  present in the join, then _all_ tables from B are required. As a trivial
  example, one can imagine t1 \<opA\> (t2 \<opB\> t3); if \<opA\> has a CR
  {t2} → {t3}, then the rewrite (t1 \<opA\> t2) \<opB\> t3 would not be allowed,
  since t2 is present but t3 is not. However, in the absence of other CRs,
  and given appropriate connectivity in the graph, the rewrite
  (t1 \<opA\> t3) \<opB\> t2 _would_ be allowed.

  Conflict rules are both expressive enough to precisely limit invalid rewrites,
  and in the majority of cases, can be folded into hyperedges, relegating
  the task of producing only valid plans to the subgraph enumeration (DPhyp),
  which is highly efficient at it. In the few cases that remain, they will need
  to be checked manually in CostingReceiver, but this is fast (only a few bitmap
  operations per remaining CR).

  The gist of the algorithm is to compare every operator with every operator
  below it in the join tree, looking for illegal rewrites between them, and
  adding precise CRs to stop only those rewrites. For instance, assume a query
  like

    t1 LEFT JOIN (t2 JOIN t3 USING (y)) ON t1.x=t2.x

  Looking at the root predicate (the LEFT JOIN), the question is what CRs
  and hyperedge to produce. The join predicate only mentions t1 and t2,
  so it only gives rise to the simple edge {t1}→{t2}. So without any conflict
  rules, nothing would stop us from joining t1/t2 without including t3,
  and we would allow a generated plan essentially equal to

    (t1 LEFT JOIN t2 ON t1.x=t2.x) JOIN t3 USING (y)

  which is illegal; we have attempted to use associativity illegally.
  So when we compare the LEFT JOIN (in the original query tree) with the JOIN,
  we look up those two operator types using OperatorsAreAssociative()
  (which essentially does a lookup into a small table), see that the combination
  LEFT JOIN and JOIN is not associative, and thus create a conflict rule that
  prevents this:

    {t2} → {t3}

  t2 here is everything on the left side of the inner join, and t3 is every
  table on the right side of the inner join that is mentioned in the join
  condition (which happens to also be everything on the right side).
  This rule, posted on the LEFT JOIN, prevents it from including t2 until
  it has been combined with t3, which is exactly what we want. There are some
  tweaks for degenerate conditions, but that's really all for associativity
  conflict rules.

  The other source of conflict rules comes from a parallel property
  called l-asscom and r-asscom; see OperatorsAreLeftAsscom() and
  OperatorsAreRightAsscom(). They work in exactly the same way; look at
  every pair between and operator and its children, look it up in a table,
  and add a conflict rule that prevents the rewrite if it is illegal.

  When we have the CRs, we want to fold them into the hyperedge
  we are about to create. See AbsorbConflictRulesIntoTES() for details.

  Note that in the presence of degenerate predicates or Cartesian products,
  we may make overly broad hyperedges, ie., we will disallow otherwise
  valid plans (but never allow invalid plans). This is the only case where
  the algorithm misses a valid join ordering, and also the only place where
  we diverge somewhat from the paper, which doesn't discuss hyperedges in
  the presence of such cases.
 */
Hyperedge FindHyperedgeAndJoinConflicts(THD *thd, NodeMap used_nodes,
                                        RelationalExpression *expr) {
  assert(expr->type != RelationalExpression::TABLE);

  Mem_root_array<ConflictRule> conflict_rules(thd->mem_root);
  ForEachJoinOperator(
      expr->left, [expr, &conflict_rules](RelationalExpression *child) {
        if (!OperatorsAreAssociative(*child, *expr)) {
          // Prevent associative rewriting; we cannot apply this operator
          // (rule kicks in as soon as _any_ table from the right side
          // is seen) until we have all nodes mentioned on the left side of
          // the join condition.
          const NodeMap left = IntersectIfNotDegenerate(
              child->conditions_used_tables, child->left->nodes_in_subtree);
          conflict_rules.push_back(
              ConflictRule{child->right->nodes_in_subtree, left});
        }
        if (!OperatorsAreLeftAsscom(*child, *expr)) {
          // Prevent l-asscom rewriting; we cannot apply this operator
          // (rule kicks in as soon as _any_ table from the left side
          // is seen) until we have all nodes mentioned on the right side of
          // the join condition.
          const NodeMap right = IntersectIfNotDegenerate(
              child->conditions_used_tables, child->right->nodes_in_subtree);
          conflict_rules.push_back(
              ConflictRule{child->left->nodes_in_subtree, right});
        }
      });

  // Exactly the same as the previous, just mirrored left/right.
  ForEachJoinOperator(
      expr->right, [expr, &conflict_rules](RelationalExpression *child) {
        if (!OperatorsAreAssociative(*expr, *child)) {
          const NodeMap right = IntersectIfNotDegenerate(
              child->conditions_used_tables, child->right->nodes_in_subtree);
          conflict_rules.push_back(
              ConflictRule{child->left->nodes_in_subtree, right});
        }
        if (!OperatorsAreRightAsscom(*expr, *child)) {
          const NodeMap left = IntersectIfNotDegenerate(
              child->conditions_used_tables, child->left->nodes_in_subtree);
          conflict_rules.push_back(
              ConflictRule{child->right->nodes_in_subtree, left});
        }
      });

  // Now go through all of the conflict rules and use them to grow the
  // hypernode, making it more restrictive if possible/needed.
  NodeMap total_eligibility_set =
      AbsorbConflictRulesIntoTES(used_nodes, &conflict_rules);

  // Check for degenerate predicates and Cartesian products;
  // we cannot have hyperedges with empty end points. If we have to
  // go down this path, re-check if there are any conflict rules
  // that we can now get rid of.
  if (!Overlaps(total_eligibility_set, expr->left->nodes_in_subtree)) {
    total_eligibility_set |= expr->left->nodes_in_subtree;
    total_eligibility_set =
        AbsorbConflictRulesIntoTES(total_eligibility_set, &conflict_rules);
  }
  if (!Overlaps(total_eligibility_set, expr->right->nodes_in_subtree)) {
    total_eligibility_set |= expr->right->nodes_in_subtree;
    total_eligibility_set =
        AbsorbConflictRulesIntoTES(total_eligibility_set, &conflict_rules);
  }
  expr->conflict_rules = std::move(conflict_rules);

  const NodeMap left = total_eligibility_set & expr->left->nodes_in_subtree;
  const NodeMap right = total_eligibility_set & expr->right->nodes_in_subtree;
  return {left, right};
}

}  // namespace

/**
  Convert a join rooted at “expr” into a join hypergraph that encapsulates
  the constraints given by the relational expressions (e.g. inner joins are
  more freely reorderable than outer joins).

  The function in itself only does some bookkeeping around node bitmaps,
  and then defers the actual conflict detection logic to
  FindHyperedgeAndJoinConflicts().
 */
void MakeJoinGraphFromRelationalExpression(THD *thd, RelationalExpression *expr,
                                           string *trace,
                                           JoinHypergraph *graph) {
  if (expr->type == RelationalExpression::TABLE) {
    graph->graph.AddNode();
    graph->nodes.push_back(JoinHypergraph::Node{
        expr->table->table,
        Mem_root_array<Item *>{thd->mem_root,
                               expr->join_conditions_pushable_to_this},
        Mem_root_array<SargablePredicate>{thd->mem_root}});
    assert(expr->table->tableno() < MAX_TABLES);
    graph->table_num_to_node_num[expr->table->tableno()] =
        graph->graph.nodes.size() - 1;
    expr->nodes_in_subtree = NodeMap{1} << (graph->graph.nodes.size() - 1);
    return;
  }

  MakeJoinGraphFromRelationalExpression(thd, expr->left, trace, graph);
  MakeJoinGraphFromRelationalExpression(thd, expr->right, trace, graph);
  expr->nodes_in_subtree =
      expr->left->nodes_in_subtree | expr->right->nodes_in_subtree;

  table_map used_tables = 0;
  for (Item *condition : expr->join_conditions) {
    used_tables |= condition->used_tables();
  }
  for (Item *condition : expr->equijoin_conditions) {
    used_tables |= condition->used_tables();
  }
  const NodeMap used_nodes = GetNodeMapFromTableMap(
      used_tables & ~PSEUDO_TABLE_BITS, graph->table_num_to_node_num);

  const Hyperedge edge = FindHyperedgeAndJoinConflicts(thd, used_nodes, expr);
  graph->graph.AddEdge(edge.left, edge.right);

  if (trace != nullptr) {
    *trace += StringPrintf("Selectivity of join %s:\n",
                           GenerateExpressionLabel(expr).c_str());
  }
  double selectivity = 1.0;
  for (Item *item : expr->equijoin_conditions) {
    selectivity *= EstimateSelectivity(current_thd, item, trace);
  }
  for (Item *item : expr->join_conditions) {
    selectivity *= EstimateSelectivity(current_thd, item, trace);
  }
  if (trace != nullptr &&
      expr->equijoin_conditions.size() + expr->join_conditions.size() > 1) {
    *trace += StringPrintf("  - total: %.3f\n", selectivity);
  }

  graph->edges.push_back(JoinPredicate{expr, selectivity});
}

NodeMap GetNodeMapFromTableMap(
    table_map table_map, const array<int, MAX_TABLES> &table_num_to_node_num) {
  NodeMap ret = 0;
  for (int table_num : BitsSetIn(table_map)) {
    assert(table_num < int(MAX_TABLES));
    assert(table_num_to_node_num[table_num] != -1);
    ret |= TableBitmap(table_num_to_node_num[table_num]);
  }
  return ret;
}

const JOIN *JoinHypergraph::join() const { return m_query_block->join; }

bool MakeJoinHypergraph(THD *thd, string *trace, JoinHypergraph *graph) {
  const Query_block *query_block = graph->query_block();
  const JOIN *join = graph->join();

  if (trace != nullptr) {
    // TODO(sgunders): Do we want to keep this in the trace indefinitely?
    // It's only useful for debugging, not as much for understanding what's
    // going on.
    *trace += "Join list after simplification:\n";
    *trace += PrintJoinList(query_block->top_join_list, /*level=*/0);
    *trace += "\n";
  }

  RelationalExpression *root =
      MakeRelationalExpressionFromJoinList(thd, query_block->top_join_list);

  if (trace != nullptr) {
    // TODO(sgunders): Same question as above; perhaps the version after
    // pushdown is sufficient.
    *trace +=
        StringPrintf("Made this relational tree; WHERE condition is %s:\n",
                     ItemToString(join->where_cond).c_str());
    *trace += PrintRelationalExpression(root, 0);
    *trace += "\n";
  }

  Mem_root_array<Item *> table_filters(thd->mem_root);
  if (ConcretizeAllMultipleEquals(thd, root, &table_filters)) {
    return true;
  }
  PushDownJoinConditions(thd, root, &table_filters);

  // Split up WHERE conditions, and push them down into the tree as much as
  // we can. (They have earlier been hoisted up as far as possible; see
  // comments on PushDownAsMuchAsPossible() and PushDownJoinConditions().)
  // Note that we do this after pushing down join conditions, so that we
  // don't push down WHERE conditions to join conditions and then re-process
  // them later.
  Mem_root_array<Item *> where_conditions(thd->mem_root);
  if (join->where_cond != nullptr) {
    ExtractConditions(join->where_cond, &where_conditions);
    if (ConcretizeMultipleEquals(thd, &where_conditions)) {
      return true;
    }
    where_conditions = PushDownAsMuchAsPossible(
        thd, std::move(where_conditions), root,
        /*is_join_condition_for_expr=*/false, &table_filters);
  }

  if (CanonicalizeJoinConditions(thd, root)) {
    return true;
  }
  FindConditionsUsedTables(thd, root);
  MakeHashJoinConditions(thd, root);

  if (trace != nullptr) {
    *trace += StringPrintf(
        "After pushdown; remaining WHERE conditions are %s, "
        "table filters are %s:\n",
        ItemsToString(where_conditions).c_str(),
        ItemsToString(table_filters).c_str());
    *trace += PrintRelationalExpression(root, 0);
    *trace += '\n';
  }

  // Construct the hypergraph from the relational expression.
#ifndef NDEBUG
  std::fill(begin(graph->table_num_to_node_num),
            end(graph->table_num_to_node_num), -1);
#endif
  MakeJoinGraphFromRelationalExpression(thd, root, trace, graph);

  if (trace != nullptr) {
    *trace += "\nConstructed hypergraph:\n";
    *trace += PrintDottyHypergraph(*graph);

    if (DEBUGGING_DPHYP) {
      // DPhyp printouts talk mainly about R1, R2, etc., so if debugging
      // the algorithm, it is useful to have a link to the table names.
      *trace += "Node mappings, for reference:\n";
      for (size_t i = 0; i < graph->nodes.size(); ++i) {
        *trace +=
            StringPrintf("  R%zu = %s\n", i + 1, graph->nodes[i].table->alias);
      }
    }
    *trace += "\n";
  }

  // Find TES and selectivity for each WHERE predicate that was not pushed
  // down earlier.
  for (Item *condition : where_conditions) {
    Predicate pred;
    pred.condition = condition;
    table_map total_eligibility_set =
        FindTESForCondition(condition->used_tables(), root) &
        ~(INNER_TABLE_BIT | OUTER_REF_TABLE_BIT);
    pred.total_eligibility_set =
        GetNodeMapFromTableMap(total_eligibility_set & ~RAND_TABLE_BIT,
                               graph->table_num_to_node_num) |
        (total_eligibility_set & RAND_TABLE_BIT);
    pred.selectivity = EstimateSelectivity(thd, condition, trace);
    graph->predicates.push_back(pred);

    if (trace != nullptr) {
      *trace += StringPrintf("Total eligibility set for %s: {",
                             ItemToString(condition).c_str());
      bool first = true;
      for (TABLE_LIST *tl = query_block->leaf_tables; tl != nullptr;
           tl = tl->next_leaf) {
        if (tl->map() & total_eligibility_set) {
          if (!first) *trace += ',';
          *trace += tl->alias;
          first = false;
        }
      }
      *trace += "}\n";
    }
  }

  // Table filters should be applied at the bottom, without extending the TES.
  for (Item *condition : table_filters) {
    Predicate pred;
    pred.condition = condition;
    pred.total_eligibility_set =
        condition->used_tables() & ~(INNER_TABLE_BIT | OUTER_REF_TABLE_BIT);
    assert(IsSingleBitSet(pred.total_eligibility_set));
    pred.selectivity = EstimateSelectivity(thd, condition, trace);
    graph->predicates.push_back(pred);
  }

  // Cache constant expressions in predicates, and add cast nodes if there are
  // incompatible arguments in comparisons. (We did join conditions earlier.)
  for (Predicate &predicate : graph->predicates) {
    predicate.condition->walk(&Item::cast_incompatible_args, enum_walk::POSTFIX,
                              nullptr);

    cache_const_expr_arg cache_arg;
    cache_const_expr_arg *analyzer_arg = &cache_arg;
    predicate.condition = predicate.condition->compile(
        &Item::cache_const_expr_analyzer, pointer_cast<uchar **>(&analyzer_arg),
        &Item::cache_const_expr_transformer, pointer_cast<uchar *>(&cache_arg));
    if (predicate.condition == nullptr) {
      return true;
    }
  }

  if (graph->predicates.size() > sizeof(table_map) * CHAR_BIT) {
    my_error(ER_HYPERGRAPH_NOT_SUPPORTED_YET, MYF(0),
             "more than 64 WHERE/ON predicates");
    return true;
  }
  graph->num_where_predicates = graph->predicates.size();

  return false;
}
