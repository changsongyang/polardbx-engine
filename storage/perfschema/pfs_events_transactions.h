/* Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved.

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

#ifndef PFS_EVENTS_TRANSACTIONS_H
#define PFS_EVENTS_TRANSACTIONS_H

/**
  @file storage/perfschema/pfs_events_transactions.h
  Events transactions data structures (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_events.h"
#include "rpl_gtid.h"

struct PFS_thread;
struct PFS_account;
struct PFS_user;
struct PFS_host;

#define PSI_XIDDATASIZE 128
/**
  struct PSI_xid is binary compatible with the XID structure as
  in the X/Open CAE Specification, Distributed Transaction Processing:
  The XA Specification, X/Open Company Ltd., 1991.
  http://www.opengroup.org/bookstore/catalog/c193.htm

  @see XID in sql/handler.h
  @see MYSQL_XID in mysql/plugin.h
*/
struct PSI_xid
{
  long formatID;
  long gtrid_length;
  long bqual_length;
  char data[PSI_XIDDATASIZE];  /* Not \0-terminated */

  PSI_xid() {null();}
  bool is_null() { return formatID == -1; }
  void null() { formatID= -1; gtrid_length= 0; bqual_length= 0;}
};
typedef struct PSI_xid PSI_xid;

/** A transaction record. */
struct PFS_events_transactions : public PFS_events
{
  /** Source identifier, mapped from internal format. */
  rpl_sid m_sid;
  /** InnoDB transaction ID. */
  ulonglong m_trxid;
  /** Status */
  enum_transaction_state m_state;
  /** Global Transaction ID specifier. */
  Gtid_specification m_gtid_spec;
  /** True if XA transaction. */
  my_bool m_xa;
  /** XA transaction ID. */
  PSI_xid m_xid;
  /** XA status */
  enum_xa_transaction_state m_xa_state;
  /** Transaction isolation level. */
  enum_isolation_level m_isolation_level;
  /** True if read-only transaction, otherwise read-write. */
  my_bool m_read_only;
  /** True if autocommit transaction. */
  my_bool m_autocommit;
  /** Total number of savepoints. */
  ulonglong m_savepoint_count;
  /** Number of rollback_to_savepoint. */
  ulonglong m_rollback_to_savepoint_count;
  /** Number of release_savepoint. */
  ulonglong m_release_savepoint_count;
};

bool xid_printable(PSI_xid *xid);

void insert_events_transactions_history(PFS_thread *thread, PFS_events_transactions *transaction);
void insert_events_transactions_history_long(PFS_events_transactions *transaction);

extern bool flag_events_transactions_current;
extern bool flag_events_transactions_history;
extern bool flag_events_transactions_history_long;

extern bool events_transactions_history_long_full;
extern PFS_cacheline_uint32 events_transactions_history_long_index;
extern PFS_events_transactions *events_transactions_history_long_array;
extern ulong events_transactions_history_long_size;

int init_events_transactions_history_long(uint events_transactions_history_long_sizing);
void cleanup_events_transactions_history_long();

void reset_events_transactions_current();
void reset_events_transactions_history();
void reset_events_transactions_history_long();
void reset_events_transactions_by_thread();
void reset_events_transactions_by_account();
void reset_events_transactions_by_user();
void reset_events_transactions_by_host();
void reset_events_transactions_global();
void aggregate_account_transactions(PFS_account *account);
void aggregate_user_transactions(PFS_user *user);
void aggregate_host_transactions(PFS_host *host);

#endif

