/*
   Copyright (c) 2011, 2016, Oracle and/or its affiliates. All rights reserved.

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
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <ndb_global.h>
#include "NdbIndexStatImpl.hpp"

/*
 The SQL commands used for creating
 the ndb index stat system tables:

use mysql;

create table ndb_index_stat_head (
  index_id int unsigned not null,
  index_version int unsigned not null,
  table_id int unsigned not null,
  frag_count int unsigned not null,
  value_format int unsigned not null,
  sample_version int unsigned not null,
  load_time int unsigned not null,
  sample_count int unsigned not null,
  key_bytes int unsigned not null,
  primary key using hash (
    index_id, index_version)
) engine=ndb;

create table ndb_index_stat_sample (
  index_id int unsigned not null,
  index_version int unsigned not null,
  sample_version int unsigned not null,
  stat_key varbinary(3056) not null, -- ((3072/4) - 3 - 1) * 4
  stat_value varbinary(2048) not null, -- 512 * 4
  primary key using hash (
    index_id, index_version, sample_version, stat_key),
  index ndb_index_stat_sample_x1 (
    index_id, index_version, sample_version)
) engine=ndb;

*/

// rest dumped from *.frm by tools/ndb_dump_frm_data

/*
  name: ndb_index_stat_head
  orig: 8918
  pack: 402
*/

const uint g_ndb_index_stat_head_frm_len = 402;

const uint8 g_ndb_index_stat_head_frm_data[402] =
{
  0x01,0x00,0x00,0x00,0xd6,0x22,0x00,0x00,
  0x86,0x01,0x00,0x00,0x78,0x9c,0xed,0xda,
  0xbf,0x4e,0xc2,0x40,0x1c,0x07,0xf0,0x6f,
  0xcb,0xdf,0x1e,0x50,0x08,0x21,0x0c,0x0c,
  0xa6,0x31,0x21,0x11,0x17,0x9d,0x9d,0xc4,
  0xc4,0x81,0x18,0x95,0x10,0x16,0x5c,0x9a,
  0x02,0x87,0xa9,0x02,0x35,0xb4,0x10,0xd9,
  0x78,0x27,0x1e,0xc1,0x77,0xf0,0x29,0x7c,
  0x06,0xcf,0x2b,0x50,0x3c,0x36,0x16,0x83,
  0x9a,0xdf,0x67,0xba,0xfb,0xf6,0x2e,0xf9,
  0xe5,0xbb,0xb5,0xe9,0xa7,0x66,0x98,0x31,
  0xa0,0xa0,0x01,0xe7,0x80,0xab,0x55,0xb1,
  0xa5,0x9f,0x22,0x0d,0x24,0xc2,0x65,0x3a,
  0xca,0x5c,0x79,0xee,0xe3,0x0d,0xb8,0x58,
  0xed,0x4c,0xe0,0xec,0x0c,0xb0,0x40,0x08,
  0x21,0x84,0x10,0x42,0x08,0x21,0x84,0x90,
  0xdf,0x4c,0xd3,0x01,0x86,0xf0,0x0d,0x5f,
  0x8f,0xc9,0xdd,0x42,0x6e,0x2b,0x97,0x71,
  0xe8,0x8b,0xe4,0x7a,0x21,0x9a,0xad,0xc6,
  0x6d,0xbd,0xd5,0x11,0x87,0x1e,0xf4,0xaf,
  0xdb,0xb3,0x40,0x86,0x71,0xbf,0xdb,0x1b,
  0x4e,0xfd,0x80,0x4f,0x4a,0x72,0x6f,0x35,
  0xeb,0xad,0x76,0xa3,0xdd,0xb8,0xbf,0xb3,
  0xae,0x3a,0xd6,0xcd,0x75,0xc7,0x3a,0xa9,
  0x41,0x2b,0xfe,0xe8,0xa8,0x84,0x10,0x42,
  0x08,0x21,0x84,0x10,0x42,0xfe,0x9f,0x77,
  0x1d,0x85,0x43,0xcf,0x70,0x48,0x1a,0x0c,
  0x2c,0xf1,0x20,0x57,0x55,0x3c,0x6d,0xd3,
  0x26,0xca,0x9b,0xd5,0x12,0xcc,0xd0,0x4b,
  0x35,0x6b,0x4f,0x88,0xc3,0x70,0xc7,0x7d,
  0xfe,0x6a,0xbb,0x7d,0x24,0x60,0xae,0xd7,
  0x33,0x3e,0xf1,0x5d,0x6f,0x8c,0x24,0x8c,
  0xc0,0xe9,0x0e,0x79,0xf8,0x30,0x85,0xcc,
  0x60,0xe2,0x3c,0xda,0x3d,0x6f,0x3a,0x0e,
  0x90,0x46,0x6e,0xe6,0x0c,0xa7,0xdc,0x1e,
  0x78,0x93,0x91,0x13,0xc8,0xa1,0xf2,0xbe,
  0x33,0x7a,0x91,0x47,0xa3,0xbb,0x0c,0x6c,
  0xe8,0x39,0x7d,0x3b,0x70,0x47,0x1c,0x19,
  0xe4,0x36,0x8f,0xd7,0xd7,0xb3,0x60,0xcf,
  0x7c,0x6e,0x77,0xe7,0x01,0xf7,0x11,0x37,
  0x18,0xc3,0xea,0x3b,0x8e,0x9c,0x3f,0x16,
  0xfe,0xc2,0x61,0xca,0x20,0xa9,0x04,0xc9,
  0xf0,0x04,0x53,0x82,0x54,0x46,0x06,0xa6,
  0x12,0xa4,0x73,0x32,0x28,0x2a,0x81,0x91,
  0x97,0x41,0x59,0x09,0x98,0x84,0x8a,0x12,
  0x64,0xc2,0x2b,0x47,0x4a,0x90,0x0d,0x4f,
  0x1c,0x2b,0x81,0x88,0x9a,0x11,0x3b,0xb5,
  0x88,0xa8,0x13,0xf1,0x5d,0x88,0x50,0xdb,
  0x10,0xbb,0x55,0x88,0x6d,0x0f,0x42,0x2d,
  0x41,0x6c,0x1b,0x10,0xf8,0x02,0xe7,0x16,
  0x7d,0xd4
};

/*
  name: ndb_index_stat_sample
  orig: 12842
  pack: 371
*/

const uint g_ndb_index_stat_sample_frm_len = 371;

const uint8 g_ndb_index_stat_sample_frm_data[371] =
{
  0x01,0x00,0x00,0x00,0x2a,0x32,0x00,0x00,
  0x67,0x01,0x00,0x00,0x78,0x9c,0xed,0xda,
  0x3b,0x4f,0xc2,0x60,0x18,0x05,0xe0,0xd3,
  0x1b,0xf4,0xa2,0x85,0x81,0x38,0x38,0x35,
  0x71,0x01,0x17,0x74,0x71,0x32,0x11,0x8c,
  0x9a,0x10,0xa3,0x12,0xc2,0xc2,0xd4,0x80,
  0xed,0x40,0xc4,0x6a,0xb8,0x05,0xb6,0xfe,
  0x36,0xfd,0x3b,0x0e,0xfc,0x00,0xe3,0x67,
  0x69,0xa9,0xc0,0x62,0xdc,0x8a,0xc9,0x79,
  0xa6,0xb7,0xa7,0x5f,0x9a,0xf3,0x6d,0x1d,
  0xde,0x2f,0xc9,0xb4,0x15,0xa0,0x28,0x01,
  0x35,0xe0,0x4d,0x46,0x09,0x29,0x79,0x06,
  0x03,0xd0,0x96,0xa3,0x9e,0x66,0xd1,0x01,
  0x7c,0xbc,0x03,0x67,0xf1,0x93,0x0d,0x54,
  0xab,0xc0,0x09,0x88,0x88,0x88,0x88,0x88,
  0x88,0x88,0x68,0x97,0xc9,0x79,0xe0,0x08,
  0x65,0x7c,0x5a,0xaa,0x02,0x48,0xa1,0x04,
  0x1c,0xd6,0x54,0xc8,0xa1,0x96,0x0c,0x4a,
  0x68,0x24,0x83,0x1a,0xee,0x47,0xef,0x6b,
  0x0b,0x4b,0xc2,0x1e,0xa2,0xa3,0xbf,0x9d,
  0x15,0xcd,0x56,0xe3,0xae,0xde,0xea,0x88,
  0xc0,0xeb,0xb9,0xfd,0xc0,0xf3,0x67,0xee,
  0x68,0xdc,0x1d,0xbb,0xa3,0xee,0xf3,0xeb,
  0xc0,0x77,0x67,0xa7,0x22,0xeb,0x6b,0x13,
  0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
  0x11,0xd1,0x3f,0x60,0x22,0xf0,0x7a,0x8f,
  0x83,0xc9,0x68,0xec,0x0f,0x97,0xeb,0x6b,
  0x4e,0xb3,0xde,0x6a,0x37,0xda,0x8d,0x87,
  0x7b,0xe7,0xb2,0xe3,0xdc,0x5e,0x77,0x9c,
  0x72,0x05,0x92,0x9d,0x75,0x4d,0x22,0x22,
  0x22,0xa2,0x9d,0x70,0x2c,0xa3,0x98,0x75,
  0x87,0x2c,0x49,0xd0,0x30,0x87,0xbd,0xfc,
  0x6d,0x2c,0x9d,0xff,0xa4,0x4d,0x1c,0xac,
  0xa6,0x39,0x72,0x9a,0x5c,0xaa,0x38,0x7f,
  0x04,0x15,0x46,0xb2,0xf0,0xd0,0xf7,0xa2,
  0x2f,0xdb,0xc9,0x3c,0xf5,0x87,0xa3,0xfe,
  0x4b,0x80,0x1c,0x0a,0xab,0x15,0x88,0x34,
  0xc9,0xc3,0x88,0x37,0x23,0x9e,0xfc,0x39,
  0x74,0x58,0xf1,0x3c,0xed,0x0e,0x26,0x3e,
  0x54,0xc3,0x34,0x11,0x6f,0x58,0x44,0x2d,
  0x14,0x1d,0xd0,0xec,0x28,0xd0,0x36,0x82,
  0x5c,0x21,0x0a,0x8c,0x8d,0x20,0x6f,0xdc,
  0x2c,0xac,0x78,0x4b,0x23,0x0a,0x0a,0x17,
  0x80,0x6e,0x5d,0x41,0x17,0xd6,0x3a,0x10,
  0x69,0x37,0xb1,0x55,0x4c,0x6c,0xb7,0x12,
  0x69,0x25,0xb1,0xee,0x23,0xf0,0x0d,0x53,
  0x4c,0x66,0xbc
};
