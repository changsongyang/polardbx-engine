/* Copyright (c) 2000, 2019, Alibaba and/or its affiliates. All rights reserved.

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

#include <cstddef>

#include "my_config.h"
#include "mysqld_error.h"

#ifdef RDS_HAVE_JEMALLOC

#include "mysql/components/services/log_builtins.h"

// #ifdef __cplusplus
// extern "C" {
// #endif

// /* It's special for the mysqld_clean compiling */
// int mallctl(const char *, void *oldp, size_t *, void *, size_t) {
//  if (oldp) {
//    bool *ptr = static_cast<bool *>(oldp);
//    *ptr = false;
//  }
//  LogErr(ERROR_LEVEL, ER_JEMALLOC_API_MISTAKE);
//  return 1;
// }

// #ifdef __cplusplus
// }
// #endif

#endif
