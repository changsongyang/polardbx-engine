/*
 Copyright (c) 2012, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */
#ifndef NDBMEMCACHE_ENGINE_ERRORS_H
#define NDBMEMCACHE_ENGINE_ERRORS_H

#include <ndberror.h>

/* Errors 9000 - 9099 are reported as "Scheduler Error" */
extern ndberror_struct AppError9001_ReconfLock;
extern ndberror_struct AppError9002_NoNDBs;
extern ndberror_struct AppError9003_SyncClose;
extern ndberror_struct AppError9004_autogrow;

/* Errors 9100 and up are reported as "Memcached Error" */

#endif
