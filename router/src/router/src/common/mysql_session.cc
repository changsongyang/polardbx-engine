/*
  Copyright (c) 2016, 2020, Oracle and/or its affiliates.

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

#include "mysqlrouter/mysql_session.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>
#include <string>

#include <mysql.h>

#include "mysqlrouter/mysql_client_thread_token.h"
#define MYSQL_ROUTER_LOG_DOMAIN "sql"
#include "mysql/harness/logging/logging.h"

IMPORT_LOG_FUNCTIONS()

using namespace mysqlrouter;
using namespace std::string_literals;

/*static*/ const char MySQLSession::kSslModeDisabled[] = "DISABLED";
/*static*/ const char MySQLSession::kSslModePreferred[] = "PREFERRED";
/*static*/ const char MySQLSession::kSslModeRequired[] = "REQUIRED";
/*static*/ const char MySQLSession::kSslModeVerifyCa[] = "VERIFY_CA";
/*static*/ const char MySQLSession::kSslModeVerifyIdentity[] =
    "VERIFY_IDENTITY";

/*static*/ const std::function<void(unsigned, MYSQL_FIELD *)>
    MySQLSession::null_field_validator = [](unsigned, MYSQL_FIELD *) {};

MySQLSession::MySQLSession(std::unique_ptr<LoggingStrategy> &&logging_strategy)
    : logging_strategy_(std::move(logging_strategy)) {
  MySQLClientThreadToken api_token;

  connection_ = new MYSQL();
  connected_ = false;
  if (!mysql_init(connection_)) {
    // not supposed to happen
    throw std::logic_error("Error initializing MySQL connection structure");
  }

  log_filter_.add_default_sql_patterns();
}

MySQLSession::~MySQLSession() {
  mysql_close(connection_);

  delete connection_;
}

/*static*/
mysql_ssl_mode MySQLSession::parse_ssl_mode(std::string ssl_mode) {
  // we allow lowercase equivalents, to be consistent with mysql client
  std::transform(ssl_mode.begin(), ssl_mode.end(), ssl_mode.begin(), toupper);

  if (ssl_mode == kSslModeDisabled)
    return SSL_MODE_DISABLED;
  else if (ssl_mode == kSslModePreferred)
    return SSL_MODE_PREFERRED;
  else if (ssl_mode == kSslModeRequired)
    return SSL_MODE_REQUIRED;
  else if (ssl_mode == kSslModeVerifyCa)
    return SSL_MODE_VERIFY_CA;
  else if (ssl_mode == kSslModeVerifyIdentity)
    return SSL_MODE_VERIFY_IDENTITY;
  else
    throw std::logic_error(std::string("Unrecognised SSL mode '") + ssl_mode +
                           "'");
}

/*static*/
const char *MySQLSession::ssl_mode_to_string(mysql_ssl_mode ssl_mode) noexcept {
  switch (ssl_mode) {
    case SSL_MODE_DISABLED:
      return kSslModeDisabled;
    case SSL_MODE_PREFERRED:
      return kSslModePreferred;
    case SSL_MODE_REQUIRED:
      return kSslModeRequired;
    case SSL_MODE_VERIFY_CA:
      return kSslModeVerifyCa;
    case SSL_MODE_VERIFY_IDENTITY:
      return kSslModeVerifyIdentity;
  }

  return nullptr;
}

bool MySQLSession::check_for_yassl(MYSQL *connection) {
  static bool check_done = false;
  static bool is_yassl = false;
  if (!check_done) {
    const char *old_version{nullptr};
    // the assumption is that yaSSL does not support this version
    const char *kTlsNoYassl = "TLSv1.2";

    if (mysql_get_option(connection, MYSQL_OPT_TLS_VERSION, &old_version)) {
      throw Error("Error checking for SSL implementation",
                  mysql_errno(connection));
    }
    int res = mysql_options(connection, MYSQL_OPT_TLS_VERSION, kTlsNoYassl);
    is_yassl = (res != 0);
    if (mysql_options(connection, MYSQL_OPT_TLS_VERSION, old_version)) {
      throw Error("Error checking for SSL implementation",
                  mysql_errno(connection));
    }
    check_done = true;
  }

  return is_yassl;
}

void MySQLSession::set_ssl_options(mysql_ssl_mode ssl_mode,
                                   const std::string &tls_version,
                                   const std::string &ssl_cipher,
                                   const std::string &ca,
                                   const std::string &capath,
                                   const std::string &crl,
                                   const std::string &crlpath) {
  if (check_for_yassl(connection_)) {
    if ((ssl_mode >= SSL_MODE_VERIFY_CA) || (!ca.empty()) ||
        (!capath.empty()) || (!crl.empty()) || (!crlpath.empty())) {
      throw std::invalid_argument(
          "Certificate Verification is disabled in this build of the MySQL "
          "Router. \n"
          "The following parameters are not supported: \n"
          " --ssl-mode=VERIFY_CA, --ssl-mode=VERIFY_IDENTITY, \n"
          " --ssl-ca, --ssl-capath, --ssl-crl, --ssl-crlpath \n"
          "Please check documentation for the details.");
    }
  }

  if (!ssl_cipher.empty() && mysql_options(connection_, MYSQL_OPT_SSL_CIPHER,
                                           ssl_cipher.c_str()) != 0) {
    throw Error(("Error setting SSL_CIPHER option for MySQL connection: " +
                 std::string(mysql_error(connection_)))
                    .c_str(),
                mysql_errno(connection_));
  }

  if (!tls_version.empty() && mysql_options(connection_, MYSQL_OPT_TLS_VERSION,
                                            tls_version.c_str()) != 0) {
    throw Error("Error setting TLS_VERSION option for MySQL connection",
                mysql_errno(connection_));
  }

  if (!ca.empty() &&
      mysql_options(connection_, MYSQL_OPT_SSL_CA, ca.c_str()) != 0) {
    throw Error(("Error setting SSL_CA option for MySQL connection: " +
                 std::string(mysql_error(connection_)))
                    .c_str(),
                mysql_errno(connection_));
  }

  if (!capath.empty() &&
      mysql_options(connection_, MYSQL_OPT_SSL_CAPATH, capath.c_str()) != 0) {
    throw Error(("Error setting SSL_CAPATH option for MySQL connection: " +
                 std::string(mysql_error(connection_)))
                    .c_str(),
                mysql_errno(connection_));
  }

  if (!crl.empty() &&
      mysql_options(connection_, MYSQL_OPT_SSL_CRL, crl.c_str()) != 0) {
    throw Error(("Error setting SSL_CRL option for MySQL connection: " +
                 std::string(mysql_error(connection_)))
                    .c_str(),
                mysql_errno(connection_));
  }

  if (!crlpath.empty() &&
      mysql_options(connection_, MYSQL_OPT_SSL_CRLPATH, crlpath.c_str()) != 0) {
    throw Error(("Error setting SSL_CRLPATH option for MySQL connection: " +
                 std::string(mysql_error(connection_)))
                    .c_str(),
                mysql_errno(connection_));
  }

  // this has to be the last option that gets set due to what appears to be a
  // bug in libmysql causing ssl_mode downgrade from REQUIRED if other options
  // (like tls_version) are also specified
  if (mysql_options(connection_, MYSQL_OPT_SSL_MODE, &ssl_mode) != 0) {
    const char *text = ssl_mode_to_string(ssl_mode);
    std::string msg = std::string("Setting SSL mode to '") + text +
                      "' on connection failed: " + mysql_error(connection_);
    throw Error(msg.c_str(), mysql_errno(connection_));
  }

  // archive options for future connection templating
  conn_params_.ssl_opts = {ssl_mode, tls_version, ssl_cipher, ca,
                           capath,   crl,         crlpath};
}

void MySQLSession::set_ssl_cert(const std::string &cert,
                                const std::string &key) {
  if (mysql_options(connection_, MYSQL_OPT_SSL_CERT, cert.c_str()) != 0 ||
      mysql_options(connection_, MYSQL_OPT_SSL_KEY, key.c_str()) != 0) {
    throw Error(("Error setting client SSL certificate for connection: " +
                 std::string(mysql_error(connection_)))
                    .c_str(),
                mysql_errno(connection_));
  }

  // archive options for future connection templating
  conn_params_.ssl_cert = {cert, key};
}

void MySQLSession::connect(const std::string &host, unsigned int port,
                           const std::string &username,
                           const std::string &password,
                           const std::string &unix_socket,
                           const std::string &default_schema,
                           int connect_timeout, int read_timeout) {
  unsigned int protocol = MYSQL_PROTOCOL_TCP;
  connected_ = false;

  // Following would fail only when invalid values are given. It is not possible
  // for the user to change these values.
  mysql_options(connection_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
  mysql_options(connection_, MYSQL_OPT_READ_TIMEOUT, &read_timeout);

  if (unix_socket.length() > 0) {
#ifdef _WIN32
    protocol = MYSQL_PROTOCOL_PIPE;
#else
    protocol = MYSQL_PROTOCOL_SOCKET;
#endif
  }
  mysql_options(connection_, MYSQL_OPT_PROTOCOL,
                reinterpret_cast<char *>(&protocol));

  const unsigned long client_flags =
      (CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 |
       CLIENT_MULTI_RESULTS);
  std::string tmp_conn_addr = unix_socket.length() > 0
                                  ? unix_socket
                                  : host + ":" + std::to_string(port);
  if (!mysql_real_connect(connection_, host.c_str(), username.c_str(),
                          password.c_str(), default_schema.c_str(), port,
                          unix_socket.c_str(), client_flags)) {
    std::stringstream ss;
    ss << "Error connecting to MySQL server at " << tmp_conn_addr;
    ss << ": " << mysql_error(connection_) << " (" << mysql_errno(connection_)
       << ")";
    throw Error(ss.str().c_str(), mysql_errno(connection_));
  }
  connected_ = true;
  connection_address_ = tmp_conn_addr;

  // archive options for future connection templating
  conn_params_.conn_opts = {
      host,        port,           username,        password,
      unix_socket, default_schema, connect_timeout, read_timeout};
}

void MySQLSession::connect_and_set_opts(
    const ConnectionParameters &conn_params) {
  // should only be used on fresh objects
  // assert(!connected_);

  // below methods can throw:
  //   MySQLSession::Error (std::runtime_error)
  //   std::invalid_argument (std::logic_error)

  set_ssl_options(conn_params.ssl_opts.ssl_mode,
                  conn_params.ssl_opts.tls_version,
                  conn_params.ssl_opts.ssl_cipher, conn_params.ssl_opts.ca,
                  conn_params.ssl_opts.capath, conn_params.ssl_opts.crl,
                  conn_params.ssl_opts.crlpath);

  if (!conn_params.ssl_cert.cert.empty() || !conn_params.ssl_cert.key.empty())
    set_ssl_cert(conn_params.ssl_cert.cert, conn_params.ssl_cert.key);

  connect(conn_params.conn_opts.host, conn_params.conn_opts.port,
          conn_params.conn_opts.username, conn_params.conn_opts.password,
          conn_params.conn_opts.unix_socket,
          conn_params.conn_opts.default_schema,
          conn_params.conn_opts.connect_timeout,
          conn_params.conn_opts.read_timeout);
}

void MySQLSession::disconnect() {
  // close the socket and free internal data
  mysql_close(connection_);

  // initialize the connection handle again as _close() is also free()ing
  // a lot of internal data.
  MySQLClientThreadToken api_token;
  mysql_init(connection_);
  connected_ = false;
  connection_address_.clear();
}

void MySQLSession::execute(const std::string &q) {
  logging_strategy_->log("Executing query: "s + log_filter_.filter(q).c_str());
  std::shared_ptr<void> exit_guard(nullptr, [this](void *) {
    logging_strategy_->log("Done executing query");
  });
  if (connected_) {
    if (mysql_real_query(connection_, q.data(), q.length()) != 0) {
      std::stringstream ss;
      ss << "Error executing MySQL query \"" << log_filter_.filter(q);
      ss << "\": " << mysql_error(connection_) << " ("
         << mysql_errno(connection_) << ")";
      throw Error(ss.str().c_str(), mysql_errno(connection_),
                  mysql_error(connection_));
    }
    MYSQL_RES *res = mysql_store_result(connection_);
    if (res) mysql_free_result(res);
  } else
    throw std::logic_error("Not connected");
}

/*
  Execute query on the session and iterate the results with the given callback.

  The processor callback is called with a vector of strings, which conain the
  values of each field of a row. It is called once per row.
  If the processor returns false, the result row iteration stops.
 */
// throws MySQLSession::Error, std::logic_error, whatever processor() throws,
// ...?
void MySQLSession::query(
    const std::string &q, const RowProcessor &processor,
    const FieldValidator &validator /*=null_field_validator*/) {
  logging_strategy_->log("Executing query: "s + log_filter_.filter(q).c_str());
  std::shared_ptr<void> exit_guard(nullptr, [this](void *) {
    logging_strategy_->log("Done executing query");
  });
  if (connected_) {
    if (mysql_real_query(connection_, q.data(), q.length()) != 0) {
      std::stringstream ss;
      ss << "Error executing MySQL query \"" << log_filter_.filter(q);
      ss << "\": " << mysql_error(connection_) << " ("
         << mysql_errno(connection_) << ")";
      throw Error(ss.str().c_str(), mysql_errno(connection_),
                  mysql_error(connection_));
    }
    MYSQL_RES *res = mysql_store_result(connection_);
    if (res) {
      // get column info and give it to field validator,
      // which should throw if it doesn't like the columns
      unsigned int nfields = mysql_num_fields(res);
      MYSQL_FIELD *fields = mysql_fetch_fields(res);
      validator(nfields, fields);

      std::vector<const char *> outrow;
      outrow.resize(nfields);
      MYSQL_ROW row;
      while ((row = mysql_fetch_row(res))) {
        for (unsigned int i = 0; i < nfields; i++) {
          outrow[i] = row[i];
        }
        try {
          if (!processor(outrow)) break;
        } catch (...) {
          mysql_free_result(res);
          throw;
        }
      }
      mysql_free_result(res);
    } else {
      std::stringstream ss;
      ss << "Error fetching query results: ";
      ss << mysql_error(connection_) << " (" << mysql_errno(connection_) << ")";
      throw Error(ss.str().c_str(), mysql_errno(connection_),
                  mysql_error(connection_));
    }
  } else
    throw std::logic_error("Not connected");
}

class RealResultRow : public MySQLSession::ResultRow {
 public:
  RealResultRow(MySQLSession::Row row, MYSQL_RES *res)
      : ResultRow(std::move(row)), res_(res) {}

  ~RealResultRow() override { mysql_free_result(res_); }

 private:
  MYSQL_RES *res_;
};

std::unique_ptr<MySQLSession::ResultRow> MySQLSession::query_one(
    const std::string &q,
    const FieldValidator &validator /*= null_field_validator*/) {
  logging_strategy_->log("Executing query: "s + log_filter_.filter(q).c_str());
  std::shared_ptr<void> exit_guard(nullptr, [this](void *) {
    logging_strategy_->log("Done executing query");
  });
  if (connection_) {
    if (mysql_real_query(connection_, q.data(), q.length()) != 0) {
      std::stringstream ss;
      ss << "Error executing MySQL query \"" << log_filter_.filter(q);
      ss << "\": " << mysql_error(connection_) << " ("
         << mysql_errno(connection_) << ")";
      throw Error(ss.str().c_str(), mysql_errno(connection_),
                  mysql_error(connection_));
    }
    MYSQL_RES *res = mysql_store_result(connection_);
    if (res) {
      // get column info and give it to field validator,
      // which should throw if it doesn't like the columns
      unsigned int nfields = mysql_num_fields(res);
      MYSQL_FIELD *fields = mysql_fetch_fields(res);
      validator(nfields, fields);

      std::vector<const char *> outrow;
      MYSQL_ROW row;
      if ((row = mysql_fetch_row(res))) {
        outrow.resize(nfields);
        for (unsigned int i = 0; i < nfields; i++) {
          outrow[i] = row[i];
        }
      }
      if (outrow.empty()) {
        mysql_free_result(res);
        return nullptr;
      }
      return std::make_unique<RealResultRow>(outrow, res);
    } else {
      std::stringstream ss;
      ss << "Error fetching query results: ";
      ss << mysql_error(connection_) << " (" << mysql_errno(connection_) << ")";
      throw Error(ss.str().c_str(), mysql_errno(connection_),
                  mysql_error(connection_));
    }
  }
  throw Error("Not connected", 0);  // TODO: query() returns std::logic_error()
                                    // in such case, should probably be the same
}

uint64_t MySQLSession::last_insert_id() noexcept {
  return mysql_insert_id(connection_);
}

unsigned MySQLSession::warning_count() noexcept {
  return mysql_warning_count(connection_);
}

std::string MySQLSession::quote(const std::string &s, char qchar) noexcept {
  std::string r;
  r.resize(s.length() * 2 + 3);
  r[0] = qchar;
  unsigned long len = mysql_real_escape_string_quote(
      connection_, &r[1], s.c_str(), s.length(), qchar);
  r.resize(len + 2);
  r[len + 1] = qchar;
  return r;
}

const char *MySQLSession::last_error() {
  return connection_ ? mysql_error(connection_) : nullptr;
}

unsigned int MySQLSession::last_errno() {
  return connection_ ? mysql_errno(connection_) : 0;
}

const char *MySQLSession::ssl_cipher() {
  return connection_ ? mysql_get_ssl_cipher(connection_) : nullptr;
}

void MySQLSession::LoggingStrategyDebugLogger::log(const std::string &msg) {
  log_debug("%s", msg.c_str());
}
