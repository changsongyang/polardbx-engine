/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

// MySQL DB access module, for use by plugins and others
// For the module that implements interactive DB functionality see mod_db

#ifndef X_CLIENT_XQUERY_RESULT_IMPL_H_
#define X_CLIENT_XQUERY_RESULT_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include "mysqlxclient/xprotocol.h"
#include "mysqlxclient/xquery_result.h"
#include "xcontext.h"
#include "xquery_instances.h"
#include "xrow_impl.h"


namespace xcl {

class Query_result: public XQuery_result {
 public:
  using Row_ptr = std::unique_ptr<Mysqlx::Resultset::Row>;

 public:
  explicit Query_result(std::shared_ptr<XProtocol> protocol,
                        Query_instances *query_instances,
                        std::shared_ptr<Context> context);
  ~Query_result() override;

  bool try_get_last_insert_id(uint64_t *out_last_id) const override;
  bool try_get_affected_rows(uint64_t *out_affected_number) const override;
  bool try_get_info_message(std::string *out_message) const override;

  const Metadata &get_metadata(XError *out_error) override;
  const Warnings &get_warnings() override;

  bool next_resultset(XError *out_error) override;

  Row_ptr get_next_row_raw(XError *out_error) override;
  bool    get_next_row(const XRow **out_row,
                       XError *out_error) override;
  const XRow *get_next_row(XError *out_error) override;
  bool has_resultset(XError *out_error) override;

 private:
  void clear();

  Handler_result handle_notice(
      const Mysqlx::Notice::Frame::Type type,
      const char *payload,
      const uint32_t payload_size);

  bool    had_fetch_not_ended() const;
  void    read_stmt_ok();
  void    read_if_needed_metadata();
  Row_ptr read_row();
  XError  read_metadata(
      const XProtocol::Server_message_type_id msg_id,
      std::unique_ptr<XProtocol::Message> &msg);
  bool is_end_resultset_msg() const;


  static XError read_dump_out_params_or_resultset(
      const XProtocol::Server_message_type_id msg_id,
      std::unique_ptr<XProtocol::Message> &msg);

  void check_error(const XError &error);
  bool verify_current_instance(XError *out_error);
  void set_result_fetch_done();

  template<typename Type>
  class Optional_value {
   public:
    Optional_value()
    : m_value(Type()),
      m_has_value(false) {
    }

    Optional_value &operator=(const Type &value) {
      m_value = value;
      m_has_value = true;

      return *this;
    }

    bool get_value(Type *out_value) const {
      if (!m_has_value)
        return false;

      if (out_value)
        *out_value = m_value;

      return true;
    }

   private:
    Type m_value;
    bool m_has_value { false };
  };

  bool       m_received_fetch_done{ false };
  bool       m_read_metadata { true };
  std::shared_ptr<XProtocol> m_protocol;
  XError     m_error;
  Metadata   m_metadata;

  XProtocol::Handler_id        m_notice_handler_id;
  Optional_value<uint64_t>     m_last_insert_id;
  Optional_value<uint64_t>     m_affected_rows;
  Optional_value<std::string>  m_producted_message;
  Message_holder               m_holder;
  Warnings                     m_warnings;
  XRow_impl                    m_row;
  Query_instances             *m_query_instances;
  Query_instances::Instance_id m_instance_id;
  std::shared_ptr<Context>     m_context;
};

}  // namespace xcl

#endif  // X_CLIENT_XQUERY_RESULT_IMPL_H_
