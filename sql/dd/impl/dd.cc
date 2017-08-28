/* Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.

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

#include "sql/dd/dd.h"

#include "sql/dd/impl/cache/shared_dictionary_cache.h" // dd::cache::Shared_...
#include "sql/dd/impl/dictionary_impl.h"            // dd::Dictionary_impl
#include "sql/dd/impl/system_registry.h"            // dd::System_tables
#include "sql/dd/impl/types/entity_object_impl.h"
#include "sql/dd/impl/types/charset_impl.h"
#include "sql/dd/impl/types/collation_impl.h"
#include "sql/dd/impl/types/column_impl.h"
#include "sql/dd/impl/types/column_statistics_impl.h"
#include "sql/dd/impl/types/column_type_element_impl.h"
#include "sql/dd/impl/types/event_impl.h"
#include "sql/dd/impl/types/foreign_key_impl.h"
#include "sql/dd/impl/types/foreign_key_element_impl.h"
#include "sql/dd/impl/types/function_impl.h"
#include "sql/dd/impl/types/index_impl.h"
#include "sql/dd/impl/types/index_element_impl.h"
#include "sql/dd/impl/types/index_stat_impl.h"
#include "sql/dd/impl/types/partition_impl.h"
#include "sql/dd/impl/types/partition_index_impl.h"
#include "sql/dd/impl/types/partition_value_impl.h"
#include "sql/dd/impl/types/procedure_impl.h"
#include "sql/dd/impl/types/resource_group_impl.h"
#include "sql/dd/impl/types/schema_impl.h"
#include "sql/dd/impl/types/spatial_reference_system_impl.h"
#include "sql/dd/impl/types/table_impl.h"
#include "sql/dd/impl/types/table_stat_impl.h"
#include "sql/dd/impl/types/tablespace_impl.h"
#include "sql/dd/impl/types/tablespace_file_impl.h"
#include "sql/dd/impl/types/view_impl.h"

namespace dd {

bool init(enum_dd_init_type dd_init)
{
  if (dd_init == enum_dd_init_type::DD_INITIALIZE ||
      dd_init == enum_dd_init_type::DD_RESTART_OR_UPGRADE)
  {
    cache::Shared_dictionary_cache::init();
    System_tables::instance()->add_inert_dd_tables();
    System_views::instance()->init();
  }

  return Dictionary_impl::init(dd_init);
}

///////////////////////////////////////////////////////////////////////////

bool shutdown()
{
  cache::Shared_dictionary_cache::shutdown();
  return Dictionary_impl::shutdown();
}


Dictionary *get_dictionary()
{
  return Dictionary_impl::instance();
}

template <typename X>
X *create_object()
{ return dynamic_cast<X *> (new (std::nothrow) typename X::Impl()); }

template Charset_impl *create_object<Charset_impl>();
template Collation *create_object<Collation>();
template Collation_impl *create_object<Collation_impl>();
template Column *create_object<Column>();
template Column_statistics *create_object<Column_statistics>();
template Column_type_element *create_object<Column_type_element>();
template Event *create_object<Event>();
template Function *create_object<Function>();
template Foreign_key *create_object<Foreign_key>();
template Foreign_key_element *create_object<Foreign_key_element>();
template Index *create_object<Index>();
template Index_element *create_object<Index_element>();
template Index_stat *create_object<Index_stat>();
template Partition *create_object<Partition>();
template Partition_index *create_object<Partition_index>();
template Partition_value *create_object<Partition_value>();
template Procedure *create_object<Procedure>();
template Resource_group *create_object<Resource_group>();
template Schema *create_object<Schema>();
template Spatial_reference_system *create_object<Spatial_reference_system>();
template Table *create_object<Table>();
template Table_stat *create_object<Table_stat>();
template Tablespace *create_object<Tablespace>();
template Tablespace_file *create_object<Tablespace_file>();
template View *create_object<View>();


} // namespace dd
