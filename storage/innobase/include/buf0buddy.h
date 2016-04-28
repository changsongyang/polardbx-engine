/*****************************************************************************

Copyright (c) 2006, 2016, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/buf0buddy.h
Binary buddy allocator for compressed pages

Created December 2006 by Marko Makela
*******************************************************/

#ifndef buf0buddy_h
#define buf0buddy_h

#ifdef UNIV_MATERIALIZE
# undef UNIV_INLINE
# define UNIV_INLINE
#endif

#include "univ.i"
#include "buf0types.h"

/** Allocate a block. This function should only be used for allocating
compressed page frames. The thread calling this function must hold
buf_pool->LRU_list_mutex and must not hold buf_pool->zip_mutex or any
block->mutex.
@param[in,out]	buf_pool	buffer pool in which the page resides
@param[in]	size		compressed page size, between
				UNIV_ZIP_SIZE_MIN and UNIV_PAGE_SIZE
@return allocated block, never NULL */
UNIV_INLINE
byte*
buf_buddy_alloc(
	buf_pool_t*	buf_pool,
	ulint		size)
	MY_ATTRIBUTE((malloc));

/** Deallocate a block.
@param[in,out]	buf_pool	buffer pool in which the block resides
@param[in]	buf		block to be freed, must not be pointed to
				by the buffer pool
@param[in]	size		block size, up to UNIV_PAGE_SIZE */
UNIV_INLINE
void
buf_buddy_free(
	buf_pool_t*	buf_pool,
	void*		buf,
	ulint		size);

/** Try to reallocate a block.
@param[in]	buf_pool	buffer pool instance
@param[in]	buf		block to be reallocated, must be pointed
to by the buffer pool
@param[in]	size		block size, up to UNIV_PAGE_SIZE
@retval true	if succeeded or if failed because the block was fixed
@retval false	if failed because of no free blocks. */
bool
buf_buddy_realloc(
	buf_pool_t*	buf_pool,
	void*		buf,
	ulint		size);

/** Combine all pairs of free buddies.
@param[in]	buf_pool	buffer pool instance */
void
buf_buddy_condense_free(
	buf_pool_t*	buf_pool);

#include "buf0buddy.ic"

#endif /* buf0buddy_h */
