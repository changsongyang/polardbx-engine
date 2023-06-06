/*****************************************************************************

Copyright (c) 2013, 2020, Alibaba and/or its affiliates. All Rights Reserved.

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
#include <memory>

#include "libbinlogevents/include/binlog_event.h"
#include "my_sys.h"
#include "raft0err.h"
#include "raft0recovery.h"
#include "sql/binlog/recovery.h"
#include "sql/binlog/tools/iterators.h"
#include "sql/consensus_log_manager.h"
#include "sql/consensus_recovery_manager.h"
#include "sql/log_event.h"
#include "sql/rpl_gtid.h"
#include "sql/xa/xid_extract.h"

namespace raft {

static uchar *remove_const(std::string &str) {
  return (uchar *)const_cast<char *>(str.c_str());
}

static int fetch_binlog_by_offset(Binlog_file_reader &binlog_file_reader,
                                  uint64 start_pos, uint64 end_pos,
                                  Consensus_cluster_info_log_event *rci_ev,
                                  std::string &log_content) {
  if (start_pos == end_pos) {
    log_content.assign("");
    return 0;
  }
  if (rci_ev == nullptr) {
    unsigned int buf_size = end_pos - start_pos;
    auto *buffer =
        (uchar *)my_malloc(key_memory_thd_main_mem_root, buf_size, MYF(MY_WME));
    binlog_file_reader.seek(start_pos);
    my_b_read(binlog_file_reader.get_io_cache(), buffer, buf_size);
    log_content.assign((char *)buffer, buf_size);
    my_free(buffer);
  } else {
    log_content.assign(rci_ev->get_info(), (size_t)rci_ev->get_info_length());
  }
  return 0;
}

bool Recovery_manager::is_raft_instance_recovering() const {
  return !opt_initialize && is_raft_instance();
}

std::unique_ptr<binlog::Binlog_recovery> Recovery_manager::create_recovery(
    Binlog_file_reader &binlog_file_reader) {
  binlog::Binlog_recovery *recovery = nullptr;
  if (is_raft_instance()) {
    recovery = new Consensus_binlog_recovery(binlog_file_reader);
  } else {
    recovery = new binlog::Binlog_recovery(binlog_file_reader);
  }
  return std::unique_ptr<binlog::Binlog_recovery>(recovery);
}

binlog::Binlog_recovery &Consensus_binlog_recovery::recover() {
  binlog::tools::Iterator it{&m_reader};
  it.set_copy_event_buffer();
  m_valid_pos = m_reader.position();

  for (Log_event *ev = it.begin(); ev != it.end(); ev = it.next()) {
    if (ev->get_type_code() == binary_log::CONSENSUS_LOG_EVENT) {
      process_consensus_event(dynamic_cast<Consensus_log_event &>(*ev));
    } else if (ev->get_type_code() ==
               binary_log::PREVIOUS_CONSENSUS_INDEX_LOG_EVENT) {
      process_previous_consensus_index_event(
          dynamic_cast<Previous_consensus_index_log_event &>(*ev));
    } else if (!ev->is_control_event()) {
      m_end_pos = my_b_tell(m_reader.get_io_cache());

      if (ev->get_type_code() == binary_log::CONSENSUS_CLUSTER_INFO_EVENT) {
        m_rci_ev = dynamic_cast<Consensus_cluster_info_log_event *>(ev);
      } else if (ev->get_type_code() == binary_log::QUERY_EVENT) {
        process_query_event(dynamic_cast<Query_log_event &>(*ev));
      } else if (ev->get_type_code() == binary_log::GCN_LOG_EVENT) {
        // todo process gcn event
      } else if (ev->get_type_code() == binary_log::XA_PREPARE_LOG_EVENT) {
        process_xa_prepare_event(dynamic_cast<XA_prepare_log_event &>(*ev));
      } else if (ev->get_type_code() == binary_log::XID_EVENT) {
        assert(m_in_transaction);
        process_xid_event(dynamic_cast<Xid_log_event &>(*ev));
      } else if (ev->get_type_code() == binary_log::GTID_LOG_EVENT) {
        process_gtid_event(dynamic_cast<Gtid_log_event &>(*ev));
      }

      // todo handle gcn event
      if (!m_in_transaction && !is_gtid_event(ev) /*&& !is_gcn_event(ev)*/) {
        // todo store the start apply pos of recovery for apply
        m_gtid.clear();
      }

      // find a integrated consensus log
      if (m_begin_consensus && m_end_pos > m_start_pos &&
          m_end_pos - m_start_pos == m_current_length) {
        if (m_current_flag & Consensus_log_event_flag::FLAG_BLOB) {
          m_blob_index_list.emplace_back(m_current_index);
          m_blob_term_list.emplace_back(m_current_term);
          m_blob_flag_list.emplace_back(m_current_flag);
          m_blob_crc32_list.emplace_back(m_current_crc32);
        } else if (m_current_flag & Consensus_log_event_flag::FLAG_BLOB_END) {
          m_blob_index_list.push_back(m_current_index);
          m_blob_term_list.push_back(m_current_term);
          m_blob_flag_list.push_back(m_current_flag);
          m_blob_crc32_list.push_back(m_current_crc32);
          uint64 split_len = opt_consensus_large_event_split_size;
          uint64 blob_start_pos = m_start_pos,
                 blob_end_pos = m_start_pos + split_len;
          uint64 save_position = m_reader.position();

          for (size_t i = 0; i < m_blob_index_list.size(); ++i) {
            fetch_binlog_by_offset(m_reader, blob_start_pos, blob_end_pos,
                                   m_rci_ev, m_log_content);
            consensus_log_manager.get_fifo_cache_manager()->add_log_to_cache(
                m_blob_term_list[i], m_blob_index_list[i], m_log_content.size(),
                remove_const(m_log_content), (m_rci_ev != nullptr),
                m_blob_flag_list[i], m_blob_crc32_list[i]);
            blob_start_pos = blob_end_pos;
            blob_end_pos = blob_end_pos + split_len > m_end_pos
                               ? m_end_pos
                               : blob_end_pos + split_len;
          }
          m_blob_index_list.clear();
          m_blob_term_list.clear();
          m_blob_flag_list.clear();
          m_blob_crc32_list.clear();
          m_begin_consensus = false;
          m_valid_index = m_current_index;
          /*
            fetch_binlog_by_offset will modify the position
            of binlog_file_reader.
          */
          m_reader.seek(save_position);
        } else {
          uint64 save_position = m_reader.position();

          // copy log to buffer
          fetch_binlog_by_offset(m_reader, m_start_pos, m_end_pos, m_rci_ev,
                                 m_log_content);
          consensus_log_manager.get_fifo_cache_manager()->add_log_to_cache(
              m_current_term, m_current_index, m_log_content.size(),
              remove_const(m_log_content), (m_rci_ev != nullptr),
              m_current_flag, m_current_crc32);
          m_begin_consensus = false;
          m_valid_index = m_current_index;

          /*
            fetch_binlog_by_offset will modify the position
            of binlog_file_reader.
          */
          m_reader.seek(save_position);
        }
        m_rci_ev = nullptr;
      }
    }

    if (!m_reader.has_fatal_error() && !m_begin_consensus &&
        !is_gtid_event(ev) && !is_gcn_event(ev)) {
      m_valid_pos = my_b_tell(m_reader.get_io_cache());
    }

    delete ev;
    ev = nullptr;
  }

  if (m_start_pos < m_valid_pos && m_end_pos > m_valid_pos) {
    m_end_pos = m_valid_pos;
  }

  // recover current/sync index
  //
  // if the last log is not integrated
  if (m_begin_consensus) {
    raft::warn(ER_RAFT_RECOVERY) << "last consensus log is not integrated, "
                                 << "sync index should set to " << m_valid_index
                                 << " instead of " << m_current_index;
  }
  consensus_log_manager.set_cache_index(m_valid_index);
  consensus_log_manager.set_sync_index(m_valid_index);
  consensus_log_manager.set_current_index(
      consensus_log_manager.get_sync_index() + 1);
  consensus_log_manager.set_enable_rotate(!(m_current_flag & FLAG_LARGE_TRX));

  if (!this->m_is_malformed && total_ha_2pc > 1) {
    Xa_state_list xa_list{this->m_external_xids};
    this->m_no_engine_recovery = ha_recover(&this->m_internal_xids, &xa_list);
    if (this->m_no_engine_recovery) {
      this->m_failure_message.assign("Recovery failed in storage engines");
    }
  }
  return *this;
}

void Consensus_binlog_recovery::process_consensus_event(
    const Consensus_log_event &ev) {
  if (m_current_index > ev.get_index()) {
    raft::error(ER_RAFT_RECOVERY) << "consensus log index out of order";
    exit(-1);
  } else if (m_end_pos < m_start_pos) {
    raft::error(ER_RAFT_RECOVERY) << "consensus log structure broken";
    exit(-1);
  }

  m_current_index = ev.get_index();
  m_current_term = ev.get_term();
  m_current_length = ev.get_length();
  m_current_flag = ev.get_flag();
  m_current_crc32 = ev.get_reserve();
  m_end_pos = m_start_pos = my_b_tell(m_reader.get_io_cache());
  m_begin_consensus = true;

  m_ev_start_pos = m_reader.event_start_pos();
}

void Consensus_binlog_recovery::process_previous_consensus_index_event(
    const Previous_consensus_index_log_event &ev) {
  m_current_index = ev.get_index() - 1;
  m_valid_index = m_current_index;
}

void Consensus_binlog_recovery::process_gtid_event(Gtid_log_event &) {
  // m_gtid.set(ev.get_sidno(false), ev.get_gno());
}

void Consensus_binlog_recovery::process_internal_xid(ulong unmasked_server_id,
                                                     my_xid xid) {
  if (unmasked_server_id == server_id) {
    if (m_recover_term == 0 || m_current_term > m_recover_term) {
      consensus_log_manager.get_recovery_manager()->clear_trx_in_binlog();
      m_internal_xids.clear();
      m_external_xids.clear();
      m_recover_term = m_current_term;
    }
    consensus_log_manager.get_recovery_manager()->add_trx_in_binlog(
        m_current_index, xid);
  } else {
    // should not add xid generated by other server
    // trx started by follower can rollback safely
    m_internal_xids.erase(xid);
  }
}

void Consensus_binlog_recovery::process_external_xid(ulong unmasked_server_id,
                                                     const XID &xid) {
  if (unmasked_server_id == server_id) {
    if (m_recover_term == 0 || m_current_term > m_recover_term) {
      consensus_log_manager.get_recovery_manager()->clear_trx_in_binlog();
      m_internal_xids.clear();
      m_external_xids.clear();
      m_recover_term = m_current_term;
    }
    consensus_log_manager.get_recovery_manager()->add_trx_in_binlog(
        m_current_index, xid);
  } else {
    // should not add xid generated by other server
    // trx started by follower can rollback safely
    m_external_xids.erase(xid);
  }
}

std::unique_ptr<binlog::Binlog_recovery::Process_hook<std::string>>
Consensus_binlog_recovery::create_process_xa_commit_hook(
    std::string const &query) {
  auto after = [&](std::string const &arg) {
    assert(m_query_ev != nullptr);
    xa::XID_extractor tokenizer{arg, 1};
    process_external_xid(m_query_ev->common_header->unmasked_server_id,
                         tokenizer[0]);
  };
  return std::make_unique<
      binlog::Binlog_recovery::Process_hook<std::string>>(query, nullptr,
                                                              after);
}

std::unique_ptr<binlog::Binlog_recovery::Process_hook<Query_log_event>>
Consensus_binlog_recovery::create_process_query_event_hook(
    Query_log_event const &ev) {
  auto before = [&](Query_log_event const &arg) { m_query_ev = &arg; };

  auto after = [&](Query_log_event const &) { m_query_ev = nullptr; };

  return std::make_unique<
      binlog::Binlog_recovery::Process_hook<Query_log_event>>(ev, before,
                                                                  after);
}

std::unique_ptr<binlog::Binlog_recovery::Process_hook<std::string>>
Consensus_binlog_recovery::create_process_xa_rollback_hook(
    std::string const &query) {
  auto after = [&](std::string const &arg) {
    assert(m_query_ev != nullptr);
    xa::XID_extractor tokenizer{arg, 1};
    process_external_xid(m_query_ev->common_header->unmasked_server_id,
                         tokenizer[0]);
  };
  return std::make_unique<
      binlog::Binlog_recovery::Process_hook<std::string>>(query, nullptr,
                                                              after);
}

std::unique_ptr<binlog::Binlog_recovery::Process_hook<Xid_log_event>>
Consensus_binlog_recovery::create_process_xid_event_hook(
    Xid_log_event const &ev) {
  auto after = [&](Xid_log_event const &arg) {
    process_internal_xid(arg.common_header->unmasked_server_id, arg.xid);
  };
  return std::make_unique<Process_hook<Xid_log_event>>(ev, nullptr, after);
}

std::unique_ptr<binlog::Binlog_recovery::Process_hook<XA_prepare_log_event>>
Consensus_binlog_recovery::create_process_xa_prepare_event_hook(
    XA_prepare_log_event const &ev) {
  auto after = [&](XA_prepare_log_event const &arg) {
    XID xid;
    xid = arg.get_xid();
    process_external_xid(arg.common_header->unmasked_server_id, xid);
  };
  return std::make_unique<Process_hook<XA_prepare_log_event>>(ev, nullptr,
                                                                  after);
}

}  // namespace raft
