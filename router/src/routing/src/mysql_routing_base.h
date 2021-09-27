/*
  Copyright (c) 2021, Oracle and/or its affiliates.

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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef ROUTING_MYSQL_ROUTING_BASE_INCLUDED
#define ROUTING_MYSQL_ROUTING_BASE_INCLUDED

#include "context.h"
#include "destination.h"

class MySQLRoutingBase;

/**
 * Interface for communicating with common quarantined destinations instance.
 */
struct DestinationQuarantineHandlerInterface {
  using SharedQuarantineUpdateCallback =
      std::function<void(mysql_harness::TCPAddress)>;
  using SharedQuarantineQueryCallback =
      std::function<bool(mysql_harness::TCPAddress)>;
  using SharedQuarantineClearCallback = std::function<void()>;
  using SharedQuarantineRefreshCallback =
      std::function<void(MySQLRoutingBase *, const bool, const AllowedNodes &)>;

  virtual ~DestinationQuarantineHandlerInterface() = default;

  virtual void register_shared_quarantine_update_callback(
      SharedQuarantineUpdateCallback clb) = 0;
  virtual void register_shared_quarantine_query_callback(
      SharedQuarantineQueryCallback clb) = 0;
  virtual void register_shared_quarantine_clear_callback(
      SharedQuarantineClearCallback clb) = 0;
  virtual void register_shared_quarantine_md_nodes_refresh_callback(
      SharedQuarantineRefreshCallback clb) = 0;
  virtual void unregister_shared_quarantine_callbacks() = 0;
};

/** @class MySQLRoutingBase
 *  @brief Facade to avoid a tight coupling between Routing component and
 * actuall routing endpoint implementation. Allows replacing the routing
 * endpoint with an alternative implementation.
 */
class MySQLRoutingBase : public DestinationQuarantineHandlerInterface {
 public:
  virtual MySQLRoutingContext &get_context() = 0;
  virtual int get_max_connections() const noexcept = 0;
  virtual std::vector<mysql_harness::TCPAddress> get_destinations() const = 0;
  virtual std::vector<MySQLRoutingAPI::ConnData> get_connections() = 0;
  virtual bool is_accepting_connections() const = 0;
  virtual routing::RoutingStrategy get_routing_strategy() const = 0;
  virtual routing::AccessMode get_mode() const = 0;
  virtual stdx::expected<void, std::error_code> start_accepting_connections(
      const mysql_harness::PluginFuncEnv *env) = 0;
  virtual void stop_socket_acceptors(
      const mysql_harness::PluginFuncEnv *env) = 0;
  virtual ~MySQLRoutingBase() {}
};

#endif  // ROUTING_MYSQL_ROUTING_BASE_INCLUDED
