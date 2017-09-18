// Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation; version 2 of the License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, 51 Franklin
// Street, Suite 500, Boston, MA 02110-1335 USA.

/// @file
///
/// This file implements the union functor and function.

#include <boost/geometry.hpp>
#include <memory>  // std::unique_ptr

#include "sql/gis/geometries.h"
#include "sql/gis/geometries_traits.h"
#include "sql/gis/union_functor.h"

namespace bg = boost::geometry;

namespace gis {

Union::Union(double semi_major, double semi_minor)
    : m_semi_major(semi_major),
      m_semi_minor(semi_minor),
      m_geographic_pl_pa_strategy(
          bg::srs::spheroid<double>(semi_major, semi_minor)),
      m_geographic_ll_la_aa_strategy(
          bg::srs::spheroid<double>(semi_major, semi_minor)) {}

Geometry *Union::operator()(const Geometry *g1, const Geometry *g2) const {
  return apply(*this, g1, g2);
}

Geometry *Union::eval(const Geometry *g1, const Geometry *g2) const {
  DBUG_ASSERT(false);
  throw not_implemented_exception(g1->coordinate_system(), g1->type(),
                                  g2->type());
}

//////////////////////////////////////////////////////////////////////////////

// union(Cartesian_multilinestring, *)

Geometry *Union::eval(const Cartesian_multilinestring *g1,
                      const Cartesian_linestring *g2) const {
  std::unique_ptr<Cartesian_multilinestring> result(
      new Cartesian_multilinestring());
  bg::union_(*g1, *g2, *result);
  return result.release();
}

//////////////////////////////////////////////////////////////////////////////

// union(Cartesian_multipolygon, *)

Geometry *Union::eval(const Cartesian_multipolygon *g1,
                      const Cartesian_polygon *g2) const {
  std::unique_ptr<Cartesian_multipolygon> result(new Cartesian_multipolygon());
  bg::union_(*g1, *g2, *result);
  return result.release();
}

//////////////////////////////////////////////////////////////////////////////

// union(Geographic_multilinestring, *)

Geometry *Union::eval(const Geographic_multilinestring *g1,
                      const Geographic_linestring *g2) const {
  std::unique_ptr<Geographic_multilinestring> result(
      new Geographic_multilinestring());
  bg::union_(*g1, *g2, *result, m_geographic_ll_la_aa_strategy);
  return result.release();
}

//////////////////////////////////////////////////////////////////////////////

// union(Geographic_multipolygon, *)

Geometry *Union::eval(const Geographic_multipolygon *g1,
                      const Geographic_polygon *g2) const {
  std::unique_ptr<Geographic_multipolygon> result(
      new Geographic_multipolygon());
  bg::union_(*g1, *g2, *result, m_geographic_ll_la_aa_strategy);
  return result.release();
}

}  // namespace gis
