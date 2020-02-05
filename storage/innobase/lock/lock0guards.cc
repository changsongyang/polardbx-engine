/*****************************************************************************

Copyright (c) 2020, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

#define LOCK_MODULE_IMPLEMENTATION

#include "lock0guards.h"
#include "lock0priv.h"
#include "sync0rw.h"

namespace locksys {

/* Global_exclusive_latch_guard */

Global_exclusive_latch_guard::Global_exclusive_latch_guard() {
  lock_sys->latches.global_latch.x_lock();
}

Global_exclusive_latch_guard::~Global_exclusive_latch_guard() {
  lock_sys->latches.global_latch.x_unlock();
}

/* Global_exclusive_try_latch */

Global_exclusive_try_latch::Global_exclusive_try_latch() {
  m_owns_exclusive_global_latch = lock_sys->latches.global_latch.try_x_lock();
}

Global_exclusive_try_latch::~Global_exclusive_try_latch() {
  if (m_owns_exclusive_global_latch) {
    lock_sys->latches.global_latch.x_unlock();
    m_owns_exclusive_global_latch = false;
  }
}

/* Shard_naked_latch_guard */

Shard_naked_latch_guard::Shard_naked_latch_guard(Lock_mutex &shard_mutex)
    : m_shard_mutex{shard_mutex} {
  ut_ad(owns_shared_global_latch());
  mutex_enter(&m_shard_mutex);
}

Shard_naked_latch_guard::Shard_naked_latch_guard(const dict_table_t &table)
    : Shard_naked_latch_guard{lock_sys->latches.table_shards.get_mutex(table)} {
}

Shard_naked_latch_guard::Shard_naked_latch_guard(const page_id_t &page_id)
    : Shard_naked_latch_guard{
          lock_sys->latches.page_shards.get_mutex(page_id)} {}

Shard_naked_latch_guard::~Shard_naked_latch_guard() {
  mutex_exit(&m_shard_mutex);
}

/* Global_shared_latch_guard */

Global_shared_latch_guard::Global_shared_latch_guard() {
  lock_sys->latches.global_latch.s_lock();
}

Global_shared_latch_guard::~Global_shared_latch_guard() {
  lock_sys->latches.global_latch.s_unlock();
}

/* Shard_latches_guard */

Shard_latches_guard::Shard_latches_guard(Lock_mutex &shard_mutex_a,
                                         Lock_mutex &shard_mutex_b)
    : m_global_shared_latch_guard{},
      m_shard_mutex_1{*std::min(&shard_mutex_a, &shard_mutex_b, MUTEX_ORDER)},
      m_shard_mutex_2{*std::max(&shard_mutex_a, &shard_mutex_b, MUTEX_ORDER)} {
  if (&m_shard_mutex_1 != &m_shard_mutex_2) {
    mutex_enter(&m_shard_mutex_1);
  }
  mutex_enter(&m_shard_mutex_2);
}

Shard_latches_guard::Shard_latches_guard(const buf_block_t &block_a,
                                         const buf_block_t &block_b)
    : Shard_latches_guard{
          lock_sys->latches.page_shards.get_mutex(block_a.get_page_id()),
          lock_sys->latches.page_shards.get_mutex(block_b.get_page_id())} {}

Shard_latches_guard::~Shard_latches_guard() {
  mutex_exit(&m_shard_mutex_2);
  if (&m_shard_mutex_1 != &m_shard_mutex_2) {
    mutex_exit(&m_shard_mutex_1);
  }
}

}  // namespace locksys
