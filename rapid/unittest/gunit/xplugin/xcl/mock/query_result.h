/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef XPLUGIN_XCL_MOCK_QUERY_RESULT_H_
#define XPLUGIN_XCL_MOCK_QUERY_RESULT_H_

#include <gmock/gmock.h>
#include <cstdint>
#include <memory>
#include <string>

#include "mysqlxclient/xquery_result.h"


namespace xcl {
namespace test {

class Mock_query_result : public XQuery_result {
 public:
  MOCK_METHOD1(get_metadata,
       const Metadata & (XError *out_error));
  MOCK_METHOD0(get_warnings,
      const Warnings &());
  MOCK_METHOD2(get_next_row,
      bool(const XRow **out_row, XError *out_error));
  MOCK_METHOD1(get_next_row,
      const XRow *(XError *));
  MOCK_METHOD1(get_next_row_raw_raw,
      XQuery_result::Row* (XError *));
  MOCK_METHOD1(next_resultset,
      bool(XError *));
  MOCK_CONST_METHOD1(try_get_last_insert_id,
      bool(uint64_t*));
  MOCK_CONST_METHOD1(try_get_affected_rows,
      bool(uint64_t*));
  MOCK_CONST_METHOD1(try_get_info_message,
      bool(std::string*));
  MOCK_METHOD1(has_resultset,
      bool(XError *));

 private:
  std::unique_ptr<XQuery_result::Row> get_next_row_raw(
      XError *out_error) override {
    return std::unique_ptr<XQuery_result::Row>(get_next_row_raw_raw(out_error));
  }
};

}  // namespace test
}  // namespace xcl

#endif  // XPLUGIN_XCL_MOCK_QUERY_RESULT_H_
