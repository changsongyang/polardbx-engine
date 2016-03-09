/*
 * Copyright (c) 2015, 2016 Oracle and/or its affiliates. All rights reserved.
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

#ifndef _NGS_SCHEDULER_H_
#define _NGS_SCHEDULER_H_

#include "ngs/thread.h"
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <string>
#include <vector>
#include <list>


namespace ngs
{
  // Scheduler with dynamic thread pool.
  class Scheduler_dynamic
  {
  public:
    class Monitor
    {
    public:
      virtual ~Monitor() {}

      virtual void on_worker_thread_create() = 0;
      virtual void on_worker_thread_destroy() = 0;
      virtual void on_task_start() = 0;
      virtual void on_task_end() = 0;
    };

    typedef boost::function<void()> Task;

    Scheduler_dynamic(const char* name);
    virtual ~Scheduler_dynamic();

    virtual void launch();
    virtual void set_num_workers(unsigned int n);
    virtual void stop();
    void set_idle_worker_timeout(unsigned long long milliseconds);
    bool post(Task* task);
    bool post(const Task& task);
    bool post_and_wait(const Task& task);

    virtual bool thread_init() { return true; }
    virtual void thread_end() {}

    void set_monitor(Monitor *monitor);

    bool is_worker_thread(my_thread_t thread_id);

  private:
    template<typename Element_type>
    class lock_list
    {
    public:
      lock_list()
      : m_access_mutex(KEY_mutex_x_lock_list_access)
      {
      }

      bool empty()
      {
        Mutex_lock guard(m_access_mutex);
        return m_list.empty();
      }

      bool push(const Element_type &t)
      {
        Mutex_lock guard(m_access_mutex);
        m_list.push_back(t);
        return true;
      }

      bool pop(Element_type &result)
      {
        Mutex_lock guard(m_access_mutex);
        if (m_list.empty())
          return false;

        result = m_list.front();

        m_list.pop_front();
        return true;
      }

    private:
      Mutex m_access_mutex;
      std::list<Element_type> m_list;
    };

    Scheduler_dynamic(const Scheduler_dynamic&);
    Scheduler_dynamic& operator=(const Scheduler_dynamic&);

    static void* worker_proxy(void* data);
    void* worker();

    void create_thread();
    bool is_running();

    int32 increase_workers_count();
    int32 decrease_workers_count();
    int32 increase_tasks_count();
    int32 decrease_tasks_count();

    const std::string m_name;
    Mutex m_task_pending_mutex;
    Cond m_task_pending_cond;
    Mutex m_thread_exit_mutex;
    Cond m_thread_exit_cond;
    volatile int32 m_is_running;
    volatile int32 m_min_workers_count;
    volatile int32 m_workers_count;
    volatile int32 m_tasks_count;
    volatile int64 m_idle_worker_timeout; // milliseconds
    //boost::lockfree::queue<Task*> m_tasks;
    lock_list<Task *> m_tasks;
    lock_list<Thread_t> m_threads;
    boost::scoped_ptr<Monitor> m_monitor;
  };
}

#endif
