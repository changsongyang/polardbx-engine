/*
   Copyright (c) 2003-2006 MySQL AB
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <signaldata/DropTab.hpp>

bool 
printDROP_TAB_REQ(FILE * output, const Uint32 * theData, Uint32 len, Uint16 receiverBlockNo)
{
  const DropTabReq * const sig = (DropTabReq *) theData;
  
  fprintf(output, 
	  " senderRef: %x senderData: %d TableId: %d requestType: %d\n",
	  sig->senderRef, sig->senderData, sig->tableId, sig->requestType);
  return true;
}

bool printDROP_TAB_CONF(FILE * output, const Uint32 * theData, Uint32 len, Uint16 receiverBlockNo)
{
  const DropTabConf * const sig = (DropTabConf *) theData;

  fprintf(output, 
	  " senderRef: %x senderData: %d TableId: %d\n",
	  sig->senderRef, sig->senderData, sig->tableId);
  
  return true;
}

bool printDROP_TAB_REF(FILE * output, const Uint32 * theData, Uint32 len, Uint16 receiverBlockNo)
{
  const DropTabRef * const sig = (DropTabRef *) theData;

  fprintf(output, 
	  " senderRef: %x senderData: %d TableId: %d errorCode: %d\n",
	  sig->senderRef, sig->senderData, sig->tableId, sig->errorCode);
  
  return true;
}
