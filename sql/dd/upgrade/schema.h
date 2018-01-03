/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

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

#ifndef DD_UPGRADE__SCHEMA_H_INCLUDED
#define DD_UPGRADE__SCHEMA_H_INCLUDED

#include <vector>

#include "sql/dd/string_type.h"                // dd::String_type

class THD;

namespace dd {
namespace upgrade_57 {

/**
  Create entry in mysql.schemata for all the folders found in data directory.
  If db.opt file is not present in any folder, that folder will be treated as
  a database and a warning is issued.

  @param[in]  thd        Thread handle.
  @param[in]  dbname     Schema name.

  @retval false  ON SUCCESS
  @retval true   ON FAILURE
*/
bool migrate_schema_to_dd(THD *thd, const char *dbname);

/**
  Find all the directories inside data directory. Every directory will be
  treated as a schema. These directories are in filename-encoded form.

  @param[out] db_name    An std::vector containing all database name.

  @retval false  ON SUCCESS
  @retval true   ON FAILURE
*/
bool find_schema_from_datadir(std::vector<String_type> *db_name);

} // namespace upgrade
} // namespace dd

#endif // DD_UPGRADE__SCHEMA_H_INCLUDED
