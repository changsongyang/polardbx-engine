/* Copyright (c) 2017 Oracle and/or its affiliates. All rights reserved.

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

#ifndef DD_SYSTEM_VIEWS__SYSTEM_VIEW_DEFINITION_IMPL_INCLUDED
#define DD_SYSTEM_VIEWS__SYSTEM_VIEW_DEFINITION_IMPL_INCLUDED

#include <map>
#include <vector>

#include "sql/dd/string_type.h"               // dd::String_type
#include "sql/dd/types/system_view_definition.h" // dd::System_view_definition
#include "sql/mysqld.h"                       // lower_case_table_names

namespace dd {
namespace system_views {

class System_view_definition_impl : public System_view_definition
{
public:
  /**
    Get view name.

    @return name of the view.
  */
  virtual const String_type &view_name() const
  { return m_view_name; }

  /**
    Set view name.

    @return void.
  */
  virtual void set_view_name(const String_type &name)
  { m_view_name= name; }

  /**
    Get collation clause to append to view definition for some
    view columns based on lower_case_table_names.

    @return Empty string if lctn=0, other wise " COLLATE utf8_tolower_ci".
  */
  static const String_type fs_name_collation()
  {
     if (lower_case_table_names != 0)
       return " COLLATE utf8_tolower_ci";
     return "";
  }

  virtual String_type build_ddl_create_view() const = 0;

private:
  // Name of I_S system view;
  String_type m_view_name;
};


class System_view_select_definition_impl: public System_view_definition_impl
{
public:

  /**
    Add a field definition for the SELECT projection.
    This function can be called more than once. The call will add a new
    projection to the SELECT command.

    @param field_number  Ordinal position of field in the projection list.
    @param field_name    Field name used for the SELECT's projection.
    @param field_definition Expression representing the projection.

    @return void.
  */
  virtual void add_field(int field_number, const String_type &field_name,
                         const String_type field_definition)
  {
    // Make sure the field_number and field_name are not added twise.
    DBUG_ASSERT(
      m_field_numbers.find(field_name) == m_field_numbers.end() &&
      m_field_definitions.find(field_number) == m_field_definitions.end());

    // Store the field number.
    m_field_numbers[field_name]= field_number;

    // Store the field definition expression.
    Stringstream_type ss;
    ss << field_definition << " AS " << field_name;
    m_field_definitions[field_number]= ss.str();
  }

  /**
    Add FROM clause for the SELECT.
    This function can be called more than once. The clause will be appended to
    the previous FROM clause string.

    @param from  String representing the FROM clause.

    @return void.
  */
  virtual void add_from(const String_type &from)
  { m_from_clauses.push_back(from); }

  /**
    Add WHERE clause for the SELECT.
    This function can be called more than once. The clause will be appended to
    the previous WHERE clause string.

    @param where  String representing the WHERE clause.

    @return void.
  */
  virtual void add_where(const String_type &where)
  { m_where_clauses.push_back(where); }

  /**
    Get the field ordinal position number for the given field name.

    @param field_name  Column name for which the field number is returned.

    @return Integer representing position of column in projection list.
  */
  virtual int field_number(const String_type &field_name) const
  {
    DBUG_ASSERT(m_field_numbers.find(field_name) != m_field_numbers.end());
    return m_field_numbers.find(field_name)->second;
  }

  /**
    Build the SELECT query that is used in the CREATE VIEW command.

    @return The SELECT query string.
  */
  String_type build_select_query() const
  {
    Stringstream_type ss;

    ss << "SELECT \n";
    // Output view column definitions
    for (Field_definitions::const_iterator field= m_field_definitions.begin();
         field != m_field_definitions.end(); ++field)
    {
      if (field != m_field_definitions.begin())
        ss << ",\n";
      ss << "  " << field->second;
    }

    // Output FROM clauses
    for (From_clauses::const_iterator from= m_from_clauses.begin();
         from != m_from_clauses.end(); ++from)
    {
      if (from == m_from_clauses.begin())
        ss << " FROM ";

      ss << "\n  " << *from;
    }

    // Output WHERE clauses
    for (Where_clauses::const_iterator where= m_where_clauses.begin();
         where != m_where_clauses.end(); ++where)
    {
      if (where == m_where_clauses.begin())
        ss << " WHERE ";

      ss << "\n  " << *where;
    }

    ss << "\n";

    return ss.str();
  }

  virtual String_type build_ddl_create_view() const
  {
    Stringstream_type ss;
    ss << "CREATE OR REPLACE DEFINER=`root`@`localhost` VIEW "
       << "information_schema." << view_name()
       << " AS " + build_select_query();

    return ss.str();
  }

private:
  // Map of field_names and the ordinal position in SELECT projection.
  typedef std::map<String_type, int> Field_numbers;

  // Map of field ordinal position and their view column definition.
  typedef std::map<int, String_type> Field_definitions;

  // List of FROM clause definintion in the SELECT
  typedef std::vector<String_type> From_clauses;

  // List of WHERE clause definition in the SELECT
  typedef std::vector<String_type> Where_clauses;

  Field_numbers m_field_numbers;
  Field_definitions m_field_definitions;
  From_clauses m_from_clauses;
  Where_clauses m_where_clauses;
};


class System_view_union_definition_impl: public System_view_definition_impl
{

public:
  /**
    Get the object for first SELECT view definition to be used in UNION.

    @return The System_view_select_definition_impl*.
  */
  System_view_select_definition_impl* get_first_select()
  { return &m_first_select; }

  /**
    Get the object for second SELECT view definition to be used in UNION.

    @return The System_view_select_definition_impl*.
  */
  System_view_select_definition_impl* get_second_select()
  { return &m_second_select; }

  virtual String_type build_ddl_create_view() const
  {
    Stringstream_type ss;
    ss << "CREATE OR REPLACE DEFINER=`root`@`localhost` VIEW "
       << "information_schema." << view_name() << " AS "
       << "(" << m_first_select.build_select_query() << ")"
       << " UNION " << "(" << m_second_select.build_select_query() << ")";

    return ss.str();
  }

private:
  // Member that holds two SELECT's used for UNION
  System_view_select_definition_impl m_first_select, m_second_select;
};

}
}

#endif	// DD_SYSTEM_VIEWS__SYSTEM_VIEW_DEFINITION_IMPL_INCLUDED

