/*
  Copyright (c) 2020, Oracle and/or its affiliates.

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

#include "certificate_generator.h"

#include <fstream>

#include <openssl/evp.h>

#include "mysql/harness/filesystem.h"
#include "mysqlrouter/tls_context.h"
#include "router_test_helpers.h"
#include "test/helpers.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

class CertificateGeneratorTest : public ::testing::Test {
 public:
  const BIGNUM *RSA_get0_n(const RSA *rsa) const {
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    return rsa->n;
#else
    const BIGNUM *result;
    RSA_get0_key(rsa, &result, nullptr, nullptr);
    return result;
#endif
  }

#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
  RSA *EVP_PKEY_get0_RSA(EVP_PKEY *pkey) const { return pkey->pkey.rsa; }
#endif

#if (OPENSSL_VERSION_NUMBER < 0x10000000L)
  int EVP_PKEY_id(const EVP_PKEY *pkey) const { return pkey->type; }
#endif

  TlsLibraryContext m_tls_lib_ctx;
  CertificateGenerator m_cert_gen;
};

TEST_F(CertificateGeneratorTest, test_EVP_PKEY_generation) {
  const auto evp = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(evp);

  EXPECT_EQ(EVP_PKEY_id(evp->get()), EVP_PKEY_RSA);
  const auto rsa = EVP_PKEY_get0_RSA(evp->get());
  ASSERT_TRUE(rsa);

  const auto bn_modulus = RSA_get0_n(rsa);
  ASSERT_STRNE(BN_bn2dec(bn_modulus), nullptr);
}

TEST_F(CertificateGeneratorTest, test_write_PKEY_to_string) {
  const auto evp = m_cert_gen.generate_evp_pkey();
  const auto &key_string = m_cert_gen.pkey_to_string(evp.value());

  EXPECT_THAT(key_string, ::testing::HasSubstr("BEGIN RSA PRIVATE KEY"));
}

TEST_F(CertificateGeneratorTest, test_generate_CA_cert) {
  const auto ca_key = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(ca_key);

  const auto ca_cert =
      m_cert_gen.generate_x509(ca_key.value(), "CA", 1, nullptr, nullptr);
  ASSERT_TRUE(ca_cert);

  ASSERT_TRUE(X509_verify(ca_cert->get(), ca_key->get()));
}

TEST_F(CertificateGeneratorTest, test_generate_router_cert) {
  const auto ca_key = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(ca_key);

  const auto ca_cert =
      m_cert_gen.generate_x509(ca_key.value(), "CA", 1, nullptr, nullptr);
  ASSERT_TRUE(ca_cert);

  const auto router_key = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(router_key);

  const auto router_cert = m_cert_gen.generate_x509(
      router_key.value(), "router CN", 1, ca_cert.value(), ca_key.value());
  ASSERT_TRUE(router_cert);

  std::unique_ptr<X509_STORE, decltype(&::X509_STORE_free)> store{
      X509_STORE_new(), ::X509_STORE_free};
  std::unique_ptr<X509_STORE_CTX, decltype(&::X509_STORE_CTX_free)> ctx{
      X509_STORE_CTX_new(), ::X509_STORE_CTX_free};
  X509_STORE_add_cert(store.get(), ca_cert->get());
  X509_STORE_CTX_init(ctx.get(), store.get(), router_cert->get(), nullptr);

  EXPECT_TRUE(X509_verify_cert(ctx.get()));
  EXPECT_EQ(X509_STORE_CTX_get_error(ctx.get()), 0);
}

#ifndef NDEBUG
TEST_F(CertificateGeneratorTest, death_test_generate_cert_wrong_serial) {
  const auto key = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(key);

  ASSERT_DEATH(
      m_cert_gen.generate_x509(key.value(), "test CN", 0, nullptr, nullptr),
      "");
}

TEST_F(CertificateGeneratorTest, death_test_generate_cert_wrong_CN) {
  const auto key = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(key);

  std::string too_long(100, 'x');
  ASSERT_DEATH(
      m_cert_gen.generate_x509(key.value(), too_long, 1, nullptr, nullptr), "");
}

TEST_F(CertificateGeneratorTest, death_test_generate_cert_no_CA_key) {
  const auto ca_key = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(ca_key);

  const auto ca_cert =
      m_cert_gen.generate_x509(ca_key.value(), "CA", 1, nullptr, nullptr);
  ASSERT_TRUE(ca_cert);

  const auto router_key = m_cert_gen.generate_evp_pkey();
  ASSERT_DEATH(m_cert_gen.generate_x509(router_key.value(), "router CN", 1,
                                        ca_cert.value(), nullptr),
               "");
}

TEST_F(CertificateGeneratorTest, death_test_generate_cert_no_CA_cert) {
  const auto ca_key = m_cert_gen.generate_evp_pkey();
  ASSERT_TRUE(ca_key);

  const auto router_key = m_cert_gen.generate_evp_pkey();
  ASSERT_DEATH(m_cert_gen.generate_x509(router_key.value(), "router CN", 1,
                                        nullptr, ca_key.value()),
               "");
}
#endif

int main(int argc, char *argv[]) {
  init_test_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
