/* Copyright (c) 2018, 2021, Alibaba and/or its affiliates. All rights reserved.
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.
   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL/Apsara GalaxyEngine hereby grant you an
   additional permission to link the program and your derivative works with the
   separately licensed software that they have included with
   MySQL/Apsara GalaxyEngine.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef PLUGIN_X_SRC_IO_GALAXY_LISTENER_H
#define PLUGIN_X_SRC_IO_GALAXY_LISTENER_H

#include "plugin/x/src/interface/galaxy_listener_factory.h"
#include "plugin/x/src/io/galaxy_listener_tcp.h"

namespace gx {

class Galaxy_listener_context {
 public:
  Galaxy_listener_context(const xpl::iface::Listener_factory &factory,
                          const uint16_t port)
      : m_listener_factory(factory), m_tcp_port(port) {}

  virtual ~Galaxy_listener_context() {}

  const xpl::iface::Listener_factory &get_listener_factory() const {
    return m_listener_factory;
  };

  uint16_t get_port() const { return m_tcp_port; }

 private:
  const xpl::iface::Listener_factory &m_listener_factory;
  const uint16_t m_tcp_port;
};

}  // namespace gx

#endif
