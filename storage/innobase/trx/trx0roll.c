/*****************************************************************************

Copyright (c) 1996, 2009, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/**************************************************//**
@file trx/trx0roll.c
Transaction rollback

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0roll.h"

#ifdef UNIV_NONINL
#include "trx0roll.ic"
#endif

#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0undo.h"
#include "trx0rec.h"
#include "que0que.h"
#include "usr0sess.h"
#include "srv0start.h"
#include "read0read.h"
#include "row0undo.h"
#include "row0mysql.h"
#include "lock0lock.h"
#include "pars0pars.h"

/** This many pages must be undone before a truncate is tried within
rollback */
#define TRX_ROLL_TRUNC_THRESHOLD	1

/** In crash recovery, the current trx to be rolled back */
static trx_t*		trx_roll_crash_recv_trx	= NULL;

/** In crash recovery we set this to the undo n:o of the current trx to be
rolled back. Then we can print how many % the rollback has progressed. */
static ib_int64_t	trx_roll_max_undo_no;

/** Auxiliary variable which tells the previous progress % we printed */
static ulint		trx_roll_progress_printed_pct;

/****************************************************************//**
Finishes a transaction rollback. */
static
void
trx_finish_rollback(
/*================*/
	trx_t*		trx);	/*!< in: transaction */

/*******************************************************************//**
Rollback a transaction used in MySQL. */
static
void
trx_general_rollback_for_mysql_low(
/*===============================*/
	trx_t*		trx,	/*!< in: transaction handle */
	trx_savept_t*	savept)	/*!< in: pointer to savepoint undo number, if
				partial rollback requested, or NULL for
				complete rollback */
{
	que_thr_t*	thr;
	mem_heap_t*	heap;
	roll_node_t*	roll_node;

	heap = mem_heap_create(512);

	roll_node = roll_node_create(heap);

	if (savept != NULL) {
		roll_node->partial = TRUE;
		roll_node->savept = *savept;
	}

	trx->error_state = DB_SUCCESS;

	thr = pars_complete_graph_for_exec(roll_node, trx, heap);

	ut_a(thr == que_fork_start_command(que_node_get_parent(thr)));
	que_run_threads(thr);

	ut_a(roll_node->undo_thr != NULL);
	que_run_threads(roll_node->undo_thr);

	/* Free the memory reserved by the undo graph */
	que_graph_free(roll_node->undo_thr->common.parent);

	if (savept == NULL) {

		trx_finish_rollback(trx);

		mem_heap_free(heap);

		trx_mutex_enter(trx);

		ut_a(trx->que_state == TRX_QUE_RUNNING);

		ut_a(trx->error_state == DB_SUCCESS);

		trx_mutex_exit(trx);
	}
}

/*******************************************************************//**
Rollback a transaction used in MySQL.
@return	error code or DB_SUCCESS */
UNIV_INTERN
int
trx_general_rollback_for_mysql(
/*===========================*/
	trx_t*		trx,	/*!< in: transaction handle */
	trx_savept_t*	savept)	/*!< in: pointer to savepoint undo number, if
				partial rollback requested, or NULL for
				complete rollback */
{
	ut_ad(!trx_mutex_own(trx));

	/* Tell Innobase server that there might be work for
	utility threads: */

	srv_active_wake_master_thread();

	trx_start_if_not_started_xa(trx);

	trx_general_rollback_for_mysql_low(trx, savept);

	/* Tell Innobase server that there might be work for
	utility threads: */

	srv_active_wake_master_thread();

	return((int) trx->error_state);
}

/*******************************************************************//**
Rollback a transaction used in MySQL.
@return	error code or DB_SUCCESS */
UNIV_INTERN
int
trx_rollback_for_mysql(
/*===================*/
	trx_t*	trx)	/*!< in: transaction handle */
{

	/* Tell Innobase server that there might be work for
	utility threads: */

	srv_active_wake_master_thread();

	trx_mutex_enter(trx);

	if (trx->conc_state != TRX_NOT_STARTED) {

		trx->op_info = "rollback";

		trx_mutex_exit(trx);

		/* If we are doing the XA recovery of prepared transactions,
	       	then the transaction object does not have an InnoDB session
	       	object, and we set a dummy session that we use for all MySQL
	       	transactions. */

		trx_general_rollback_for_mysql_low(trx, NULL);

		trx_mutex_enter(trx);
	}

	trx->op_info = "";

	ut_a(trx->error_state == DB_SUCCESS);

	trx_mutex_exit(trx);

	/* Tell Innobase server that there might be work for
	utility threads: */

	srv_active_wake_master_thread();

	return((int) trx->error_state);
}

/*******************************************************************//**
Rollback the latest SQL statement for MySQL.
@return	error code or DB_SUCCESS */
UNIV_INTERN
int
trx_rollback_last_sql_stat_for_mysql(
/*=================================*/
	trx_t*	trx)	/*!< in: transaction handle */
{
	int	err;

	if (trx->conc_state == TRX_NOT_STARTED) {

		return(DB_SUCCESS);
	}

	trx->op_info = "rollback of SQL statement";

	err = trx_general_rollback_for_mysql(trx, &trx->last_sql_stat_start);

	/* The following call should not be needed, but we play safe: */
	trx_mark_sql_stat_end(trx);

	trx->op_info = "";

	return(err);
}

/*******************************************************************//**
Frees a single savepoint struct. */
UNIV_INTERN
void
trx_roll_savepoint_free(
/*=====================*/
	trx_t*			trx,	/*!< in: transaction handle */
	trx_named_savept_t*	savep)	/*!< in: savepoint to free */
{
	ut_ad(trx_mutex_own(trx));

	ut_a(savep != NULL);
	ut_a(UT_LIST_GET_LEN(trx->trx_savepoints) > 0);

	UT_LIST_REMOVE(trx_savepoints, trx->trx_savepoints, savep);
	mem_free(savep->name);
	mem_free(savep);
}

/*******************************************************************//**
Frees savepoint structs starting from savep, if savep == NULL then
free all savepoints. */
UNIV_INTERN
void
trx_roll_savepoints_free(
/*=====================*/
	trx_t*			trx,	/*!< in: transaction handle */
	trx_named_savept_t*	savep)	/*!< in: free all savepoints > this one;
					if this is NULL, free all savepoints
					of trx */
{
	trx_named_savept_t*	next_savep;

	ut_ad(trx_mutex_own(trx));

	if (savep == NULL) {
		savep = UT_LIST_GET_FIRST(trx->trx_savepoints);
	} else {
		savep = UT_LIST_GET_NEXT(trx_savepoints, savep);
	}

	while (savep != NULL) {
		next_savep = UT_LIST_GET_NEXT(trx_savepoints, savep);

		trx_roll_savepoint_free(trx, savep);

		savep = next_savep;
	}
}

/*******************************************************************//**
Rolls back a transaction back to a named savepoint. Modifications after the
savepoint are undone but InnoDB does NOT release the corresponding locks
which are stored in memory. If a lock is 'implicit', that is, a new inserted
row holds a lock where the lock information is carried by the trx id stored in
the row, these locks are naturally released in the rollback. Savepoints which
were set after this savepoint are deleted.
@return if no savepoint of the name found then DB_NO_SAVEPOINT,
otherwise DB_SUCCESS */
UNIV_INTERN
ulint
trx_rollback_to_savepoint_for_mysql(
/*================================*/
	trx_t*		trx,			/*!< in: transaction handle */
	const char*	savepoint_name,		/*!< in: savepoint name */
	ib_int64_t*	mysql_binlog_cache_pos)	/*!< out: the MySQL binlog cache
						position corresponding to this
						savepoint; MySQL needs this
						information to remove the
						binlog entries of the queries
						executed after the savepoint */
{
	trx_named_savept_t*	savep;
	ulint			err;

	ut_ad(trx_mutex_own(trx));

	savep = UT_LIST_GET_FIRST(trx->trx_savepoints);

	while (savep != NULL) {
		if (0 == ut_strcmp(savep->name, savepoint_name)) {
			/* Found */
			break;
		}
		savep = UT_LIST_GET_NEXT(trx_savepoints, savep);
	}

	if (savep == NULL) {

		return(DB_NO_SAVEPOINT);
	}

	if (trx->conc_state == TRX_NOT_STARTED) {
		ut_print_timestamp(stderr);
		fputs("  InnoDB: Error: transaction has a savepoint ", stderr);
		ut_print_name(stderr, trx, FALSE, savep->name);
		fputs(" though it is not started\n", stderr);
		return(DB_ERROR);
	}

	/* We can now free all savepoints strictly later than this one */

	trx_roll_savepoints_free(trx, savep);

	*mysql_binlog_cache_pos = savep->mysql_binlog_cache_pos;

	trx->op_info = "rollback to a savepoint";

	err = trx_general_rollback_for_mysql(trx, &savep->savept);

	/* Store the current undo_no of the transaction so that we know where
	to roll back if we have to roll back the next SQL statement: */

	trx_mark_sql_stat_end(trx);

	trx->op_info = "";

	return(err);
}

/*******************************************************************//**
Creates a named savepoint. If the transaction is not yet started, starts it.
If there is already a savepoint of the same name, this call erases that old
savepoint and replaces it with a new. Savepoints are deleted in a transaction
commit or rollback.
@return	always DB_SUCCESS */
UNIV_INTERN
ulint
trx_savepoint_for_mysql(
/*====================*/
	trx_t*		trx,			/*!< in: transaction handle */
	const char*	savepoint_name,		/*!< in: savepoint name */
	ib_int64_t	binlog_cache_pos)	/*!< in: MySQL binlog cache
						position corresponding to this
						connection at the time of the
						savepoint */
{
	trx_named_savept_t*	savep;

	ut_a(trx);
	ut_a(savepoint_name);

	trx_start_if_not_started_xa(trx);

	savep = UT_LIST_GET_FIRST(trx->trx_savepoints);

	while (savep != NULL) {
		if (0 == ut_strcmp(savep->name, savepoint_name)) {
			/* Found */
			break;
		}
		savep = UT_LIST_GET_NEXT(trx_savepoints, savep);
	}

	if (savep) {
		/* There is a savepoint with the same name: free that */

		UT_LIST_REMOVE(trx_savepoints, trx->trx_savepoints, savep);

		mem_free(savep->name);
		mem_free(savep);
	}

	/* Create a new savepoint and add it as the last in the list */

	savep = mem_alloc(sizeof(trx_named_savept_t));

	savep->name = mem_strdup(savepoint_name);

	savep->savept = trx_savept_take(trx);

	savep->mysql_binlog_cache_pos = binlog_cache_pos;

	UT_LIST_ADD_LAST(trx_savepoints, trx->trx_savepoints, savep);

	return(DB_SUCCESS);
}

/*******************************************************************//**
Releases only the named savepoint. Savepoints which were set after this
savepoint are left as is.
@return if no savepoint of the name found then DB_NO_SAVEPOINT,
otherwise DB_SUCCESS */
UNIV_INTERN
ulint
trx_release_savepoint_for_mysql(
/*============================*/
	trx_t*		trx,			/*!< in: transaction handle */
	const char*	savepoint_name)		/*!< in: savepoint name */
{
	trx_named_savept_t*	savep;

	savep = UT_LIST_GET_FIRST(trx->trx_savepoints);

	/* Search for the savepoint by name and free if found. */
	while (savep != NULL) {
		if (0 == ut_strcmp(savep->name, savepoint_name)) {
			trx_roll_savepoint_free(trx, savep);
			return(DB_SUCCESS);
		}
		savep = UT_LIST_GET_NEXT(trx_savepoints, savep);
	}

	return(DB_NO_SAVEPOINT);
}

/*******************************************************************//**
Determines if this transaction is rolling back an incomplete transaction
in crash recovery.
@return TRUE if trx is an incomplete transaction that is being rolled
back in crash recovery */
UNIV_INTERN
ibool
trx_is_recv(
/*========*/
	const trx_t*	trx)	/*!< in: transaction */
{
	return(trx == trx_roll_crash_recv_trx);
}

/*******************************************************************//**
Returns a transaction savepoint taken at this point in time.
@return	savepoint */
UNIV_INTERN
trx_savept_t
trx_savept_take(
/*============*/
	trx_t*	trx)	/*!< in: transaction */
{
	trx_savept_t	savept;

	savept.least_undo_no = trx->undo_no;

	return(savept);
}

/*******************************************************************//**
Roll back an active transaction. */
static
void
trx_rollback_active(
/*================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;
	roll_node_t*	roll_node;
	dict_table_t*	table;
	ib_int64_t	rows_to_undo;
	const char*	unit		= "";
	ibool		dictionary_locked = FALSE;

	heap = mem_heap_create(512);

	fork = que_fork_create(NULL, NULL, QUE_FORK_RECOVERY, heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap);

	roll_node = roll_node_create(heap);

	thr->child = roll_node;
	roll_node->common.parent = thr;

	trx->graph = fork;

	ut_a(thr == que_fork_start_command(fork));

	// FIXME: Document this
	trx_sys_mutex_enter();

	trx_roll_max_undo_no = ut_conv_dulint_to_longlong(trx->undo_no);

	trx_roll_crash_recv_trx	= trx;

	trx_roll_progress_printed_pct = 0;

	rows_to_undo = trx_roll_max_undo_no;

	trx_sys_mutex_exit();

	if (rows_to_undo > 1000000000) {
		rows_to_undo = rows_to_undo / 1000000;
		unit = "M";
	}

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: Rolling back trx with id " TRX_ID_FMT ", %lu%s"
		" rows to undo\n",
		TRX_ID_PREP_PRINTF(trx->id),
		(ulong) rows_to_undo, unit);

	trx->mysql_thread_id = os_thread_get_curr_id();

	trx->mysql_process_no = os_proc_get_number();

	if (trx_get_dict_operation(trx) != TRX_DICT_OP_NONE) {
		row_mysql_lock_data_dictionary(trx);
		dictionary_locked = TRUE;
	}

	que_run_threads(thr);
	ut_a(roll_node->undo_thr != NULL);

	que_run_threads(roll_node->undo_thr);

	/* Free the memory reserved by the undo graph */
	que_graph_free(roll_node->undo_thr->common.parent);

	trx_finish_rollback(thr_get_trx(roll_node->undo_thr));

	ut_a(trx->que_state == TRX_QUE_RUNNING);

	if (trx_get_dict_operation(trx) != TRX_DICT_OP_NONE
	    && !ut_dulint_is_zero(trx->table_id)) {

		/* If the transaction was for a dictionary operation, we
		drop the relevant table, if it still exists */

		fprintf(stderr,
			"InnoDB: Dropping table with id %lu %lu"
			" in recovery if it exists\n",
			(ulong) ut_dulint_get_high(trx->table_id),
			(ulong) ut_dulint_get_low(trx->table_id));

		table = dict_table_get_on_id_low(trx->table_id);

		if (table) {
			ulint	err;

			fputs("InnoDB: Table found: dropping table ", stderr);
			ut_print_name(stderr, trx, TRUE, table->name);
			fputs(" in recovery\n", stderr);

			err = row_drop_table_for_mysql(table->name, trx, TRUE);
			trx_commit_for_mysql(trx);

			ut_a(err == (int) DB_SUCCESS);
		}
	}

	if (dictionary_locked) {
		row_mysql_unlock_data_dictionary(trx);
	}

	fprintf(stderr, "\nInnoDB: Rolling back of trx id " TRX_ID_FMT
		" completed\n",
		TRX_ID_PREP_PRINTF(trx->id));
	mem_heap_free(heap);

	trx_roll_crash_recv_trx	= NULL;
}

/*******************************************************************//**
Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back. */
UNIV_INTERN
void
trx_rollback_or_clean_recovered(
/*============================*/
	ibool	all)	/*!< in: FALSE=roll back dictionary transactions;
			TRUE=roll back all non-PREPARED transactions */
{
	trx_t*	trx;

	trx_sys_mutex_enter();

	if (!UT_LIST_GET_FIRST(trx_sys->trx_list)) {
		goto leave_function;
	}

	trx_sys_mutex_exit();

	if (all) {
		fprintf(stderr,
			"InnoDB: Starting in background the rollback"
			" of uncommitted transactions\n");
	}

loop:
	trx_sys_mutex_enter();

	for (trx = UT_LIST_GET_FIRST(trx_sys->trx_list);
	     trx != NULL;
	     trx = UT_LIST_GET_NEXT(trx_list, trx)) {

		trx_mutex_enter(trx);

		if (!trx->is_recovered) {
			trx_mutex_exit(trx);
			continue;
		}

		switch (trx->conc_state) {
		case TRX_NOT_STARTED:
		case TRX_PREPARED:
			trx_mutex_exit(trx);
			continue;

		case TRX_COMMITTED_IN_MEMORY:

			trx_mutex_exit(trx);

			trx_sys_mutex_exit();

			fprintf(stderr,
				"InnoDB: Cleaning up trx with id "
				TRX_ID_FMT "\n",
				TRX_ID_PREP_PRINTF(trx->id));

			trx_cleanup_at_db_startup(trx);

			goto loop;

		case TRX_ACTIVE:
			if (all || trx_get_dict_operation(trx)
			    != TRX_DICT_OP_NONE) {

				trx_mutex_exit(trx);

				trx_sys_mutex_exit();

				trx_rollback_active(trx);
				goto loop;
			}
		}

		trx_mutex_exit(trx);
	}

	if (all) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: Rollback of non-prepared"
			" transactions completed\n");
	}

leave_function:
	trx_sys_mutex_exit();
}

/*******************************************************************//**
Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back.
Note: this is done in a background thread.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
trx_rollback_or_clean_all_recovered(
/*================================*/
	void*	arg __attribute__((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
#ifdef UNIV_PFS_THREAD
	pfs_register_thread(trx_rollback_clean_thread_key);
#endif /* UNIV_PFS_THREAD */

	trx_rollback_or_clean_recovered(TRUE);

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/*******************************************************************//**
Creates an undo number array.
@return	own: undo number array */
UNIV_INTERN
trx_undo_arr_t*
trx_undo_arr_create(
/*================*/
	ulint		n_cells)	/*!< Number of cells */
{
	trx_undo_arr_t*	arr;
	mem_heap_t*	heap;

	heap = mem_heap_create(1024);

	arr = mem_heap_zalloc(heap, sizeof(trx_undo_arr_t));

	arr->infos = mem_heap_zalloc(heap, sizeof(*arr->infos) * n_cells);

	arr->n_cells = n_cells;

	arr->heap = heap;

	return(arr);
}

/*******************************************************************//**
Frees an undo number array. */
UNIV_INTERN
void
trx_undo_arr_free(
/*==============*/
	trx_undo_arr_t*	arr)	/*!< in: undo number array */
{
	mem_heap_free(arr->heap);
}

/*******************************************************************//**
Stores info of an undo log record to the array if it is not stored yet.
@return	FALSE if the record already existed in the array */
static
ibool
trx_undo_arr_store_info(
/*====================*/
	trx_t*		trx,	/*!< in: transaction */
	undo_no_t	undo_no)/*!< in: undo number */
{
	trx_undo_inf_t*	cell;
	trx_undo_inf_t*	stored_here;
	trx_undo_arr_t*	arr;
	ulint		n_used;
	ulint		n;
	ulint		i;

	n = 0;
	arr = trx->undo_no_arr;
	n_used = arr->n_used;
	stored_here = NULL;

	for (i = 0;; i++) {
		cell = trx_undo_arr_get_nth_info(arr, i);

		if (!cell->in_use) {
			if (!stored_here) {
				/* Not in use, we may store here */
				cell->undo_no = undo_no;
				cell->in_use = TRUE;

				arr->n_used++;

				stored_here = cell;
			}
		} else {
			n++;

			if (0 == ut_dulint_cmp(cell->undo_no, undo_no)) {

				if (stored_here) {
					stored_here->in_use = FALSE;
					ut_ad(arr->n_used > 0);
					arr->n_used--;
				}

				ut_ad(arr->n_used == n_used);

				return(FALSE);
			}
		}

		if (n == n_used && stored_here) {

			ut_ad(arr->n_used == 1 + n_used);

			return(TRUE);
		}
	}
}

/*******************************************************************//**
Removes an undo number from the array. */
static
void
trx_undo_arr_remove_info(
/*=====================*/
	trx_undo_arr_t*	arr,	/*!< in: undo number array */
	undo_no_t	undo_no)/*!< in: undo number */
{
	trx_undo_inf_t*	cell;
	ulint		n_used;
	ulint		n;
	ulint		i;

	n_used = arr->n_used;
	n = 0;

	for (i = 0;; i++) {
		cell = trx_undo_arr_get_nth_info(arr, i);

		if (cell->in_use
		    && 0 == ut_dulint_cmp(cell->undo_no, undo_no)) {

			cell->in_use = FALSE;

			ut_ad(arr->n_used > 0);

			arr->n_used--;

			return;
		}
	}
}

/*******************************************************************//**
Gets the biggest undo number in an array.
@return	biggest value, ut_dulint_zero if the array is empty */
static
undo_no_t
trx_undo_arr_get_biggest(
/*=====================*/
	trx_undo_arr_t*	arr)	/*!< in: undo number array */
{
	trx_undo_inf_t*	cell;
	ulint		n_used;
	undo_no_t	biggest;
	ulint		n;
	ulint		i;

	n = 0;
	n_used = arr->n_used;
	biggest = ut_dulint_zero;

	for (i = 0;; i++) {
		cell = trx_undo_arr_get_nth_info(arr, i);

		if (cell->in_use) {
			n++;
			if (ut_dulint_cmp(cell->undo_no, biggest) > 0) {

				biggest = cell->undo_no;
			}
		}

		if (n == n_used) {
			return(biggest);
		}
	}
}

/***********************************************************************//**
Tries truncate the undo logs. */
UNIV_INTERN
void
trx_roll_try_truncate(
/*==================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	trx_undo_arr_t*	arr;
	undo_no_t	limit;
	undo_no_t	biggest;

	ut_ad(mutex_own(&(trx->undo_mutex)));
	ut_ad(mutex_own(&((trx->rseg)->mutex)));

	trx->pages_undone = 0;

	arr = trx->undo_no_arr;

	limit = trx->undo_no;

	if (arr->n_used > 0) {
		biggest = trx_undo_arr_get_biggest(arr);

		if (ut_dulint_cmp(biggest, limit) >= 0) {

			limit = ut_dulint_add(biggest, 1);
		}
	}

	if (trx->insert_undo) {
		trx_undo_truncate_end(trx, trx->insert_undo, limit);
	}

	if (trx->update_undo) {
		trx_undo_truncate_end(trx, trx->update_undo, limit);
	}
}

/***********************************************************************//**
Pops the topmost undo log record in a single undo log and updates the info
about the topmost record in the undo log memory struct.
@return	undo log record, the page s-latched */
static
trx_undo_rec_t*
trx_roll_pop_top_rec(
/*=================*/
	trx_t*		trx,	/*!< in: transaction */
	trx_undo_t*	undo,	/*!< in: undo log */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_t*		undo_page;
	ulint		offset;
	trx_undo_rec_t*	prev_rec;
	page_t*		prev_rec_page;

	ut_ad(mutex_own(&(trx->undo_mutex)));

	undo_page = trx_undo_page_get_s_latched(undo->space, undo->zip_size,
						undo->top_page_no, mtr);
	offset = undo->top_offset;

	/*	fprintf(stderr, "Thread %lu undoing trx %lu undo record %lu\n",
	os_thread_get_curr_id(), ut_dulint_get_low(trx->id),
	ut_dulint_get_low(undo->top_undo_no)); */

	prev_rec = trx_undo_get_prev_rec(undo_page + offset,
					 undo->hdr_page_no, undo->hdr_offset,
					 mtr);
	if (prev_rec == NULL) {

		undo->empty = TRUE;
	} else {
		prev_rec_page = page_align(prev_rec);

		if (prev_rec_page != undo_page) {

			trx->pages_undone++;
		}

		undo->top_page_no = page_get_page_no(prev_rec_page);
		undo->top_offset  = prev_rec - prev_rec_page;
		undo->top_undo_no = trx_undo_rec_get_undo_no(prev_rec);
	}

	return(undo_page + offset);
}

/********************************************************************//**
Pops the topmost record when the two undo logs of a transaction are seen
as a single stack of records ordered by their undo numbers. Inserts the
undo number of the popped undo record to the array of currently processed
undo numbers in the transaction. When the query thread finishes processing
of this undo record, it must be released with trx_undo_rec_release.
@return undo log record copied to heap, NULL if none left, or if the
undo number of the top record would be less than the limit */
UNIV_INTERN
trx_undo_rec_t*
trx_roll_pop_top_rec_of_trx(
/*========================*/
	trx_t*		trx,	/*!< in: transaction */
	undo_no_t	limit,	/*!< in: least undo number we need */
	roll_ptr_t*	roll_ptr,/*!< out: roll pointer to undo record */
	mem_heap_t*	heap)	/*!< in: memory heap where copied */
{
	trx_undo_t*	undo;
	trx_undo_t*	ins_undo;
	trx_undo_t*	upd_undo;
	trx_undo_rec_t*	undo_rec;
	trx_undo_rec_t*	undo_rec_copy;
	undo_no_t	undo_no;
	ibool		is_insert;
	trx_rseg_t*	rseg;
	ulint		progress_pct;
	mtr_t		mtr;

	rseg = trx->rseg;
try_again:
	mutex_enter(&(trx->undo_mutex));

	if (trx->pages_undone >= TRX_ROLL_TRUNC_THRESHOLD) {
		mutex_enter(&(rseg->mutex));

		trx_roll_try_truncate(trx);

		mutex_exit(&(rseg->mutex));
	}

	ins_undo = trx->insert_undo;
	upd_undo = trx->update_undo;

	if (!ins_undo || ins_undo->empty) {
		undo = upd_undo;
	} else if (!upd_undo || upd_undo->empty) {
		undo = ins_undo;
	} else if (ut_dulint_cmp(upd_undo->top_undo_no,
				 ins_undo->top_undo_no) > 0) {
		undo = upd_undo;
	} else {
		undo = ins_undo;
	}

	if (!undo || undo->empty
	    || (ut_dulint_cmp(limit, undo->top_undo_no) > 0)) {

		if ((trx->undo_no_arr)->n_used == 0) {
			/* Rollback is ending */

			mutex_enter(&(rseg->mutex));

			trx_roll_try_truncate(trx);

			mutex_exit(&(rseg->mutex));
		}

		mutex_exit(&(trx->undo_mutex));

		return(NULL);
	}

	if (undo == ins_undo) {
		is_insert = TRUE;
	} else {
		is_insert = FALSE;
	}

	*roll_ptr = trx_undo_build_roll_ptr(is_insert, (undo->rseg)->id,
					    undo->top_page_no,
					    undo->top_offset);
	mtr_start(&mtr);

	undo_rec = trx_roll_pop_top_rec(trx, undo, &mtr);

	undo_no = trx_undo_rec_get_undo_no(undo_rec);

	ut_ad(ut_dulint_cmp(ut_dulint_add(undo_no, 1), trx->undo_no) == 0);

	/* We print rollback progress info if we are in a crash recovery
	and the transaction has at least 1000 row operations to undo. */

	if (trx == trx_roll_crash_recv_trx && trx_roll_max_undo_no > 1000) {

		progress_pct = 100 - (ulint)
			((ut_conv_dulint_to_longlong(undo_no) * 100)
			 / trx_roll_max_undo_no);
		if (progress_pct != trx_roll_progress_printed_pct) {
			if (trx_roll_progress_printed_pct == 0) {
				fprintf(stderr,
					"\nInnoDB: Progress in percents:"
					" %lu", (ulong) progress_pct);
			} else {
				fprintf(stderr,
					" %lu", (ulong) progress_pct);
			}
			fflush(stderr);
			trx_roll_progress_printed_pct = progress_pct;
		}
	}

	trx->undo_no = undo_no;

	if (!trx_undo_arr_store_info(trx, undo_no)) {
		/* A query thread is already processing this undo log record */

		mutex_exit(&(trx->undo_mutex));

		mtr_commit(&mtr);

		goto try_again;
	}

	undo_rec_copy = trx_undo_rec_copy(undo_rec, heap);

	mutex_exit(&(trx->undo_mutex));

	mtr_commit(&mtr);

	return(undo_rec_copy);
}

/********************************************************************//**
Reserves an undo log record for a query thread to undo. This should be
called if the query thread gets the undo log record not using the pop
function above.
@return	TRUE if succeeded */
UNIV_INTERN
ibool
trx_undo_rec_reserve(
/*=================*/
	trx_t*		trx,	/*!< in/out: transaction */
	undo_no_t	undo_no)/*!< in: undo number of the record */
{
	ibool	ret;

	mutex_enter(&(trx->undo_mutex));

	ret = trx_undo_arr_store_info(trx, undo_no);

	mutex_exit(&(trx->undo_mutex));

	return(ret);
}

/*******************************************************************//**
Releases a reserved undo record. */
UNIV_INTERN
void
trx_undo_rec_release(
/*=================*/
	trx_t*		trx,	/*!< in/out: transaction */
	undo_no_t	undo_no)/*!< in: undo number */
{
	trx_undo_arr_t*	arr;

	mutex_enter(&(trx->undo_mutex));

	arr = trx->undo_no_arr;

	trx_undo_arr_remove_info(arr, undo_no);

	mutex_exit(&(trx->undo_mutex));
}

/****************************************************************//**
Builds an undo 'query' graph for a transaction. The actual rollback is
performed by executing this query graph like a query subprocedure call.
The reply about the completion of the rollback will be sent by this
graph.
@return	own: the query graph */
static
que_t*
trx_roll_graph_build(
/*=================*/
	trx_t*	trx)	/*!< in: trx handle */
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;

	ut_ad(trx_mutex_own(trx));

	heap = mem_heap_create(512);
	fork = que_fork_create(NULL, NULL, QUE_FORK_ROLLBACK, heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap);

	thr->child = row_undo_node_create(trx, thr, heap);

	return(fork);
}

/*********************************************************************//**
Starts a rollback operation.
@return query graph that will perform the UNDO operations. */
static
que_thr_t*
trx_rollback_start(
/*===============*/
	trx_t*		trx,	/*!< in: transaction */
	dulint		roll_limit) /*!< in: rollback to undo no */
{
	que_t*		roll_graph;

	ut_ad(trx_mutex_own(trx));

	ut_ad(trx->undo_no_arr == NULL || trx->undo_no_arr->n_used == 0);

	/* Initialize the rollback field in the transaction */

	trx->roll_limit = roll_limit;

	ut_a(ut_dulint_cmp(trx->roll_limit, trx->undo_no) <= 0);

	trx->pages_undone = 0;

	if (trx->undo_no_arr == NULL) {
		/* Single query thread -> 1 */
		trx->undo_no_arr = trx_undo_arr_create(1);
	}

	/* Build a 'query' graph which will perform the undo operations */

	roll_graph = trx_roll_graph_build(trx);

	trx->graph = roll_graph;
	trx->que_state = TRX_QUE_ROLLING_BACK;

	return(que_fork_start_command(roll_graph));
}

/****************************************************************//**
Finishes a transaction rollback. */
static
void
trx_finish_rollback(
/*================*/
	trx_t*		trx)	/*!< in: transaction */
{
	trx_mutex_enter(trx);

	ut_a(trx->undo_no_arr == NULL || trx->undo_no_arr->n_used == 0);

	trx_mutex_exit(trx);

	trx_commit(trx);

	trx_mutex_enter(trx);

	trx->que_state = TRX_QUE_RUNNING;

	trx_mutex_exit(trx);
}

/*********************************************************************//**
Creates a rollback command node struct.
@return	own: rollback node struct */
UNIV_INTERN
roll_node_t*
roll_node_create(
/*=============*/
	mem_heap_t*	heap)	/*!< in: mem heap where created */
{
	roll_node_t*	node;

	node = mem_heap_zalloc(heap, sizeof(*node));

	node->state = ROLL_NODE_SEND;

	node->common.type = QUE_NODE_ROLLBACK;

	return(node);
}

/***********************************************************//**
Performs an execution step for a rollback command node in a query graph.
@return	query thread to run next, or NULL */
UNIV_INTERN
que_thr_t*
trx_rollback_step(
/*==============*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	roll_node_t*	node;

	node = thr->run_node;

	ut_ad(que_node_get_type(node) == QUE_NODE_ROLLBACK);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = ROLL_NODE_SEND;
	}

	if (node->state == ROLL_NODE_SEND) {
		trx_t*		trx;
		dulint		roll_limit = ut_dulint_zero;

		trx = thr_get_trx(thr);

		trx_mutex_enter(trx);

		node->state = ROLL_NODE_WAIT;

		ut_a(node->undo_thr == NULL);

		roll_limit = node->partial
			? node->savept.least_undo_no : ut_dulint_zero;

		trx_commit_or_rollback_prepare(trx);

		node->undo_thr = trx_rollback_start(trx, roll_limit);

		trx_mutex_exit(trx);

	} else {
		ut_ad(node->state == ROLL_NODE_WAIT);

		thr->run_node = que_node_get_parent(node);
	}

	return(thr);
}
