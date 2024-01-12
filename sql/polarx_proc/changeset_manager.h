//
// Created by wumu on 2022/10/19.
//

#ifndef MYSQL_CHANGESET_MANAGER_H
#define MYSQL_CHANGESET_MANAGER_H

#include "changeset.h"

namespace im {
/**
 * @brief ChangesetManager manage the life-cycle of changeset
 */
class ChangesetManager {
 public:
  ChangesetManager() { InitThreadPool(8); }

  ~ChangesetManager() = default;

  void *operator new(size_t size) {
    return my_malloc(key_memory_CS_PRIMARY_KEY, size,
                     MYF(MY_WME | ME_FATALERROR));
  }

  void operator delete(void *ptr) { my_free(ptr); }

  int start_track(const std::string &table_name, u_int64_t memory_limit);

  int stop_track(const std::string &table_name);

  int fence_change(const std::string &table_name);

  int fetch_change(const std::string &table_name, bool delete_last_cs,
                   std::map<std::string, ChangesetResult *> &changes,
                   TABLE_SHARE *table_share);

  int fetch_times(const std::string &table_name, Changeset::Stats &stats);

  /**
   * @brief Report stats of change-set
   *
   * @param result
   * @return int
   */
  int stats(std::map<im::DBTableName, Changeset::Stats> *stats) {
    polarx_rpc::CautoSpinRWLock lock(rw_lock, false, 2000);
    for (auto &table : changeset_map) {
      auto &full_table_name = table.first;
      auto &changeset = table.second;
      (*stats).emplace(full_table_name, changeset->update_stats());
    }
    return 0;
  }

  /**
   * cache map
   */
  ChangeSetCache *get_changeset_from_cache(THD *thd,
                                           const DBTableName &full_table_name);

  void erase_changeset_cache_by_thd(THD *thd);

  /**
   * register changes
   */
  void write_row_to_cache(THD *thd, TABLE *table, uchar const *record);

  void update_row_to_cache(THD *thd, TABLE *table, const uchar *beforeRecord,
                           const uchar *afterRecord);

  void delete_row_to_cache(THD *thd, TABLE *table, uchar const *record);

  void set_save_point(THD *thd, my_off_t pos);

  void rollback_to_save_point(THD *thd, my_off_t pos);

  void commit_change(THD *thd);

  void rollback_change(THD *thd);

  static void get_primary_keys(uchar const *record, KEY *key_info,
                               std::string &pk);

  int open_table(const std::string &tableName, TABLE **output) {
    return open_table(tableName, output, TL_WRITE);
  }

  int open_table(const std::string &tableName, TABLE **output,
                 thr_lock_type lock_type);

  static atomic_bool changeset_start;

  static inline bool is_changeset_enable() {
    return changeset_start.load() && opt_enable_changeset;
  }

  void erase_all_changeset();

 private:
  bool is_changeset_stop(const DBTableName &full_table_name);

  void set_changeset_stop(const DBTableName &full_table_name);

  void commit(const DBTableName &full_table_name,
              std::unique_ptr<ChangeSetCache> &cache);

  bool has_changeset(const DBTableName &full_table_name);

  void erase_changeset(const DBTableName &full_table_name);

  void init_changeset(const DBTableName &full_table_name,
                      u_int64_t memory_limit);

  Changeset *get_changeset_ptr(const DBTableName &full_table_name);

  void fetch_changeset(const DBTableName &full_table_name, bool delete_last_cs,
                       std::map<std::string, ChangesetResult *> &res,
                       TABLE_SHARE *table_share);

  Changeset::Stats fetch_changeset_stats(const DBTableName &full_table_name);

  mutable polarx_rpc::CspinRWLock rw_lock;
  std::unordered_map<im::DBTableName, std::unique_ptr<Changeset>> changeset_map;
};

/** Global varaible of changeset */
extern ChangesetManager gChangesetManager;

}  // namespace im

#endif  // MYSQL_CHANGESET_MANAGER_H
