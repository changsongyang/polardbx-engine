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

#ifndef SQL_JOIN_OPTIMIZER_MAKE_JOIN_HYPERGRAPH
#define SQL_JOIN_OPTIMIZER_MAKE_JOIN_HYPERGRAPH 1

#include <array>
#include <string>

#include "map_helpers.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/hypergraph.h"
#include "sql/mem_root_array.h"
#include "sql/sql_const.h"

class Field;
class Item;
class JOIN;
class Query_block;
class THD;
struct MEM_ROOT;
struct TABLE;

/**
  A sargable (from “Search ARGument”) predicate is one that we can attempt
  to push down into an index (what we'd call “ref access” or “index range
  scan”/“quick”). This structure denotes one such instance, precomputed from
  all the predicates in the given hypergraph.
 */
struct SargablePredicate {
  // Index into the “predicates” array in the graph.
  int predicate_index;

  // The predicate is assumed to be <field> = <other_side>.
  // Later, we could push down other kinds of relations, such as
  // greater-than.
  Field *field;
  Item *other_side;
};

/**
  A struct containing a join hypergraph of a single query block, encapsulating
  the constraints given by the relational expressions (e.g. inner joins are
  more freely reorderable than outer joins).

  Since the Hypergraph class does not carry any payloads for nodes and edges,
  and we need to associate e.g.  TABLE pointers with each node, we store our
  extra data in “nodes” and “edges”, indexed the same way the hypergraph is
  indexed.
 */
struct JoinHypergraph {
  JoinHypergraph(MEM_ROOT *mem_root, const Query_block *query_block)
      : nodes(mem_root),
        edges(mem_root),
        predicates(mem_root),
        sargable_join_predicates(mem_root),
        m_query_block(query_block) {}

  hypergraph::Hypergraph graph;

  // Maps table->tableno() to an index in “nodes”, also suitable for
  // a bit index in a NodeMap. This is normally the identity mapping,
  // except for when scalar-to-derived conversion is active.
  std::array<int, MAX_TABLES> table_num_to_node_num;

  struct Node {
    TABLE *table;

    // Join conditions that are potentially pushable to this node
    // as sargable predicates (if they are sargable, they will be
    // added to sargable_predicates below, together with sargable
    // non-join conditions). This is a verbatim copy of
    // the join_conditions_pushable_to_this member in RelationalExpression,
    // which is computed as a side effect during join pushdown.
    // (We could in principle have gone and collected all join conditions
    // ourselves when determining sargable conditions, but there would be
    // a fair amount of duplicated code in determining pushability,
    // which is why regular join pushdown does the computation.)
    Mem_root_array<Item *> join_conditions_pushable_to_this;

    // List of all sargable predicates (see SargablePredicate) where
    // the field is part of this table. When we see the node for
    // the first time, we will evaluate all of these and consider
    // creating access paths that exploit these predicates.
    Mem_root_array<SargablePredicate> sargable_predicates;
  };
  Mem_root_array<Node> nodes;

  // Note that graph.edges contain each edge twice (see Hypergraph
  // for more information), so edges[i] corresponds to graph.edges[i*2].
  Mem_root_array<JoinPredicate> edges;

  // The first <num_where_predicates> are WHERE predicates;
  // the rest are sargable join predicates. The latter are in the array
  // solely so they can be part of the regular “applied_filters” bitmap
  // if they are pushed down into an index, so that we know that we
  // don't need to apply them as join conditions later.
  Mem_root_array<Predicate> predicates;

  unsigned num_where_predicates = 0;

  // For each sargable join condition, maps into its index in “predicates”.
  // We need the predicate index when applying the join to figure out whether
  // we have already applied the predicate or not; see
  // {applied,subsumed}_sargable_join_predicates in AccessPath.
  mem_root_unordered_map<Item *, int> sargable_join_predicates;

  /// Returns a pointer to the query block that is being planned.
  const Query_block *query_block() const { return m_query_block; }

  /// Returns a pointer to the JOIN object of the query block being planned.
  const JOIN *join() const;

 private:
  /// A pointer to the query block being planned.
  const Query_block *m_query_block;
};

/**
  Make a join hypergraph from the query block given by “graph->query_block”,
  converting from MySQL's join list structures to the ones expected
  by the hypergraph join optimizer. This includes pushdown of WHERE
  predicates, and detection of conditions suitable for hash join.
  However, it does not include simplification of outer to inner joins;
  that is presumed to have happened earlier.

  The result is suitable for running DPhyp (subgraph_enumeration.h)
  to find optimal join planning.
 */
bool MakeJoinHypergraph(THD *thd, std::string *trace, JoinHypergraph *graph);

hypergraph::NodeMap GetNodeMapFromTableMap(
    table_map table_map,
    const std::array<int, MAX_TABLES> &table_num_to_node_num);

#endif  // SQL_JOIN_OPTIMIZER_MAKE_JOIN_HYPERGRAPH
