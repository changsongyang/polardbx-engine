/*****************************************************************************

Copyright (c) 1996, 2013, Oracle and/or its affiliates. All Rights Reserved.

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

/********************************************************************//**
@file include/btr0types.h
The index tree general types

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#ifndef btr0types_h
#define btr0types_h

#include "univ.i"

#include "rem0types.h"
#include "page0types.h"
#include "sync0rw.h"

/** Persistent cursor */
struct btr_pcur_t;
/** B-tree cursor */
struct btr_cur_t;
/** B-tree search information for the adaptive hash index */
struct btr_search_t;

#ifndef UNIV_HOTBACKUP

/** @brief The latch protecting the adaptive search system

This latch protects the
(1) hash index;
(2) columns of a record to which we have a pointer in the hash index;

but does NOT protect:

(3) next record offset field in a record;
(4) next or previous records on the same page.

Bear in mind (3) and (4) when using the hash index.
*/
extern rw_lock_t*	btr_search_latch_temp;

#endif /* UNIV_HOTBACKUP */

/** The latch protecting the adaptive search system */
#define btr_search_latch	(*btr_search_latch_temp)

/** Flag: has the search system been enabled?
Protected by btr_search_latch. */
extern char	btr_search_enabled;

#ifdef UNIV_BLOB_DEBUG
# include "buf0types.h"
/** An index->blobs entry for keeping track of off-page column references */
struct btr_blob_dbg_t;

/** Insert to index->blobs a reference to an off-page column.
@param index the index tree
@param b the reference
@param ctx context (for logging) */

void
btr_blob_dbg_rbt_insert(
/*====================*/
	dict_index_t*		index,	/*!< in/out: index tree */
	const btr_blob_dbg_t*	b,	/*!< in: the reference */
	const char*		ctx)	/*!< in: context (for logging) */
	__attribute__((nonnull));

/** Remove from index->blobs a reference to an off-page column.
@param index the index tree
@param b the reference
@param ctx context (for logging) */

void
btr_blob_dbg_rbt_delete(
/*====================*/
	dict_index_t*		index,	/*!< in/out: index tree */
	const btr_blob_dbg_t*	b,	/*!< in: the reference */
	const char*		ctx)	/*!< in: context (for logging) */
	__attribute__((nonnull));

/**************************************************************//**
Add to index->blobs any references to off-page columns from a record.
@return number of references added */

ulint
btr_blob_dbg_add_rec(
/*=================*/
	const rec_t*	rec,	/*!< in: record */
	dict_index_t*	index,	/*!< in/out: index */
	const ulint*	offsets,/*!< in: offsets */
	const char*	ctx)	/*!< in: context (for logging) */
	__attribute__((nonnull));
/**************************************************************//**
Remove from index->blobs any references to off-page columns from a record.
@return number of references removed */

ulint
btr_blob_dbg_remove_rec(
/*====================*/
	const rec_t*	rec,	/*!< in: record */
	dict_index_t*	index,	/*!< in/out: index */
	const ulint*	offsets,/*!< in: offsets */
	const char*	ctx)	/*!< in: context (for logging) */
	__attribute__((nonnull));
/**************************************************************//**
Count and add to index->blobs any references to off-page columns
from records on a page.
@return number of references added */

ulint
btr_blob_dbg_add(
/*=============*/
	const page_t*	page,	/*!< in: rewritten page */
	dict_index_t*	index,	/*!< in/out: index */
	const char*	ctx)	/*!< in: context (for logging) */
	__attribute__((nonnull));
/**************************************************************//**
Count and remove from index->blobs any references to off-page columns
from records on a page.
Used when reorganizing a page, before copying the records.
@return number of references removed */

ulint
btr_blob_dbg_remove(
/*================*/
	const page_t*	page,	/*!< in: b-tree page */
	dict_index_t*	index,	/*!< in/out: index */
	const char*	ctx)	/*!< in: context (for logging) */
	__attribute__((nonnull));
/**************************************************************//**
Restore in index->blobs any references to off-page columns
Used when page reorganize fails due to compressed page overflow. */

void
btr_blob_dbg_restore(
/*=================*/
	const page_t*	npage,	/*!< in: page that failed to compress */
	const page_t*	page,	/*!< in: copy of original page */
	dict_index_t*	index,	/*!< in/out: index */
	const char*	ctx)	/*!< in: context (for logging) */
	__attribute__((nonnull));

/** Operation that processes the BLOB references of an index record
@param[in] rec record on index page
@param[in/out] index the index tree of the record
@param[in] offsets rec_get_offsets(rec,index)
@param[in] ctx context (for logging)
@return number of BLOB references processed */
typedef ulint (*btr_blob_dbg_op_f)
(const rec_t* rec,dict_index_t* index,const ulint* offsets,const char* ctx);

/**************************************************************//**
Count and process all references to off-page columns on a page.
@return number of references processed */

ulint
btr_blob_dbg_op(
/*============*/
	const page_t*		page,	/*!< in: B-tree leaf page */
	const rec_t*		rec,	/*!< in: record to start from
					(NULL to process the whole page) */
	dict_index_t*		index,	/*!< in/out: index */
	const char*		ctx,	/*!< in: context (for logging) */
	const btr_blob_dbg_op_f	op)	/*!< in: operation on records */
	__attribute__((nonnull(1,3,4,5)));
#else /* UNIV_BLOB_DEBUG */
# define btr_blob_dbg_add_rec(rec, index, offsets, ctx)		((void) 0)
# define btr_blob_dbg_add(page, index, ctx)			((void) 0)
# define btr_blob_dbg_remove_rec(rec, index, offsets, ctx)	((void) 0)
# define btr_blob_dbg_remove(page, index, ctx)			((void) 0)
# define btr_blob_dbg_restore(npage, page, index, ctx)		((void) 0)
# define btr_blob_dbg_op(page, rec, index, ctx, op)		((void) 0)
#endif /* UNIV_BLOB_DEBUG */

/** The size of a reference to data stored on a different page.
The reference is stored at the end of the prefix of the field
in the index record. */
#define BTR_EXTERN_FIELD_REF_SIZE	20

/** If the data don't exceed the size, the data are stored locally. */
#define BTR_EXTERN_LOCAL_STORED_MAX_SIZE	\
	(BTR_EXTERN_FIELD_REF_SIZE * 2)

/** A BLOB field reference full of zero, for use in assertions and tests.
Initially, BLOB field references are set to zero, in
dtuple_convert_big_rec(). */
extern const byte field_ref_zero[BTR_EXTERN_FIELD_REF_SIZE];

/** The information is used for creating a new index tree when
applying TRUNCATE log record during recovery */
struct btr_create_t {

	explicit btr_create_t(const byte* const ptr)
		:
		format_flags(),
		n_fields(),
		field_len(),
		fields(ptr),
		trx_id_pos(ULINT_UNDEFINED)
	{
		/* Do nothing */
	}

	/** Page format */
	ulint			format_flags;

	/** Numbr of index fields */
	ulint			n_fields;

	/** The length of the encoded meta-data */
	ulint			field_len;

	/** Field meta-data, encoded. */
	const byte* const	fields;

	/** Position of trx-id column. */
	ulint			trx_id_pos;
};

#endif
