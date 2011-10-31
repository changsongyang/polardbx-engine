/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

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

// Typedefs for long names
typedef NdbDictionary::Object NDBOBJ;
typedef NdbDictionary::Column NDBCOL;
typedef NdbDictionary::Table NDBTAB;
typedef NdbDictionary::Index  NDBINDEX;
typedef NdbDictionary::Dictionary  NDBDICT;
typedef NdbDictionary::Event  NDBEVENT;

#define IS_TMP_PREFIX(A) (is_prefix(A, tmp_file_prefix))

#define INJECTOR_EVENT_LEN 200

#define NDB_INVALID_SCHEMA_OBJECT 241

extern handlerton *ndbcluster_hton;

const uint max_ndb_nodes= 256; /* multiple of 32 */

static const char *ha_ndb_ext=".ndb";

#ifdef HAVE_NDB_BINLOG
#define NDB_EXCEPTIONS_TABLE_SUFFIX "$EX"
#define NDB_EXCEPTIONS_TABLE_SUFFIX_LOWER "$ex"

const uint error_conflict_fn_violation= 9999;
#endif /* HAVE_NDB_BINLOG */

extern Ndb_cluster_connection* g_ndb_cluster_connection;

extern unsigned char g_node_id_map[max_ndb_nodes];
extern pthread_mutex_t LOCK_ndb_util_thread;
extern pthread_cond_t COND_ndb_util_thread;
extern pthread_mutex_t LOCK_ndb_index_stat_thread;
extern pthread_cond_t COND_ndb_index_stat_thread;
extern pthread_mutex_t ndbcluster_mutex;
extern HASH ndbcluster_open_tables;

/*
  Initialize the binlog part of the ndb handlerton
*/
void ndbcluster_binlog_init_handlerton();
/*
  Initialize the binlog part of the NDB_SHARE
*/
int ndbcluster_binlog_init_share(THD *thd, NDB_SHARE *share, TABLE *table);
int ndbcluster_create_binlog_setup(THD *thd, Ndb *ndb, const char *key,
                                   uint key_len,
                                   const char *db,
                                   const char *table_name,
                                   TABLE * table);
int ndbcluster_create_event(THD *thd, Ndb *ndb, const NDBTAB *table,
                            const char *event_name, NDB_SHARE *share,
                            int push_warning= 0);
int ndbcluster_create_event_ops(THD *thd,
                                NDB_SHARE *share,
                                const NDBTAB *ndbtab,
                                const char *event_name);
int ndbcluster_log_schema_op(THD *thd,
                             const char *query, int query_length,
                             const char *db, const char *table_name,
                             uint32 ndb_table_id,
                             uint32 ndb_table_version,
                             enum SCHEMA_OP_TYPE type,
                             const char *new_db,
                             const char *new_table_name);
int ndbcluster_drop_event(THD *thd, Ndb *ndb, NDB_SHARE *share,
                          const char *type_str,
                          const char * dbname, const char * tabname);
int ndbcluster_handle_drop_table(THD *thd, Ndb *ndb, NDB_SHARE *share,
                                 const char *type_str,
                                 const char * db, const char * tabname);
void ndb_rep_event_name(String *event_name,
                        const char *db, const char *tbl, my_bool full);
#ifdef HAVE_NDB_BINLOG
int
ndbcluster_get_binlog_replication_info(THD *thd, Ndb *ndb,
                                       const char* db,
                                       const char* table_name,
                                       uint server_id,
                                       const TABLE *table,
                                       Uint32* binlog_flags,
                                       const st_conflict_fn_def** conflict_fn,
                                       st_conflict_fn_arg* args,
                                       Uint32* num_args);
int
ndbcluster_apply_binlog_replication_info(THD *thd,
                                         NDB_SHARE *share,
                                         const NDBTAB* ndbtab,
                                         TABLE* table,
                                         const st_conflict_fn_def* conflict_fn,
                                         const st_conflict_fn_arg* args,
                                         Uint32 num_args,
                                         bool do_set_binlog_flags,
                                         Uint32 binlog_flags);
int
ndbcluster_read_binlog_replication(THD *thd, Ndb *ndb,
                                   NDB_SHARE *share,
                                   const NDBTAB *ndbtab,
                                   uint server_id,
                                   TABLE *table,
                                   bool do_set_binlog_flags);
#endif
int ndb_create_table_from_engine(THD *thd, const char *db,
                                 const char *table_name);
int ndbcluster_binlog_start();


/*
  Setup function for the ndb binlog component. The function should be
  called on startup until it succeeds(to allow initial setup) and with
  regular intervals afterwards to reconnect after a lost cluster
  connection
*/
bool ndb_binlog_setup(THD *thd);

/*
  Will return true when the ndb binlog component is properly setup
  and ready to receive events from the cluster. As long as function
  returns false, all tables in this MySQL Server are opened in read only
  mode to avoid writes before the binlog is ready to record them.
 */
bool ndb_binlog_is_read_only(void);

extern NDB_SHARE *ndb_apply_status_share;
extern NDB_SHARE *ndb_schema_share;

extern my_bool ndb_binlog_running;

bool
ndbcluster_show_status_binlog(THD* thd, stat_print_fn *stat_print,
                              enum ha_stat_type stat_type);

/*
  prototypes for ndb handler utility function also needed by
  the ndb binlog code
*/
int cmp_frm(const NDBTAB *ndbtab, const void *pack_data,
            size_t pack_length);

/*
  Helper functions
*/
bool
ndbcluster_check_if_local_table(const char *dbname, const char *tabname);
