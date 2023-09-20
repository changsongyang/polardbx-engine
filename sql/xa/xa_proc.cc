/* Copyright (c) 2018, 2023, Alibaba and/or its affiliates. All rights reserved.

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

#include "storage/innobase/include/lizard0xa0iface.h"
#include "storage/innobase/include/lizard0ut.h"
#include "sql/xa/xa_proc.h"
#include "sql/xa/xa_trx.h"
#include "sql/binlog_ext.h"

namespace lizard {
namespace xa {
const char *Transaction_csr_str[] = {"AUTOMATIC_GCN", "ASSIGNED_GCN", "NONE"};

/** trans gcn source to message string. */
static const char *trx_slot_trx_gcn_csr_to_str(const enum my_csr_t my_csr) {
  if (my_csr == MYSQL_CSR_NONE) {
    return Transaction_csr_str[2];
  } else {
    return Transaction_csr_str[my_csr];
  }
}

}  // namespace xa
}  // namespace lizard

namespace im {

static const char *xa_status_str[] = {
  "ATTACHED",
  "DETACHED",
  "COMMIT",
  "ROLLBACK",
  "NOTSTART_OR_FORGET",
  "NOT_SUPPORT",
};

enum XA_status {
  /** Another seesion has attached XA. */
  ATTACHED,
  /** Detached XA, the real state of the XA can't be ACTIVE. */
  DETACHED,
  /** The XA has been erased from transaction cache, and also has been
  committed. */
  COMMIT,
  /** The XA has been erased from transaction cache, and also has been
  rollbacked. */
  ROLLBACK,
  /** Can't find such a XA in transaction cache and in transaction slots, it
  might never exist or has been forgotten. */
  NOTSTART_OR_FORGET,
  /** Found the XA in transaction slots, but the real state (commit/rollback)
  of the transaction can't be confirmed (using the old TXN format.). */
  NOT_SUPPORT,
};

/* All concurrency control system memory usage */
PSI_memory_key key_memory_xa_proc;

/* The uniform schema name for xa */
LEX_CSTRING XA_PROC_SCHEMA = {C_STRING_WITH_LEN("dbms_xa")};

/* Singleton instance for find_by_xid */
Proc *Xa_proc_find_by_xid::instance() {
  static Proc *proc = new Xa_proc_find_by_xid(key_memory_xa_proc);
  return proc;
}

Sql_cmd *Xa_proc_find_by_xid::evoke_cmd(THD *thd, List<Item> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

/**
  Parse the GTRID from the parameter list

  @param[in]  list    parameter list
  @param[out] gtrid   GTRID
  @param[out] length  length of gtrid

  @retval     true if parsing error.
*/
bool get_gtrid(const List<Item> *list, char *gtrid, unsigned &length) {
  char buff[128];

  String str(buff, sizeof(buff), system_charset_info);
  String *res;

  /* gtrid */
  res = (*list)[0]->val_str(&str);
  length = res->length();
  if (length > MAXGTRIDSIZE) {
    return true;
  }
  memcpy(gtrid, res->ptr(), length);

  return false;
}

/**
  Parse the XID from the parameter list

  @param[in]  list  parameter list
  @param[out] xid   XID

  @retval     true if parsing error.
*/
bool get_xid(const List<Item> *list, XID *xid) {
  char buff[256];
  char gtrid[MAXGTRIDSIZE];
  char bqual[MAXBQUALSIZE];
  size_t gtrid_length;
  size_t bqual_length;
  size_t formatID;

  String str(buff, sizeof(buff), system_charset_info);
  String *res;

  /* gtrid */
  res = (*list)[0]->val_str(&str);
  gtrid_length = res->length();
  if (gtrid_length > MAXGTRIDSIZE) {
    return true;
  }
  memcpy(gtrid, res->ptr(), gtrid_length);

  /* bqual */
  res = (*list)[1]->val_str(&str);
  bqual_length = res->length();
  if (bqual_length > MAXBQUALSIZE) {
    return true;
  }
  memcpy(bqual, res->ptr(), bqual_length);

  /* formatID */
  formatID = (*list)[2]->val_int();

  /** Set XID. */
  xid->set(formatID, gtrid, gtrid_length, bqual, bqual_length, MyGCN_NULL);

  return false;
}

bool Sql_cmd_xa_proc_find_by_xid::pc_execute(THD *) {
  DBUG_ENTER("Sql_cmd_xa_proc_find_by_xid::pc_execute");
  DBUG_RETURN(false);
}

void Sql_cmd_xa_proc_find_by_xid::send_result(THD *thd, bool error) {
  DBUG_ENTER("Sql_cmd_xa_proc_find_by_xid::send_result");
  Protocol *protocol;
  XID xid;
  XID_STATE *xs;
  XA_status xa_status;
  lizard::xa::Transaction_info info;
  bool found;
  my_gcn_t gcn;
  enum my_csr_t csr;

  protocol = thd->get_protocol();

  if (error) {
    DBUG_ASSERT(thd->is_error());
    DBUG_VOID_RETURN;
  }

  if (get_xid(m_list, &xid)) {
    my_error(ER_XA_PROC_WRONG_XID, MYF(0), MAXGTRIDSIZE, MAXBQUALSIZE);
    DBUG_VOID_RETURN;
  }

  std::shared_ptr<Transaction_ctx> transaction = transaction_cache_search(&xid);
  if (transaction) {
    /** Case 1: DETACHED or ATTACHED. */
    xs = transaction->xid_state();

    xa_status =
        xs->is_in_recovery() ? XA_status::DETACHED : XA_status::ATTACHED;

    gcn = MYSQL_GCN_NULL;

    csr = MYSQL_CSR_NONE;
  } else {
    found = lizard::xa::trx_slot_get_trx_info_by_xid(&xid, &info);
    if (found) {
      /** Case 2: Finish state. */
      switch (info.state) {
        case lizard::xa::TRANS_STATE_COMMITTED:
          xa_status = XA_status::COMMIT;
          break;
        case lizard::xa::TRANS_STATE_ROLLBACK:
          xa_status = XA_status::ROLLBACK;
          break;
        case lizard::xa::TRANS_STATE_UNKNOWN:
          xa_status = XA_status::NOT_SUPPORT;
          break;
      };

      gcn = info.my_gcn.get_gcn();

      csr = info.my_gcn.get_csr();
    } else {
      /** Case 3: Not ever start or already forget. */
      xa_status = XA_status::NOTSTART_OR_FORGET;

      gcn = MYSQL_GCN_NULL;

      csr = MYSQL_CSR_NONE;
    }
  }

  if (m_proc->send_result_metadata(thd)) DBUG_VOID_RETURN;

  protocol->start_row();

  protocol->store_string(xa_status_str[xa_status],
                         strlen(xa_status_str[xa_status]), system_charset_info);

  protocol->store((ulonglong)gcn);

  const char *csr_msg = lizard::xa::trx_slot_trx_gcn_csr_to_str(csr);
  protocol->store_string(csr_msg, strlen(csr_msg), system_charset_info);

  if (protocol->end_row()) DBUG_VOID_RETURN;

  my_eof(thd);
  DBUG_VOID_RETURN;
}

Proc *Xa_proc_prepare_with_trx_slot::instance() {
  static Proc *proc = new Xa_proc_prepare_with_trx_slot(key_memory_xa_proc);
  return proc;
}

Sql_cmd *Xa_proc_prepare_with_trx_slot::evoke_cmd(THD *thd, List<Item> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

bool Sql_cmd_xa_proc_prepare_with_trx_slot::pc_execute(THD *thd) {
  DBUG_ENTER("Sql_cmd_xa_proc_prepare_with_trx_slot");

  XID xid;
  XID_STATE *xid_state = thd->get_transaction()->xid_state();

  /** 1. parsed XID from parameters list. */
  if (get_xid(m_list, &xid)) {
    my_error(ER_XA_PROC_WRONG_XID, MYF(0), MAXGTRIDSIZE, MAXBQUALSIZE);
    DBUG_RETURN(true);
  }

  /** 2. Check whether it is an xa transaction that has completed "XA END" */
  if (!xid_state->has_state(XID_STATE::XA_IDLE)) {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    DBUG_RETURN(true);
  } else if (!xid_state->has_same_xid(&xid)) {
    my_error(ER_XAER_NOTA, MYF(0));
    DBUG_RETURN(true);
  }

  /** 3. Assign transaction slot. */
  if (lizard::xa::transaction_slot_assign(thd, &xid, &m_tsa)) {
    DBUG_RETURN(true);
  }

  /** 4. Do xa prepare. */
  Sql_cmd_xa_prepare *executor = new (thd->mem_root) Sql_cmd_xa_prepare(&xid);
  executor->set_delay_ok();
  if (executor->execute(thd)) {
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}

void Sql_cmd_xa_proc_prepare_with_trx_slot::send_result(THD *thd, bool error) {
  DBUG_ENTER("Sql_cmd_xa_proc_prepare_with_trx_slot::send_result");
  Protocol *protocol;

  if (error) {
    DBUG_ASSERT(thd->is_error());
    DBUG_VOID_RETURN;
  }

  protocol = thd->get_protocol();

  if (m_proc->send_result_metadata(thd)) DBUG_VOID_RETURN;

  protocol->start_row();
  DBUG_ASSERT(strlen(server_uuid_ptr) <= 256);
  protocol->store_string(server_uuid_ptr, strlen(server_uuid_ptr),
                         system_charset_info);
  protocol->store((ulonglong)m_tsa);
  if (protocol->end_row()) DBUG_VOID_RETURN;

  my_eof(thd);
  DBUG_VOID_RETURN;
}

bool Sql_cmd_xa_proc_send_heartbeat::pc_execute(THD *) {
  DBUG_ENTER("Sql_cmd_xa_proc_send_heartbeat::pc_execute");
  lizard::xa::hb_freezer_heartbeat();

  DBUG_RETURN(false);
}

Proc *Xa_proc_send_heartbeat::instance() {
  static Proc *proc = new Xa_proc_send_heartbeat(key_memory_xa_proc);
  return proc;
}

Sql_cmd *Xa_proc_send_heartbeat::evoke_cmd(THD *thd, List<Item> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

bool cn_heartbeat_timeout_freeze_updating(LEX *const lex) {
  DBUG_EXECUTE_IF("hb_timeout_do_not_freeze_operation", { return false; });
  switch (lex->sql_command) {
    case SQLCOM_ADMIN_PROC:
      break;

    default:
      if ((sql_command_flags[lex->sql_command] & CF_CHANGES_DATA) &&
          lizard::xa::hb_freezer_is_freeze() && likely(mysqld_server_started)) {
        my_error(ER_XA_PROC_HEARTBEAT_FREEZE, MYF(0));
        return true;
      }
  }

  return false;
}

bool cn_heartbeat_timeout_freeze_applying_event(THD *thd) {
  static lizard::Lazy_printer printer(60);

  if (lizard::xa::hb_freezer_is_freeze()) {
    THD_STAGE_INFO(thd, stage_wait_for_cn_heartbeat);

    printer.print(
        "Applying event is blocked because no heartbeat has been received "
        "for a long time. If you want to advance it, please call "
        "dbms_xa.send_heartbeat() (or set global innodb_cn_no_heartbeat_freeze "
        "= 0).");

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    return true;
  } else {
    printer.reset();
    return false;
  }
}

/**
  Parse the GCN from the parameter list

  @param[in]  list    parameter list
  @param[out] gcn     GCN

  @retval     true if parsing error.
*/
bool parse_gcn_from_parameter_list(const List<Item> *list, my_gcn_t *gcn) {
  /* GCN */
  *gcn = (*list)[0]->val_uint();

  return false;
}

bool Sql_cmd_xa_proc_advance_gcn_no_flush::pc_execute(THD *) {
  DBUG_ENTER("Sql_cmd_xa_proc_advance_gcn_no_flush::pc_execute");
  my_gcn_t gcn;
  if (parse_gcn_from_parameter_list(m_list, &gcn)) {
    /** Not possible. */
    DBUG_ABORT();
    DBUG_RETURN(true);
  }

  lizard::gcs_set_gcn_if_bigger(gcn);
  DBUG_RETURN(false);
}

Proc *Xa_proc_advance_gcn_no_flush::instance() {
  static Proc *proc = new Xa_proc_advance_gcn_no_flush(key_memory_xa_proc);
  return proc;
}

Sql_cmd *Xa_proc_advance_gcn_no_flush::evoke_cmd(THD *thd,
                                                 List<Item> *list) const {
  return new (thd->mem_root) Sql_cmd_type(thd, list, this);
}

}
