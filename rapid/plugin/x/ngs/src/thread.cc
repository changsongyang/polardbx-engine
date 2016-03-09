/*
 * Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
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

#if !defined(MYSQL_DYNAMIC_PLUGIN) && defined(WIN32) && !defined(XPLUGIN_UNIT_TESTS)
// Needed for importing PERFORMANCE_SCHEMA plugin API.
#define MYSQL_DYNAMIC_PLUGIN 1
#endif // WIN32

#include "ngs/thread.h"


int ngs::thread_create(PSI_thread_key key, Thread_t *thread,
                       const Thread_attr_t *attr,
                       Start_routine_t func, void *arg)
{
  return mysql_thread_create(key, thread, attr, func, arg);
}


int ngs::thread_join(Thread_t *thread, void **ret)
{
  return my_thread_join(thread, ret);
}


ngs::Mutex::Mutex(PSI_mutex_key key)
{
  mysql_mutex_init(key, &m_mutex, NULL);
}


ngs::Mutex::~Mutex()
{
  mysql_mutex_destroy(&m_mutex);
}


ngs::Mutex::operator mysql_mutex_t*()
{
  return &m_mutex;
}



ngs::RWLock::RWLock(PSI_rwlock_key key)
{
  mysql_rwlock_init(key, &m_rwlock);
}


ngs::RWLock::~RWLock()
{
  mysql_rwlock_destroy(&m_rwlock);
}


ngs::Cond::Cond(PSI_cond_key key)
{
  mysql_cond_init(key, &m_cond);
}


ngs::Cond::~Cond()
{
  mysql_cond_destroy(&m_cond);
}


void ngs::Cond::wait(Mutex& mutex)
{
  mysql_cond_wait(&m_cond, &mutex.m_mutex);
}


int ngs::Cond::timed_wait(Mutex& mutex, unsigned long long nanoseconds)
{
  timespec ts;

  set_timespec_nsec(&ts, nanoseconds);

  return mysql_cond_timedwait(&m_cond, &mutex.m_mutex, &ts);
}


void ngs::Cond::signal()
{
  mysql_cond_signal(&m_cond);
}


void ngs::Cond::signal(Mutex& mutex)
{
  Mutex_lock lock(mutex);

  signal();
}


void ngs::Cond::broadcast()
{
  mysql_cond_broadcast(&m_cond);
}


void ngs::Cond::broadcast(Mutex& mutex)
{
  Mutex_lock lock(mutex);

  broadcast();
}
