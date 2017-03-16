/* Copyright (c) 2010, 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_esms_by_thread_by_event_name.cc
  Table EVENTS_STATEMENTS_SUMMARY_BY_THREAD_BY_EVENT_NAME (implementation).
*/

#include "storage/perfschema/table_esms_by_thread_by_event_name.h"

#include <stddef.h>

#include "field.h"
#include "my_dbug.h"
#include "my_thread.h"
#include "pfs_buffer_container.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "pfs_global.h"
#include "pfs_instr_class.h"
#include "pfs_visitor.h"

THR_LOCK table_esms_by_thread_by_event_name::m_table_lock;

/* clang-format off */
static const TABLE_FIELD_TYPE field_types[]=
{
  {
    { C_STRING_WITH_LEN("THREAD_ID") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("EVENT_NAME") },
    { C_STRING_WITH_LEN("varchar(128)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("COUNT_STAR") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_TIMER_WAIT") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("MIN_TIMER_WAIT") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("AVG_TIMER_WAIT") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("MAX_TIMER_WAIT") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_LOCK_TIME") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_ERRORS") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_WARNINGS") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_ROWS_AFFECTED") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_ROWS_SENT") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_ROWS_EXAMINED") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_CREATED_TMP_DISK_TABLES") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_CREATED_TMP_TABLES") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SELECT_FULL_JOIN") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SELECT_FULL_RANGE_JOIN") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SELECT_RANGE") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SELECT_RANGE_CHECK") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SELECT_SCAN") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SORT_MERGE_PASSES") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SORT_RANGE") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SORT_ROWS") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_SORT_SCAN") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_NO_INDEX_USED") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("SUM_NO_GOOD_INDEX_USED") },
    { C_STRING_WITH_LEN("bigint(20)") },
    { NULL, 0}
  }
};
/* clang-format on */

TABLE_FIELD_DEF
table_esms_by_thread_by_event_name::m_field_def = {26, field_types};

PFS_engine_table_share table_esms_by_thread_by_event_name::m_share = {
  {C_STRING_WITH_LEN("events_statements_summary_by_thread_by_event_name")},
  &pfs_truncatable_acl,
  table_esms_by_thread_by_event_name::create,
  NULL, /* write_row */
  table_esms_by_thread_by_event_name::delete_all_rows,
  table_esms_by_thread_by_event_name::get_row_count,
  sizeof(pos_esms_by_thread_by_event_name),
  &m_table_lock,
  &m_field_def,
  false, /* checked */
  false  /* perpetual */
};

bool
PFS_index_esms_by_thread_by_event_name::match(PFS_thread *pfs)
{
  if (m_fields >= 1)
  {
    if (!m_key_1.match(pfs))
    {
      return false;
    }
  }
  return true;
}

bool
PFS_index_esms_by_thread_by_event_name::match(PFS_statement_class *klass)
{
  if (klass->is_mutable())
  {
    return false;
  }

  if (m_fields >= 2)
  {
    if (!m_key_2.match(klass))
    {
      return false;
    }
  }
  return true;
}

PFS_engine_table *
table_esms_by_thread_by_event_name::create(void)
{
  return new table_esms_by_thread_by_event_name();
}

int
table_esms_by_thread_by_event_name::delete_all_rows(void)
{
  reset_events_statements_by_thread();
  return 0;
}

ha_rows
table_esms_by_thread_by_event_name::get_row_count(void)
{
  return global_thread_container.get_row_count() * statement_class_max;
}

table_esms_by_thread_by_event_name::table_esms_by_thread_by_event_name()
  : PFS_engine_table(&m_share, &m_pos), m_pos(), m_next_pos()
{
}

void
table_esms_by_thread_by_event_name::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int
table_esms_by_thread_by_event_name::rnd_init(bool)
{
  m_normalizer = time_normalizer::get(statement_timer);
  return 0;
}

int
table_esms_by_thread_by_event_name::rnd_next(void)
{
  PFS_thread *thread;
  PFS_statement_class *statement_class;
  bool has_more_thread = true;

  for (m_pos.set_at(&m_next_pos); has_more_thread; m_pos.next_thread())
  {
    thread = global_thread_container.get(m_pos.m_index_1, &has_more_thread);
    if (thread != NULL)
    {
      statement_class = find_statement_class(m_pos.m_index_2);
      if (statement_class)
      {
        m_next_pos.set_after(&m_pos);
        return make_row(thread, statement_class);
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int
table_esms_by_thread_by_event_name::rnd_pos(const void *pos)
{
  PFS_thread *thread;
  PFS_statement_class *statement_class;

  set_position(pos);

  thread = global_thread_container.get(m_pos.m_index_1);
  if (thread != NULL)
  {
    statement_class = find_statement_class(m_pos.m_index_2);
    if (statement_class)
    {
      return make_row(thread, statement_class);
    }
  }

  return HA_ERR_RECORD_DELETED;
}

int
table_esms_by_thread_by_event_name::index_init(uint idx MY_ATTRIBUTE((unused)),
                                               bool)
{
  m_normalizer = time_normalizer::get(statement_timer);

  DBUG_ASSERT(idx == 0);
  m_opened_index = PFS_NEW(PFS_index_esms_by_thread_by_event_name);
  m_index = m_opened_index;
  return 0;
}

int
table_esms_by_thread_by_event_name::index_next()
{
  PFS_thread *thread;
  PFS_statement_class *statement_class;
  bool has_more_thread = true;

  for (m_pos.set_at(&m_next_pos); has_more_thread; m_pos.next_thread())
  {
    thread = global_thread_container.get(m_pos.m_index_1, &has_more_thread);
    if (thread != NULL)
    {
      if (m_opened_index->match(thread))
      {
        do
        {
          statement_class = find_statement_class(m_pos.m_index_2);
          if (statement_class != NULL)
          {
            if (m_opened_index->match(statement_class))
            {
              if (!make_row(thread, statement_class))
              {
                m_next_pos.set_after(&m_pos);
                return 0;
              }
            }
            m_pos.m_index_2++;
          }
        } while (statement_class != NULL);
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int
table_esms_by_thread_by_event_name::make_row(PFS_thread *thread,
                                             PFS_statement_class *klass)
{
  pfs_optimistic_state lock;

  if (klass->is_mutable())
  {
    return HA_ERR_RECORD_DELETED;
  }

  /* Protect this reader against a thread termination */
  thread->m_lock.begin_optimistic_lock(&lock);

  m_row.m_thread_internal_id = thread->m_thread_internal_id;

  m_row.m_event_name.make_row(klass);

  PFS_connection_statement_visitor visitor(klass);
  PFS_connection_iterator::visit_thread(thread, &visitor);

  if (!thread->m_lock.end_optimistic_lock(&lock))
  {
    return HA_ERR_RECORD_DELETED;
  }

  m_row.m_stat.set(m_normalizer, &visitor.m_stat);
  return 0;
}

int
table_esms_by_thread_by_event_name::read_row_values(TABLE *table,
                                                    unsigned char *,
                                                    Field **fields,
                                                    bool read_all)
{
  Field *f;

  /* Set the null bits */
  DBUG_ASSERT(table->s->null_bytes == 0);

  for (; (f = *fields); fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch (f->field_index)
      {
      case 0: /* THREAD_ID */
        set_field_ulonglong(f, m_row.m_thread_internal_id);
        break;
      case 1: /* EVENT_NAME */
        m_row.m_event_name.set_field(f);
        break;
      default: /* 2, ... COUNT/SUM/MIN/AVG/MAX */
        m_row.m_stat.set_field(f->field_index - 2, f);
        break;
      }
    }
  }

  return 0;
}
