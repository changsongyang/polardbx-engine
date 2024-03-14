//
// Created by zzy on 2022/7/25.
//

#pragma once

#include <atomic>
#include <memory>
#include <queue>
#include <string>

#include "mysql/service_command.h"

#include "../coders/polarx_encoder.h"
#include "../common_define.h"
#include "../polarx_rpc.h"
#include "../server/server_variables.h"
#include "../sql_query/query_string_builder.h"
#include "../utility/atomicex.h"
#include "../utility/error.h"
#include "../utility/perf.h"
#include "../utility/time.h"

#include "pkt.h"
#include "session_base.h"

namespace polarx_rpc {

class CtcpConnection;
class CcommandDelegate;
class CmtEpoll;

extern std::atomic<int> g_session_count;

class Csession final : public CsessionBase {
  NO_COPY_MOVE(Csession)

 private:
  /// epoll info(always valid)
  CmtEpoll &epoll_;

  /// tcp info(be careful, and never touch it when shutdown is true)
  /// tcp may invalid after shutdown(and after timeout)
  CtcpConnection &tcp_;
  std::atomic<bool> shutdown_;

  /// global working lock
  CspinLock working_;

  /// queued messages
  CmcsSpinLock queue_lock_;
  std::queue<msg_t> message_queue_{};
  int64_t schedule_time_{0};

  /// shared encoder
  CpolarxEncoder encoder_;

  /// query builder
  Query_string_builder qb_{1024};

  /// cascade error
  err_t last_error_;

 public:
  explicit Csession(CmtEpoll &epoll, CtcpConnection &tcp, const uint64_t &sid)
      : CsessionBase(sid),
        epoll_(epoll),
        tcp_(tcp),
        shutdown_(false),
        encoder_(sid) {
    g_session_count.fetch_add(1, std::memory_order_acq_rel);
  }

  ~Csession() final;

  void wait_begin(THD *thd, int wait_type);
  void wait_end(THD *thd);

  inline void shutdown(bool log) {
    shutdown_.store(true, std::memory_order_release);
    remote_kill(log);
  }

  bool flush();

  void run();

  void dispatch(msg_t &&msg, bool &run);

  err_t sql_stmt_execute(const PolarXRPC::Sql::StmtExecute &msg);
  err_t sql_plan_execute(const PolarXRPC::ExecPlan::ExecPlan &msg);

  err_t table_space_file_info(
      const PolarXRPC::PhysicalBackfill::GetFileInfoOperator &msg);
  err_t table_space_file_transfer(
      const PolarXRPC::PhysicalBackfill::TransferFileDataOperator &msg);
  err_t table_space_file_manage(
      const PolarXRPC::PhysicalBackfill::FileManageOperator &msg, bool &run);

  inline bool push_message(msg_t &&msg, bool &need_notify) {
    CautoMcsSpinLock lck(queue_lock_, mcs_spin_cnt);
    if (message_queue_.empty()) {
      need_notify = true;
      if (enable_perf_hist) schedule_time_ = Ctime::steady_ns();
    } else if (message_queue_.size() >= max_queued_messages)
      return false;
    message_queue_.emplace(std::forward<msg_t>(msg));
    return true;
  }
};

}  // namespace polarx_rpc
