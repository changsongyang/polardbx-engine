/*
   Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.

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

#include <HugoOperations.hpp>

#undef ERR
#define ERR(error) \
{ \
  const NdbError &_error= (error); \
  if (!m_quiet) ERR_OUT(g_err, _error); \
}

int HugoOperations::startTransaction(Ndb* pNdb,
                                     const NdbDictionary::Table *table,
                                     const char  *keyData, Uint32 keyLen){
  
  if (pTrans != NULL){
    ndbout << "HugoOperations::startTransaction, pTrans != NULL" << endl;
    return NDBT_FAILED;
  }
  pTrans = pNdb->startTransaction(table, keyData, keyLen);
  if (pTrans == NULL) {
    const NdbError err = pNdb->getNdbError();
    ERR(err);
    setNdbError(err);
    return NDBT_FAILED;
  }
  return NDBT_OK;
}

int HugoOperations::setTransaction(NdbTransaction* new_trans, bool not_null_ok){
  
  if (pTrans != NULL && !not_null_ok){
    ndbout << "HugoOperations::startTransaction, pTrans != NULL" << endl;
    return NDBT_FAILED;
  }
  pTrans = new_trans;
  if (pTrans == NULL) {
    return NDBT_FAILED;
  }
  return NDBT_OK;
}

void 
HugoOperations::setTransactionId(Uint64 id){
  if (pTrans != NULL){
    pTrans->setTransactionId(id);
  }
}

int HugoOperations::closeTransaction(Ndb* pNdb){

  UtilTransactions::closeTransaction(pNdb);

  m_result_sets.clear();
  m_executed_result_sets.clear();

  return NDBT_OK;
}

NdbConnection* HugoOperations::getTransaction(){
  return pTrans;
}

int HugoOperations::pkReadRecord(Ndb* pNdb,
				 int recordNo,
				 int numRecords,
				 NdbOperation::LockMode lm,
                                 NdbOperation::LockMode *lmused){
  int a;  
  allocRows(numRecords);
  indexScans.clear();  
  int check;

  NdbOperation* pOp = 0;
  pIndexScanOp = 0;

  for(int r=0; r < numRecords; r++){
    
    if(pOp == 0)
    {
      pOp = getOperation(pTrans, NdbOperation::ReadRequest);
    }
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
rand_lock_mode:
    switch(lm){
    case NdbOperation::LM_Read:
    case NdbOperation::LM_Exclusive:
    case NdbOperation::LM_CommittedRead:
    case NdbOperation::LM_SimpleRead:
      if (lmused)
        * lmused = lm;
      if(idx && idx->getType() == NdbDictionary::Index::OrderedIndex)
      {
        if (pIndexScanOp == 0)
        {
          pIndexScanOp = ((NdbIndexScanOperation*)pOp);
          bool mrrScan= (numRecords > 1);
          Uint32 flags= mrrScan? NdbScanOperation::SF_MultiRange : 0; 
          check = pIndexScanOp->readTuples(lm, flags);
          /* Record NdbIndexScanOperation ptr for later... */
          indexScans.push_back(pIndexScanOp);
        }
      }
      else
	check = pOp->readTuple(lm);
      break;
    default:
      lm = (NdbOperation::LockMode)((rand() >> 16) & 3);
      goto rand_lock_mode;
    }
    
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    // Define primary keys
    if (equalForRow(pOp, r+recordNo) != 0)
      return NDBT_FAILED;

    Uint32 partId;
    /* Do we need to set the partitionId for this operation? */
    if (getPartIdForRow(pOp, r+recordNo, partId))
    {
      g_info << "Setting operation partition Id" << endl;
      pOp->setPartitionId(partId);
    }

    if(pIndexScanOp)
      pIndexScanOp->end_of_bound(r);
    
    if(r == 0 || pIndexScanOp == 0)
    {
      // Define attributes to read  
      for(a = 0; a<tab.getNoOfColumns(); a++){
	if((rows[r]->attributeStore(a) = 
	    pOp->getValue(tab.getColumn(a)->getName())) == 0) {
	  ERR(pTrans->getNdbError());
          setNdbError(pTrans->getNdbError());
	  return NDBT_FAILED;
	}
      } 
    }
    /* Note pIndexScanOp will point to the 'last' index scan op
     * we used.  The full list is in the indexScans vector
     */
    pOp = pIndexScanOp;
  }
  return NDBT_OK;
}

int HugoOperations::pkReadRandRecord(Ndb* pNdb,
                                     int records,
                                     int numRecords,
                                     NdbOperation::LockMode lm,
                                     NdbOperation::LockMode *lmused){
  int a;  
  allocRows(numRecords);
  indexScans.clear();
  int check;

  NdbOperation* pOp = 0;
  pIndexScanOp = 0;

  for(int r=0; r < numRecords; r++){
    
    if(pOp == 0)
    {
      pOp = getOperation(pTrans, NdbOperation::ReadRequest);
    }
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
rand_lock_mode:
    switch(lm){
    case NdbOperation::LM_Read:
    case NdbOperation::LM_Exclusive:
    case NdbOperation::LM_CommittedRead:
    case NdbOperation::LM_SimpleRead:
      if (lmused)
        * lmused = lm;
      if(idx && idx->getType() == NdbDictionary::Index::OrderedIndex && 
	 pIndexScanOp == 0)
      {
	pIndexScanOp = ((NdbIndexScanOperation*)pOp);
	check = pIndexScanOp->readTuples(lm);
        /* Record NdbIndexScanOperation ptr for later... */
        indexScans.push_back(pIndexScanOp);
      }
      else
	check = pOp->readTuple(lm);
      break;
    default:
      lm = (NdbOperation::LockMode)((rand() >> 16) & 3);
      goto rand_lock_mode;
    }
    
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    int rowid= rand() % records;

    // Define primary keys
    if (equalForRow(pOp, rowid) != 0)
      return NDBT_FAILED;

    Uint32 partId;
    /* Do we need to set the partitionId for this operation? */
    if (getPartIdForRow(pOp, rowid, partId))
    {
      g_info << "Setting operation partition Id" << endl;
      pOp->setPartitionId(partId);
    }

    if(pIndexScanOp)
      pIndexScanOp->end_of_bound(r);
    
    if(r == 0 || pIndexScanOp == 0)
    {
      // Define attributes to read  
      for(a = 0; a<tab.getNoOfColumns(); a++){
	if((rows[r]->attributeStore(a) = 
	    pOp->getValue(tab.getColumn(a)->getName())) == 0) {
	  ERR(pTrans->getNdbError());
          setNdbError(pTrans->getNdbError());
	  return NDBT_FAILED;
	}
      } 
    }
    /* Note pIndexScanOp will point to the 'last' index scan op
     * we used.  The full list is in the indexScans vector
     */
    pOp = pIndexScanOp;
  }
  return NDBT_OK;
}

int HugoOperations::pkUpdateRecord(Ndb* pNdb,
				   int recordNo,
				   int numRecords,
				   int updatesValue){
  allocRows(numRecords);
  int check;
  for(int r=0; r < numRecords; r++){
    NdbOperation* pOp = getOperation(pTrans, NdbOperation::UpdateRequest);
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    check = pOp->updateTuple();
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    if(setValues(pOp, r+recordNo, updatesValue) != NDBT_OK)
    {
      return NDBT_FAILED;
    }

    Uint32 partId;
    if(getPartIdForRow(pOp, r+recordNo, partId))
      pOp->setPartitionId(partId);
    
  }
  return NDBT_OK;
}

int 
HugoOperations::setValues(NdbOperation* pOp, int rowId, int updateId)
{
  // Define primary keys
  int a;
  if (equalForRow(pOp, rowId) != 0)
    return NDBT_FAILED;
  
  for(a = 0; a<tab.getNoOfColumns(); a++){
    if (tab.getColumn(a)->getPrimaryKey() == false){
      if(setValueForAttr(pOp, a, rowId, updateId ) != 0){ 
	ERR(pTrans->getNdbError());
        setNdbError(pTrans->getNdbError());
	return NDBT_FAILED;
      }
    }
  }
  
  return NDBT_OK;
}

int HugoOperations::pkInsertRecord(Ndb* pNdb,
				   int recordNo,
				   int numRecords,
				   int updatesValue){
  
  int check;
  for(int r=0; r < numRecords; r++){
    NdbOperation* pOp = getOperation(pTrans, NdbOperation::InsertRequest);
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    check = pOp->insertTuple();
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    if(setValues(pOp, r+recordNo, updatesValue) != NDBT_OK)
    {
      m_error.code = pTrans->getNdbError().code;
      return NDBT_FAILED;
    }

    Uint32 partId;
    if(getPartIdForRow(pOp, r+recordNo, partId))
      pOp->setPartitionId(partId);
    
  }
  return NDBT_OK;
}

int HugoOperations::pkWriteRecord(Ndb* pNdb,
				  int recordNo,
				  int numRecords,
				  int updatesValue){
  
  int a, check;
  for(int r=0; r < numRecords; r++){
    NdbOperation* pOp = pTrans->getNdbOperation(tab.getName());	
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    check = pOp->writeTuple();
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    // Define primary keys
    if (equalForRow(pOp, r+recordNo) != 0)
      return NDBT_FAILED;
    
    Uint32 partId;
    if(getPartIdForRow(pOp, r+recordNo, partId))
      pOp->setPartitionId(partId);
    

    // Define attributes to update
    for(a = 0; a<tab.getNoOfColumns(); a++){
      if (tab.getColumn(a)->getPrimaryKey() == false){
	if(setValueForAttr(pOp, a, recordNo+r, updatesValue ) != 0){ 
	  ERR(pTrans->getNdbError());
          setNdbError(pTrans->getNdbError());
	  return NDBT_FAILED;
	}
      }
    } 
  }
  return NDBT_OK;
}

int HugoOperations::pkWritePartialRecord(Ndb* pNdb,
					 int recordNo,
					 int numRecords){
  
  int check;
  for(int r=0; r < numRecords; r++){
    NdbOperation* pOp = pTrans->getNdbOperation(tab.getName());	
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    check = pOp->writeTuple();
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    // Define primary keys
    if (equalForRow(pOp, r+recordNo) != 0)
      return NDBT_FAILED;

    Uint32 partId;
    if(getPartIdForRow(pOp, r+recordNo, partId))
      pOp->setPartitionId(partId);
    
  }
  return NDBT_OK;
}

int HugoOperations::pkDeleteRecord(Ndb* pNdb,
				   int recordNo,
				   int numRecords){
  
  int check;
  for(int r=0; r < numRecords; r++){
    NdbOperation* pOp = getOperation(pTrans, NdbOperation::DeleteRequest);
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    check = pOp->deleteTuple();
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    // Define primary keys
    if (equalForRow(pOp, r+recordNo) != 0)
      return NDBT_FAILED;

    Uint32 partId;
    if(getPartIdForRow(pOp, r+recordNo, partId))
      pOp->setPartitionId(partId);
  }
  return NDBT_OK;
}

int HugoOperations::execute_Commit(Ndb* pNdb,
				   AbortOption eao){

  int check = 0;
  check = pTrans->execute(Commit, eao);   

  const NdbError err = pTrans->getNdbError();
  if( check == -1 || err.code) {
    ERR(err);
    setNdbError(err);
    NdbOperation* pOp = pTrans->getNdbErrorOperation();
    if (pOp != NULL){
      const NdbError err2 = pOp->getNdbError();
      ERR(err2);
      setNdbError(err2);
    }
    if (err.code == 0)
      return NDBT_FAILED;
    return err.code;
  }

  for(unsigned int i = 0; i<m_result_sets.size(); i++){
    m_executed_result_sets.push_back(m_result_sets[i]);

    int rows = m_result_sets[i].records;
    NdbScanOperation* rs = m_result_sets[i].m_result_set;
    int res = rs->nextResult();
    switch(res){
    case 1:
      return 626;
    case -1:
      const NdbError err = pTrans->getNdbError();
      ERR(err);
      setNdbError(err);
      return (err.code > 0 ? err.code : NDBT_FAILED);
    }

    // A row found

    switch(rows){
    case 0:
      return 4000;
    default:
      m_result_sets[i].records--;
      break;
    }
  }

  m_result_sets.clear();
  
  return NDBT_OK;
}

int HugoOperations::execute_NoCommit(Ndb* pNdb, AbortOption eao){

  int check;
  check = pTrans->execute(NoCommit, eao);   

  const NdbError err = pTrans->getNdbError();
  if( check == -1 || err.code) {
    ERR(err);
    setNdbError(err);
    const NdbOperation* pOp = pTrans->getNdbErrorOperation();
    while (pOp != NULL)
    {
      const NdbError err2 = pOp->getNdbError();
      if (err2.code)
      {
	ERR(err2);
        setNdbError(err2);
      }
      pOp = pTrans->getNextCompletedOperation(pOp);
    }
    if (err.code == 0)
      return NDBT_FAILED;
    return err.code;
  }

  for(unsigned int i = 0; i<m_result_sets.size(); i++){
    m_executed_result_sets.push_back(m_result_sets[i]);

    int rows = m_result_sets[i].records;
    NdbScanOperation* rs = m_result_sets[i].m_result_set;
    int res = rs->nextResult();
    switch(res){
    case 1:
      return 626;
    case -1:
      const NdbError err = pTrans->getNdbError();
      ERR(err);
      setNdbError(err);
      return (err.code > 0 ? err.code : NDBT_FAILED);
    }

    // A row found

    switch(rows){
    case 0:
      return 4000;
    default:
    case 1:
      break;
    }
  }

  m_result_sets.clear();

  return NDBT_OK;
}

int HugoOperations::execute_Rollback(Ndb* pNdb){
  int check;
  check = pTrans->execute(Rollback);   
  if( check == -1 ) {
    const NdbError err = pTrans->getNdbError();
    ERR(err);
    setNdbError(err);
    return NDBT_FAILED;
  }
  return NDBT_OK;
}

void
HugoOperations_async_callback(int res, NdbTransaction* pCon, void* ho)
{
  ((HugoOperations*)ho)->callback(res, pCon);
}

void
HugoOperations::callback(int res, NdbTransaction* pCon)
{
  assert(pCon == pTrans);
  m_async_reply= 1;
  if(res)
  {
    m_async_return = pCon->getNdbError().code;
  }
  else
  {
    m_async_return = 0;
  }
}

int 
HugoOperations::execute_async(Ndb* pNdb, NdbTransaction::ExecType et, 
			      NdbOperation::AbortOption eao){
  
  m_async_reply= 0;
  pTrans->executeAsynchPrepare(et,
			       HugoOperations_async_callback,
			       this,
			       eao);
  
  pNdb->sendPreparedTransactions();
  
  return NDBT_OK;
}

int 
HugoOperations::execute_async_prepare(Ndb* pNdb, NdbTransaction::ExecType et, 
				      NdbOperation::AbortOption eao){
  
  m_async_reply= 0;
  pTrans->executeAsynchPrepare(et,
			       HugoOperations_async_callback,
			       this,
			       eao);
  
  return NDBT_OK;
}

int
HugoOperations::wait_async(Ndb* pNdb, int timeout)
{
  volatile int * wait = &m_async_reply;
  while (!* wait)
  {
    pNdb->sendPollNdb(1000);
    
    if(* wait)
    {
      if(m_async_return)
	ndbout << "ERROR: " << pNdb->getNdbError(m_async_return) << endl;
      return m_async_return;
    }
  }
  ndbout_c("wait returned nothing...");
  return -1;
}

HugoOperations::HugoOperations(const NdbDictionary::Table& _tab,
			       const NdbDictionary::Index* idx):
  UtilTransactions(_tab, idx),
  pIndexScanOp(NULL),
  calc(_tab),
  m_quiet(false)
{
}

HugoOperations::~HugoOperations(){
  deallocRows();
  if (pTrans != NULL)
  {
    pTrans->close();
    pTrans = NULL;
  }
}

int
HugoOperations::equalForRow(NdbOperation* pOp, int row)
{
  for(int a = 0; a<tab.getNoOfColumns(); a++)
  {
    if (tab.getColumn(a)->getPrimaryKey() == true)
    {
      if(equalForAttr(pOp, a, row) != 0)
      {
        ERR(pOp->getNdbError());
        setNdbError(pOp->getNdbError());
        return NDBT_FAILED;
      }
    }
  }
  return NDBT_OK;
}

bool HugoOperations::getPartIdForRow(const NdbOperation* pOp,
                                     int rowid,
                                     Uint32& partId)
{
  if (tab.getFragmentType() == NdbDictionary::Object::UserDefined)
  {
    /* Primary keys and Ordered indexes are partitioned according
     * to the row number
     * PartitionId must be set for PK access.  Ordered indexes
     * can scan all partitions.
     */
    if (pOp->getType() == NdbOperation::PrimaryKeyAccess)
    {
      /* Need to set the partitionId for this op
       * For Hugo, we use 'HASH' partitioning, which is probably
       * better called 'MODULO' partitioning with
       * FragId == rowNum % NumPartitions
       * This gives a good balance with the normal Hugo data, but different
       * row to partition assignments than normal key partitioning.
       */
      const Uint32 numFrags= tab.getFragmentCount();
      partId= rowid % numFrags;
      g_info << "Returning partition Id of " << partId << endl;
      return true;
    }
  }
  partId= ~0;
  return false;
}

int HugoOperations::equalForAttr(NdbOperation* pOp,
				   int attrId, 
				   int rowId){
  const NdbDictionary::Column* attr = tab.getColumn(attrId);  
  if (attr->getPrimaryKey() == false){
    g_info << "Can't call equalForAttr on non PK attribute" << endl;
    return NDBT_FAILED;
  }
  
  int len = attr->getSizeInBytes();
  char buf[8000];
  memset(buf, 0, sizeof(buf));
  Uint32 real_len;
  const char * value = calc.calcValue(rowId, attrId, 0, buf, len, &real_len);
  return pOp->equal( attr->getName(), value, real_len);
}

int HugoOperations::setValueForAttr(NdbOperation* pOp,
				      int attrId, 
				      int rowId,
				      int updateId){
  const NdbDictionary::Column* attr = tab.getColumn(attrId);     
  
  if (! (attr->getType() == NdbDictionary::Column::Blob))
  {
    int len = attr->getSizeInBytes();
    char buf[8000];
    memset(buf, 0, sizeof(buf));
    Uint32 real_len;
    const char * value = calc.calcValue(rowId, attrId,
                                        updateId, buf, len, &real_len);
    return pOp->setValue( attr->getName(), value, real_len);
  }
  else
  {
    char buf[32000];
    int len = (int)sizeof(buf);
    Uint32 real_len;
    const char * value = calc.calcValue(rowId, attrId,
                                        updateId, buf, len, &real_len);
    NdbBlob * b = pOp->getBlobHandle(attrId);
    if (b == 0)
      return -1;

    if (real_len == 0)
      return b->setNull();
    else
      return b->setValue(value, real_len);
  }
}

int
HugoOperations::verifyUpdatesValue(int updatesValue, int _numRows){
  _numRows = (_numRows == 0 ? rows.size() : _numRows);
  
  int result = NDBT_OK;
  
  for(int i = 0; i<_numRows; i++){
    if(calc.verifyRowValues(rows[i]) != NDBT_OK){
      g_err << "Inconsistent row" 
	    << endl << "\t" << rows[i]->c_str().c_str() << endl;
      result = NDBT_FAILED;
      continue;
    }
    
    if(calc.getUpdatesValue(rows[i]) != updatesValue){
      result = NDBT_FAILED;
      g_err << "Invalid updates value for row " << i << endl
	    << " updatesValue: " << updatesValue << endl
	    << " calc.getUpdatesValue: " << calc.getUpdatesValue(rows[i]) << endl 
	    << rows[i]->c_str().c_str() << endl;
      continue;
    }
  }
  
  if(_numRows == 0){
    g_err << "No rows -> Invalid updates value" << endl;
    return NDBT_FAILED;
  }

  return result;
}

void HugoOperations::allocRows(int _numRows){
  if(_numRows <= 0){
    g_info << "Illegal value for num rows : " << _numRows << endl;
    abort();
  }
  
  for(int b=rows.size(); b<_numRows; b++){
    rows.push_back(new NDBT_ResultRow(tab));
  }
}

void HugoOperations::deallocRows(){
  while(rows.size() > 0){
    delete rows.back();
    rows.erase(rows.size() - 1);
  }
}

int HugoOperations::saveCopyOfRecord(int numRecords ){

  if (numRecords > (int)rows.size())
    return NDBT_FAILED;

  for (int i = 0; i < numRecords; i++){
    savedRecords.push_back(rows[i]->c_str());    
  }
  return NDBT_OK;  
}

BaseString HugoOperations::getRecordStr(int recordNum){
  if (recordNum > (int)rows.size())
    return NULL;
  return rows[recordNum]->c_str();
}

int HugoOperations::getRecordGci(int recordNum){
  return pTrans->getGCI();
}


int HugoOperations::compareRecordToCopy(int numRecords ){
  if (numRecords > (int)rows.size())
    return NDBT_FAILED;
  if ((unsigned)numRecords > savedRecords.size())
    return NDBT_FAILED;

  int result = NDBT_OK;
  for (int i = 0; i < numRecords; i++){
    BaseString str = rows[i]->c_str();
    ndbout << "row["<<i<<"]: " << str << endl;
    ndbout << "sav["<<i<<"]: " << savedRecords[i] << endl;
    if (savedRecords[i] == str){
      ;
    } else {
      result = NDBT_FAILED;
    }    
  }
  return result;
}

void
HugoOperations::refresh() {
  NdbTransaction * t = getTransaction(); 
  if(t)
    t->refresh();
}

int HugoOperations::indexReadRecords(Ndb*, const char * idxName, int recordNo,
				     bool exclusive,
				     int numRecords){
    
  int a;
  allocRows(numRecords);
  int check;
  for(int r=0; r < numRecords; r++){
    NdbOperation* pOp = pTrans->getNdbIndexOperation(idxName, tab.getName());
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    if (exclusive == true)
      check = pOp->readTupleExclusive();
    else
      check = pOp->readTuple();
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    // Define primary keys
    if (equalForRow(pOp, r+recordNo) != 0)
      return NDBT_FAILED;
    
    // Define attributes to read  
    for(a = 0; a<tab.getNoOfColumns(); a++){
      if((rows[r]->attributeStore(a) = 
	  pOp->getValue(tab.getColumn(a)->getName())) == 0) {
	ERR(pTrans->getNdbError());
        setNdbError(pTrans->getNdbError());
	return NDBT_FAILED;
      }
    } 
  }
  return NDBT_OK;
}

int 
HugoOperations::indexUpdateRecord(Ndb*,
				  const char * idxName, 
				  int recordNo,
				  int numRecords,
				  int updatesValue){
  int a; 
  allocRows(numRecords);
  int check;
  for(int r=0; r < numRecords; r++){
    NdbOperation* pOp = pTrans->getNdbIndexOperation(idxName, tab.getName());
    if (pOp == NULL) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    check = pOp->updateTuple();
    if( check == -1 ) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
    
    // Define primary keys
    if (equalForRow(pOp, r+recordNo) != 0)
      return NDBT_FAILED;
    
    // Define attributes to update
    for(a = 0; a<tab.getNoOfColumns(); a++){
      if (tab.getColumn(a)->getPrimaryKey() == false){
	if(setValueForAttr(pOp, a, recordNo+r, updatesValue ) != 0){ 
	  ERR(pTrans->getNdbError());
          setNdbError(pTrans->getNdbError());
	  return NDBT_FAILED;
	}
      }
    } 
  }
  return NDBT_OK;
}

int 
HugoOperations::scanReadRecords(Ndb* pNdb, NdbScanOperation::LockMode lm,
				int records){

  allocRows(records);
  NdbScanOperation * pOp = pTrans->getNdbScanOperation(tab.getName());
  
  if(!pOp)
    return -1;

  if(pOp->readTuples(lm, 0, 1)){
    return -1;
  }
  
  for(int a = 0; a<tab.getNoOfColumns(); a++){
    if((rows[0]->attributeStore(a) = 
	pOp->getValue(tab.getColumn(a)->getName())) == 0) {
      ERR(pTrans->getNdbError());
      setNdbError(pTrans->getNdbError());
      return NDBT_FAILED;
    }
  } 
  
  RsPair p = {pOp, records};
  m_result_sets.push_back(p);
  
  return 0;
}

static void
update(const NdbError & _err)
{
  NdbError & error = (NdbError &) _err;
  ndberror_struct ndberror = (ndberror_struct)error;
  ndberror_update(&ndberror);
  error = NdbError(ndberror);
}

const NdbError &
HugoOperations::getNdbError() const
{
  update(m_error);
  return m_error;
}

void
HugoOperations::setNdbError(const NdbError& error)
{
  m_error.code = error.code ? error.code : 1;
}

template class Vector<HugoOperations::RsPair>;
