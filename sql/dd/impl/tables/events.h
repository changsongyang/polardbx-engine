/* Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.

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

#ifndef DD_TABLES__EVENTS_INCLUDED
#define DD_TABLES__EVENTS_INCLUDED

#include <string>

#include "sql/dd/impl/raw/raw_record.h"
#include "sql/dd/impl/types/entity_object_table_impl.h"
#include "sql/dd/object_id.h"
#include "sql/dd/string_type.h"
#include "sql/dd/types/event.h"

namespace dd {

class Item_name_key;
class Object_key;

namespace tables {

///////////////////////////////////////////////////////////////////////////

class Events : public Entity_object_table_impl
{
public:
  static const Events &instance();

  enum enum_fields
  {
    FIELD_ID= static_cast<uint>(Common_field::ID),
    FIELD_SCHEMA_ID,
    FIELD_NAME,
    FIELD_DEFINER,
    FIELD_TIME_ZONE,
    FIELD_DEFINITION,
    FIELD_DEFINITION_UTF8,
    FIELD_EXECUTE_AT,
    FIELD_INTERVAL_VALUE,
    FIELD_INTERVAL_FIELD,
    FIELD_SQL_MODE,
    FIELD_STARTS,
    FIELD_ENDS,
    FIELD_STATUS,
    FIELD_ON_COMPLETION,
    FIELD_CREATED,
    FIELD_LAST_ALTERED,
    FIELD_LAST_EXECUTED,
    FIELD_COMMENT,
    FIELD_ORIGINATOR,
    FIELD_CLIENT_COLLATION_ID,
    FIELD_CONNECTION_COLLATION_ID,
    FIELD_SCHEMA_COLLATION_ID,
    FIELD_OPTIONS
  };

  enum enum_indexes
  {
    INDEX_PK_ID= static_cast<uint>(Common_index::PK_ID),
    INDEX_UK_SCHEMA_ID_NAME= static_cast<uint>(Common_index::UK_NAME),
    INDEX_K_CLIENT_COLLATION_ID,
    INDEX_K_CONNECTION_COLLATION_ID,
    INDEX_K_SCHEMA_COLLATION_ID
  };

  enum enum_foreign_keys
  {
    FK_SCHEMA_ID,
    FK_CLIENT_COLLATION_ID,
    FK_CONNECTION_COLLATION_ID,
    FK_SCHEMA_COLLATION_ID
  };

  Events();

  virtual Event *create_entity_object(const Raw_record &) const;

  static bool update_object_key(Item_name_key *key,
                                Object_id schema_id,
                                const String_type &event_name);

  static Object_key *create_key_by_schema_id(Object_id schema_id);
};

///////////////////////////////////////////////////////////////////////////

}
}

#endif // DD_TABLES__EVENTS_INCLUDED
