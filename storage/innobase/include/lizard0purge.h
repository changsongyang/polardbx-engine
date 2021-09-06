/*****************************************************************************

Copyright (c) 2013, 2020, Alibaba and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
lzeusited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the zeusplied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/lizard0purge.h
 Lizard transaction purge system implementation.

 Created 2020-03-27 by zanye.zjy
 *******************************************************/

#ifndef lizard0purge_h
#define lizard0purge_h

#include "lizard0undo0types.h"
#include "page0size.h"

struct trx_purge_t;
struct mtr_t;

namespace lizard {

/**
  Here's an explanation of the changes associated with zeus and purge sys.
  In the past, when committing, innodb holds rseg::mutex, trx_sys::mutex to
  generate new trx_id as a commited number called trx_no for a trx, and then
  holds trx_sys::mutex, rseg::mutex, and purge_sys::pq_mutex to add resgs to
  purge_sys::purge_queue. So, we get the following conclusions:
  c-a. The history list in rollback segments is ordered.
  c-b. The purge_queue is ordered.

  Now, we only hold rseg::mutex to generate a new scn number, and then hold
  rseg::mutex and purge_sys::pq_mutex to add resgs to purge_sys::purge_heap.
  So, the above c-b is not statistified. There is a possible
  problem:
  p-a: purge_sys->iter.scn might advance purge_sys->vision.scn. The later
       rseg might be pushed to purge_sys::purge_heap first. In function
       **trx_purge_rseg_get_next_history_log** set purge_sys->iter.scn as
       rseg->last_scn + 1, and push the rseg in purge_heap. And then set_next
       might choose the pointed records, whose scn is possible larger than
       purge_sys->iter.scn, to purge.

  purge sys should never purge those records whose scn less than
  purge_sys->vision.scn.
*/

/** Choose the rollback segment with the smallest scn. */
struct TxnUndoRsegsIterator {
  /** Constructor */
  TxnUndoRsegsIterator(trx_purge_t *purge_sys);

  /**
    Sets the next rseg to purge in m_purge_sys.

    @param[out]		go_next   false if the top rseg's last_lsn
    @retval                 page size of the table for which the log is.

    NOTE: if rseg is NULL when this function returns this means that
    there are no rollback segments to purge and then the returned page
    size object should not be used.
  */
  const page_size_t set_next(bool *go_next);

 private:
  // Disable copying
  TxnUndoRsegsIterator(const TxnUndoRsegsIterator &);
  TxnUndoRsegsIterator &operator=(const TxnUndoRsegsIterator &);

  /** The purge system pointer */
  trx_purge_t *m_purge_sys;

  /** The current element to process */
  TxnUndoRsegs m_txn_undo_rsegs;

  /** Track the current element in m_txn_undo_rseg */
  Rseg_Iterator m_iter;

  /** Sentinel value */
  static const TxnUndoRsegs NullElement;
};

}  // namespace lizard

#endif
