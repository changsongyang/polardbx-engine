/* Copyright (c) 2008, 2015, Oracle and/or its affiliates. All rights reserved.

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
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef PFS_COLUMN_VALUES_H
#define PFS_COLUMN_VALUES_H

#include "m_string.h"                           /* LEX_STRING */

/**
  @file storage/perfschema/pfs_column_values.h
  Literal values for columns used in the
  performance schema tables (declarations).
*/

/** String, "PERFORMANCE_SCHEMA". */
extern LEX_STRING PERFORMANCE_SCHEMA_str;

/** String prefix for all mutex instruments. */
extern LEX_STRING mutex_instrument_prefix;
/** String prefix for all rwlock instruments. */
extern LEX_STRING rwlock_instrument_prefix;
/** String prefix for all sxlock instruments. */
extern LEX_STRING sxlock_instrument_prefix;
/** String prefix for all cond instruments. */
extern LEX_STRING cond_instrument_prefix;
/** String prefix for all thread instruments. */
extern LEX_STRING thread_instrument_prefix;
/** String prefix for all file instruments. */
extern LEX_STRING file_instrument_prefix;
/** String prefix for all stage instruments. */
extern LEX_STRING stage_instrument_prefix;
/** String prefix for all statement instruments. */
extern LEX_STRING statement_instrument_prefix;
/** String prefix for all transaction instruments. */
extern LEX_STRING transaction_instrument_prefix;
/** String prefix for all socket instruments. */
extern LEX_STRING socket_instrument_prefix;
/** String prefix for all memory instruments. */
extern LEX_STRING memory_instrument_prefix;

#endif

