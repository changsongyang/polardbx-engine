/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.0,
 * as published by the Free Software Foundation.
 *
 * This program is also distributed with certain software (including
 * but not limited to OpenSSL) that is licensed under separate terms,
 * as designated in a particular file or component or in included license
 * documentation.  The authors of MySQL hereby grant you an additional
 * permission to link the program and your derivative works with the
 * separately licensed software that they have included with MySQL.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License, version 2.0, for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
 */

// MySQL DB access module, for use by plugins and others
// For the module that implements interactive DB functionality see mod_db

#ifndef X_CLIENT_XSESSION_IMPL_H_
#define X_CLIENT_XSESSION_IMPL_H_

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "plugin/x/client/mysqlxclient/xargument.h"
#include "plugin/x/client/mysqlxclient/xsession.h"
#include "plugin/x/client/xcontext.h"


namespace xcl {

class Result;
class Protocol_impl;
class Protocol_factory;

class Session_impl : public XSession {
 public:
  enum class Auth {
    Auto,
    Mysql41,
    Plain,
    Sha256_memory
  };

 public:
  explicit Session_impl(std::unique_ptr<Protocol_factory> factory = {});
  ~Session_impl() override;

  XProtocol::Client_id client_id() const override { return m_context->m_client_id; }
  XProtocol &get_protocol() override;

  XError set_mysql_option(const Mysqlx_option option,
                          const bool value) override;
  XError set_mysql_option(const Mysqlx_option option,
                          const std::string &value) override;
  XError set_mysql_option(const Mysqlx_option option,
                          const std::vector<std::string> &values_list) override;
  XError set_mysql_option(const Mysqlx_option option,
                          const char *value) override;
  XError set_mysql_option(const Mysqlx_option option,
                          const int64_t value) override;

  XError set_capability(const Mysqlx_capability capability,
                        const bool value) override;
  XError set_capability(const Mysqlx_capability capability,
                        const std::string &value) override;
  XError set_capability(const Mysqlx_capability capability,
                        const char *value) override;
  XError set_capability(const Mysqlx_capability capability,
                        const int64_t value) override;

  XError connect(const char *host,
                 const uint16_t port,
                 const char *user,
                 const char *pass,
                 const char *schema) override;

  XError connect(const char *socket_file,
                 const char *user,
                 const char *pass,
                 const char *schema) override;

  XError reauthenticate(const char *user,
                        const char *pass,
                        const char *schema) override;

  std::unique_ptr<XQuery_result> execute_sql(
      const std::string &sql,
      XError *out_error) override;

  std::unique_ptr<XQuery_result> execute_stmt(
      const std::string &ns,
      const std::string &sql,
      const Arguments &args,
      XError *out_error) override;

  void close() override;

 private:
  using Context_ptr          = std::shared_ptr<Context>;
  using Protocol_factory_ptr = std::unique_ptr<Protocol_factory>;
  using XProtocol_ptr        = std::shared_ptr<XProtocol>;


  void   setup_protocol();
  void   setup_session_notices_handler();
  void   setup_general_notices_handler();
  XError setup_authentication_methods_from_text(
      const std::vector<std::string> &value_list);
  XError setup_ssl_mode_from_text(const std::string &value);
  XError setup_ip_mode_from_text(const std::string &value);

  static std::string get_method_from_auth(const Auth auth);

  bool is_connected();
  XError authenticate(const char *user,
                      const char *pass,
                      const char *schema,
                      Connection_type connection_type);
  static Handler_result handle_notices(
        std::shared_ptr<Context> context,
        const Mysqlx::Notice::Frame::Type,
        const char *,
        const uint32_t);

  std::pair<XError, std::vector<std::string>> validate_and_adjust_auth_methods(
      std::vector<Auth> auth_methods, const bool can_use_plain);

  Object                m_capabilities;
  XProtocol_ptr         m_protocol;
  Context_ptr           m_context;
  Protocol_factory_ptr  m_factory;
  Internet_protocol     m_internet_protocol { Internet_protocol::Any };
  std::vector<Auth>     m_auth_methods;
  bool                  m_compatibility_mode = false;
};

}  // namespace xcl

#endif  // X_CLIENT_XSESSION_IMPL_H_
