/*****************************************************************************

Copyright (c) 2014, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file gis/gis0rtree.cc
InnoDB R-tree interfaces

Created 2013/03/27 Allen Lai and Jimmy Yang
***********************************************************************/

#include "fsp0fsp.h"
#include "page0page.h"
#include "page0cur.h"
#include "page0zip.h"
#include "gis0rtree.h"

#ifndef UNIV_HOTBACKUP
#include "btr0cur.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "ibuf0ibuf.h"
#include "trx0trx.h"
#include "srv0mon.h"
#include "gis0geo.h"

#endif /* UNIV_HOTBACKUP */

/*************************************************************//**
Initial split nodes info for R-tree split.
@return initialized split nodes array */
static
rtr_split_node_t*
rtr_page_split_initialize_nodes(
/*============================*/
	mem_heap_t*	heap,	/*!< in: pointer to memory heap, or NULL */
	btr_cur_t*	cursor,	/*!< in: cursor at which to insert; when the
				function returns, the cursor is positioned
				on the predecessor of the inserted record */
	ulint**		offsets,/*!< in: offsets on inserted record */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	double**	buf_pos)/*!< in/out: current buffer position */
{
	rtr_split_node_t*	split_node_array;
	double*			buf;
	ulint			n_recs;
	rtr_split_node_t*	task;
	rtr_split_node_t*	stop;
	rtr_split_node_t*	cur;
	rec_t*			rec;
	buf_block_t*		block;
	page_t*			page;
	ulint			n_uniq;
	ulint			len;
	byte*			source_cur;

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	n_uniq = dict_index_get_n_unique_in_tree(cursor->index);

	n_recs = page_get_n_recs(page) + 1;

	/*We reserve 2 MBRs memory space for temp result of split
	algrithm. And plus the new mbr that need to insert, we
	need (n_recs + 3)*MBR size for storing all MBRs.*/
	buf = static_cast<double*>(mem_heap_alloc(
		heap, DATA_MBR_LEN * (n_recs + 3)
		+ sizeof(rtr_split_node_t) * (n_recs + 1)));

	split_node_array = (rtr_split_node_t*)(buf + SPDIMS * 2 * (n_recs + 3));
	task = split_node_array;
	*buf_pos = buf;
	stop = task + n_recs;

	rec = page_rec_get_next(page_get_infimum_rec(page));
	*offsets = rec_get_offsets(rec, cursor->index, *offsets,
				   n_uniq, &heap);

	source_cur = rec_get_nth_field(rec, *offsets, 0, &len);

	for (cur = task; cur < stop - 1; ++cur) {
		cur->coords = reserve_coords(buf_pos, SPDIMS);
		cur->key = rec;

		memcpy(cur->coords, source_cur, DATA_MBR_LEN);

		rec = page_rec_get_next(rec);
		*offsets = rec_get_offsets(rec, cursor->index, *offsets,
					   n_uniq, &heap);
		source_cur = rec_get_nth_field(rec, *offsets, 0, &len);
	}

	/* Put the insert key to node list */
	source_cur = static_cast<byte*>(dfield_get_data(
		dtuple_get_nth_field(tuple, 0)));
	cur->coords = reserve_coords(buf_pos, SPDIMS);
	rec = (byte*) mem_heap_alloc(
		heap, rec_get_converted_size(cursor->index, tuple, 0));

	rec = rec_convert_dtuple_to_rec(rec, cursor->index, tuple, 0);
	cur->key = rec;

	memcpy(cur->coords, source_cur, DATA_MBR_LEN);

	return split_node_array;
}

/**********************************************************************//**
Builds a Rtree node pointer out of a physical record and a page number.
@return	own: node pointer */

dtuple_t*
rtr_index_build_node_ptr(
/*=====================*/
	const dict_index_t*	index,	/*!< in: index */
	const rtr_mbr_t*	mbr,	/*!< in: mbr of lower page */
	const rec_t*		rec,	/*!< in: record for which to build node
					pointer */
	ulint			page_no,/*!< in: page number to put in node
					pointer */
	mem_heap_t*		heap,	/*!< in: memory heap where pointer
					created */
	ulint			level)	/*!< in: level of rec in tree:
					0 means leaf level */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	byte*		buf;
	ulint		n_unique;

	n_unique = dict_index_get_n_unique_in_tree(index);

	tuple = dtuple_create(heap, n_unique + 1);

	/* When searching in the tree for the node pointer, we must not do
	comparison on the last field, the page number field, as on upper
	levels in the tree there may be identical node pointers with a
	different page number; therefore, we set the n_fields_cmp to one
	less: */

	dtuple_set_n_fields_cmp(tuple, n_unique);

	dict_index_copy_types(tuple, index, n_unique);

	/* Write page no field */
	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, page_no);

	field = dtuple_get_nth_field(tuple, n_unique);
	dfield_set_data(field, buf, 4);

	dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4);

	/* Copy the prefix */
	rec_copy_prefix_to_dtuple(tuple, rec, index, n_unique, heap);
	dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple)
				    | REC_STATUS_NODE_PTR);

	/* Set mbr as index entry data */
	field = dtuple_get_nth_field(tuple, 0);

	buf = static_cast<byte*>(mem_heap_alloc(heap, DATA_MBR_LEN));

	rtr_write_mbr(buf, mbr);

	dfield_set_data(field, buf, DATA_MBR_LEN);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/**************************************************************//**
In-place update the mbr field of a spatial index row.
@return true if update is successful */
static
bool
rtr_update_mbr_field_in_place(
/*==========================*/
	dict_index_t*	index,		/*!< in: spatial index. */
	rec_t*		rec,		/*!< in/out: rec to be modified.*/
	ulint*		offsets,	/*!< in/out: offsets on rec. */
	rtr_mbr_t*	mbr,		/*!< in: the new mbr. */
	mtr_t*		mtr)		/*!< in: mtr */
{
	void*		new_mbr_ptr;
	double		new_mbr[SPDIMS * 2];
	byte*		log_ptr;
	page_t*		page = page_align(rec);
	ulint		len = DATA_MBR_LEN;
	ulint		flags = BTR_NO_UNDO_LOG_FLAG
			| BTR_NO_LOCKING_FLAG
			| BTR_KEEP_SYS_FLAG;
	ulint		rec_info;

	rtr_write_mbr(reinterpret_cast<byte*>(&new_mbr), mbr);
	new_mbr_ptr = static_cast<void*>(new_mbr);
	/* Otherwise, set the mbr to the new_mbr. */
	rec_set_nth_field(rec, offsets, 0, new_mbr_ptr, len);

	rec_info = rec_get_info_bits(rec, rec_offs_comp(offsets));

	/* Write redo log. */
	/* For now, we use LOG_REC_UPDATE_IN_PLACE to log this enlarge.
	In the future, we may need to add a new log type for this. */
	log_ptr = mlog_open_and_write_index(mtr, rec, index, page_is_comp(page)
					    ? MLOG_COMP_REC_UPDATE_IN_PLACE
					    : MLOG_REC_UPDATE_IN_PLACE,
					    1 + DATA_ROLL_PTR_LEN + 14 + 2
					    + MLOG_BUF_MARGIN);

	if (!log_ptr) {
		/* Logging in mtr is switched off during
		crash recovery */
		return(false);
	}

	/* Flags */
	mach_write_to_1(log_ptr, flags);
	log_ptr++;
	/* TRX_ID Position */
	log_ptr += mach_write_compressed(log_ptr, 0);
	/* ROLL_PTR */
	trx_write_roll_ptr(log_ptr, 0);
	log_ptr += DATA_ROLL_PTR_LEN;
	/* TRX_ID */
	log_ptr += mach_u64_write_compressed(log_ptr, 0);

	/* Offset */
	mach_write_to_2(log_ptr, page_offset(rec));
	log_ptr += 2;
	/* Info bits */
	mach_write_to_1(log_ptr, rec_info);
	log_ptr++;
	/* N fields */
	log_ptr += mach_write_compressed(log_ptr, 1);
	/* Field no, len */
	log_ptr += mach_write_compressed(log_ptr, 0);
	log_ptr += mach_write_compressed(log_ptr, len);
	/* Data */
	memcpy(log_ptr, new_mbr_ptr, len);
	log_ptr += len;

	mlog_close(mtr, log_ptr);

	return(true);
}

/**************************************************************//**
Update the mbr field of a spatial index row.
@return true if update is successful */
bool
rtr_update_mbr_field(
/*=================*/
	btr_cur_t*	cursor,		/*!< in/out: cursor pointed to rec.*/
	ulint*		offsets,	/*!< in/out: offsets on rec. */
	btr_cur_t*	cursor2,	/*!< in/out: cursor pointed to rec
					that should be deleted.
					this cursor is for btr_compress to
					delete the merged page's father rec.*/
	page_t*		child_page,	/*!< in: child page. */
	rtr_mbr_t*	mbr,		/*!< in: the new mbr. */
	rec_t*		new_rec,	/*!< in: rec to use */
	mtr_t*		mtr)		/*!< in: mtr */
{
	dict_index_t*	index = cursor->index;
	mem_heap_t*	heap;
	page_t*		page;
	rec_t*		rec;
	ulint		flags = BTR_NO_UNDO_LOG_FLAG
			| BTR_NO_LOCKING_FLAG
			| BTR_KEEP_SYS_FLAG;
	dberr_t		err;
	big_rec_t*	dummy_big_rec;
	buf_block_t*	block;
	rec_t*		child_rec;
	ulint		up_match = 0;
	ulint		low_match = 0;
	ulint		child;
	ulint		level;
	ulint		rec_info;
	page_zip_des_t*	page_zip;
	bool		ins_suc = true;
	ulint		cur2_pos = 0;
	ulint		del_page_no = 0;
	ulint*		offsets2;

	rec = btr_cur_get_rec(cursor);
	page = page_align(rec);

	rec_info = rec_get_info_bits(rec, rec_offs_comp(offsets));

	heap = mem_heap_create(100);
	block = btr_cur_get_block(cursor);
	ut_ad(page == buf_block_get_frame(block));
	page_zip = buf_block_get_page_zip(block);

	child = btr_node_ptr_get_child_page_no(rec, offsets);
	level = btr_page_get_level(buf_block_get_frame(block), mtr);

	if (new_rec) {
		child_rec = new_rec;
	} else {
		child_rec = page_rec_get_next(page_get_infimum_rec(child_page));
	}

	dtuple_t* node_ptr = rtr_index_build_node_ptr(
		index, mbr, child_rec, child, heap, level);

	/* We need to remember the child page no of cursor2, since page could be
	reorganized or insert a new rec before it. */
	if (cursor2) {
		rec_t*	del_rec = btr_cur_get_rec(cursor2);
		offsets2 = rec_get_offsets(btr_cur_get_rec(cursor2),
					   index, NULL,
					   ULINT_UNDEFINED, &heap);
		del_page_no = btr_node_ptr_get_child_page_no(del_rec, offsets2);
		cur2_pos = page_rec_get_n_recs_before(btr_cur_get_rec(cursor2));
	}

	if (rec_info & REC_INFO_MIN_REC_FLAG) {
		/* When the rec is minimal rec in this level, we do
		 in-place update for avoiding it move to other place. */

		if (page_zip) {
			/* Check if there's enough space for in-place
			update the zip page. */
			if (!btr_cur_update_alloc_zip(
					page_zip,
					btr_cur_get_page_cur(cursor),
					index, offsets,
					rec_offs_size(offsets),
					false, mtr)) {

				/* If there's not enought space for
				inplace update zip page, we do delete
				insert. */
				ins_suc = false;

				/* Since btr_cur_update_alloc_zip could
				reorganize the page, we need to repositon
				cursor2. */
				if (cursor2) {
					cursor2->page_cur.rec =
						page_rec_get_nth(page,
								 cur2_pos);
				}

				goto update_mbr;
			}

		}

		if (!rtr_update_mbr_field_in_place(index, rec,
						   offsets, mbr, mtr)) {
			return(false);
		}

		if (page_zip) {
			page_zip_write_rec(page_zip, rec, index, offsets, 0);
		}

		if (cursor2) {
			ulint* offsets2;

			if (page_zip) {
				cursor2->page_cur.rec
					= page_rec_get_nth(page, cur2_pos);
			}
			offsets2 = rec_get_offsets(btr_cur_get_rec(cursor2),
						   index, NULL,
						   ULINT_UNDEFINED, &heap);
			ut_ad(del_page_no == btr_node_ptr_get_child_page_no(
							cursor2->page_cur.rec,
							offsets2));

			page_cur_delete_rec(btr_cur_get_page_cur(cursor2),
					    index, offsets2, mtr);
		}
	} else if (page_get_n_recs(page) == 1) {
		/* When there's only one rec in the page, we do insert/delete to
		avoid page merge. */

		page_cur_t		page_cur;
		rec_t*			insert_rec;
		ulint*			insert_offsets = NULL;
		ulint			old_pos;
		rec_t*			old_rec;

		ut_ad(cursor2 == NULL);

		/* Insert the new mbr rec. */
		old_pos = page_rec_get_n_recs_before(rec);

		err = btr_cur_optimistic_insert(
			flags,
			cursor, &insert_offsets, &heap,
			node_ptr, &insert_rec, &dummy_big_rec, 0, NULL, mtr);

		ut_ad(err == DB_SUCCESS);

		btr_cur_position(index, insert_rec, block, cursor);

		/* Delete the old mbr rec. */
		old_rec = page_rec_get_nth(page, old_pos);
		ut_ad(old_rec != insert_rec);

		page_cur_position(old_rec, block, &page_cur);
		offsets2 = rec_get_offsets(old_rec,
					   index, NULL,
					   ULINT_UNDEFINED, &heap);
		page_cur_delete_rec(&page_cur, index, offsets2, mtr);

	} else {
update_mbr:
		/* When there're not only 1 rec in the page, we do delete/insert
		to avoid page split. */
		rec_t*			insert_rec;
		ulint*			insert_offsets = NULL;
		rec_t*			next_rec;

		/* Delete the rec which cursor point to. */
		next_rec = page_rec_get_next(rec);
		page_cur_delete_rec(btr_cur_get_page_cur(cursor),
				    index, offsets, mtr);
		if (!ins_suc) {
			ut_ad(rec_info & REC_INFO_MIN_REC_FLAG);

			btr_set_min_rec_mark(next_rec, mtr);
		}

		/* If there's more than 1 rec left in the page, delete
		the rec which cursor2 point to. Otherwise, delete it later.*/
		if (cursor2 && page_get_n_recs(page) > 1) {
			ulint		cur2_rec_info;
			rec_t*		cur2_rec;

			cur2_rec = cursor2->page_cur.rec;
			offsets2 = rec_get_offsets(cur2_rec, index, NULL,
						   ULINT_UNDEFINED, &heap);
			
			cur2_rec_info = rec_get_info_bits(cur2_rec,
						rec_offs_comp(offsets2));
			if (cur2_rec_info & REC_INFO_MIN_REC_FLAG) {
				/* If we delete the leftmost node
				pointer on a non-leaf level, we must
				mark the new leftmost node pointer as
				the predefined minimum record */
				rec_t*	next_rec = page_rec_get_next(cur2_rec);
				btr_set_min_rec_mark(next_rec, mtr);
			}

			ut_ad(del_page_no
			      == btr_node_ptr_get_child_page_no(cur2_rec,
								offsets2));
			page_cur_delete_rec(btr_cur_get_page_cur(cursor2),
					    index, offsets2, mtr);
			cursor2 = NULL;
		}

		/* Insert the new rec. */
		page_cur_search_with_match(block, index, node_ptr,
					   PAGE_CUR_LE , &up_match, &low_match,
					   btr_cur_get_page_cur(cursor), NULL);

		err = btr_cur_optimistic_insert(flags, cursor, &insert_offsets,
						&heap, node_ptr, &insert_rec,
						&dummy_big_rec, 0, NULL, mtr);

		if (!ins_suc && err == DB_SUCCESS) {
			ins_suc = true;
		}

		/* If optimistic insert fail, try reorganize the page
		and insert again. */
		if (err != DB_SUCCESS && ins_suc) {
			btr_page_reorganize(btr_cur_get_page_cur(cursor),
					    index, mtr);

			err = btr_cur_optimistic_insert(flags,
							cursor,
							&insert_offsets,
							&heap,
							node_ptr,
							&insert_rec,
							&dummy_big_rec,
							0, NULL, mtr);

			/* Will do pessimistic insert */
			if (err != DB_SUCCESS) {
				ins_suc = false;
			}
		}

		/* Insert succeed, position cursor the inserted rec.*/
		if (ins_suc) {
			btr_cur_position(index, insert_rec, block, cursor);
			offsets = rec_get_offsets(insert_rec,
						  index, offsets,
						  ULINT_UNDEFINED, &heap);
		}

		/* Delete the rec which cursor2 point to. */
		if (cursor2) {
			ulint		cur2_pno;
			rec_t*		cur2_rec;

			cursor2->page_cur.rec = page_rec_get_nth(page,
								 cur2_pos);

			cur2_rec = btr_cur_get_rec(cursor2);

			offsets2 = rec_get_offsets(cur2_rec, index, NULL,
						   ULINT_UNDEFINED, &heap);

			/* If the cursor2 position is on a wrong rec, we
			need to reposition it. */
			cur2_pno = btr_node_ptr_get_child_page_no(cur2_rec, offsets2);
			if ((del_page_no != cur2_pno)
			    || (cur2_rec == insert_rec)) {
				cur2_rec = page_rec_get_next(
					page_get_infimum_rec(page));

				while (!page_rec_is_supremum(cur2_rec)) {
					offsets2 = rec_get_offsets(cur2_rec, index,
								   NULL,
								   ULINT_UNDEFINED,
								   &heap);
					cur2_pno = btr_node_ptr_get_child_page_no(
							cur2_rec, offsets2);
					if (cur2_pno == del_page_no) {
						if (insert_rec != cur2_rec) {
							cursor2->page_cur.rec =
								cur2_rec;
							break;
						}
					}
					cur2_rec = page_rec_get_next(cur2_rec);
				}

				ut_ad(!page_rec_is_supremum(cur2_rec));
			}

			rec_info = rec_get_info_bits(cur2_rec,
						     rec_offs_comp(offsets2));
			if (rec_info & REC_INFO_MIN_REC_FLAG) {
				/* If we delete the leftmost node
				pointer on a non-leaf level, we must
				mark the new leftmost node pointer as
				the predefined minimum record */
				rec_t*	next_rec = page_rec_get_next(cur2_rec);
				btr_set_min_rec_mark(next_rec, mtr);
			}

			ut_ad(cur2_pno == del_page_no && cur2_rec != insert_rec);

			page_cur_delete_rec(btr_cur_get_page_cur(cursor2),
					    index, offsets2, mtr);
		}

		if (!ins_suc) {
			mem_heap_t*	new_heap = NULL;

			err = btr_cur_pessimistic_insert(
				flags,
				cursor, &insert_offsets, &new_heap,
				node_ptr, &insert_rec, &dummy_big_rec,
				0, NULL, mtr);

			ut_ad(err == DB_SUCCESS);

			if (new_heap) {
				mem_heap_free(new_heap);
			}

		}

		if (cursor2) {
			btr_cur_compress_if_useful(cursor, FALSE, mtr);
		}
	}

#ifdef UNIV_DEBUG
	ulint	left_page_no = btr_page_get_prev(page, mtr);

	if (left_page_no == FIL_NULL) {

		ut_a(REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
			page_rec_get_next(page_get_infimum_rec(page)),
			page_is_comp(page)));
	}
#endif
	mem_heap_free(heap);

	return(true);
}

/**************************************************************//**
Update parent page's MBR and Predicate lock information during a split */
static __attribute__((nonnull))
void
rtr_adjust_upper_level(
/*===================*/
	btr_cur_t*	sea_cur,	/*!< in: search cursor */
	ulint		flags,		/*!< in: undo logging and
					locking flags */
	buf_block_t*	block,		/*!< in/out: page to be split */
	buf_block_t*	new_block,	/*!< in/out: the new half page */
	rtr_mbr_t*	mbr,		/*!< in: MBR on the old page */
	rtr_mbr_t*	new_mbr,	/*!< in: MBR on the new page */
	ulint		direction,	/*!< in: FSP_UP or FSP_DOWN */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		page;
	page_t*		new_page;
	ulint		page_no;
	ulint		new_page_no;
	page_zip_des_t*	page_zip;
	page_zip_des_t*	new_page_zip;
	dict_index_t*	index = sea_cur->index;
	btr_cur_t	cursor;
	ulint*		offsets;
	mem_heap_t*	heap;
	ulint		level;
	dtuple_t*	node_ptr_upper;
	ulint		prev_page_no;
	ulint		next_page_no;
	ulint		space;
	page_cur_t*	page_cursor;
	rtr_mbr_t	parent_mbr;
	lock_prdt_t	prdt;
	lock_prdt_t	new_prdt;
	lock_prdt_t	parent_prdt;
	dberr_t		err;
	big_rec_t*	dummy_big_rec;
	rec_t*		rec;

	/* Create a memory heap where the data tuple is stored */
	heap = mem_heap_create(1024);
	memset(&cursor, 0, sizeof(cursor));

	cursor.thr = sea_cur->thr;

	/* Get the level of the split pages */
	level = btr_page_get_level(buf_block_get_frame(block), mtr);
	ut_ad(level
	      == btr_page_get_level(buf_block_get_frame(new_block), mtr));

	page = buf_block_get_frame(block);
	page_no = block->page.id.page_no();
	page_zip = buf_block_get_page_zip(block);

	new_page = buf_block_get_frame(new_block);
	new_page_no = new_block->page.id.page_no();
	new_page_zip = buf_block_get_page_zip(new_block);

	/* Set new mbr for the old page on the upper level. */
	/* Look up the index for the node pointer to page */
	offsets = rtr_page_get_father_block(
		NULL, heap, index, block, mtr, sea_cur, &cursor);

	page_cursor = btr_cur_get_page_cur(&cursor);

	rtr_get_mbr_from_rec(page_cursor->rec, offsets, &parent_mbr);

	rtr_update_mbr_field(&cursor, offsets, NULL, page, mbr, NULL, mtr);

	/* Already updated parent MBR, reset in our path */
	if (sea_cur->rtr_info) {
		node_visit_t*	node_visit = rtr_get_parent_node(
						sea_cur, level + 1, true);
		if (node_visit) {
			node_visit->mbr_inc = 0;
		}
	}

	/* Insert the node for the new page. */
	node_ptr_upper = rtr_index_build_node_ptr(
		index, new_mbr,
		page_rec_get_next(page_get_infimum_rec(new_page)),
		new_page_no, heap, level);

	ulint	up_match = 0;
	ulint	low_match = 0;

	buf_block_t*	father_block = btr_cur_get_block(&cursor);

	page_cur_search_with_match(
		father_block, index, node_ptr_upper,
		PAGE_CUR_LE , &up_match, &low_match,
		btr_cur_get_page_cur(&cursor), NULL);

	err = btr_cur_optimistic_insert(
		flags
		| BTR_NO_LOCKING_FLAG
		| BTR_KEEP_SYS_FLAG
		| BTR_NO_UNDO_LOG_FLAG,
		&cursor, &offsets, &heap,
		node_ptr_upper, &rec, &dummy_big_rec, 0, NULL, mtr);

	if (err == DB_FAIL) {
		ut_ad(!cursor.rtr_info);

		cursor.rtr_info = sea_cur->rtr_info;
		cursor.tree_height = sea_cur->tree_height;

		err = btr_cur_pessimistic_insert(flags
						 | BTR_NO_LOCKING_FLAG
						 | BTR_KEEP_SYS_FLAG
						 | BTR_NO_UNDO_LOG_FLAG,
						 &cursor, &offsets, &heap,
						 node_ptr_upper, &rec,
						 &dummy_big_rec, 0, NULL, mtr);
		cursor.rtr_info = NULL;
		ut_a(err == DB_SUCCESS);
	}

	prdt.data = static_cast<void*>(mbr);
	prdt.op = 0;
	new_prdt.data = static_cast<void*>(new_mbr);
	new_prdt.op = 0;
	parent_prdt.data = static_cast<void*>(&parent_mbr);
	parent_prdt.op = 0;

	lock_prdt_update_parent(block, new_block, &prdt, &new_prdt,
				&parent_prdt, dict_index_get_space(index),
				page_cursor->block->page.id.page_no());

	mem_heap_free(heap);

	/* Get the previous and next pages of page */
	prev_page_no = btr_page_get_prev(page, mtr);
	next_page_no = btr_page_get_next(page, mtr);
	space = block->page.id.space();
	const page_size_t&	page_size = dict_table_page_size(index->table);

	/* Update page links of the level */
	if (prev_page_no != FIL_NULL) {
		page_id_t	prev_page_id(space, prev_page_no);

		buf_block_t*	prev_block = btr_block_get(
			prev_page_id, page_size, RW_X_LATCH, index, mtr);
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(prev_block->frame) == page_is_comp(page));
		ut_a(btr_page_get_next(prev_block->frame, mtr)
		     == block->page.id.page_no());
#endif /* UNIV_BTR_DEBUG */

		btr_page_set_next(buf_block_get_frame(prev_block),
				  buf_block_get_page_zip(prev_block),
				  page_no, mtr);
	}

	if (next_page_no != FIL_NULL) {
		page_id_t	next_page_id(space, next_page_no);

		buf_block_t*	next_block = btr_block_get(
			next_page_id, page_size, RW_X_LATCH, index, mtr);
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(next_block->frame) == page_is_comp(page));
		ut_a(btr_page_get_prev(next_block->frame, mtr)
		     == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

		btr_page_set_prev(buf_block_get_frame(next_block),
				  buf_block_get_page_zip(next_block),
				  new_page_no, mtr);
	}

	btr_page_set_prev(page, page_zip, prev_page_no, mtr);
	btr_page_set_next(page, page_zip, new_page_no, mtr);

	btr_page_set_prev(new_page, new_page_zip, page_no, mtr);
	btr_page_set_next(new_page, new_page_zip, next_page_no, mtr);
}

/*************************************************************//**
Moves record list to another page for rtree splitting.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return TRUE on success; FALSE on compression failure */
ibool
rtr_split_page_move_rec_list(
/*=========================*/
	rtr_split_node_t*	node_array,	/*!< in: split node array. */
	int			first_rec_group,/*!< in: group number of the
						first rec. */
	buf_block_t*		new_block,	/*!< in/out: index page
						where to move */
	buf_block_t*		block,		/*!< in/out: page containing
						split_rec */
	rec_t*			first_rec,	/*!< in: first record not to
						move */
	dict_index_t*		index,		/*!< in: record descriptor */
	mem_heap_t*		heap,		/*!< in: pointer to memory
						heap, or NULL */
	mtr_t*			mtr)		/*!< in: mtr */
{
	rtr_split_node_t*	cur_split_node;
	rtr_split_node_t*	end_split_node;
	page_cur_t		page_cursor;
	page_cur_t		new_page_cursor;
	page_t*			page;
	page_t*			new_page;
	ulint			offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*			offsets		= offsets_;
	page_zip_des_t*		new_page_zip
		= buf_block_get_page_zip(new_block);
	rec_t*			rec;
	rec_t*			ret;

	rec_offs_init(offsets_);

	page_cur_set_before_first(block, &page_cursor);
	page_cur_set_before_first(new_block, &new_page_cursor);

	page = buf_block_get_frame(block);
	new_page = buf_block_get_frame(new_block);
	ret = page_rec_get_prev(page_get_supremum_rec(new_page));

	end_split_node = node_array + page_get_n_recs(page);

	mtr_log_t	log_mode = MTR_LOG_NONE;

	if (new_page_zip) {
		log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);
	}

	/* Insert the recs in group 2 to new page.  */
	for (cur_split_node = node_array;
	     cur_split_node < end_split_node; ++cur_split_node) {
		if (cur_split_node->n_node != first_rec_group) {
			lock_rec_store_on_page_infimum(
				block, cur_split_node->key);

			offsets = rec_get_offsets(cur_split_node->key,
						  index, offsets,
						  ULINT_UNDEFINED, &heap);

			ut_ad (cur_split_node->key != first_rec
			       || !page_is_leaf(page));

			rec = page_cur_insert_rec_low(
					page_cur_get_rec(&new_page_cursor),
					index,
					cur_split_node->key,
					offsets,
					mtr);

			ut_a(rec);

			lock_rec_restore_from_page_infimum(
				new_block, rec, block);

			page_cur_move_to_next(&new_page_cursor);
		}
	}

	/* Update PAGE_MAX_TRX_ID on the uncompressed page.
	Modifications will be redo logged and copied to the compressed
	page in page_zip_compress() or page_zip_reorganize() below.
	Multiple transactions cannot simultaneously operate on the
	same temp-table in parallel.
	max_trx_id is ignored for temp tables because it not required
	for MVCC. */
	if (dict_index_is_sec_or_ibuf(index)
	    && page_is_leaf(page)
	    && !dict_table_is_temporary(index->table)) {
		page_update_max_trx_id(new_block, NULL,
				       page_get_max_trx_id(page),
				       mtr);
	}

	if (new_page_zip) {
		mtr_set_log_mode(mtr, log_mode);

		if (!page_zip_compress(new_page_zip, new_page, index,
				       page_zip_level, NULL, mtr)) {
			ulint	ret_pos;

			/* Before trying to reorganize the page,
			store the number of preceding records on the page. */
			ret_pos = page_rec_get_n_recs_before(ret);
			/* Before copying, "ret" was the predecessor
			of the predefined supremum record.  If it was
			the predefined infimum record, then it would
			still be the infimum, and we would have
			ret_pos == 0. */

			if (UNIV_UNLIKELY
			    (!page_zip_reorganize(new_block, index, mtr))) {

				if (UNIV_UNLIKELY
				    (!page_zip_decompress(new_page_zip,
							  new_page, FALSE))) {
					ut_error;
				}
#ifdef UNIV_GIS_DEBUG
				ut_ad(page_validate(new_page, index));
#endif

				return(false);
			}

			/* The page was reorganized: Seek to ret_pos. */
			ret = page_rec_get_nth(new_page, ret_pos);
		}
	}

	/* Delete recs in second group from the old page. */
	for (cur_split_node = node_array;
	     cur_split_node < end_split_node; ++cur_split_node) {
		if (cur_split_node->n_node != first_rec_group) {
			page_cur_position(cur_split_node->key,
					  block, &page_cursor);
			offsets = rec_get_offsets(
				page_cur_get_rec(&page_cursor), index,
				offsets, ULINT_UNDEFINED,
				&heap);
			page_cur_delete_rec(&page_cursor,
				index, offsets, mtr);
		}
	}

	return(true);
}

/*************************************************************//**
Splits an R-tree index page to halves and inserts the tuple. It is assumed
that mtr holds an x-latch to the index tree. NOTE: the tree x-latch is
released within this function! NOTE that the operation of this
function must always succeed, we cannot reverse it: therefore enough
free disk space (2 pages) must be guaranteed to be available before
this function is called.
@return inserted record */

rec_t*
rtr_page_split_and_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in/out: cursor at which to insert; when the
				function returns, the cursor is positioned
				on the predecessor of the inserted record */
	ulint**		offsets,/*!< out: offsets on inserted record */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr)	/*!< in: mtr */
{
	buf_block_t*		block;
	page_t*			page;
	page_t*			new_page;
	ulint			page_no;
	byte			direction;
	ulint			hint_page_no;
	buf_block_t*		new_block;
	page_zip_des_t*		page_zip;
	page_zip_des_t*		new_page_zip;
	buf_block_t*		insert_block;
	page_cur_t*		page_cursor;
	rec_t*			rec = 0;
	ulint			n_recs;
	ulint			total_data;
	ulint			insert_size;
	rtr_split_node_t*	rtr_split_node_array;
	rtr_split_node_t*	cur_split_node;
	rtr_split_node_t*	end_split_node;
	double*			buf_pos;
	ulint			page_level;
	node_seq_t		current_ssn;
	node_seq_t		next_ssn;
	buf_block_t*		root_block;
	rtr_mbr_t		mbr;
	rtr_mbr_t		new_mbr;
	lock_prdt_t		prdt;
	lock_prdt_t		new_prdt;
	rec_t*			first_rec = NULL;
	int			first_rec_group = 1;

	if (!*heap) {
		*heap = mem_heap_create(1024);
	}

func_start:
	mem_heap_empty(*heap);
	*offsets = NULL;

	ut_ad(mtr_memo_contains_flagged(mtr, dict_index_get_lock(cursor->index),
					MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
	ut_ad(!dict_index_is_online_ddl(cursor->index)
	      || (flags & BTR_CREATE_FLAG)
	      || dict_index_is_clust(cursor->index));
#ifdef UNIV_SYNC_DEBUG
	ut_ad(rw_lock_own_flagged(dict_index_get_lock(cursor->index),
				  RW_LOCK_FLAG_X | RW_LOCK_FLAG_SX));
#endif /* UNIV_SYNC_DEBUG */

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	page_zip = buf_block_get_page_zip(block);
	page_level = btr_page_get_level(page, mtr);
	current_ssn = page_get_ssn_id(page);

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(page_get_n_recs(page) >= 1);

	page_no = block->page.id.page_no();

	if (btr_page_get_prev(page, mtr) == FIL_NULL && !page_is_leaf(page)) {
		first_rec = page_rec_get_next(
			page_get_infimum_rec(buf_block_get_frame(block)));
	}

	/* Initial split nodes array. */
	rtr_split_node_array = rtr_page_split_initialize_nodes(
		*heap, cursor, offsets, tuple, &buf_pos);

	/* Divide all mbrs to two groups. */
	n_recs = page_get_n_recs(page) + 1;

	end_split_node = rtr_split_node_array + n_recs;

#ifdef UNIV_GIS_DEBUG
	fprintf(stderr, "Before split a page:\n");
	for (cur_split_node = rtr_split_node_array;
		cur_split_node < end_split_node; ++cur_split_node) {
		for (int i = 0; i < SPDIMS * 2; i++) {
			fprintf(stderr, "%.2lf ",
			        *(cur_split_node->coords + i));
		}
		fprintf(stderr, "\n");
	}
#endif

	insert_size = rec_get_converted_size(cursor->index, tuple, n_ext);
	total_data = page_get_data_size(page) + insert_size;
	first_rec_group = split_rtree_node(rtr_split_node_array,
					   static_cast<int>(n_recs),
					   static_cast<int>(total_data),
					   static_cast<int>(insert_size),
					   0, 2, 2, &buf_pos, SPDIMS,
					   static_cast<uchar*>(first_rec));

	/* Allocate a new page to the index */
	direction = FSP_UP;
	hint_page_no = page_no + 1;
	new_block = btr_page_alloc(cursor->index, hint_page_no, direction,
				   page_level, mtr, mtr);
	new_page_zip = buf_block_get_page_zip(new_block);
	btr_page_create(new_block, new_page_zip, cursor->index,
			page_level, mtr);

	new_page = buf_block_get_frame(new_block);
	ut_ad(page_get_ssn_id(new_page) == 0);

	/* Set new ssn to the new page and page. */
	page_set_ssn_id(new_block, new_page_zip, current_ssn, mtr);
	next_ssn = rtr_get_new_ssn_id(cursor->index);

	page_set_ssn_id(block, page_zip, next_ssn, mtr);

	/* Keep recs in first group to the old page, move recs in second
	groups to the new page. */
	if (0
#ifdef UNIV_ZIP_COPY
	    || page_zip
#endif
	    || !rtr_split_page_move_rec_list(rtr_split_node_array,
					     first_rec_group,
					     new_block, block, first_rec,
					     cursor->index, *heap, mtr)) {
		ulint n = 0;
		/* For some reason, compressing new_page failed,
		even though it should contain fewer records than
		the original page.  Copy the page byte for byte
		and then delete the records from both pages
		as appropriate.  Deleting will always succeed. */
		ut_a(new_page_zip);

		page_zip_copy_recs(new_page_zip, new_page,
				   page_zip, page, cursor->index, mtr);

		page_cursor = btr_cur_get_page_cur(cursor);

		/* Delete recs in first group from the new page. */
		for (cur_split_node = rtr_split_node_array;
		     cur_split_node < end_split_node - 1; ++cur_split_node) {
			if (cur_split_node->n_node == first_rec_group) {
				ulint	pos;

				pos = page_rec_get_n_recs_before(
						cur_split_node->key);
				ut_a(pos > 0);
				rec_t* new_rec = page_rec_get_nth(new_page,
								  pos - n);

				ut_a(new_rec && page_rec_is_user_rec(new_rec));
				page_cur_position(new_rec, new_block,
						  page_cursor);

				*offsets = rec_get_offsets(
					page_cur_get_rec(page_cursor),
					cursor->index,
					*offsets, ULINT_UNDEFINED,
					heap);

				page_cur_delete_rec(page_cursor,
					cursor->index, *offsets, mtr);
				n++;
			}
		}

		/* Delete recs in second group from the old page. */
		for (cur_split_node = rtr_split_node_array;
		     cur_split_node < end_split_node - 1; ++cur_split_node) {
			if (cur_split_node->n_node != first_rec_group) {
				page_cur_position(cur_split_node->key,
						  block, page_cursor);
				*offsets = rec_get_offsets(
					page_cur_get_rec(page_cursor),
					cursor->index,
					*offsets, ULINT_UNDEFINED,
					heap);
				page_cur_delete_rec(page_cursor,
					cursor->index, *offsets, mtr);
			}
		}

#ifdef UNIV_GIS_DEBUG
		ut_ad(page_validate(new_page, cursor->index));
		ut_ad(page_validate(page, cursor->index));
#endif
	}

	/* Insert the new rec to the proper page. */
	cur_split_node = end_split_node - 1;
	if (cur_split_node->n_node != first_rec_group) {
		insert_block = new_block;
	} else {
		insert_block = block;
	}

	/* Reposition the cursor for insert and try insertion */
	page_cursor = btr_cur_get_page_cur(cursor);

	page_cur_search(insert_block, cursor->index, tuple,
			PAGE_CUR_LE, page_cursor);

	rec = page_cur_tuple_insert(page_cursor, tuple, cursor->index,
				    offsets, heap, n_ext, mtr);

	/* If insert did not fit, try page reorganization.
	For compressed pages, page_cur_tuple_insert() will have
	attempted this already. */
	if (rec == NULL) {
		if (!page_cur_get_page_zip(page_cursor)
		    && btr_page_reorganize(page_cursor, cursor->index, mtr)) {
			rec = page_cur_tuple_insert(page_cursor, tuple,
						    cursor->index, offsets,
						    heap, n_ext, mtr);

		}
		/* If insert fail, we will try to split the insert_block
		again. */
	}

	/* Calculate the mbr on the upper half-page, and the mbr on
	original page. */
	rtr_page_cal_mbr(cursor->index, block, &mbr, *heap);
	rtr_page_cal_mbr(cursor->index, new_block, &new_mbr, *heap);
	prdt.data = &mbr;
	new_prdt.data = &new_mbr;

	/* Check any predicate locks need to be moved/copied to the
	new page */
	lock_prdt_update_split(block, new_block, &prdt, &new_prdt,
			       dict_index_get_space(cursor->index), page_no);

	/* Adjust the upper level. */
	rtr_adjust_upper_level(cursor, flags, block, new_block,
			       &mbr, &new_mbr, direction, mtr);

	/* Save the new ssn to the root page, since we need to reinit
	the first ssn value from it after restart server. */

	root_block = btr_root_block_get(cursor->index, RW_SX_LATCH, mtr);

	page_zip = buf_block_get_page_zip(root_block);
	page_set_ssn_id(root_block, page_zip, next_ssn, mtr);

	/* Insert fit on the page: update the free bits for the
	left and right pages in the same mtr */

	if (page_is_leaf(page)) {
		ibuf_update_free_bits_for_two_pages_low(
			block, new_block, mtr);
	}

	/* If the new res insert fail, we need to do another split
	 again. */
	if (!rec) {
		/* We play safe and reset the free bits for new_page */
		if (!dict_index_is_clust(cursor->index)
		    && !dict_table_is_temporary(cursor->index->table)) {
			ibuf_reset_free_bits(new_block);
			ibuf_reset_free_bits(block);
		}

		*offsets = rtr_page_get_father_block(
                                NULL, *heap, cursor->index, block, mtr,
				NULL, cursor);

		rec_t* i_rec = page_rec_get_next(page_get_infimum_rec(
			buf_block_get_frame(block)));
		btr_cur_position(cursor->index, i_rec, block, cursor);

		goto func_start;
	}

#ifdef UNIV_GIS_DEBUG
	ut_ad(page_validate(buf_block_get_frame(block), cursor->index));
	ut_ad(page_validate(buf_block_get_frame(new_block), cursor->index));

	ut_ad(!rec || rec_offs_validate(rec, cursor->index, *offsets));
#endif
	MONITOR_INC(MONITOR_INDEX_SPLIT);

	return(rec);
}

/****************************************************************//**
Following the right link to find the proper block for insert.
@return the proper block.*/

dberr_t
rtr_ins_enlarge_mbr(
/*================*/
	btr_cur_t*		btr_cur,	/*!< in: btr cursor */
	que_thr_t*		thr,		/*!< in: query thread */
	mtr_t*			mtr)		/*!< in: mtr */
{
	dberr_t			err = DB_SUCCESS;
	rtr_mbr_t		new_mbr;
	buf_block_t*		block;
	mem_heap_t*		heap;
	dict_index_t*		index = btr_cur->index;
	page_cur_t*		page_cursor;
	ulint*			offsets;
	node_visit_t*		node_visit;
	btr_cur_t		cursor;
	page_t*			page;

	ut_ad(dict_index_is_spatial(index));

	/* If no rtr_info or rtree is one level tree, return. */
	if (!btr_cur->rtr_info || btr_cur->tree_height == 1) {
		return(err);
	}

	/* Check path info is not empty. */
	ut_ad(!btr_cur->rtr_info->parent_path->empty());

	/* Create a memory heap. */
	heap = mem_heap_create(1024);

	/* Leaf level page is stored in cursor */
	page_cursor = btr_cur_get_page_cur(btr_cur);
	block = page_cur_get_block(page_cursor);

	for (ulint i = 1; i < btr_cur->tree_height; i++) {
		node_visit = rtr_get_parent_node(btr_cur, i, true);
		ut_ad(node_visit != NULL);

		/* If there's no mbr enlarge, return.*/
		if (node_visit->mbr_inc == 0) {
			block = btr_pcur_get_block(node_visit->cursor);
			continue;
		}

		/* Calculate the mbr of the child page. */
		rtr_page_cal_mbr(index, block, &new_mbr, heap);

		/* Get father block. */
		memset(&cursor, 0, sizeof(cursor));
		offsets = rtr_page_get_father_block(
			NULL, heap, index, block, mtr, btr_cur, &cursor);

		page = buf_block_get_frame(block);

		/* Update the mbr field of the rec. */
		if (!rtr_update_mbr_field(&cursor, offsets, NULL, page,
					  &new_mbr, NULL, mtr)) {
			err = DB_ERROR;
			break;
		}

		page_cursor = btr_cur_get_page_cur(&cursor);
		block = page_cur_get_block(page_cursor);
	}

	mem_heap_free(heap);

	return(err);
}

/*************************************************************//**
Copy recs from a page to new_block of rtree.
Differs from page_copy_rec_list_end, because this function does not
touch the lock table and max trx id on page or compress the page.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit(). */

void
rtr_page_copy_rec_list_end_no_locks(
/*================================*/
	buf_block_t*	new_block,	/*!< in: index page to copy to */
	buf_block_t*	block,		/*!< in: index page of rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	rtr_rec_move_t*	rec_move,	/*!< in: recording records moved */
	ulint		max_move,	/*!< in: num of rec to move */
	ulint*		num_moved,	/*!< out: num of rec to move */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		new_page	= buf_block_get_frame(new_block);
	page_cur_t	page_cur;
	page_cur_t	cur1;
	rec_t*		cur_rec;
	dtuple_t*	tuple;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets		= offsets_;
	ulint		n_fields = 0;
	ulint		moved = 0;
	bool		is_leaf = page_is_leaf(new_page);

	rec_offs_init(offsets_);

	page_cur_position(rec, block, &cur1);

	if (page_cur_is_before_first(&cur1)) {

		page_cur_move_to_next(&cur1);
	}

	btr_assert_not_corrupted(new_block, index);
	ut_a(page_is_comp(new_page) == page_rec_is_comp(rec));
	ut_a(mach_read_from_2(new_page + UNIV_PAGE_SIZE - 10) == (ulint)
	     (page_is_comp(new_page) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM));

	cur_rec = page_rec_get_next(
		page_get_infimum_rec(buf_block_get_frame(new_block)));
	page_cur_position(cur_rec, new_block, &page_cur);

	n_fields = dict_index_get_n_fields(index);

	/* Copy records from the original page to the new page */
	while (!page_cur_is_after_last(&cur1)) {
		rec_t*	cur1_rec = page_cur_get_rec(&cur1);
		rec_t*	ins_rec;

		/* Find the place to insert. */
		tuple = dict_index_build_data_tuple(index, cur1_rec,
						    n_fields, heap);

		if (page_rec_is_infimum(cur_rec)) {
			cur_rec = page_rec_get_next(cur_rec);
		}

		while (!page_rec_is_supremum(cur_rec)) {
			ulint		cur_matched_fields = 0;
			int		cmp;

			offsets = rec_get_offsets(
					cur_rec, index, offsets,
					dtuple_get_n_fields_cmp(tuple), &heap);
			cmp = cmp_dtuple_rec_with_match(tuple, cur_rec, offsets,
							&cur_matched_fields);
			if (cmp < 0) {
				page_cur_move_to_prev(&page_cur);
				break;
			} else if (cmp > 0) {
				/* Skip small recs. */
				page_cur_move_to_next(&page_cur);
				cur_rec = page_cur_get_rec(&page_cur);
			} else if (is_leaf) {
				if (rec_get_deleted_flag(cur1_rec,
					dict_table_is_comp(index->table))) {
					goto next;
				} else {
					btr_rec_set_deleted_flag(
						cur_rec,
						buf_block_get_page_zip(
							new_block),
						FALSE);
					goto next;
				}
			}
		}

		/* If position is on suprenum rec, need to move to
		previous rec. */
		if (page_rec_is_supremum(cur_rec)) {
			page_cur_move_to_prev(&page_cur);
		}

		cur_rec = page_cur_get_rec(&page_cur);

		offsets = rec_get_offsets(cur1_rec, index, offsets,
					  ULINT_UNDEFINED, &heap);

		ins_rec = page_cur_insert_rec_low(cur_rec, index,
						  cur1_rec, offsets, mtr);
		if (UNIV_UNLIKELY(!ins_rec)) {
			buf_page_print(new_page, univ_page_size,
				       BUF_PAGE_PRINT_NO_CRASH);
			buf_page_print(page_align(rec), univ_page_size,
				       BUF_PAGE_PRINT_NO_CRASH);

			fprintf(stderr, "page number %ld and %ld\n",
				(long)new_block->page.id.page_no(),
				(long)block->page.id.page_no());
			ib_logf(IB_LOG_LEVEL_FATAL,
				"rec offset %lu, cur1 offset %lu,"
				" cur_rec offset %lu",
				(ulong) page_offset(rec),
				(ulong) page_offset(page_cur_get_rec(&cur1)),
				(ulong) page_offset(cur_rec));
		}

		rec_move[moved].new_rec = ins_rec;
		rec_move[moved].old_rec = cur1_rec;
		rec_move[moved].moved = false;
		moved++;
next:
		if (moved > max_move) {
			ut_ad(0);
			break;
		}

		page_cur_move_to_next(&cur1);
	}

	*num_moved = moved;
}

/*************************************************************//**
Copy recs till a specified rec from a page to new_block of rtree. */

void
rtr_page_copy_rec_list_start_no_locks(
/*==================================*/
	buf_block_t*	new_block,	/*!< in: index page to copy to */
	buf_block_t*	block,		/*!< in: index page of rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	rtr_rec_move_t*	rec_move,	/*!< in: recording records moved */
	ulint		max_move,	/*!< in: num of rec to move */
	ulint*		num_moved,	/*!< out: num of rec to move */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_cur_t	cur1;
	rec_t*		cur_rec;
	dtuple_t*	tuple;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets	= offsets_;
	ulint		n_fields = 0;
	page_cur_t	page_cur;
	ulint		moved = 0;
	bool		is_leaf = page_is_leaf(buf_block_get_frame(block));

	rec_offs_init(offsets_);

	n_fields = dict_index_get_n_fields(index);

	page_cur_set_before_first(block, &cur1);
	page_cur_move_to_next(&cur1);

	cur_rec = page_rec_get_next(
		page_get_infimum_rec(buf_block_get_frame(new_block)));
	page_cur_position(cur_rec, new_block, &page_cur);

	while (page_cur_get_rec(&cur1) != rec) {
		rec_t*	cur1_rec = page_cur_get_rec(&cur1);
		rec_t*	ins_rec;

		/* Find the place to insert. */
		tuple = dict_index_build_data_tuple(index, cur1_rec,
						    n_fields, heap);

		if (page_rec_is_infimum(cur_rec)) {
			cur_rec = page_rec_get_next(cur_rec);
		}

		while (!page_rec_is_supremum(cur_rec)) {
			ulint		cur_matched_fields = 0;
			int		cmp;

			offsets = rec_get_offsets(cur_rec, index, offsets,
						  dtuple_get_n_fields_cmp(tuple),
						  &heap);
			cmp = cmp_dtuple_rec_with_match(tuple, cur_rec, offsets,
							&cur_matched_fields);
			if (cmp < 0) {
				page_cur_move_to_prev(&page_cur);
				cur_rec = page_cur_get_rec(&page_cur);
				break;
			} else if (cmp > 0) {
				/* Skip small recs. */
				page_cur_move_to_next(&page_cur);
				cur_rec = page_cur_get_rec(&page_cur);
			} else if (is_leaf) {
				if (rec_get_deleted_flag(
					cur1_rec,
					dict_table_is_comp(index->table))) {
					goto next;
				} else {
					/* We have two identical leaf records,
					skip copying the undeleted one, and
					unmark deleted on the current page */
					btr_rec_set_deleted_flag(
						cur_rec, NULL, FALSE);
					goto next;
				}
			}
		}

		/* If position is on suprenum rec, need to move to
		previous rec. */
		if (page_rec_is_supremum(cur_rec)) {
			page_cur_move_to_prev(&page_cur);
		}

		cur_rec = page_cur_get_rec(&page_cur);

		offsets = rec_get_offsets(cur1_rec, index, offsets,
					  ULINT_UNDEFINED, &heap);

		ins_rec = page_cur_insert_rec_low(cur_rec, index,
						  cur1_rec, offsets, mtr);
		if (UNIV_UNLIKELY(!ins_rec)) {

			buf_page_print(buf_block_get_frame(new_block),
				       univ_page_size,
				       BUF_PAGE_PRINT_NO_CRASH);
			buf_page_print(page_align(rec), univ_page_size,
				       BUF_PAGE_PRINT_NO_CRASH);

			fprintf(stderr, "page number %ld and %ld\n",
				(long)new_block->page.id.page_no(),
				(long)block->page.id.page_no());
			ib_logf(IB_LOG_LEVEL_FATAL,
				"rec offset %lu, cur1 offset %lu,"
				" cur_rec offset %lu",
				(ulong) page_offset(rec),
				(ulong) page_offset(page_cur_get_rec(&cur1)),
				(ulong) page_offset(cur_rec));
		}

		rec_move[moved].new_rec = ins_rec;
		rec_move[moved].old_rec = cur1_rec;
		rec_move[moved].moved = false;
		moved++;
next:
		if (moved > max_move) {
			ut_ad(0);
			break;
		}

		page_cur_move_to_next(&cur1);
	}

	*num_moved = moved;
}

/****************************************************************//**
Check two MBRs are identical or need to be merged */
bool
rtr_merge_mbr_changed(
/*==================*/
	btr_cur_t*		cursor,		/*!< in/out: cursor */
	btr_cur_t*		cursor2,	/*!< in: the other cursor */
	ulint*			offsets,	/*!< in: rec offsets */
	ulint*			offsets2,	/*!< in: rec offsets */
	rtr_mbr_t*		new_mbr,	/*!< out: MBR to update */
	buf_block_t*		merge_block,	/*!< in: page to merge */
	buf_block_t*		block,		/*!< in: page be merged */
	dict_index_t*		index)		/*!< in: index */
{
	double*		mbr;
	double		mbr1[SPDIMS * 2];
	double		mbr2[SPDIMS * 2];
	rec_t*		rec;
	ulint		len;
	bool		changed = false;

	ut_ad(dict_index_is_spatial(cursor->index));

	rec = btr_cur_get_rec(cursor);

	rtr_read_mbr(rec_get_nth_field(rec, offsets, 0, &len),
		     reinterpret_cast<rtr_mbr_t*>(mbr1));

	rec = btr_cur_get_rec(cursor2);

	rtr_read_mbr(rec_get_nth_field(rec, offsets2, 0, &len),
		     reinterpret_cast<rtr_mbr_t*>(mbr2));

	mbr = reinterpret_cast<double*>(new_mbr);

	for (int i = 0; i < SPDIMS * 2; i += 2) {
		changed = (changed || mbr1[i] != mbr2[i]);
		*mbr = mbr1[i] < mbr2[i] ? mbr1[i] : mbr2[i];
		mbr++;
		changed = (changed || mbr1[i + 1] != mbr2 [i + 1]);
		*mbr = mbr1[i + 1] > mbr2[i + 1] ? mbr1[i + 1] : mbr2[i + 1];
		mbr++;
	}

	if (!changed) {
		rec_t*	rec1;
		rec_t*	rec2;
		ulint*	offsets1;
		ulint*	offsets2;
		mem_heap_t*	heap;

		heap = mem_heap_create(100);

		rec1 = page_rec_get_next(
			page_get_infimum_rec(
				buf_block_get_frame(merge_block)));

		offsets1 = rec_get_offsets(
			rec1, index, NULL, ULINT_UNDEFINED, &heap);

		rec2 = page_rec_get_next(
			page_get_infimum_rec(
				buf_block_get_frame(block)));
		offsets2 = rec_get_offsets(
			rec2, index, NULL, ULINT_UNDEFINED, &heap);

		/* Check any primary key fields have been changed */
		if (cmp_rec_rec(rec1, rec2, offsets1, offsets2, index) != 0) {
			changed = true;
		}

		mem_heap_free(heap);
	}

	return(changed);
}

/****************************************************************//**
Merge 2 mbrs and update the the mbr that cursor is on. */
dberr_t
rtr_merge_and_update_mbr(
/*=====================*/
	btr_cur_t*		cursor,		/*!< in/out: cursor */
	btr_cur_t*		cursor2,	/*!< in: the other cursor */
	ulint*			offsets,	/*!< in: rec offsets */
	ulint*			offsets2,	/*!< in: rec offsets */
	page_t*			child_page,	/*!< in: the page. */
	buf_block_t*		merge_block,	/*!< in: page to merge */
	buf_block_t*		block,		/*!< in: page be merged */
	dict_index_t*		index,		/*!< in: index */
	mtr_t*			mtr)		/*!< in: mtr */
{
	dberr_t			err = DB_SUCCESS;
	rtr_mbr_t		new_mbr;
	bool			changed = false;

	ut_ad(dict_index_is_spatial(cursor->index));

	changed = rtr_merge_mbr_changed(cursor, cursor2, offsets, offsets2,
					&new_mbr, merge_block,
					block, index);

	/* Update the mbr field of the rec. And will delete the record
	pointed by cursor2 */
	if (changed) {
		if (!rtr_update_mbr_field(cursor, offsets, cursor2, child_page,
					  &new_mbr, NULL, mtr)) {
			err = DB_ERROR;
		}
	} else {
		rtr_node_ptr_delete(cursor2->index, cursor2, block, mtr);
	}

	return(err);
}

/*************************************************************//**
Deletes on the upper level the node pointer to a page. */

void
rtr_node_ptr_delete(
/*================*/
	dict_index_t*	index,	/*!< in: index tree */
	btr_cur_t*	cursor, /*!< in: search cursor, contains information
				about parent nodes in search */
	buf_block_t*	block,	/*!< in: page whose node pointer is deleted */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ibool		compressed;
	dberr_t		err;

	compressed = btr_cur_pessimistic_delete(&err, TRUE, cursor,
						BTR_CREATE_FLAG, false, mtr);
	ut_a(err == DB_SUCCESS);

	if (!compressed) {
		btr_cur_compress_if_useful(cursor, FALSE, mtr);
	}
}

/**************************************************************//**
Check whether a Rtree page is child of a parent page
@return true if there is child/parent relationship */

bool
rtr_check_same_block(
/*================*/
	dict_index_t*	index,	/*!< in: index tree */
	btr_cur_t*	cursor,	/*!< in/out: position at the parent entry
				pointing to the child if successful */
	buf_block_t*	parentb,/*!< in: parent page to check */
	buf_block_t*	childb,	/*!< in: child Page */
	mem_heap_t*	heap)	/*!< in: memory heap */

{
	ulint		page_no = childb->page.id.page_no();
	ulint*		offsets;
	rec_t*		rec = page_rec_get_next(page_get_infimum_rec(
				buf_block_get_frame(parentb)));

	while (!page_rec_is_supremum(rec)) {
		offsets = rec_get_offsets(
			rec, index, NULL, ULINT_UNDEFINED, &heap);

		if (btr_node_ptr_get_child_page_no(rec, offsets) == page_no) {
			btr_cur_position(index, rec, parentb, cursor);
			return(true);
		}

		rec = page_rec_get_next(rec);
	}

	return(false);
}

/****************************************************************//**
Calculate the area increased for a new record
@return area increased */

double
rtr_rec_cal_increase(
/*=================*/
	const dtuple_t*	dtuple,	/*!< in: data tuple to insert, which
				cause area increase */
	const rec_t*	rec,	/*!< in: physical record which differs from
				dtuple in some of the common fields, or which
				has an equal number or more fields than
				dtuple */
	const ulint*	offsets,/*!< in: array returned by rec_get_offsets() */
	double*		area)	/*!< out: increased area */
{
	const dfield_t*	dtuple_field;
	ulint		dtuple_f_len;
	ulint		rec_f_len;
	const byte*	rec_b_ptr;
	double		ret = 0;

	ut_ad(!page_rec_is_supremum(rec));
	ut_ad(!page_rec_is_infimum(rec));

	dtuple_field = dtuple_get_nth_field(dtuple, 0);
	dtuple_f_len = dfield_get_len(dtuple_field);

	rec_b_ptr = rec_get_nth_field(rec, offsets, 0, &rec_f_len);
	ret = rtree_area_increase(
		rec_b_ptr,
		static_cast<const byte*>(dfield_get_data(dtuple_field)),
		static_cast<int>(dtuple_f_len), area);

	return(ret);
}
