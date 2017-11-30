/* Copyright (c) 2005, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "sql/sql_plugin.h"

#include "my_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "m_ctype.h"
#include "m_string.h"
#include "map_helpers.h"
#include "mutex_lock.h"        // MUTEX_LOCK
#include "my_alloc.h"
#include "my_base.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_default.h"        // free_defaults
#include "my_getopt.h"
#include "my_inttypes.h"
#include "my_list.h"
#include "my_loglevel.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "my_sharedlib.h"
#include "my_sys.h"
#include "my_thread_local.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/components/services/log_shared.h"
#include "mysql/components/services/psi_memory_bits.h"
#include "mysql/components/services/psi_mutex_bits.h"
#include "mysql/components/services/system_variable_source_type.h"
#include "mysql/plugin_audit.h"
#include "mysql/plugin_auth.h"
#include "mysql/plugin_clone.h"
#include "mysql/plugin_group_replication.h"
#include "mysql/plugin_keyring.h"
#include "mysql/plugin_validate_password.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/mysql_rwlock.h"
#include "mysql/psi/psi_base.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql_com.h"
#include "mysql_version.h"
#include "mysqld_error.h"
#include "prealloced_array.h"
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h" // check_table_access
#include "sql/auto_thd.h"               // Auto_THD
#include "sql/current_thd.h"
#include "sql/dd_sql_view.h"      // update_referencing_views_metadata
#include "sql/dd/cache/dictionary_client.h" // dd::cache::Dictionary_client
#include "sql/dd/dd_schema.h"            // dd::Schema_MDL_locker
#include "sql/dd/info_schema/metadata.h" // dd::info_schema::store_dynamic_p...
#include "sql/dd/string_type.h" // dd::String_type
#include "sql/dd_sql_view.h"      // update_referencing_views_metadata
#include "sql/debug_sync.h"    // DEBUG_SYNC
#include "sql/derror.h"        // ER_THD
#include "sql/field.h"
#include "sql/handler.h"       // ha_initalize_handlerton
#include "sql/key.h"           // key_copy
#include "sql/log.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"        // files_charset_info
#include "sql/persisted_variable.h"// Persisted_variables_cache
#include "sql/protocol_classic.h"
#include "sql/psi_memory_key.h"
#include "sql/records.h"       // READ_RECORD
#include "sql/set_var.h"
#include "sql/sql_audit.h"     // mysql_audit_acquire_plugins
#include "sql/sql_base.h"      // close_mysql_tables
#include "sql/sql_class.h"     // THD
#include "sql/sql_const.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_parse.h"     // check_string_char_length
#include "sql/sql_plugin_var.h"
#include "sql/sql_show.h"      // add_status_vars
#include "sql/sql_table.h"
#include "sql/sys_vars_resource_mgr.h"
#include "sql/sys_vars_shared.h" // intern_find_sys_var
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/thd_raii.h"
#include "sql/thr_malloc.h"
#include "sql/transaction.h"   // trans_rollback_stmt
#include "sql_string.h"
#include "template_utils.h"    // pointer_cast
#include "thr_lock.h"
#include "thr_mutex.h"
#include "typelib.h"


/**
  @page page_ext_plugins Plugins

  The Big Picture
  ----------------

  @startuml
  actor "SQL client" as client
  box "MySQL Server" #LightBlue
    participant "Server Code" as server
    participant "Plugin" as plugin
  endbox

  == INSTALL PLUGIN ==
  server -> plugin : initialize
  activate plugin
  plugin --> server : initialization done

  == CLIENT SESSION ==
  loop many
    client -> server : SQL command
    server -> server : Add reference for Plugin if absent
    loop one or many
      server -> plugin : plugin API call
      plugin --> server : plugin API call result
    end
    server -> server : Optionally release reference for Plugin
    server --> client : SQL command reply
  end

  == UNINSTALL PLUGIN ==
  server -> plugin : deinitialize
  plugin --> server : deinitialization done
  deactivate plugin
  @enduml

  @sa Sql_cmd_install_plugin, Sql_cmd_uninstall_plugin.
*/

/**
  @page page_ext_plugin_services Plugin Services

  Adding Plugin Services Into The Big Picture
  ------------------------------------

  You probably remember the big picture for @ref page_ext_plugins.
  Below is an extended version of it with plugin services added.

  @startuml

  actor "SQL client" as client
  box "MySQL Server" #LightBlue
    participant "Server Code" as server
    participant "Plugin" as plugin
  endbox

  == INSTALL PLUGIN ==
  server -> plugin : initialize
  activate plugin

  loop zero or many
    plugin -> server : service API call
    server --> plugin : service API result
  end
  plugin --> server : initialization done

  == CLIENT SESSION ==
  loop many
    client -> server : SQL command
    server -> server : Add reference for Plugin if absent
    loop one or many
      server -> plugin : plugin API call
      loop zero or many
        plugin -> server : service API call
        server --> plugin : service API result
      end
      plugin --> server : plugin API call result
    end
    server -> server : Optionally release reference for Plugin
    server --> client : SQL command reply
  end

  == UNINSTALL PLUGIN ==
  server -> plugin : deinitialize
  loop zero or many
    plugin -> server : service API call
    server --> plugin : service API result
  end
  plugin --> server : deinitialization done
  deactivate plugin
  @enduml

  Understanding and creating plugin services
  -----------------------------

  - @subpage page_ext_plugin_svc_anathomy
  - @subpage page_ext_plugin_svc_new_service_howto
  - @subpage page_ext_plugin_api_goodservices

  @section sect_ext_plugin_svc_reference Plugin Services Reference

   See @ref group_ext_plugin_services
*/

/**
  @page page_ext_plugin_svc_anathomy Plugin Service Anathomy

  A "service" is a struct of C function pointers.

  It is a tool to expose a pre-exitsing set of server functions to plugins.
  You need the actual server functions as a starting point.

  The server has all service structs defined and initialized so
  that the the function pointers point to the actual service implementation
  functions.

  The server also keeps a global list of the plugin service reference
  structures called ::list_of_services.

  See ::st_service_ref for details of what a service reference is.

  The server copies of all plugin structures are filled in at compile time
  with the function pointers of the actual server functions that implement
  the service functions. References to them are stored into the relevant
  element of ::list_of_services.

  Each plugin must export pointer symbols for every plugin service that
  the server knows about.

  The plugin service pointers are initialized with the version of the plugin
  service that the plugin expects.

  When a dynamic plugin shared object is loaded by ::plugin_dl_add it will
  iterate over ::list_of_services, find the plugin symbol by name,
  check the service version stored in that symbol against the one stored into
  ::st_service_ref and then will replace the version stored in plugin's struct
  pointer with the actual pointer of the server's copy of the same structure.

  When that is filled in the plugin can use the newly set server structure
  through its local pointer to call into the service method pointers that point
  to the server implementaiton functions.

  Once set to the server's structure, the plugin's service pointer value is
  never reset back to service version.

  The plugin service header also defines a set of convenience macros
  that replace top level plugin service calls with the corresponding function
  pointer call, i.e. for service foo:

  ~~~~
  struct foo_service_st {
     int (*foo_mtd_1)(int a);
  }

  struct foo_service_st *foo_service;
  ~~~~

  a convenience macro is defined for `foo_mtd_1` as follows:

  ~~~~
  #define foo_mtd_1(a)  foo_service->foo_mtd_1(a)
  ~~~~

  This trick allows plugin service functions to look as top level function
  calls inside the plugin code.

  @sa plugin_add, plugin_del, plugin_dl_add, plugin_dl_del, list_of_services,
    st_service_ref
*/

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif

#include <algorithm>
#include <new>
#include <unordered_map>
#include <utility>

#include "sql/srv_session.h"   // Srv_session::check_for_stale_threads()

using std::min;
using std::max;

#define REPORT_TO_LOG  1
#define REPORT_TO_USER 2

#ifndef DBUG_OFF
static PSI_memory_key key_memory_plugin_ref;
#endif

static PSI_memory_key key_memory_plugin_mem_root;
static PSI_memory_key key_memory_plugin_init_tmp;
static PSI_memory_key key_memory_plugin_int_mem_root;
static PSI_memory_key key_memory_mysql_plugin;
static PSI_memory_key key_memory_mysql_plugin_dl;
static PSI_memory_key key_memory_plugin_bookmark;

extern st_mysql_plugin *mysql_optional_plugins[];
extern st_mysql_plugin *mysql_mandatory_plugins[];

/**
  @note The order of the enumeration is critical.
  @see construct_options
*/
const char *global_plugin_typelib_names[]=
  { "OFF", "ON", "FORCE", "FORCE_PLUS_PERMANENT", NULL };
static TYPELIB global_plugin_typelib=
  { array_elements(global_plugin_typelib_names)-1,
    "", global_plugin_typelib_names, NULL };

static I_List<i_string> opt_plugin_load_list;
I_List<i_string> *opt_plugin_load_list_ptr= &opt_plugin_load_list;
static I_List<i_string> opt_early_plugin_load_list;
I_List<i_string> *opt_early_plugin_load_list_ptr= &opt_early_plugin_load_list;
char *opt_plugin_dir_ptr;
char opt_plugin_dir[FN_REFLEN];
/*
  When you ad a new plugin type, add both a string and make sure that the
  init and deinit array are correctly updated.
*/
const LEX_STRING plugin_type_names[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  { C_STRING_WITH_LEN("UDF") },
  { C_STRING_WITH_LEN("STORAGE ENGINE") },
  { C_STRING_WITH_LEN("FTPARSER") },
  { C_STRING_WITH_LEN("DAEMON") },
  { C_STRING_WITH_LEN("INFORMATION SCHEMA") },
  { C_STRING_WITH_LEN("AUDIT") },
  { C_STRING_WITH_LEN("REPLICATION") },
  { C_STRING_WITH_LEN("AUTHENTICATION") },
  { C_STRING_WITH_LEN("VALIDATE PASSWORD") },
  { C_STRING_WITH_LEN("GROUP REPLICATION") },
  { C_STRING_WITH_LEN("KEYRING") },
  { C_STRING_WITH_LEN("CLONE") }
};

extern int initialize_schema_table(st_plugin_int *plugin);
extern int finalize_schema_table(st_plugin_int *plugin);

/*
  The number of elements in both plugin_type_initialize and
  plugin_type_deinitialize should equal to the number of plugins
  defined.
*/
plugin_type_init plugin_type_initialize[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0,ha_initialize_handlerton,0,0,initialize_schema_table,
  initialize_audit_plugin,0,0,0
};

plugin_type_init plugin_type_deinitialize[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0,ha_finalize_handlerton,0,0,finalize_schema_table,
  finalize_audit_plugin,0,0,0
};

static const char *plugin_interface_version_sym=
                   "_mysql_plugin_interface_version_";
static const char *sizeof_st_plugin_sym=
                   "_mysql_sizeof_struct_st_plugin_";
static const char *plugin_declarations_sym= "_mysql_plugin_declarations_";
static int min_plugin_interface_version= MYSQL_PLUGIN_INTERFACE_VERSION & ~0xFF;

static void*	innodb_callback_data;

/* Note that 'int version' must be the first field of every plugin
   sub-structure (plugin->info).
*/
static int min_plugin_info_interface_version[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0x0000,
  MYSQL_HANDLERTON_INTERFACE_VERSION,
  MYSQL_FTPARSER_INTERFACE_VERSION,
  MYSQL_DAEMON_INTERFACE_VERSION,
  MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION,
  MYSQL_AUDIT_INTERFACE_VERSION,
  MYSQL_REPLICATION_INTERFACE_VERSION,
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  MYSQL_VALIDATE_PASSWORD_INTERFACE_VERSION,
  MYSQL_GROUP_REPLICATION_INTERFACE_VERSION,
  MYSQL_KEYRING_INTERFACE_VERSION,
  MYSQL_CLONE_INTERFACE_VERSION
};
static int cur_plugin_info_interface_version[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0x0000, /* UDF: not implemented */
  MYSQL_HANDLERTON_INTERFACE_VERSION,
  MYSQL_FTPARSER_INTERFACE_VERSION,
  MYSQL_DAEMON_INTERFACE_VERSION,
  MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION,
  MYSQL_AUDIT_INTERFACE_VERSION,
  MYSQL_REPLICATION_INTERFACE_VERSION,
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  MYSQL_VALIDATE_PASSWORD_INTERFACE_VERSION,
  MYSQL_GROUP_REPLICATION_INTERFACE_VERSION,
  MYSQL_KEYRING_INTERFACE_VERSION,
  MYSQL_CLONE_INTERFACE_VERSION
};

/* support for Services */

#include "sql/sql_plugin_services.h"

/*
  A mutex LOCK_plugin_delete must be acquired before calling plugin_del
  function.
*/
mysql_mutex_t LOCK_plugin_delete;

/**
  Serializes access to the global plugin memory list.

  LOCK_plugin must be acquired before accessing
  plugin_dl_array, plugin_array and plugin_hash.
  We are always manipulating ref count, so a rwlock here is unneccessary.
  If it must be taken together with the LOCK_system_variables_hash then
  LOCK_plugin must be taken before LOCK_system_variables_hash.
*/
mysql_mutex_t LOCK_plugin;
/**
  Serializes the INSTALL and UNINSTALL PLUGIN commands.
  Must be taken before LOCK_plugin.
*/
mysql_mutex_t LOCK_plugin_install;
static Prealloced_array<st_plugin_dl*, 16> *plugin_dl_array;
static Prealloced_array<st_plugin_int*, 16> *plugin_array;
static collation_unordered_map<std::string, st_plugin_int*>
  *plugin_hash[MYSQL_MAX_PLUGIN_TYPE_NUM]= {nullptr};
static bool reap_needed= false;
static int plugin_array_version=0;

static bool initialized= false;

static MEM_ROOT plugin_mem_root;
static uint global_variables_dynamic_size= 0;
static malloc_unordered_map<std::string, st_bookmark *> *bookmark_hash;
/** Hash for system variables of string type with MEMALLOC flag. */
static malloc_unordered_map<std::string, st_bookmark *>
  *malloced_string_type_sysvars_bookmark_hash;

/* prototypes */
static void plugin_load(MEM_ROOT *tmp_root, int *argc, char **argv);
static bool plugin_load_list(MEM_ROOT *tmp_root, int *argc, char **argv,
                             const char *list);
static bool check_if_option_is_deprecated(int optid,
                                          const struct my_option *opt,
                                          char *argument);
static int test_plugin_options(MEM_ROOT *, st_plugin_int *,
                               int *, char **);
static bool register_builtin(st_mysql_plugin *, st_plugin_int *,
                             st_plugin_int **);
static void unlock_variables(struct System_variables *vars);
static void cleanup_variables(THD *thd, struct System_variables *vars);
static void plugin_vars_free_values(sys_var *vars);
static void plugin_var_memalloc_free(struct System_variables *vars);
static void restore_pluginvar_names(sys_var *first);
#define my_intern_plugin_lock(A,B) intern_plugin_lock(A,B)
#define my_intern_plugin_lock_ci(A,B) intern_plugin_lock(A,B)
static plugin_ref intern_plugin_lock(LEX *lex, plugin_ref plugin);
static void intern_plugin_unlock(LEX *lex, plugin_ref plugin);
static void reap_plugins(void);

malloc_unordered_map<std::string, st_bookmark *>* get_bookmark_hash(void)
{
  return bookmark_hash;
}

static void report_error(int where_to, uint error, ...)
{
  va_list args;
  if (where_to & REPORT_TO_USER)
  {
    va_start(args, error);
    my_printv_error(error, ER_THD(current_thd, error), MYF(0), args);
    va_end(args);
  }
  if (where_to & REPORT_TO_LOG)
  {
    va_start(args, error);
    error_log_printf(ERROR_LEVEL, ER_DEFAULT(error), args);
    va_end(args);
  }
}

/**
   Check if the provided path is valid in the sense that it does cause
   a relative reference outside the directory.

   @note Currently, this function only check if there are any
   characters in FN_DIRSEP in the string, but it might change in the
   future.

   @code
   check_valid_path("../foo.so") -> true
   check_valid_path("foo.so") -> false
   @endcode
 */
bool check_valid_path(const char *path, size_t len)
{
  size_t prefix= my_strcspn(files_charset_info, path, path + len, FN_DIRSEP,
                            strlen(FN_DIRSEP));
  return  prefix < len;
}

/****************************************************************************
  Plugin support code
****************************************************************************/

static st_plugin_dl *plugin_dl_find(const LEX_STRING *dl)
{
  DBUG_ENTER("plugin_dl_find");
  for (st_plugin_dl **it= plugin_dl_array->begin();
       it != plugin_dl_array->end(); ++it)
  {
    st_plugin_dl *tmp= *it;
    if (tmp->ref_count &&
        ! my_strnncoll(files_charset_info,
                       pointer_cast<uchar*>(dl->str), dl->length,
                       pointer_cast<uchar*>(tmp->dl.str), tmp->dl.length))
      DBUG_RETURN(tmp);
  }
  DBUG_RETURN(NULL);
}


static st_plugin_dl *plugin_dl_insert_or_reuse(st_plugin_dl *plugin_dl)
{
  DBUG_ENTER("plugin_dl_insert_or_reuse");
  st_plugin_dl *tmp;
  for (st_plugin_dl **it= plugin_dl_array->begin();
       it != plugin_dl_array->end(); ++it)
  {
    tmp= *it;
    if (! tmp->ref_count)
    {
      memcpy(tmp, plugin_dl, sizeof(st_plugin_dl));
      DBUG_RETURN(tmp);
    }
  }
  if (plugin_dl_array->push_back(plugin_dl))
    DBUG_RETURN(NULL);
  tmp= plugin_dl_array->back()=
    static_cast<st_plugin_dl*>(memdup_root(&plugin_mem_root, plugin_dl,
                                           sizeof(st_plugin_dl)));
  DBUG_RETURN(tmp);
}


static inline void free_plugin_mem(st_plugin_dl *p)
{
#ifdef HAVE_VALGRIND
  /*
    The valgrind leak report is done at the end of the program execution.
    But since the plugins are unloaded from the memory,
    it is impossible for valgrind to correctly report the leak locations.
    So leave the shared objects (.DLL/.so) open for the symbols definition.
  */
#else /* not HAVE_VALGRIND */
  if (p->handle)
    dlclose(p->handle);
#endif
  my_free(p->dl.str);
  if (p->version != MYSQL_PLUGIN_INTERFACE_VERSION)
    my_free(p->plugins);
}

/**
  Loads a dynamic plugin

  Fills in a ::st_plugin_dl structure.
  Initializes the plugin services pointer inside the plugin.
  Does not initialize the individual plugins.
  Must have LOCK_plugin locked. On error releases LOCK_plugin.

  @arg dl      The path to the plugin binary to load
  @arg report  a bitmask that's passed down to report_error()

  @return      A plugin reference.
  @retval      NULL      failed to load the plugin
*/
static st_plugin_dl *plugin_dl_add(const LEX_STRING *dl, int report)
{
  char dlpath[FN_REFLEN];
  uint dummy_errors, i;
  size_t plugin_dir_len, dlpathlen;
  st_plugin_dl *tmp, plugin_dl;
  void *sym;
  DBUG_ENTER("plugin_dl_add");
  DBUG_PRINT("enter", ("dl->str: '%s', dl->length: %d",
                       dl->str, (int) dl->length));
  plugin_dir_len= strlen(opt_plugin_dir);
  /*
    Ensure that the dll doesn't have a path.
    This is done to ensure that only approved libraries from the
    plugin directory are used (to make this even remotely secure).
  */
  LEX_CSTRING dl_cstr= {dl->str, dl->length};
  if (check_valid_path(dl->str, dl->length) ||
      check_string_char_length(dl_cstr, "", NAME_CHAR_LEN,
                               system_charset_info, 1) ||
      plugin_dir_len + dl->length + 1 >= FN_REFLEN)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    report_error(report, ER_UDF_NO_PATHS);
    DBUG_RETURN(NULL);
  }
  /* If this dll is already loaded just increase ref_count. */
  if ((tmp= plugin_dl_find(dl)))
  {
    tmp->ref_count++;
    DBUG_RETURN(tmp);
  }
  memset(&plugin_dl, 0, sizeof(plugin_dl));
  /* Compile dll path */
  dlpathlen=
    strxnmov(dlpath, sizeof(dlpath) - 1, opt_plugin_dir, "/", dl->str, NullS) -
    dlpath;
  (void) unpack_filename(dlpath, dlpath);
  plugin_dl.ref_count= 1;
  /* Open new dll handle */
  mysql_mutex_assert_owner(&LOCK_plugin);
  if (!(plugin_dl.handle= dlopen(dlpath, RTLD_NOW)))
  {
    const char *errmsg;
    int error_number= dlopen_errno;
    /*
      Conforming applications should use a critical section to retrieve
      the error pointer and buffer...
    */
    DLERROR_GENERATE(errmsg, error_number);

    if (!strncmp(dlpath, errmsg, dlpathlen))
    { // if errmsg starts from dlpath, trim this prefix.
      errmsg+=dlpathlen;
      if (*errmsg == ':') errmsg++;
      if (*errmsg == ' ') errmsg++;
    }
    mysql_mutex_unlock(&LOCK_plugin);
    report_error(report, ER_CANT_OPEN_LIBRARY, dlpath, error_number, errmsg);

    /*
      "The messages returned by dlerror() may reside in a static buffer
       that is overwritten on each call to dlerror()."

      Some implementations have a static pointer instead, and the memory it
      points to may be reported as "still reachable" by Valgrind.
      Calling dlerror() once more will free the memory.
     */
#if !defined(_WIN32)
    errmsg= dlerror();
    DBUG_ASSERT(errmsg == NULL);
#endif
    DBUG_RETURN(NULL);
  }
  /* Determine interface version */
  if (!(sym= dlsym(plugin_dl.handle, plugin_interface_version_sym)))
  {
    free_plugin_mem(&plugin_dl);
    mysql_mutex_unlock(&LOCK_plugin);
    report_error(report, ER_CANT_FIND_DL_ENTRY, plugin_interface_version_sym);
    DBUG_RETURN(NULL);
  }
  plugin_dl.version= *(int *)sym;
  /* Versioning */
  if (plugin_dl.version < min_plugin_interface_version ||
      (plugin_dl.version >> 8) > (MYSQL_PLUGIN_INTERFACE_VERSION >> 8))
  {
    free_plugin_mem(&plugin_dl);
    mysql_mutex_unlock(&LOCK_plugin);
    report_error(report, ER_CANT_OPEN_LIBRARY, dlpath, 0,
                 "plugin interface version mismatch");
    DBUG_RETURN(NULL);
  }

  /* link the services in */
  for (i= 0; i < array_elements(list_of_services); i++)
  {
    if ((sym= dlsym(plugin_dl.handle, list_of_services[i].name)))
    {
      uint ver= (uint)(intptr)*(void**)sym;
      if ((*(void**)sym) != list_of_services[i].service && /* already replaced */
          (ver > list_of_services[i].version ||
           (ver >> 8) < (list_of_services[i].version >> 8)))
      {
        char buf[MYSQL_ERRMSG_SIZE];
        snprintf(buf, sizeof(buf),
                    "service '%s' interface version mismatch",
                    list_of_services[i].name);
        mysql_mutex_unlock(&LOCK_plugin);
        report_error(report, ER_CANT_OPEN_LIBRARY, dlpath, 0, buf);
        DBUG_RETURN(NULL);
      }
      *(void**)sym= list_of_services[i].service;
    }
  }

  /* Find plugin declarations */
  if (!(sym= dlsym(plugin_dl.handle, plugin_declarations_sym)))
  {
    free_plugin_mem(&plugin_dl);
    mysql_mutex_unlock(&LOCK_plugin);
    report_error(report, ER_CANT_FIND_DL_ENTRY, plugin_declarations_sym);
    DBUG_RETURN(NULL);
  }

  if (plugin_dl.version != MYSQL_PLUGIN_INTERFACE_VERSION)
  {
    uint sizeof_st_plugin;
    st_mysql_plugin *old, *cur;
    char *ptr= (char *)sym;

    if ((sym= dlsym(plugin_dl.handle, sizeof_st_plugin_sym)))
      sizeof_st_plugin= *(int *)sym;
    else
    {
      /*
        When the following assert starts failing, we'll have to call
        report_error(report, ER_CANT_FIND_DL_ENTRY, sizeof_st_plugin_sym);
      */
      DBUG_ASSERT(min_plugin_interface_version == 0);
      sizeof_st_plugin= (int)offsetof(st_mysql_plugin, version);
    }

    /*
      What's the purpose of this loop? If the goal is to catch a
      missing 0 record at the end of a list, it will fail miserably
      since the compiler is likely to optimize this away. /Matz
     */
    for (i= 0;
         ((st_mysql_plugin *)(ptr+i*sizeof_st_plugin))->info;
         i++)
      /* no op */;

    cur= (st_mysql_plugin*)
      my_malloc(key_memory_mysql_plugin,
                (i+1)*sizeof(st_mysql_plugin), MYF(MY_ZEROFILL|MY_WME));
    if (!cur)
    {
      free_plugin_mem(&plugin_dl);
      mysql_mutex_unlock(&LOCK_plugin);
      report_error(report, ER_OUTOFMEMORY,
                   static_cast<int>(plugin_dl.dl.length));
      DBUG_RETURN(NULL);
    }
    /*
      All st_plugin fields not initialized in the plugin explicitly, are
      set to 0. It matches C standard behaviour for struct initializers that
      have less values than the struct definition.
    */
    for (i=0;
         (old=(st_mysql_plugin *)(ptr+i*sizeof_st_plugin))->info;
         i++)
      memcpy(cur+i, old, min<size_t>(sizeof(cur[i]), sizeof_st_plugin));

    sym= cur;
  }
  plugin_dl.plugins= (st_mysql_plugin *)sym;

  /*
    If report is REPORT_TO_USER, we were called from
    mysql_install_plugin. Otherwise, we are called
    indirectly from plugin_register_dynamic_and_init_all().
   */
  if (report == REPORT_TO_USER)
  {
    st_mysql_plugin *plugin= plugin_dl.plugins;
    for ( ; plugin->info ; ++plugin)
      if (plugin->flags & PLUGIN_OPT_NO_INSTALL)
      {
        mysql_mutex_unlock(&LOCK_plugin);
        report_error(report, ER_PLUGIN_NO_INSTALL, plugin->name);
        free_plugin_mem(&plugin_dl);
        DBUG_RETURN(NULL);
   }
  }

  /* Duplicate and convert dll name */
  plugin_dl.dl.length= dl->length * files_charset_info->mbmaxlen + 1;
  if (! (plugin_dl.dl.str= (char*) my_malloc(key_memory_mysql_plugin_dl,
                                             plugin_dl.dl.length, MYF(0))))
  {
    mysql_mutex_unlock(&LOCK_plugin);
    free_plugin_mem(&plugin_dl);
    report_error(report, ER_OUTOFMEMORY,
                 static_cast<int>(plugin_dl.dl.length));
    DBUG_RETURN(NULL);
  }
  plugin_dl.dl.length= copy_and_convert(plugin_dl.dl.str, plugin_dl.dl.length,
    files_charset_info, dl->str, dl->length, system_charset_info,
    &dummy_errors);
  plugin_dl.dl.str[plugin_dl.dl.length]= 0;
  /* Add this dll to array */
  if (! (tmp= plugin_dl_insert_or_reuse(&plugin_dl)))
  {
    mysql_mutex_unlock(&LOCK_plugin);
    free_plugin_mem(&plugin_dl);
    report_error(report, ER_OUTOFMEMORY,
                 static_cast<int>(sizeof(st_plugin_dl)));
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(tmp);
}


static void plugin_dl_del(const LEX_STRING *dl)
{
  DBUG_ENTER("plugin_dl_del");

  mysql_mutex_assert_owner(&LOCK_plugin);

  for (st_plugin_dl **it= plugin_dl_array->begin();
       it != plugin_dl_array->end(); ++it)
  {
    st_plugin_dl *tmp= *it;
    if (tmp->ref_count &&
        ! my_strnncoll(files_charset_info,
                       pointer_cast<uchar*>(dl->str), dl->length,
                       pointer_cast<uchar*>(tmp->dl.str), tmp->dl.length))
    {
      /* Do not remove this element, unless no other plugin uses this dll. */
      if (! --tmp->ref_count)
      {
        free_plugin_mem(tmp);
        memset(tmp, 0, sizeof(st_plugin_dl));
      }
      break;
    }
  }
  DBUG_VOID_RETURN;
}


static st_plugin_int *plugin_find_internal(const LEX_CSTRING &name,
                                                  int type)
{
  uint i;
  DBUG_ENTER("plugin_find_internal");
  if (! initialized)
    DBUG_RETURN(NULL);

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (type == MYSQL_ANY_PLUGIN)
  {
    for (i= 0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
    {
      const auto it= plugin_hash[i]->find(to_string(name));
      if (it != plugin_hash[i]->end())
        DBUG_RETURN(it->second);
    }
  }
  else
    DBUG_RETURN(find_or_nullptr(*plugin_hash[type], to_string(name)));
  DBUG_RETURN(NULL);
}


static SHOW_COMP_OPTION plugin_status(const LEX_CSTRING &name, int type)
{
  SHOW_COMP_OPTION rc= SHOW_OPTION_NO;
  st_plugin_int *plugin;
  DBUG_ENTER("plugin_is_ready");
  mysql_mutex_lock(&LOCK_plugin);
  if ((plugin= plugin_find_internal(name, type)))
  {
    rc= SHOW_OPTION_DISABLED;
    if (plugin->state == PLUGIN_IS_READY)
      rc= SHOW_OPTION_YES;
  }
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_RETURN(rc);
}


bool plugin_is_ready(const LEX_CSTRING &name, int type)
{
  bool rc= false;
  if (plugin_status(name, type) == SHOW_OPTION_YES)
    rc= true;
  return rc;
}


SHOW_COMP_OPTION plugin_status(const char *name, size_t len, int type)
{
  LEX_CSTRING plugin_name= { name, len };
  return plugin_status(plugin_name, type);
}


static plugin_ref intern_plugin_lock(LEX *lex, plugin_ref rc)
{
  st_plugin_int *pi= plugin_ref_to_int(rc);
  DBUG_ENTER("intern_plugin_lock");

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (pi->state & (PLUGIN_IS_READY | PLUGIN_IS_UNINITIALIZED))
  {
    plugin_ref plugin;
#ifdef DBUG_OFF
    /* built-in plugins don't need ref counting */
    if (!pi->plugin_dl)
      DBUG_RETURN(pi);

    plugin= pi;
#else
    /*
      For debugging, we do an additional malloc which allows the
      memory manager and/or valgrind to track locked references and
      double unlocks to aid resolving reference counting problems.
    */
    if (!(plugin= (plugin_ref) my_malloc(key_memory_plugin_ref,
                                         sizeof(pi), MYF(MY_WME))))
      DBUG_RETURN(NULL);

    *plugin= pi;
#endif
    pi->ref_count++;
    DBUG_PRINT("info",("thd: %p, plugin: \"%s\", ref_count: %d",
                       current_thd, pi->name.str, pi->ref_count));
    if (lex)
      lex->plugins.push_back(plugin);
    DBUG_RETURN(plugin);
  }
  DBUG_RETURN(NULL);
}


plugin_ref plugin_lock(THD *thd, plugin_ref *ptr)
{
  LEX *lex= thd ? thd->lex : 0;
  plugin_ref rc;
  DBUG_ENTER("plugin_lock");
  mysql_mutex_lock(&LOCK_plugin);
  rc= my_intern_plugin_lock_ci(lex, *ptr);
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_RETURN(rc);
}


plugin_ref plugin_lock_by_name(THD *thd, const LEX_CSTRING &name, int type)
{
  LEX *lex= thd ? thd->lex : 0;
  plugin_ref rc= NULL;
  st_plugin_int *plugin;
  DBUG_ENTER("plugin_lock_by_name");
  mysql_mutex_lock(&LOCK_plugin);
  if ((plugin= plugin_find_internal(name, type)))
    rc= my_intern_plugin_lock_ci(lex, plugin_int_to_ref(plugin));
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_RETURN(rc);
}


static st_plugin_int *plugin_insert_or_reuse(st_plugin_int *plugin)
{
  DBUG_ENTER("plugin_insert_or_reuse");
  st_plugin_int *tmp;
  for (st_plugin_int **it= plugin_array->begin();
       it != plugin_array->end(); ++it)
  {
    tmp= *it;
    if (tmp->state == PLUGIN_IS_FREED)
    {
      *tmp = std::move(*plugin);
      DBUG_RETURN(tmp);
    }
  }
  if (plugin_array->push_back(plugin))
    DBUG_RETURN(NULL);
  tmp= plugin_array->back()=
    new (&plugin_mem_root) st_plugin_int(std::move(*plugin));
  DBUG_RETURN(tmp);
}


/**
  Adds a plugin to the global plugin list.

  Also installs the plugin variables.
  In case of error releases ::LOCK_plugin and reports the error
  @note Requires that a write-lock is held on ::LOCK_system_variables_hash
*/
static bool plugin_add(MEM_ROOT *tmp_root,
                       const LEX_STRING *name, const LEX_STRING *dl,
                       int *argc, char **argv, int report)
{
  st_plugin_int tmp;
  st_mysql_plugin *plugin;
  DBUG_ENTER("plugin_add");
  LEX_CSTRING name_cstr= {name->str, name->length};

  mysql_mutex_assert_owner(&LOCK_plugin);
  if (plugin_find_internal(name_cstr, MYSQL_ANY_PLUGIN))
  {
    mysql_mutex_unlock(&LOCK_plugin);
    report_error(report, ER_UDF_EXISTS, name->str);
    DBUG_RETURN(true);
  }
  if (! (tmp.plugin_dl= plugin_dl_add(dl, report)))
    DBUG_RETURN(true);
  /* Find plugin by name */
  for (plugin= tmp.plugin_dl->plugins; plugin->info; plugin++)
  {
    size_t name_len= strlen(plugin->name);
    if (plugin->type >= 0 && plugin->type < MYSQL_MAX_PLUGIN_TYPE_NUM &&
        ! my_strnncoll(system_charset_info,
                       pointer_cast<const uchar*>(name->str), name->length,
                       pointer_cast<const uchar*>(plugin->name),
                       name_len))
    {
      st_plugin_int *tmp_plugin_ptr;
      if (*(int*)plugin->info <
          min_plugin_info_interface_version[plugin->type] ||
          ((*(int*)plugin->info) >> 8) >
          (cur_plugin_info_interface_version[plugin->type] >> 8))
      {
        char buf[256], dl_name[FN_REFLEN];
        strxnmov(buf, sizeof(buf) - 1, "API version for ",
                 plugin_type_names[plugin->type].str,
                 " plugin is too different", NullS);
        /* copy the library name so we can release the mutex */
        strncpy(dl_name, dl->str, sizeof(dl_name) - 1);
        dl_name[sizeof(dl_name) - 1] = 0;
        plugin_dl_del(dl);
        mysql_mutex_unlock(&LOCK_plugin);
        report_error(report, ER_CANT_OPEN_LIBRARY, dl_name, 0, buf);
        DBUG_RETURN(true);
      }
      tmp.plugin= plugin;
      tmp.name.str= (char *)plugin->name;
      tmp.name.length= name_len;
      tmp.ref_count= 0;
      tmp.state= PLUGIN_IS_UNINITIALIZED;
      tmp.load_option= PLUGIN_ON;
      if (test_plugin_options(tmp_root, &tmp, argc, argv))
        tmp.state= PLUGIN_IS_DISABLED;

      if ((tmp_plugin_ptr= plugin_insert_or_reuse(&tmp)))
      {
        plugin_array_version++;
        if (plugin_hash[plugin->type]->emplace(
              to_string(tmp_plugin_ptr->name), tmp_plugin_ptr).second)
        {
          init_alloc_root(key_memory_plugin_int_mem_root,
                          &tmp_plugin_ptr->mem_root, 4096, 4096);
          DBUG_RETURN(false);
        }
        tmp_plugin_ptr->state= PLUGIN_IS_FREED;
      }
      mysql_del_sys_var_chain(tmp.system_vars);
      restore_pluginvar_names(tmp.system_vars);
      plugin_dl_del(dl);
      mysql_mutex_unlock(&LOCK_plugin);
      DBUG_RETURN(true);
    }
  }
  plugin_dl_del(dl);
  mysql_mutex_unlock(&LOCK_plugin);
  report_error(report, ER_CANT_FIND_DL_ENTRY, name->str);
  DBUG_RETURN(true);
}


static void plugin_deinitialize(st_plugin_int *plugin, bool ref_check)
{
  /*
    we don't want to hold the LOCK_plugin mutex as it may cause
    deinitialization to deadlock if plugins have worker threads
    with plugin locks
  */
  mysql_mutex_assert_not_owner(&LOCK_plugin);

  if (plugin->plugin->status_vars)
  {
    remove_status_vars(plugin->plugin->status_vars);
  }

  if (plugin_type_deinitialize[plugin->plugin->type])
  {
    if ((*plugin_type_deinitialize[plugin->plugin->type])(plugin))
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_FAILED_DEINITIALIZATION,
             plugin->name.str, plugin_type_names[plugin->plugin->type].str);
    }
  }
  else if (plugin->plugin->deinit)
  {
    DBUG_PRINT("info", ("Deinitializing plugin: '%s'", plugin->name.str));
    if (plugin->plugin->deinit(plugin))
    {
      DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                             plugin->name.str));
    }
  }
  plugin->state= PLUGIN_IS_UNINITIALIZED;

  Srv_session::check_for_stale_threads(plugin);
  /*
    We do the check here because NDB has a worker THD which doesn't
    exit until NDB is shut down.
  */
  if (ref_check && plugin->ref_count)
    LogErr(ERROR_LEVEL,
           ER_PLUGIN_HAS_NONZERO_REFCOUNT_AFTER_DEINITIALIZATION,
           plugin->name.str, plugin->ref_count);
}

/*
  Unload a plugin.
  Note: During valgrind testing, the plugin's shared object (.dll/.so)
        is not unloaded in order to keep the call stack
        of the leaked objects.
*/
static void plugin_del(st_plugin_int *plugin)
{
  DBUG_ENTER("plugin_del(plugin)");
  mysql_mutex_assert_owner(&LOCK_plugin);
  mysql_mutex_assert_owner(&LOCK_plugin_delete);
  /* Free allocated strings before deleting the plugin. */
  mysql_rwlock_wrlock(&LOCK_system_variables_hash);
  mysql_del_sys_var_chain(plugin->system_vars);
  mysql_rwlock_unlock(&LOCK_system_variables_hash);
  restore_pluginvar_names(plugin->system_vars);
  plugin_vars_free_values(plugin->system_vars);
  plugin_hash[plugin->plugin->type]->erase(to_string(plugin->name));

  if (plugin->plugin_dl)
    plugin_dl_del(&plugin->plugin_dl->dl);
  plugin->state= PLUGIN_IS_FREED;
  plugin_array_version++;
  free_root(&plugin->mem_root, MYF(0));
  DBUG_VOID_RETURN;
}

static void reap_plugins(void)
{
  st_plugin_int *plugin, **reap, **list;

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (!reap_needed)
    return;

  reap_needed= false;
  const size_t count= plugin_array->size();
  reap= (st_plugin_int **)my_alloca(sizeof(plugin)*(count+1));
  *(reap++)= NULL;

  for (size_t idx= 0; idx < count; idx++)
  {
    plugin= plugin_array->at(idx);
    if (plugin->state == PLUGIN_IS_DELETED && !plugin->ref_count)
    {
      /* change the status flag to prevent reaping by another thread */
      plugin->state= PLUGIN_IS_DYING;
      *(reap++)= plugin;
    }
  }

  mysql_mutex_unlock(&LOCK_plugin);

  list= reap;
  while ((plugin= *(--list)))
  {
    if (!opt_initialize)
      LogErr(INFORMATION_LEVEL, ER_PLUGIN_SHUTTING_DOWN_PLUGIN,
             plugin->name.str);
    plugin_deinitialize(plugin, true);
  }

  mysql_mutex_lock(&LOCK_plugin_delete);
  mysql_mutex_lock(&LOCK_plugin);

  while ((plugin= *(--reap)))
    plugin_del(plugin);

  mysql_mutex_unlock(&LOCK_plugin_delete);
}

static void intern_plugin_unlock(LEX *lex, plugin_ref plugin)
{
  st_plugin_int *pi;
  DBUG_ENTER("intern_plugin_unlock");

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (!plugin)
    DBUG_VOID_RETURN;

  pi= plugin_ref_to_int(plugin);

#ifdef DBUG_OFF
  if (!pi->plugin_dl)
    DBUG_VOID_RETURN;
#else
  my_free(plugin);
#endif

  DBUG_PRINT("info",("unlocking plugin, name= %s, ref_count= %d",
                     pi->name.str, pi->ref_count));
  if (lex)
  {
    /*
      Remove one instance of this plugin from the use list.
      We are searching backwards so that plugins locked last
      could be unlocked faster - optimizing for LIFO semantics.
    */
    plugin_ref *iter= lex->plugins.end() - 1;
    bool found_it MY_ATTRIBUTE((unused)) = false;
    for (; iter >= lex->plugins.begin() - 1; --iter)
    {
      if (plugin == *iter)
      {
        lex->plugins.erase(iter);
        found_it= true;
        break;
      }
    }
    DBUG_ASSERT(found_it);
  }

  DBUG_ASSERT(pi->ref_count);
  pi->ref_count--;

  if (pi->state == PLUGIN_IS_DELETED && !pi->ref_count)
    reap_needed= true;

  DBUG_VOID_RETURN;
}


void plugin_unlock(THD *thd, plugin_ref plugin)
{
  LEX *lex= thd ? thd->lex : 0;
  DBUG_ENTER("plugin_unlock");
  if (!plugin)
    DBUG_VOID_RETURN;
#ifdef DBUG_OFF
  /* built-in plugins don't need ref counting */
  if (!plugin_dlib(plugin))
    DBUG_VOID_RETURN;
#endif
  mysql_mutex_lock(&LOCK_plugin);
  intern_plugin_unlock(lex, plugin);
  reap_plugins();
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_VOID_RETURN;
}


void plugin_unlock_list(THD *thd, plugin_ref *list, size_t count)
{
  LEX *lex= thd ? thd->lex : 0;
  DBUG_ENTER("plugin_unlock_list");
  DBUG_ASSERT(list);

  /*
    In unit tests, LOCK_plugin may be uninitialized, so do not lock it.
    Besides: there's no point in locking it, if there are no plugins to unlock.
   */
  if (count == 0)
    DBUG_VOID_RETURN;

  mysql_mutex_lock(&LOCK_plugin);
  while (count--)
    intern_plugin_unlock(lex, *list++);
  reap_plugins();
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_VOID_RETURN;
}

static int plugin_initialize(st_plugin_int *plugin)
{
  int ret= 1;
  DBUG_ENTER("plugin_initialize");

  mysql_mutex_assert_owner(&LOCK_plugin);
  uint state= plugin->state;
  DBUG_ASSERT(state == PLUGIN_IS_UNINITIALIZED);

  mysql_mutex_unlock(&LOCK_plugin);

  DEBUG_SYNC(current_thd, "in_plugin_initialize");

  if (plugin_type_initialize[plugin->plugin->type])
  {
    if ((*plugin_type_initialize[plugin->plugin->type])(plugin))
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_REGISTRATION_FAILED,
             plugin->name.str, plugin_type_names[plugin->plugin->type].str);
      goto err;
    }

    /* FIXME: Need better solution to transfer the callback function
    array to memcached */
    if (strcmp(plugin->name.str, "InnoDB") == 0) {
      innodb_callback_data = ((handlerton*)plugin->data)->data;
    }
  }
  else if (plugin->plugin->init)
  {
    if (strcmp(plugin->name.str, "daemon_memcached") == 0) {
       plugin->data = innodb_callback_data;
    }

    if (plugin->plugin->init(plugin))
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_INIT_FAILED, plugin->name.str);
      goto err;
    }
  }
  state= PLUGIN_IS_READY; // plugin->init() succeeded

  if (plugin->plugin->status_vars)
  {
    if (add_status_vars(plugin->plugin->status_vars))
      goto err;
  }

  /*
    set the plugin attribute of plugin's sys vars so they are pointing
    to the active plugin
  */
  if (plugin->system_vars)
  {
    sys_var_pluginvar *var= plugin->system_vars->cast_pluginvar();
    for (;;)
    {
      var->plugin= plugin;
      if (!var->next)
        break;
      var= var->next->cast_pluginvar();
    }
  }

  ret= 0;

err:
  mysql_mutex_lock(&LOCK_plugin);
  plugin->state= state;

  DBUG_RETURN(ret);
}


static inline void convert_dash_to_underscore(char *str, size_t len)
{
  for (char *p= str; p <= str+len; p++)
    if (*p == '-')
      *p= '_';
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_plugin;
static PSI_mutex_key key_LOCK_plugin_delete;
static PSI_mutex_key key_LOCK_plugin_install;

/* clang-format off */
static PSI_mutex_info all_plugin_mutexes[]=
{
  { &key_LOCK_plugin, "LOCK_plugin", PSI_FLAG_SINGLETON, 0, PSI_DOCUMENT_ME},
  { &key_LOCK_plugin_delete, "LOCK_plugin_delete", PSI_FLAG_SINGLETON, 0, PSI_DOCUMENT_ME},
  { &key_LOCK_plugin_install, "LOCK_plugin_install", PSI_FLAG_SINGLETON, 0, PSI_DOCUMENT_ME}
};
/* clang-format on */

/* clang-format off */
static PSI_memory_info all_plugin_memory[]=
{
#ifndef DBUG_OFF
  { &key_memory_plugin_ref, "plugin_ref", PSI_FLAG_ONLY_GLOBAL_STAT, 0, PSI_DOCUMENT_ME},
#endif
  { &key_memory_plugin_mem_root, "plugin_mem_root", PSI_FLAG_ONLY_GLOBAL_STAT, 0, PSI_DOCUMENT_ME},
  { &key_memory_plugin_init_tmp, "plugin_init_tmp", 0, 0, PSI_DOCUMENT_ME},
  { &key_memory_plugin_int_mem_root, "plugin_int_mem_root", 0, 0, PSI_DOCUMENT_ME},
  { &key_memory_mysql_plugin_dl, "mysql_plugin_dl", 0, 0, PSI_DOCUMENT_ME},
  { &key_memory_mysql_plugin, "mysql_plugin", 0, 0, PSI_DOCUMENT_ME},
  { &key_memory_plugin_bookmark, "plugin_bookmark", PSI_FLAG_ONLY_GLOBAL_STAT, 0, PSI_DOCUMENT_ME}
};
/* clang-format on */

static void init_plugin_psi_keys(void)
{
  const char* category= "sql";
  int count;

  count= array_elements(all_plugin_mutexes);
  mysql_mutex_register(category, all_plugin_mutexes, count);

  count= array_elements(all_plugin_memory);
  mysql_memory_register(category, all_plugin_memory, count);
}
#endif /* HAVE_PSI_INTERFACE */

/**
  Initialize the internals of the plugin system. Allocate required
  resources, initialize mutex, etc.

  @return Operation outcome, false means no errors
 */
static bool plugin_init_internals()
{
#ifdef HAVE_PSI_INTERFACE
  init_plugin_psi_keys();
#endif

  init_alloc_root(key_memory_plugin_mem_root, &plugin_mem_root, 4096, 4096);

  bookmark_hash= new malloc_unordered_map<std::string, st_bookmark *>(
    key_memory_plugin_bookmark);

  malloced_string_type_sysvars_bookmark_hash=
    new malloc_unordered_map<std::string, st_bookmark *>(
      key_memory_plugin_bookmark);

  mysql_mutex_init(key_LOCK_plugin, &LOCK_plugin, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_plugin_delete, &LOCK_plugin_delete, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_plugin_install, &LOCK_plugin_install, MY_MUTEX_INIT_FAST);

  plugin_dl_array= new (std::nothrow)
    Prealloced_array<st_plugin_dl*, 16>(key_memory_mysql_plugin_dl);
  plugin_array= new (std::nothrow)
    Prealloced_array<st_plugin_int*, 16>(key_memory_mysql_plugin);
  if (plugin_dl_array == NULL || plugin_array == NULL)
    goto err;

  for (uint i= 0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
  {
    plugin_hash[i]= new collation_unordered_map<std::string, st_plugin_int *>(
      system_charset_info, key_memory_plugin_mem_root);
  }
  return false;

err:
  return true;
}

/**
  Initialize the plugins. Reap those that fail to initialize.

  @return Operation outcome, false means no errors
 */
static bool plugin_init_initialize_and_reap()
{
  struct st_plugin_int *plugin_ptr;
  struct st_plugin_int **reap;

  /* Now we initialize all plugins that are not already initialized */
  mysql_mutex_lock(&LOCK_plugin);
  reap= (st_plugin_int **) my_alloca((plugin_array->size()+1) * sizeof(void*));
  *(reap++)= NULL;

  for (st_plugin_int **it= plugin_array->begin();
       it != plugin_array->end(); ++it)
  {
    plugin_ptr= *it;
    if (plugin_ptr->state == PLUGIN_IS_UNINITIALIZED)
    {
      if (plugin_initialize(plugin_ptr))
      {
        plugin_ptr->state= PLUGIN_IS_DYING;
        *(reap++)= plugin_ptr;
      }
    }
  }

  /* Check if any plugins have to be reaped */
  bool reaped_mandatory_plugin= false;
  while ((plugin_ptr= *(--reap)))
  {
    mysql_mutex_unlock(&LOCK_plugin);
    if (plugin_ptr->load_option == PLUGIN_FORCE ||
        plugin_ptr->load_option == PLUGIN_FORCE_PLUS_PERMANENT)
      reaped_mandatory_plugin= true;
    plugin_deinitialize(plugin_ptr, true);
    mysql_mutex_lock(&LOCK_plugin_delete);
    mysql_mutex_lock(&LOCK_plugin);
    plugin_del(plugin_ptr);
    mysql_mutex_unlock(&LOCK_plugin_delete);
  }

  mysql_mutex_unlock(&LOCK_plugin);
  if (reaped_mandatory_plugin)
    return true;

  return false;
}

/**
   Register and initialize early plugins.

   @param argc  Command line argument counter
   @param argv  Command line arguments
   @param flags Flags to control whether dynamic loading
                and plugin initialization should be skipped

   @return Operation outcome, false if no errors
*/
bool plugin_register_early_plugins(int *argc, char **argv, int flags)
{
  bool retval= false;
  DBUG_ENTER("plugin_register_dynamic_and_init_all");

  /* Don't allow initializing twice */
  DBUG_ASSERT(!initialized);

  /* Make sure the internals are initialized */
  if ((retval= plugin_init_internals()))
    DBUG_RETURN(retval);

  /* Allocate the temporary mem root, will be freed before returning */
  MEM_ROOT tmp_root;
  init_alloc_root(key_memory_plugin_init_tmp, &tmp_root, 4096, 4096);

  I_List_iterator<i_string> iter(opt_early_plugin_load_list);
  i_string *item;
  while (NULL != (item= iter++))
    plugin_load_list(&tmp_root, argc, argv, item->ptr);

  /* Temporary mem root not needed anymore, can free it here */
  free_root(&tmp_root, MYF(0));

  if (!(flags & PLUGIN_INIT_SKIP_INITIALIZATION))
    retval= plugin_init_initialize_and_reap();

  DBUG_RETURN(retval);
}


/**
  Register the builtin plugins. Some of the plugins (MyISAM, CSV and InnoDB)
  are also initialized.

  @param argc number of arguments, propagated to the plugin
  @param argv actual arguments, propagated to the plugin
  @return Operation outcome, false means no errors
 */
bool plugin_register_builtin_and_init_core_se(int *argc, char **argv)
{
  bool mandatory= true;
  DBUG_ENTER("plugin_register_builtin_and_init_core_se");

  /* Don't allow initializing twice */
  DBUG_ASSERT(!initialized);

  /* Allocate the temporary mem root, will be freed before returning */
  MEM_ROOT tmp_root;
  init_alloc_root(key_memory_plugin_init_tmp, &tmp_root, 4096, 4096);

  mysql_mutex_lock(&LOCK_plugin);
  initialized= true;

  /* First we register the builtin mandatory and optional plugins */
  for (struct st_mysql_plugin **builtins= mysql_mandatory_plugins;
       *builtins || mandatory; builtins++)
  {
    /* Switch to optional plugins when done with the mandatory ones */
    if (!*builtins)
    {
      builtins= mysql_optional_plugins;
      mandatory= false;
      if (!*builtins)
        break;
    }
    for (struct st_mysql_plugin *plugin= *builtins; plugin->info; plugin++)
    {
      struct st_plugin_int tmp;
      tmp.plugin= plugin;
      tmp.name.str= (char *)plugin->name;
      tmp.name.length= strlen(plugin->name);
      tmp.state= 0;
      tmp.load_option= mandatory ? PLUGIN_FORCE : PLUGIN_ON;

      /*
        If the performance schema is compiled in,
        treat the storage engine plugin as 'mandatory',
        to suppress any plugin-level options such as '--performance-schema'.
        This is specific to the performance schema, and is done on purpose:
        the server-level option '--performance-schema' controls the overall
        performance schema initialization, which consists of much more that
        the underlying storage engine initialization.
        See mysqld.cc, set_vars.cc.
        Suppressing ways to interfere directly with the storage engine alone
        prevents awkward situations where:
        - the user wants the performance schema functionality, by using
          '--enable-performance-schema' (the server option),
        - yet disable explicitly a component needed for the functionality
          to work, by using '--skip-performance-schema' (the plugin)
      */
      if (!my_strcasecmp(&my_charset_latin1, plugin->name, "PERFORMANCE_SCHEMA"))
      {
        tmp.load_option= PLUGIN_FORCE;
      }

      free_root(&tmp_root, MYF(MY_MARK_BLOCKS_FREE));
      if (test_plugin_options(&tmp_root, &tmp, argc, argv))
        tmp.state= PLUGIN_IS_DISABLED;
      else
        tmp.state= PLUGIN_IS_UNINITIALIZED;

      struct st_plugin_int *plugin_ptr;        // Pointer to registered plugin
      if (register_builtin(plugin, &tmp, &plugin_ptr))
        goto err_unlock;

      /*
        Only initialize MyISAM, InnoDB and CSV at this stage.
        Note that when the --help option is supplied, InnoDB is not
        initialized because the plugin table will not be read anyway,
        as indicated by the flag set when the plugin_init() function
        is called.
      */
      bool is_myisam= !my_strcasecmp(&my_charset_latin1, plugin->name, "MyISAM");
      bool is_innodb= !my_strcasecmp(&my_charset_latin1, plugin->name, "InnoDB");
      if (!is_myisam &&
          (!is_innodb || opt_help) &&
          my_strcasecmp(&my_charset_latin1, plugin->name, "CSV"))
        continue;

      if (plugin_ptr->state != PLUGIN_IS_UNINITIALIZED ||
          plugin_initialize(plugin_ptr))
        goto err_unlock;

      /*
        Initialize the global default storage engine so that it may
        not be null in any child thread.
      */
      if (is_myisam)
      {
        DBUG_ASSERT(!global_system_variables.table_plugin);
        DBUG_ASSERT(!global_system_variables.temp_table_plugin);
        global_system_variables.table_plugin=
          my_intern_plugin_lock(NULL, plugin_int_to_ref(plugin_ptr));
        global_system_variables.temp_table_plugin=
          my_intern_plugin_lock(NULL, plugin_int_to_ref(plugin_ptr));
        DBUG_ASSERT(plugin_ptr->ref_count == 2);
      }
    }
  }

  /* Should now be set to MyISAM storage engine */
  DBUG_ASSERT(global_system_variables.table_plugin);
  DBUG_ASSERT(global_system_variables.temp_table_plugin);

  mysql_mutex_unlock(&LOCK_plugin);

  free_root(&tmp_root, MYF(0));
  DBUG_RETURN(false);

err_unlock:
  mysql_mutex_unlock(&LOCK_plugin);
  free_root(&tmp_root, MYF(0));
  DBUG_RETURN(true);
}

bool is_builtin_and_core_se_initialized()
{
  return initialized;
}

/**
  Register and initialize the dynamic plugins. Also initialize
  the remaining builtin plugins that are not initialized
  already.

  @param argc  Command line argument counter
  @param argv  Command line arguments
  @param flags Flags to control whether dynamic loading
               and plugin initialization should be skipped

  @return Operation outcome, false if no errors
*/
bool plugin_register_dynamic_and_init_all(int *argc,
                                          char **argv, int flags)
{
  DBUG_ENTER("plugin_register_dynamic_and_init_all");

  /* Make sure the internals are initialized and builtins registered */
  if (!initialized)
    DBUG_RETURN(true);

  /* Allocate the temporary mem root, will be freed before returning */
  MEM_ROOT tmp_root;
  init_alloc_root(key_memory_plugin_init_tmp, &tmp_root, 4096, 4096);

  /* Register all dynamic plugins */
  if (!(flags & PLUGIN_INIT_SKIP_DYNAMIC_LOADING))
  {
    I_List_iterator<i_string> iter(opt_plugin_load_list);
    i_string *item;
    while (NULL != (item= iter++))
      plugin_load_list(&tmp_root, argc, argv, item->ptr);

    if (!(flags & PLUGIN_INIT_SKIP_PLUGIN_TABLE))
      plugin_load(&tmp_root, argc, argv);
  }

  /* Temporary mem root not needed anymore, can free it here */
  free_root(&tmp_root, MYF(0));

  Auto_THD fake_session;
  Disable_autocommit_guard autocommit_guard(fake_session.thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(fake_session.thd->dd_client());
  if (!(flags & PLUGIN_INIT_SKIP_INITIALIZATION))
    if (plugin_init_initialize_and_reap())
    {
      DBUG_RETURN(::end_transaction(fake_session.thd, true));
    }

  DBUG_RETURN(::end_transaction(fake_session.thd, false));
}

static bool register_builtin(st_mysql_plugin *plugin,
                             st_plugin_int *tmp,
                             st_plugin_int **ptr)
{
  DBUG_ENTER("register_builtin");
  tmp->ref_count= 0;
  tmp->plugin_dl= 0;

  if (plugin_array->push_back(tmp))
    DBUG_RETURN(true);

  *ptr= plugin_array->back()=
    new (&plugin_mem_root) st_plugin_int(std::move(*tmp));

  plugin_hash[plugin->type]->emplace(to_string((*ptr)->name), *ptr);

  DBUG_RETURN(0);
}


/**
  Reads the plugins from mysql.plugin and loads them

  Called only by plugin_register_dynamic_and_init_all()
  a.k.a. the bootstrap sequence.

  @arg tmp_root  memory root to use for plugin_add()
  @arg argc      number of command line arguments to process
  @arg argv      array of command line argument to read values from
  @retval true   failure
  @retval false  success
*/
static void plugin_load(MEM_ROOT *tmp_root, int *argc, char **argv)
{
  THD thd;
  TABLE_LIST tables;
  TABLE *table;
  READ_RECORD read_record_info;
  int error;
  THD *new_thd= &thd;
  bool result;
  DBUG_ENTER("plugin_load");

  new_thd->thread_stack= (char*) &tables;
  new_thd->store_globals();
  LEX_CSTRING db_lex_cstr= { STRING_WITH_LEN("mysql") };
  new_thd->set_db(db_lex_cstr);
  thd.get_protocol_classic()->wipe_net();
  tables.init_one_table("mysql", 5, "plugin", 6, "plugin", TL_READ);

  result= open_trans_system_tables_for_read(new_thd, &tables);

  if (result)
  {
    DBUG_PRINT("error",("Can't open plugin table"));
    LogErr(ERROR_LEVEL, ER_PLUGIN_CANT_OPEN_PLUGIN_TABLE);
    DBUG_VOID_RETURN;
  }
  table= tables.table;
  if (init_read_record(&read_record_info, new_thd, table, NULL, 1, 1, false))
  {
    close_trans_system_tables(new_thd);
    DBUG_VOID_RETURN;
  }
  table->use_all_columns();
  /*
    there're no other threads running yet, so we don't need a mutex.
    but plugin_add() before is designed to work in multi-threaded
    environment, and it uses mysql_mutex_assert_owner(), so we lock
    the mutex here to satisfy the assert
  */
  while (!(error= read_record_info.read_record(&read_record_info)))
  {
    DBUG_PRINT("info", ("init plugin record"));
    String str_name, str_dl;
    get_field(tmp_root, table->field[0], &str_name);
    get_field(tmp_root, table->field[1], &str_dl);

    LEX_STRING name= {(char *)str_name.ptr(), str_name.length()};
    LEX_STRING dl= {(char *)str_dl.ptr(), str_dl.length()};

    /*
      The whole locking sequence is not strictly speaking needed since this
      is a function that's executed only during server bootstrap, but we do
      it properly for uniformity of the environment for plugin_add.
      Note that it must be done for each iteration since, unlike INSTALL PLUGIN
      the bootstrap process just reports the error and goes on.
      So to ensure the right sequence of lock and unlock we need to take and
      release both the wlock and the mutex.
    */
    mysql_mutex_lock(&LOCK_plugin);
    mysql_rwlock_wrlock(&LOCK_system_variables_hash);
    if (plugin_add(tmp_root, &name, &dl, argc, argv, REPORT_TO_LOG))
    {
      LogErr(WARNING_LEVEL, ER_PLUGIN_CANT_LOAD,
        str_name.c_ptr(), str_dl.c_ptr());
    }
    else
      mysql_mutex_unlock(&LOCK_plugin);
    mysql_rwlock_unlock(&LOCK_system_variables_hash);
    free_root(tmp_root, MYF(MY_MARK_BLOCKS_FREE));
  }
  if (error > 0)
  {
    char errbuf[MYSQL_ERRMSG_SIZE];
    LogErr(ERROR_LEVEL, ER_GET_ERRNO, my_errno(),
           my_strerror(errbuf, MYSQL_ERRMSG_SIZE, my_errno()));
  }
  end_read_record(&read_record_info);
  table->m_needs_reopen= true;                  // Force close to free memory

  close_trans_system_tables(new_thd);

  DBUG_VOID_RETURN;
}


/**
  Load a list of plugins

  Called by plugin_register_early_plugins() and
  plugin_register_dynamic_and_init_all(), a.k.a. the bootstrap sequence.

  @arg tmp_root  memory root to use for plugin_add()
  @arg argc      number of command line arguments to process
  @arg argv      array of command line argument to read values from
  @arg list      list of plugins to load. Ends with a NULL pointer
  @retval true   failure
  @retval false  success
*/
static bool plugin_load_list(MEM_ROOT *tmp_root, int *argc, char **argv,
                             const char *list)
{
  char buffer[FN_REFLEN];
  LEX_STRING name= {buffer, 0}, dl= {NULL, 0}, *str= &name;
  st_plugin_dl *plugin_dl;
  st_mysql_plugin *plugin;
  char *p= buffer;
  DBUG_ENTER("plugin_load_list");
  while (list)
  {
    if (p == buffer + sizeof(buffer) - 1)
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_LOAD_PARAMETER_TOO_LONG);
      DBUG_RETURN(true);
    }

    switch ((*(p++)= *(list++))) {
    case '\0':
      list= NULL; /* terminate the loop */
      /* fall through */
    case ';':
#ifndef _WIN32
    case ':':     /* can't use this as delimiter as it may be drive letter */
#endif
      str->str[str->length]= '\0';
      if (str == &name)  // load all plugins in named module
      {
        if (!name.length)
        {
          p--;    /* reset pointer */
          continue;
        }

        dl= name;
        /*
          The whole locking sequence is not strictly speaking needed since this
          is a function that's executed only during server bootstrap, but we do
          it properly for uniformity of the environment for plugin_add.
        */
        mysql_mutex_lock(&LOCK_plugin);
        mysql_rwlock_wrlock(&LOCK_system_variables_hash);
        if ((plugin_dl= plugin_dl_add(&dl, REPORT_TO_LOG)))
        {
          for (plugin= plugin_dl->plugins; plugin->info; plugin++)
          {
            name.str= (char *) plugin->name;
            name.length= strlen(name.str);

            free_root(tmp_root, MYF(MY_MARK_BLOCKS_FREE));
            if (plugin_add(tmp_root, &name, &dl, argc, argv, REPORT_TO_LOG))
            {
              mysql_rwlock_unlock(&LOCK_system_variables_hash);
              goto error;
            }
          }
          plugin_dl_del(&dl); // reduce ref count
        }
        else
        {
          mysql_rwlock_unlock(&LOCK_system_variables_hash);
          goto error;
        }
      }
      else
      {
        free_root(tmp_root, MYF(MY_MARK_BLOCKS_FREE));
        /*
          The whole locking sequence is not strictly speaking needed since this
          is a function that's executed only during server bootstrap, but we do
          it properly for uniformity of the environment for plugin_add.
        */
        mysql_mutex_lock(&LOCK_plugin);
        mysql_rwlock_wrlock(&LOCK_system_variables_hash);
        if (plugin_add(tmp_root, &name, &dl, argc, argv, REPORT_TO_LOG))
        {
          mysql_rwlock_unlock(&LOCK_system_variables_hash);
          goto error;
        }
      }
      mysql_mutex_unlock(&LOCK_plugin);
      mysql_rwlock_unlock(&LOCK_system_variables_hash);
      name.length= dl.length= 0;
      dl.str= NULL; name.str= p= buffer;
      str= &name;
      continue;
    case '=':
    case '#':
      if (str == &name)
      {
        name.str[name.length]= '\0';
        str= &dl;
        str->str= p;
        continue;
      }
      // Fall through.
    default:
      str->length++;
      continue;
    }
  }
  DBUG_RETURN(false);
error:
  LogErr(ERROR_LEVEL, ER_PLUGIN_CANT_LOAD, name.str, dl.str);
  DBUG_RETURN(true);
}

/*
  Shutdown memcached plugin before binlog shuts down
*/
void memcached_shutdown(void)
{
  if (initialized)
  {

    for (st_plugin_int **it= plugin_array->begin();
         it != plugin_array->end(); ++it)
    {
      st_plugin_int *plugin= *it;

      if (plugin->state == PLUGIN_IS_READY
	  && strcmp(plugin->name.str, "daemon_memcached") == 0)
      {
	plugin_deinitialize(plugin, true);

        mysql_mutex_lock(&LOCK_plugin_delete);
        mysql_mutex_lock(&LOCK_plugin);
	plugin->state= PLUGIN_IS_DYING;
	plugin_del(plugin);
        mysql_mutex_unlock(&LOCK_plugin);
        mysql_mutex_unlock(&LOCK_plugin_delete);
      }
    }

  }
}

/*
  Deinitialize and unload all the loaded plugins.
  Note: During valgrind testing, the shared objects (.dll/.so)
        are not unloaded in order to keep the call stack
        of the leaked objects.
*/
void plugin_shutdown(void)
{
  size_t i;
  st_plugin_int **plugins, *plugin;
  st_plugin_dl **dl;
  bool skip_binlog = true;

  DBUG_ENTER("plugin_shutdown");

  if (initialized)
  {
    size_t count= plugin_array->size();
    mysql_mutex_lock(&LOCK_plugin);

    reap_needed= true;

    /*
      We want to shut down plugins in a reasonable order, this will
      become important when we have plugins which depend upon each other.
      Circular references cannot be reaped so they are forced afterwards.
      TODO: Have an additional step here to notify all active plugins that
      shutdown is requested to allow plugins to deinitialize in parallel.
    */
    while (reap_needed && (count= plugin_array->size()))
    {
      reap_plugins();
      for (i= 0; i < count; i++)
      {
        plugin= plugin_array->at(i);

	if (plugin->state == PLUGIN_IS_READY
	    && strcmp(plugin->name.str, "binlog") == 0 && skip_binlog)
	{
		skip_binlog = false;

	} else if (plugin->state == PLUGIN_IS_READY)
        {
          plugin->state= PLUGIN_IS_DELETED;
          reap_needed= true;
        }
      }
      if (!reap_needed)
      {
        /*
          release any plugin references held.
        */
        unlock_variables(&global_system_variables);
        unlock_variables(&max_system_variables);
      }
    }

    plugins= (st_plugin_int **) my_alloca(sizeof(void*) * (count+1));

    /*
      If we have any plugins which did not die cleanly, we force shutdown
    */
    for (i= 0; i < count; i++)
    {
      plugins[i]= plugin_array->at(i);
      /* change the state to ensure no reaping races */
      if (plugins[i]->state == PLUGIN_IS_DELETED)
        plugins[i]->state= PLUGIN_IS_DYING;
    }
    mysql_mutex_unlock(&LOCK_plugin);

    /*
      We loop through all plugins and call deinit() if they have one.
    */
    for (i= 0; i < count; i++)
      if (!(plugins[i]->state & (PLUGIN_IS_UNINITIALIZED | PLUGIN_IS_FREED |
                                 PLUGIN_IS_DISABLED)))
      {
        LogErr(WARNING_LEVEL, ER_PLUGIN_FORCING_SHUTDOWN,
               plugins[i]->name.str);
        /*
          We are forcing deinit on plugins so we don't want to do a ref_count
          check until we have processed all the plugins.
        */
        plugin_deinitialize(plugins[i], false);
      }

    /*
      It's perfectly safe not to lock LOCK_plugin, LOCK_plugin_delete, as
      there're no concurrent threads anymore. But some functions called from
      here use mysql_mutex_assert_owner(), so we lock the mutex to satisfy it
    */
    mysql_mutex_lock(&LOCK_plugin_delete);
    mysql_mutex_lock(&LOCK_plugin);

    /*
      We defer checking ref_counts until after all plugins are deinitialized
      as some may have worker threads holding on to plugin references.
    */
    for (i= 0; i < count; i++)
    {
      if (plugins[i]->ref_count)
        LogErr(ERROR_LEVEL, ER_PLUGIN_HAS_NONZERO_REFCOUNT_AFTER_SHUTDOWN,
               plugins[i]->name.str, plugins[i]->ref_count);
      if (plugins[i]->state & PLUGIN_IS_UNINITIALIZED)
        plugin_del(plugins[i]);
    }

    /*
      Now we can deallocate all memory.
    */

    cleanup_variables(NULL, &global_system_variables);
    cleanup_variables(NULL, &max_system_variables);
    mysql_mutex_unlock(&LOCK_plugin);
    mysql_mutex_unlock(&LOCK_plugin_delete);

    initialized= false;
    mysql_mutex_destroy(&LOCK_plugin);
    mysql_mutex_destroy(&LOCK_plugin_delete);
    mysql_mutex_destroy(&LOCK_plugin_install);
  }

  /* Dispose of the memory */

  for (i= 0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
  {
    delete plugin_hash[i];
    plugin_hash[i]= nullptr;
  }
  delete plugin_array;
  plugin_array= NULL;

  if (plugin_dl_array != NULL)
  {
    size_t count= plugin_dl_array->size();
    dl= (st_plugin_dl **)my_alloca(sizeof(void*) * count);
    for (i= 0; i < count; i++)
      dl[i]= plugin_dl_array->at(i);
    for (i= 0; i < plugin_dl_array->size(); i++)
      free_plugin_mem(dl[i]);
    delete plugin_dl_array;
    plugin_dl_array= NULL;
  }

  delete bookmark_hash;
  bookmark_hash= nullptr;
  delete malloced_string_type_sysvars_bookmark_hash;
  malloced_string_type_sysvars_bookmark_hash= nullptr;
  free_root(&plugin_mem_root, MYF(0));

  global_variables_dynamic_size= 0;

  DBUG_VOID_RETURN;
}


// Helper function to do rollback or commit, depending on error.
bool end_transaction(THD *thd, bool error)
{
  if (error)
  {
    // Rollback the statement before we can rollback the real transaction.
    trans_rollback_stmt(thd);
    trans_rollback(thd);
  }
  else if (trans_commit_stmt(thd) || trans_commit(thd))
  {
    error= true;
    trans_rollback(thd);
  }

  // Close tables regardless of error.
  close_thread_tables(thd);
  return error;
}

/**
  Initialize one plugin. This function is used to early load one single
  plugin. This function is used by key migration tool.

   @param[in]   argc  Command line argument counter
   @param[in]   argv  Command line arguments
   @param[in]   plugin library file name

   @return Operation status
     @retval 0 OK
     @retval 1 ERROR
*/
bool plugin_early_load_one(int *argc, char **argv, const char* plugin)
{
  bool retval= false;
  DBUG_ENTER("plugin_early_load_one");

  /* Make sure the internals are initialized */
  if (!initialized)
  {
    if ((retval= plugin_init_internals()))
      DBUG_RETURN(retval);
    else
      initialized= true;
  }
  /* Allocate the temporary mem root, will be freed before returning */
  MEM_ROOT tmp_root;
  init_alloc_root(PSI_NOT_INSTRUMENTED, &tmp_root, 4096, 4096);

  plugin_load_list(&tmp_root, argc, argv, plugin);

  /* Temporary mem root not needed anymore, can free it here */
  free_root(&tmp_root, MYF(0));

  retval= plugin_init_initialize_and_reap();

  DBUG_RETURN(retval);
}

static bool mysql_install_plugin(THD *thd, const LEX_STRING *name,
                                 const LEX_STRING *dl)
{
  TABLE_LIST tables;
  TABLE *table;
  bool error= true;
  int argc= orig_argc;
  char **argv= orig_argv;
  st_plugin_int *tmp= nullptr;
  LEX_CSTRING name_cstr= {name->str, name->length};
  bool store_infoschema_metadata= false;
  dd::Schema_MDL_locker mdl_handler(thd);
  Persisted_variables_cache *pv= Persisted_variables_cache::get_instance();

  DBUG_ENTER("mysql_install_plugin");

  Disable_autocommit_guard autocommit_guard(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  tables.init_one_table("mysql", 5, "plugin", 6, "plugin", TL_WRITE);

  if (!opt_noacl &&
      check_table_access(thd, INSERT_ACL, &tables, false, 1, false))
    DBUG_RETURN(true);

  /* need to open before acquiring LOCK_plugin or it will deadlock */
  if (! (table = open_ltable(thd, &tables, TL_WRITE,
                             MYSQL_LOCK_IGNORE_TIMEOUT)))
    DBUG_RETURN(true);

  /*
    Pre-acquire audit plugins for events that may potentially occur
    during [UN]INSTALL PLUGIN.

    When audit event is triggered, audit subsystem acquires interested
    plugins by walking through plugin list. Evidently plugin list
    iterator protects plugin list by acquiring LOCK_plugin, see
    plugin_foreach_with_mask().

    On the other hand [UN]INSTALL PLUGIN is acquiring LOCK_plugin
    rather for a long time.

    When audit event is triggered during [UN]INSTALL PLUGIN, plugin
    list iterator acquires the same lock (within the same thread)
    second time.

    This hack should be removed when LOCK_plugin is fixed so it
    protects only what it supposed to protect.
    */
  mysql_audit_acquire_plugins(thd, MYSQL_AUDIT_GENERAL_CLASS,
                              MYSQL_AUDIT_GENERAL_ALL);

  mysql_mutex_lock(&LOCK_plugin_install);
  mysql_mutex_lock(&LOCK_plugin);
  DEBUG_SYNC(thd, "acquired_LOCK_plugin");
  mysql_rwlock_wrlock(&LOCK_system_variables_hash);

  {
    MEM_ROOT alloc{PSI_NOT_INSTRUMENTED, 512};
    if (my_load_defaults(MYSQL_CONFIG_NAME, load_default_groups,
                         &argc, &argv, &alloc, NULL))
    {
      mysql_rwlock_unlock(&LOCK_system_variables_hash);
      mysql_mutex_unlock(&LOCK_plugin);
      report_error(REPORT_TO_USER, ER_PLUGIN_IS_NOT_LOADED, name->str);
      goto err;
    }
    /*
     Append static variables present in mysqld-auto.cnf file for the
     newly installed plugin to process those options which are specific
     to this plugin.
    */
    if (pv && pv->append_read_only_variables(&argc, &argv, true))
    {
      mysql_rwlock_unlock(&LOCK_system_variables_hash);
      mysql_mutex_unlock(&LOCK_plugin);
      report_error(REPORT_TO_USER, ER_PLUGIN_IS_NOT_LOADED, name->str);
      goto err;
    }
    error= plugin_add(thd->mem_root, name, dl, &argc, argv, REPORT_TO_USER);
  }
  mysql_rwlock_unlock(&LOCK_system_variables_hash);

  /* LOCK_plugin already unlocked by plugin_add() if error */
  if (error)
    goto err;

  if (!(tmp= plugin_find_internal(name_cstr, MYSQL_ANY_PLUGIN)))
  {
    mysql_mutex_unlock(&LOCK_plugin);
    goto err;
  }

  error= false;
  if (tmp->state == PLUGIN_IS_DISABLED)
  {
    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_CANT_INITIALIZE_UDF,
                        ER_THD(thd, ER_CANT_INITIALIZE_UDF),
                        name->str, "Plugin is disabled");
  }

  // Check if we need to store I_S plugin metadata in DD.
  store_infoschema_metadata=
    (tmp->plugin->type == MYSQL_INFORMATION_SCHEMA_PLUGIN &&
     tmp->state != PLUGIN_IS_DISABLED);
  mysql_mutex_unlock(&LOCK_plugin);

  // Acquire MDL lock if we are storing metadata in DD.
  if (store_infoschema_metadata)
  {
    if (!mdl_handler.ensure_locked(INFORMATION_SCHEMA_NAME.str))
    {
      MDL_request mdl_request;
      MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE,
                       INFORMATION_SCHEMA_NAME.str, tmp->name.str,
                       MDL_EXCLUSIVE,
                       MDL_TRANSACTION);
      if (thd->mdl_context.acquire_lock(&mdl_request,
                                        thd->variables.lock_wait_timeout))
        error= true;
    }
    else
      error= true;
  }

  /*
    We do not replicate the INSTALL PLUGIN statement. Disable binlogging
    of the insert into the plugin table, so that it is not replicated in
    row based mode.
  */
  if (!error)
  {
    Disable_binlog_guard binlog_guard(thd);
    table->use_all_columns();
    restore_record(table, s->default_values);
    table->field[0]->store(name->str, name->length, system_charset_info);
    table->field[1]->store(dl->str, dl->length, files_charset_info);
    error= table->file->ha_write_row(table->record[0]);
    if (error)
    {
      table->file->print_error(error, MYF(0));
    }
    else
    {
      mysql_mutex_lock(&LOCK_plugin);

      if (tmp->state != PLUGIN_IS_DISABLED &&
          plugin_initialize(tmp))
      {
        my_error(ER_CANT_INITIALIZE_UDF, MYF(0), name->str,
                 "Plugin initialization function failed.");
        error= true;
      }

      /*
        Store plugin I_S table metadata into DD tables. The
        tables are closed before the function returns.
       */
      error= error || thd->transaction_rollback_request;
      if (!error && store_infoschema_metadata)
        error= dd::info_schema::store_dynamic_plugin_I_S_metadata(thd, tmp);
      mysql_mutex_unlock(&LOCK_plugin);

      if (!error && store_infoschema_metadata)
      {
        Uncommitted_tables_guard uncommitted_tables(thd);
        error= update_referencing_views_metadata(thd,
                                                 INFORMATION_SCHEMA_NAME.str,
                                                 tmp->name.str, false,
                                                 &uncommitted_tables);
      }
    }
  }

  if (error)
  {
    mysql_mutex_lock(&LOCK_plugin);
    tmp->state= PLUGIN_IS_DELETED;
    reap_needed= true;
    reap_plugins();
    mysql_mutex_unlock(&LOCK_plugin);
  }

err:
  mysql_mutex_unlock(&LOCK_plugin_install);
  DBUG_RETURN(end_transaction(thd, error));
}


static bool mysql_uninstall_plugin(THD *thd, const LEX_STRING *name)
{
  TABLE *table;
  TABLE_LIST tables;
  st_plugin_int *plugin;
  LEX_CSTRING name_cstr={name->str, name->length};
  bool error= true;
  int rc= 0;
  bool remove_IS_metadata_from_dd= false;
  dd::Schema_MDL_locker mdl_handler(thd);
  dd::String_type orig_plugin_name;

  DBUG_ENTER("mysql_uninstall_plugin");

  tables.init_one_table("mysql", 5, "plugin", 6, "plugin", TL_WRITE);

  if (!opt_noacl &&
      check_table_access(thd, DELETE_ACL, &tables, false, 1, false))
  {
    DBUG_ASSERT(thd->is_error());
    DBUG_RETURN(true);
  }

  Disable_autocommit_guard autocommit_guard(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  /* need to open before acquiring LOCK_plugin or it will deadlock */
  if (! (table= open_ltable(thd, &tables, TL_WRITE, MYSQL_LOCK_IGNORE_TIMEOUT)))
  {
    DBUG_ASSERT(thd->is_error());
    DBUG_RETURN(true);
  }

  mysql_mutex_lock(&LOCK_plugin_install);
  if (!table->key_info)
  {
    my_error(ER_TABLE_CORRUPT, MYF(0), table->s->db.str,
             table->s->table_name.str);
    goto err;
  }

  /*
    Pre-acquire audit plugins for events that may potentially occur
    during [UN]INSTALL PLUGIN.

    When audit event is triggered, audit subsystem acquires interested
    plugins by walking through plugin list. Evidently plugin list
    iterator protects plugin list by acquiring LOCK_plugin, see
    plugin_foreach_with_mask().

    On the other hand [UN]INSTALL PLUGIN is acquiring LOCK_plugin
    rather for a long time.

    When audit event is triggered during [UN]INSTALL PLUGIN, plugin
    list iterator acquires the same lock (within the same thread)
    second time.

    This hack should be removed when LOCK_plugin is fixed so it
    protects only what it supposed to protect.
  */
  mysql_audit_acquire_plugins(thd, MYSQL_AUDIT_GENERAL_CLASS,
                                   MYSQL_AUDIT_GENERAL_ALL);

  mysql_mutex_lock(&LOCK_plugin);
  if (!(plugin= plugin_find_internal(name_cstr, MYSQL_ANY_PLUGIN)) ||
      plugin->state & (PLUGIN_IS_UNINITIALIZED | PLUGIN_IS_DYING))
  {
    mysql_mutex_unlock(&LOCK_plugin);
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "PLUGIN", name->str);
    goto err;
  }
  if (!plugin->plugin_dl)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    my_error(ER_PLUGIN_DELETE_BUILTIN, MYF(0));
    goto err;
  }
  if (plugin->load_option == PLUGIN_FORCE_PLUS_PERMANENT)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    my_error(ER_PLUGIN_IS_PERMANENT, MYF(0), name->str);
    goto err;
  }
  /*
    Error message for ER_PLUGIN_IS_PERMANENT is not suitable for
    plugins marked as not dynamically uninstallable, so we have a
    separate one instead of changing the old one.
   */
  if (plugin->plugin->flags & PLUGIN_OPT_NO_UNINSTALL)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    my_error(ER_PLUGIN_NO_UNINSTALL, MYF(0), plugin->plugin->name);
    goto err;
  }

  /*
    FIXME: plugin rpl_semi_sync_master, check_uninstall() function.
  */

  /* Block Uninstallation of semi_sync plugins (Master/Slave)
     when they are busy
   */
  char buff[20];
  size_t buff_length;
  /*
    Master: If there are active semi sync slaves for this Master,
    then that means it is busy and rpl_semi_sync_master plugin
    cannot be uninstalled. To check whether the master
    has any semi sync slaves or not, check Rpl_semi_sync_master_cliens
    status variable value, if it is not 0, that means it is busy.
  */
  if (!strcmp(name->str, "rpl_semi_sync_master") &&
      get_status_var(thd,
                     plugin->plugin->status_vars,
                     "Rpl_semi_sync_master_clients",
                     buff, OPT_DEFAULT, &buff_length) &&
      strcmp(buff,"0") )
  {
    mysql_mutex_unlock(&LOCK_plugin);
    my_error(ER_PLUGIN_CANNOT_BE_UNINSTALLED, MYF(0), name->str,
             "Stop any active semisynchronous slaves of this master first.");
    goto err;
  }

  /*
    FIXME: plugin rpl_semi_sync_slave, check_uninstall() function.
  */

  /* Slave: If there is semi sync enabled IO thread active on this Slave,
    then that means plugin is busy and rpl_semi_sync_slave plugin
    cannot be uninstalled. To check whether semi sync
    IO thread is active or not, check Rpl_semi_sync_slave_status status
    variable value, if it is ON, that means it is busy.
  */
  if (!strcmp(name->str, "rpl_semi_sync_slave") &&
      get_status_var(thd, plugin->plugin->status_vars,
                     "Rpl_semi_sync_slave_status",
                     buff, OPT_DEFAULT, &buff_length) &&
      !strcmp(buff,"ON") )
  {
    mysql_mutex_unlock(&LOCK_plugin);
    my_error(ER_PLUGIN_CANNOT_BE_UNINSTALLED, MYF(0), name->str,
             "Stop any active semisynchronous I/O threads on this slave first.");
    goto err;
  }

  if ((plugin->plugin->check_uninstall) && (plugin->state == PLUGIN_IS_READY))
  {
    int check;
    /*
      Prevent other threads to uninstall concurrently this plugin.
    */
    plugin->state= PLUGIN_IS_DYING;
    mysql_mutex_unlock(&LOCK_plugin);

    DEBUG_SYNC(current_thd, "in_plugin_check_uninstall");

    /*
      Check uninstall may perform complex operations,
      including acquiring MDL locks, which in turn may need LOCK_plugin.
    */
    DBUG_PRINT("info", ("check uninstall plugin: '%s'", plugin->name.str));
    check= plugin->plugin->check_uninstall(plugin);

    mysql_mutex_lock(&LOCK_plugin);
    DBUG_ASSERT(plugin->state == PLUGIN_IS_DYING);

    if (check)
    {
      DBUG_PRINT("warning", ("Plugin '%s' blocked uninstall.",
                             plugin->name.str));
      plugin->state= PLUGIN_IS_READY;
      mysql_mutex_unlock(&LOCK_plugin);
      my_error(ER_PLUGIN_CANNOT_BE_UNINSTALLED, MYF(0), name->str,
               "Plugin is still in use.");
      goto err;
    }
  }

  plugin->state= PLUGIN_IS_DELETED;
  if (plugin->ref_count)
    push_warning(thd, Sql_condition::SL_WARNING,
                 WARN_PLUGIN_BUSY, ER_THD(thd, WARN_PLUGIN_BUSY));
  else
    reap_needed= true;

  // Check if we need to remove I_S plugin metadata from DD.
  remove_IS_metadata_from_dd=
    (plugin->plugin->type == MYSQL_INFORMATION_SCHEMA_PLUGIN &&
     plugin->load_option != PLUGIN_OFF);

  orig_plugin_name= dd::String_type(plugin->name.str, plugin->name.length);
  reap_plugins();
  mysql_mutex_unlock(&LOCK_plugin);

  uchar user_key[MAX_KEY_LENGTH];
  table->use_all_columns();
  table->field[0]->store(name->str, name->length, system_charset_info);
  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if ((rc=table->file->ha_index_read_idx_map(table->record[0], 0,
                                             user_key,
                                             HA_WHOLE_KEY,
                                             HA_READ_KEY_EXACT)) == 0)
  {
    /*
      We do not replicate the UNINSTALL PLUGIN statement. Disable binlogging
      of the delete from the plugin table, so that it is not replicated in
      row based mode.
    */
    DBUG_ASSERT(!thd->is_error());
    Disable_binlog_guard binlog_guard(thd);
    rc= table->file->ha_delete_row(table->record[0]);
    if (rc)
    {
      table->file->print_error(rc, MYF(0));
      DBUG_ASSERT(thd->is_error());
    }
    else
      error= false;
  }
  else if (rc != HA_ERR_KEY_NOT_FOUND && rc != HA_ERR_END_OF_FILE)
  {
    table->file->print_error(rc, MYF(0));
    DBUG_ASSERT(thd->is_error());
  }
  else
    error= false;

  if (!error &&
      !thd->transaction_rollback_request &&
      remove_IS_metadata_from_dd)
  {
    error= dd::info_schema::remove_I_S_view_metadata(
             thd, dd::String_type(orig_plugin_name.c_str(),
                                  orig_plugin_name.length()));
    DBUG_ASSERT(!error || thd->is_error());

    if (!error)
    {
      Uncommitted_tables_guard uncommitted_tables(thd);
      error= update_referencing_views_metadata(thd, INFORMATION_SCHEMA_NAME.str,
                                               orig_plugin_name.c_str(),
                                               false, &uncommitted_tables);
    }
  }

err:
  mysql_mutex_unlock(&LOCK_plugin_install);
  DBUG_RETURN(end_transaction(thd, error ||
                              thd->transaction_rollback_request));
}

bool plugin_foreach_with_mask(THD *thd, plugin_foreach_func **funcs,
                              int type, uint state_mask, void *arg)
{
  size_t idx, total;
  st_plugin_int *plugin, **plugins;
  int version=plugin_array_version;
  DBUG_ENTER("plugin_foreach_with_mask");

  if (!initialized)
    DBUG_RETURN(false);

  state_mask= ~state_mask; // do it only once

  mysql_mutex_lock(&LOCK_plugin);
  total= type == MYSQL_ANY_PLUGIN ? plugin_array->size()
                                  : plugin_hash[type]->size();
  /*
    Do the alloca out here in case we do have a working alloca:
        leaving the nested stack frame invalidates alloca allocation.
  */
  plugins=(st_plugin_int **)my_alloca(total*sizeof(plugin));
  if (type == MYSQL_ANY_PLUGIN)
  {
    for (idx= 0; idx < total; idx++)
    {
      plugin= plugin_array->at(idx);
      plugins[idx]= !(plugin->state & state_mask) ? plugin : NULL;
    }
  }
  else
  {
    collation_unordered_map<std::string, st_plugin_int *> *hash= plugin_hash[type];
    idx= 0;
    for (const auto &key_and_value : *hash)
    {
      plugin= key_and_value.second;
      plugins[idx++]= !(plugin->state & state_mask) ? plugin : NULL;
    }
  }
  mysql_mutex_unlock(&LOCK_plugin);

  for (;*funcs != NULL; ++funcs)
  {
    for (idx= 0; idx < total; idx++)
    {
      if (unlikely(version != plugin_array_version))
      {
        mysql_mutex_lock(&LOCK_plugin);
        for (size_t i=idx; i < total; i++)
          if (plugins[i] && plugins[i]->state & state_mask)
            plugins[i]=0;
        mysql_mutex_unlock(&LOCK_plugin);
      }
      plugin= plugins[idx];
      /* It will stop iterating on first engine error when "func" returns true */
      if (plugin && (*funcs)(thd, plugin_int_to_ref(plugin), arg))
          goto err;
    }
  }

  DBUG_RETURN(false);
err:
  DBUG_RETURN(true);
}

bool plugin_foreach_with_mask(THD *thd, plugin_foreach_func *func,
                              int type, uint state_mask, void *arg)
{
  plugin_foreach_func *funcs[]= { func, NULL };

  return plugin_foreach_with_mask(thd, funcs, type, state_mask, arg);
}

/****************************************************************************
  System Variables support
****************************************************************************/
/*
  This function is not thread safe as the pointer returned at the end of
  the function is outside mutex.
*/

void lock_plugin_mutex()
{
  mysql_mutex_lock(&LOCK_plugin);
}

void unlock_plugin_mutex()
{
  mysql_mutex_unlock(&LOCK_plugin);
}

sys_var *find_sys_var_ex(THD *thd, const char *str, size_t length,
                         bool throw_error, bool locked)
{
  sys_var *var;
  sys_var_pluginvar *pi= NULL;
  plugin_ref plugin;
  DBUG_ENTER("find_sys_var_ex");

  if (!locked)
    mysql_mutex_lock(&LOCK_plugin);
  mysql_rwlock_rdlock(&LOCK_system_variables_hash);
  if ((var= intern_find_sys_var(str, length)) &&
      (pi= var->cast_pluginvar()) && pi->is_plugin)
  {
    mysql_rwlock_unlock(&LOCK_system_variables_hash);
    LEX *lex= thd ? thd->lex : 0;
    if (!(plugin= my_intern_plugin_lock(lex, plugin_int_to_ref(pi->plugin))))
      var= NULL; /* failed to lock it, it must be uninstalling */
    else
    if (!(plugin_state(plugin) & PLUGIN_IS_READY))
    {
      /* initialization not completed */
      var= NULL;
      intern_plugin_unlock(lex, plugin);
    }
  }
  else
    mysql_rwlock_unlock(&LOCK_system_variables_hash);
  if (!locked)
    mysql_mutex_unlock(&LOCK_plugin);

  if (!throw_error && !var)
    my_error(ER_UNKNOWN_SYSTEM_VARIABLE, MYF(0), (char*) str);
  DBUG_RETURN(var);
}


sys_var *find_sys_var(THD *thd, const char *str, size_t length)
{
  return find_sys_var_ex(thd, str, length, false, false);
}

/*
  returns a bookmark for thd-local variables, creating if neccessary.
  returns null for non thd-local variables.
  Requires that a write lock is obtained on LOCK_system_variables_hash
*/
static st_bookmark *register_var(const char *plugin, const char *name,
                                 int flags)
{
  size_t length= strlen(plugin) + strlen(name) + 3, size= 0, offset, new_size;
  st_bookmark *result;
  char *varname, *p;

  if (!(flags & PLUGIN_VAR_THDLOCAL))
    return NULL;

  switch (flags & PLUGIN_VAR_TYPEMASK) {
  case PLUGIN_VAR_BOOL:
    size= sizeof(bool);
    break;
  case PLUGIN_VAR_INT:
    size= sizeof(int);
    break;
  case PLUGIN_VAR_LONG:
  case PLUGIN_VAR_ENUM:
    size= sizeof(long);
    break;
  case PLUGIN_VAR_LONGLONG:
  case PLUGIN_VAR_SET:
    size= sizeof(ulonglong);
    break;
  case PLUGIN_VAR_STR:
    size= sizeof(char*);
    break;
  case PLUGIN_VAR_DOUBLE:
    size= sizeof(double);
    break;
  default:
    DBUG_ASSERT(0);
    return NULL;
  };

  varname= ((char*) my_alloca(length));
  strxmov(varname + 1, plugin, "_", name, NullS);
  for (p= varname + 1; *p; p++)
    if (*p == '-')
      *p= '_';

  if (!(result= find_bookmark(NULL, varname + 1, flags)))
  {
    result= (st_bookmark*) alloc_root(&plugin_mem_root,
                                      sizeof(st_bookmark) + length-1);
    varname[0]= flags & PLUGIN_VAR_TYPEMASK;
    memcpy(result->key, varname, length);
    result->name_len= length - 2;
    result->offset= -1;

    DBUG_ASSERT(size && !(size & (size-1))); /* must be power of 2 */

    offset= global_system_variables.dynamic_variables_size;
    offset= (offset + size - 1) & ~(size - 1);
    result->offset= (int) offset;

    new_size= (offset + size + 63) & ~63;

    if (new_size > global_variables_dynamic_size)
    {
      global_system_variables.dynamic_variables_ptr= (char*)
        my_realloc(key_memory_global_system_variables,
                   global_system_variables.dynamic_variables_ptr, new_size,
                   MYF(MY_WME | MY_FAE | MY_ALLOW_ZERO_PTR));
      max_system_variables.dynamic_variables_ptr= (char*)
        my_realloc(key_memory_global_system_variables,
                   max_system_variables.dynamic_variables_ptr, new_size,
                   MYF(MY_WME | MY_FAE | MY_ALLOW_ZERO_PTR));
      /*
        Clear the new variable value space. This is required for string
        variables. If their value is non-NULL, it must point to a valid
        string.
      */
      memset(global_system_variables.dynamic_variables_ptr +
             global_variables_dynamic_size, 0,
             new_size - global_variables_dynamic_size);
      memset(max_system_variables.dynamic_variables_ptr +
             global_variables_dynamic_size, 0,
             new_size - global_variables_dynamic_size);
      global_variables_dynamic_size= new_size;
    }

    global_system_variables.dynamic_variables_head= offset;
    max_system_variables.dynamic_variables_head= offset;
    global_system_variables.dynamic_variables_size= offset + size;
    max_system_variables.dynamic_variables_size= offset + size;
    global_system_variables.dynamic_variables_version++;
    max_system_variables.dynamic_variables_version++;

    result->version= global_system_variables.dynamic_variables_version;

    /* this should succeed because we have already checked if a dup exists */
    std::string key(result->key, result->name_len + 1);
    bookmark_hash->emplace(key, result);

    /*
      Hashing vars of string type with MEMALLOC flag.
    */
    if (((flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR) &&
        (flags & PLUGIN_VAR_MEMALLOC) &&
        !malloced_string_type_sysvars_bookmark_hash->emplace(
          key, result).second)
    {
      fprintf(stderr, "failed to add placeholder to"
                      " hash of malloced string type sysvars");
      DBUG_ASSERT(0);
    }
  }
  return result;
}

static void restore_pluginvar_names(sys_var *first)
{
  for (sys_var *var= first; var; var= var->next)
  {
    sys_var_pluginvar *pv= var->cast_pluginvar();
    pv->plugin_var->name= pv->orig_pluginvar_name;
  }
}


/**
  Allocate memory and copy dynamic variables from global system variables
  to per-thread system variables copy.

  @param thd              thread context
  @param global_lock      If true LOCK_global_system_variables should be
                          acquired while copying variables from global
                          variables copy.
*/
void alloc_and_copy_thd_dynamic_variables(THD *thd, bool global_lock)
{
  mysql_rwlock_rdlock(&LOCK_system_variables_hash);

  if (global_lock)
    mysql_mutex_lock(&LOCK_global_system_variables);

  mysql_mutex_assert_owner(&LOCK_global_system_variables);

  /*
    MAINTAINER:
    The following assert is wrong on purpose, useful to debug
    when thd dynamic variables are expanded:
    DBUG_ASSERT(thd->variables.dynamic_variables_ptr == NULL);
  */

  thd->variables.dynamic_variables_ptr= (char*)
    my_realloc(key_memory_THD_variables,
               thd->variables.dynamic_variables_ptr,
               global_variables_dynamic_size,
               MYF(MY_WME | MY_FAE | MY_ALLOW_ZERO_PTR));

  /*
    Debug hook which allows tests to check that this code is not
    called for InnoDB after connection was created.
  */
  DBUG_EXECUTE_IF("verify_innodb_thdvars", DBUG_ASSERT(0););

  memcpy(thd->variables.dynamic_variables_ptr +
         thd->variables.dynamic_variables_size,
         global_system_variables.dynamic_variables_ptr +
         thd->variables.dynamic_variables_size,
         global_system_variables.dynamic_variables_size -
         thd->variables.dynamic_variables_size);

  /*
    Iterate through newly copied vars of string type with MEMALLOC
    flag and strdup value.
  */
  for (const auto &key_and_value : *malloced_string_type_sysvars_bookmark_hash)
  {
    sys_var_pluginvar *pi;
    sys_var *var;
    int varoff;
    char **thdvar, **sysvar;
    st_bookmark *v= key_and_value.second;

    if (v->version <= thd->variables.dynamic_variables_version ||
        !(var= intern_find_sys_var(v->key + 1, v->name_len)) ||
        !(pi= var->cast_pluginvar()) ||
        v->key[0] != (pi->plugin_var->flags & PLUGIN_VAR_TYPEMASK))
      continue;

    varoff= *(int *) (pi->plugin_var + 1);
    thdvar= (char **) (thd->variables.
                       dynamic_variables_ptr + varoff);
    sysvar= (char **) (global_system_variables.
                       dynamic_variables_ptr + varoff);
    *thdvar= NULL;
    plugin_var_memalloc_session_update(thd, NULL, thdvar, *sysvar);
  }

  if (global_lock)
    mysql_mutex_unlock(&LOCK_global_system_variables);

  thd->variables.dynamic_variables_version=
    global_system_variables.dynamic_variables_version;
  thd->variables.dynamic_variables_head=
    global_system_variables.dynamic_variables_head;
  thd->variables.dynamic_variables_size=
    global_system_variables.dynamic_variables_size;

  mysql_rwlock_unlock(&LOCK_system_variables_hash);
}

/**
  For correctness and simplicity's sake, a pointer to a function
  must be compatible with pointed-to type, that is, the return and
  parameters types must be the same. Thus, a callback function is
  defined for each scalar type. The functions are assigned in
  construct_options to their respective types.
*/

static bool *mysql_sys_var_bool(THD* thd, int offset)
{
  return (bool *) intern_sys_var_ptr(thd, offset, true);
}

static int *mysql_sys_var_int(THD* thd, int offset)
{
  return (int *) intern_sys_var_ptr(thd, offset, true);
}

static long *mysql_sys_var_long(THD* thd, int offset)
{
  return (long *) intern_sys_var_ptr(thd, offset, true);
}

static unsigned long *mysql_sys_var_ulong(THD* thd, int offset)
{
  return (unsigned long *) intern_sys_var_ptr(thd, offset, true);
}

static long long *mysql_sys_var_longlong(THD* thd, int offset)
{
  return (long long *) intern_sys_var_ptr(thd, offset, true);
}

static unsigned long long *mysql_sys_var_ulonglong(THD* thd, int offset)
{
  return (unsigned long long *) intern_sys_var_ptr(thd, offset, true);
}

static char **mysql_sys_var_str(THD* thd, int offset)
{
  return (char **) intern_sys_var_ptr(thd, offset, true);
}

static double *mysql_sys_var_double(THD* thd, int offset)
{
  return (double *) intern_sys_var_ptr(thd, offset, true);
}

void plugin_thdvar_init(THD *thd, bool enable_plugins)
{
  plugin_ref old_table_plugin= thd->variables.table_plugin;
  plugin_ref old_temp_table_plugin= thd->variables.temp_table_plugin;
  DBUG_ENTER("plugin_thdvar_init");

  thd->variables.table_plugin= NULL;
  thd->variables.temp_table_plugin= NULL;
  cleanup_variables(thd, &thd->variables);

  mysql_mutex_lock(&LOCK_global_system_variables);
  thd->variables= global_system_variables;
  thd->variables.table_plugin= NULL;
  thd->variables.temp_table_plugin= NULL;

  thd->variables.dynamic_variables_version= 0;
  thd->variables.dynamic_variables_size= 0;
  thd->variables.dynamic_variables_ptr= 0;

  if (enable_plugins)
  {
    mysql_mutex_lock(&LOCK_plugin);
    thd->variables.table_plugin=
      my_intern_plugin_lock(NULL, global_system_variables.table_plugin);
    intern_plugin_unlock(NULL, old_table_plugin);
    thd->variables.temp_table_plugin=
      my_intern_plugin_lock(NULL, global_system_variables.temp_table_plugin);
    intern_plugin_unlock(NULL, old_temp_table_plugin);
    mysql_mutex_unlock(&LOCK_plugin);
  }
  mysql_mutex_unlock(&LOCK_global_system_variables);

  /* Initialize all Sys_var_charptr variables here. */

  // @@session.session_track_system_variables
  thd->session_sysvar_res_mgr.init(&thd->variables.track_sysvars_ptr);

  DBUG_VOID_RETURN;
}


/*
  Unlocks all system variables which hold a reference
*/
static void unlock_variables(struct System_variables *vars)
{
  intern_plugin_unlock(NULL, vars->table_plugin);
  intern_plugin_unlock(NULL, vars->temp_table_plugin);
  vars->table_plugin= NULL;
  vars->temp_table_plugin= NULL;
}


/*
  Frees memory used by system variables

  Unlike plugin_vars_free_values() it frees all variables of all plugins,
  it's used on shutdown.
*/
static void cleanup_variables(THD *thd, struct System_variables *vars)
{
  if (thd)
  {
    /* Block the Performance Schema from accessing THD::variables. */
    mysql_mutex_lock(&thd->LOCK_thd_data);

    plugin_var_memalloc_free(&thd->variables);
    thd->session_sysvar_res_mgr.deinit();
  }
  DBUG_ASSERT(vars->table_plugin == NULL);
  DBUG_ASSERT(vars->temp_table_plugin == NULL);

  my_free(vars->dynamic_variables_ptr);
  vars->dynamic_variables_ptr= NULL;
  vars->dynamic_variables_size= 0;
  vars->dynamic_variables_version= 0;

  if (thd)
    mysql_mutex_unlock(&thd->LOCK_thd_data);
}


void plugin_thdvar_cleanup(THD *thd, bool enable_plugins)
{
  DBUG_ENTER("plugin_thdvar_cleanup");

  if (enable_plugins)
  {
    MUTEX_LOCK(plugin_lock, &LOCK_plugin);
    unlock_variables(&thd->variables);
    size_t idx;
    if ((idx= thd->lex->plugins.size()))
    {
      plugin_ref *list= thd->lex->plugins.end() - 1;
      DBUG_PRINT("info",("unlocking %u plugins", static_cast<uint>(idx)));
      while (list >= thd->lex->plugins.begin())
        intern_plugin_unlock(thd->lex, *list--);
    }

    reap_plugins();
    thd->lex->plugins.clear();
  }
  cleanup_variables(thd, &thd->variables);

  DBUG_VOID_RETURN;
}


/**
  @brief Free values of thread variables of a plugin.

  This must be called before a plugin is deleted. Otherwise its
  variables are no longer accessible and the value space is lost. Note
  that only string values with PLUGIN_VAR_MEMALLOC are allocated and
  must be freed.

  @param[in]        vars        Chain of system variables of a plugin
*/

static void plugin_vars_free_values(sys_var *vars)
{
  DBUG_ENTER("plugin_vars_free_values");

  for (sys_var *var= vars; var; var= var->next)
  {
    sys_var_pluginvar *piv= var->cast_pluginvar();
    if (piv &&
        ((piv->plugin_var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR) &&
        (piv->plugin_var->flags & PLUGIN_VAR_MEMALLOC))
    {
      /* Free the string from global_system_variables. */
      char **valptr= (char**) piv->real_value_ptr(NULL, OPT_GLOBAL);
      DBUG_PRINT("plugin", ("freeing value for: '%s'  addr: %p",
                            var->name.str, valptr));
      my_free(*valptr);
      *valptr= NULL;
    }
  }
  DBUG_VOID_RETURN;
}

/**
  Set value for a thread local variable.

  @param[in]     thd   Thread context.
  @param[in]     var   Plugin variable.
  @param[in,out] dest  Destination memory pointer.
  @param[in]     value New value.

  Note: new value should be '\0'-terminated for string variables.

  Used in plugin.h:THDVAR_SET(thd, name, value) macro.
*/

void plugin_thdvar_safe_update(THD *thd, SYS_VAR *var, char **dest, const char *value)
{
  DBUG_ASSERT(thd == current_thd);

  if (var->flags & PLUGIN_VAR_THDLOCAL)
  {
    if ((var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR &&
        var->flags & PLUGIN_VAR_MEMALLOC)
      plugin_var_memalloc_session_update(thd, var, dest, value);
    else
      var->update(thd, var, dest, value);
  }
}


/**
  Free all elements allocated by plugin_var_memalloc_session_update().

  @param[in]     vars  system variables structure

  @see plugin_var_memalloc_session_update
*/

static void plugin_var_memalloc_free(struct System_variables *vars)
{
  LIST *next, *root;
  DBUG_ENTER("plugin_var_memalloc_free");
  for (root= vars->dynamic_variables_allocs; root; root= next)
  {
    next= root->next;
    my_free(root);
  }
  vars->dynamic_variables_allocs= NULL;
  DBUG_VOID_RETURN;
}

extern "C" bool get_one_plugin_option(int, const struct my_option*,
                                      char *);

bool get_one_plugin_option(int, const struct my_option*, char*)
{
  return 0;
}


/**
  Creates a set of my_option objects associated with a specified plugin-
  handle.

  @param mem_root Memory allocator to be used.
  @param tmp A pointer to a plugin handle
  @param[out] options A pointer to a pre-allocated static array

  The set is stored in the pre-allocated static array supplied to the function.
  The size of the array is calculated as (number_of_plugin_varaibles*2+3). The
  reason is that each option can have a prefix '--plugin-' in addtion to the
  shorter form '--&lt;plugin-name&gt;'. There is also space allocated for
  terminating NULL pointers.

  @return
    @retval -1 An error occurred
    @retval 0 Success
*/

static int construct_options(MEM_ROOT *mem_root, st_plugin_int *tmp,
                             my_option *options)
{
  const char *plugin_name= tmp->plugin->name;
  const LEX_STRING plugin_dash = { C_STRING_WITH_LEN("plugin-") };
  size_t plugin_name_len= strlen(plugin_name);
  size_t optnamelen;
  const int max_comment_len= 180;
  char *comment= (char *) alloc_root(mem_root, max_comment_len + 1);
  char *optname;

  int index= 0, offset= 0;
  SYS_VAR *opt, **plugin_option;
  st_bookmark *v;

  /** Used to circumvent the const attribute on my_option::name */
  char *plugin_name_ptr, *plugin_name_with_prefix_ptr;

  DBUG_ENTER("construct_options");

  plugin_name_ptr= (char*) alloc_root(mem_root, plugin_name_len + 1);
  strcpy(plugin_name_ptr, plugin_name);
  my_casedn_str(&my_charset_latin1, plugin_name_ptr);
  convert_underscore_to_dash(plugin_name_ptr, plugin_name_len);
  plugin_name_with_prefix_ptr= (char*) alloc_root(mem_root,
                                                  plugin_name_len +
                                                  plugin_dash.length + 1);
  strxmov(plugin_name_with_prefix_ptr, plugin_dash.str, plugin_name_ptr, NullS);

  if (tmp->load_option != PLUGIN_FORCE &&
      tmp->load_option != PLUGIN_FORCE_PLUS_PERMANENT)
  {
    /* support --skip-plugin-foo syntax */
    options[0].name= plugin_name_ptr;
    options[1].name= plugin_name_with_prefix_ptr;
    options[0].id= 0;
    options[1].id= -1;
    options[0].var_type= options[1].var_type= GET_ENUM;
    options[0].arg_type= options[1].arg_type= OPT_ARG;
    options[0].def_value= options[1].def_value= 1; /* ON */
    options[0].typelib= options[1].typelib= &global_plugin_typelib;

    strxnmov(comment, max_comment_len, "Enable or disable ", plugin_name,
            " plugin. Possible values are ON, OFF, FORCE (don't start "
            "if the plugin fails to load).", NullS);
    options[0].comment= comment;
    /*
      Allocate temporary space for the value of the tristate.
      This option will have a limited lifetime and is not used beyond
      server initialization.
      GET_ENUM value is an unsigned long integer.
    */
    options[0].value= options[1].value=
                      (uchar **)alloc_root(mem_root, sizeof(ulong));
    *((ulong*) options[0].value)= (ulong) options[0].def_value;

    options[0].arg_source= options[1].arg_source=
      (get_opt_arg_source *)alloc_root(mem_root, sizeof(get_opt_arg_source));
    memset(options[0].arg_source, 0, sizeof(get_opt_arg_source));
    options[0].arg_source->m_path_name[0]= 0;
    options[1].arg_source->m_path_name[0]= 0;
    options[0].arg_source->m_source= options[1].arg_source->m_source=
      enum_variable_source::COMPILED;

    options+= 2;
  }

  if (!my_strcasecmp(&my_charset_latin1, plugin_name_ptr, "NDBCLUSTER"))
  {
    plugin_name_ptr= const_cast<char*>("ndb"); // Use legacy "ndb" prefix
    plugin_name_len= 3;
  }

  /*
    Two passes as the 2nd pass will take pointer addresses for use
    by my_getopt and register_var() in the first pass uses realloc
  */

  for (plugin_option= tmp->plugin->system_vars;
       plugin_option && *plugin_option; plugin_option++, index++)
  {
    opt= *plugin_option;
    if (!(opt->flags & PLUGIN_VAR_THDLOCAL))
      continue;
    if (!(register_var(plugin_name_ptr, opt->name, opt->flags)))
      continue;
    switch (opt->flags & PLUGIN_VAR_TYPEMASK) {
    case PLUGIN_VAR_BOOL:
      ((thdvar_bool_t *) opt)->resolve= mysql_sys_var_bool;
      break;
    case PLUGIN_VAR_INT:
      ((thdvar_int_t *) opt)->resolve= mysql_sys_var_int;
      break;
    case PLUGIN_VAR_LONG:
      ((thdvar_long_t *) opt)->resolve= mysql_sys_var_long;
      break;
    case PLUGIN_VAR_LONGLONG:
      ((thdvar_longlong_t *) opt)->resolve= mysql_sys_var_longlong;
      break;
    case PLUGIN_VAR_STR:
      ((thdvar_str_t *) opt)->resolve= mysql_sys_var_str;
      break;
    case PLUGIN_VAR_ENUM:
      ((thdvar_enum_t *) opt)->resolve= mysql_sys_var_ulong;
      break;
    case PLUGIN_VAR_SET:
      ((thdvar_set_t *) opt)->resolve= mysql_sys_var_ulonglong;
      break;
    case PLUGIN_VAR_DOUBLE:
      ((thdvar_double_t *) opt)->resolve= mysql_sys_var_double;
      break;
    default:
      LogErr(ERROR_LEVEL, ER_PLUGIN_UNKNOWN_VARIABLE_TYPE,
             opt->flags, plugin_name);
      DBUG_RETURN(-1);
    };
  }

  for (plugin_option= tmp->plugin->system_vars;
       plugin_option && *plugin_option; plugin_option++, index++)
  {
    switch ((opt= *plugin_option)->flags & PLUGIN_VAR_TYPEMASK) {
    case PLUGIN_VAR_BOOL:
      if (!opt->check)
        opt->check= check_func_bool;
      if (!opt->update)
        opt->update= update_func_bool;
      break;
    case PLUGIN_VAR_INT:
      if (!opt->check)
        opt->check= check_func_int;
      if (!opt->update)
        opt->update= update_func_int;
      break;
    case PLUGIN_VAR_LONG:
      if (!opt->check)
        opt->check= check_func_long;
      if (!opt->update)
        opt->update= update_func_long;
      break;
    case PLUGIN_VAR_LONGLONG:
      if (!opt->check)
        opt->check= check_func_longlong;
      if (!opt->update)
        opt->update= update_func_longlong;
      break;
    case PLUGIN_VAR_STR:
      if (!opt->check)
        opt->check= check_func_str;
      if (!opt->update)
      {
        opt->update= update_func_str;
        if (!(opt->flags & (PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY)))
        {
          opt->flags|= PLUGIN_VAR_READONLY;
          LogErr(WARNING_LEVEL, ER_PLUGIN_VARIABLE_SET_READ_ONLY,
                 opt->name, plugin_name);
        }
      }
      break;
    case PLUGIN_VAR_ENUM:
      if (!opt->check)
        opt->check= check_func_enum;
      if (!opt->update)
        opt->update= update_func_long;
      break;
    case PLUGIN_VAR_SET:
      if (!opt->check)
        opt->check= check_func_set;
      if (!opt->update)
        opt->update= update_func_longlong;
      break;
    case PLUGIN_VAR_DOUBLE:
      if (!opt->check)
        opt->check= check_func_double;
      if (!opt->update)
        opt->update= update_func_double;
      break;
    default:
      LogErr(ERROR_LEVEL, ER_PLUGIN_UNKNOWN_VARIABLE_TYPE,
             opt->flags, plugin_name);
      DBUG_RETURN(-1);
    }

    if ((opt->flags & (PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_THDLOCAL))
                    == PLUGIN_VAR_NOCMDOPT)
      continue;

    if (!opt->name)
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_VARIABLE_MISSING_NAME, plugin_name);
      DBUG_RETURN(-1);
    }

    if (!(opt->flags & PLUGIN_VAR_THDLOCAL))
    {
      optnamelen= strlen(opt->name);
      optname= (char*) alloc_root(mem_root, plugin_name_len + optnamelen + 2);
      strxmov(optname, plugin_name_ptr, "-", opt->name, NullS);
      optnamelen= plugin_name_len + optnamelen + 1;
    }
    else
    {
      /* this should not fail because register_var should create entry */
      if (!(v= find_bookmark(plugin_name_ptr, opt->name, opt->flags)))
      {
        LogErr(ERROR_LEVEL, ER_PLUGIN_VARIABLE_NOT_ALLOCATED_THREAD_LOCAL,
               opt->name, plugin_name);
        DBUG_RETURN(-1);
      }

      *(int*)(opt + 1)= offset= v->offset;

      if (opt->flags & PLUGIN_VAR_NOCMDOPT)
        continue;

      optname= (char*) memdup_root(mem_root, v->key + 1,
                                   (optnamelen= v->name_len) + 1);
    }

    convert_underscore_to_dash(optname, optnamelen);

    options->name= optname;
    options->comment= opt->comment;
    options->app_type= opt;
    options->id= 0;

    plugin_opt_set_limits(options, opt);

    if (opt->flags & PLUGIN_VAR_THDLOCAL)
      options->value= options->u_max_value= (uchar**)
        (global_system_variables.dynamic_variables_ptr + offset);
    else
      options->value= options->u_max_value= *(uchar***) (opt + 1);

    char *option_name_ptr;
    options[1]= options[0];
    options[1].id= -1;
    options[1].name= option_name_ptr= (char*) alloc_root(mem_root,
                                                        plugin_dash.length +
                                                        optnamelen + 1);
    options[1].comment= 0; /* Hidden from the help text */
    strxmov(option_name_ptr, plugin_dash.str, optname, NullS);

    options[0].arg_source= options[1].arg_source=
      (get_opt_arg_source *)alloc_root(mem_root, sizeof(get_opt_arg_source));
    memset(options[0].arg_source, 0, sizeof(get_opt_arg_source));
    options[0].arg_source->m_path_name[0]= 0;
    options[1].arg_source->m_path_name[0]= 0;
    options[0].arg_source->m_source= options[1].arg_source->m_source=
      enum_variable_source::COMPILED;

    options+= 2;
  }

  DBUG_RETURN(0);
}


static my_option *construct_help_options(MEM_ROOT *mem_root,
                                         st_plugin_int *p)
{
  SYS_VAR **opt;
  my_option *opts;
  uint count= EXTRA_OPTIONS;
  DBUG_ENTER("construct_help_options");

  for (opt= p->plugin->system_vars; opt && *opt; opt++, count+= 2)
    ;

  if (!(opts= (my_option*) alloc_root(mem_root, sizeof(my_option) * count)))
    DBUG_RETURN(NULL);

  memset(opts, 0, sizeof(my_option) * count);

  /**
    some plugin variables (those that don't have PLUGIN_VAR_NOSYSVAR flag)
    have their names prefixed with the plugin name. Restore the names here
    to get the correct (not double-prefixed) help text.
    We won't need @@sysvars anymore and don't care about their proper names.
  */
  restore_pluginvar_names(p->system_vars);

  if (construct_options(mem_root, p, opts))
    DBUG_RETURN(NULL);

  DBUG_RETURN(opts);
}


/**
  Check option being used and raise deprecation warning if required.

  @param optid ID of the option that was passed through command line
  @param opt List of options
  @param argument unused

  A deprecation warning will be raised if --plugin-xxx type of option
  is used.

  @return Always returns success as purpose of the function is to raise
  warning only.
  @retval 0 Success
*/

static bool check_if_option_is_deprecated(int optid,
                                          const struct my_option *opt,
                                          char *argument MY_ATTRIBUTE((unused)))
{
  if (optid == -1)
  {
    push_deprecated_warn(NULL, opt->name, (opt->name + strlen("plugin-")));
  }
  return 0;
}


/**
  Create and register system variables supplied from the plugin and
  assigns initial values from corresponding command line arguments.

  @param tmp_root Temporary scratch space
  @param[out] tmp Internal plugin structure
  @param argc Number of command line arguments
  @param argv Command line argument vector

  The plugin will be updated with a policy on how to handle errors during
  initialization.

  @note Requires that a write-lock is held on LOCK_system_variables_hash

  @return How initialization of the plugin should be handled.
    @retval  0 Initialization should proceed.
    @retval  1 Plugin is disabled.
    @retval -1 An error has occurred.
*/

static int test_plugin_options(MEM_ROOT *tmp_root, st_plugin_int *tmp,
                               int *argc, char **argv)
{
  struct sys_var_chain chain= { NULL, NULL };
  bool disable_plugin;
  enum_plugin_load_option plugin_load_option= tmp->load_option;

  /*
    We should use tmp->mem_root here instead of the global plugin_mem_root,
    but tmp->root is not always properly freed, so it will cause leaks in
    Valgrind (e.g. the main.validate_password_plugin test).
  */
  MEM_ROOT *mem_root= &plugin_mem_root;
  SYS_VAR **opt;
  my_option *opts= NULL;
  LEX_STRING plugin_name;
  char *varname;
  int error;
  sys_var *v MY_ATTRIBUTE((unused));
  st_bookmark *var;
  size_t len;
  uint count= EXTRA_OPTIONS;
  DBUG_ENTER("test_plugin_options");
  DBUG_ASSERT(tmp->plugin && tmp->name.str);

  /*
    The 'federated' and 'ndbcluster' storage engines are always disabled by
    default.
  */
  if (!(my_strcasecmp(&my_charset_latin1, tmp->name.str, "federated") &&
      my_strcasecmp(&my_charset_latin1, tmp->name.str, "ndbcluster")))
    plugin_load_option= PLUGIN_OFF;

  for (opt= tmp->plugin->system_vars; opt && *opt; opt++)
    count+= 2; /* --{plugin}-{optname} and --plugin-{plugin}-{optname} */

  if (count > EXTRA_OPTIONS || (*argc > 1))
  {
    if (!(opts= (my_option*) alloc_root(tmp_root, sizeof(my_option) * count)))
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_OOM, tmp->name.str);
      DBUG_RETURN(-1);
    }
    memset(opts, 0, sizeof(my_option) * count);

    if (construct_options(tmp_root, tmp, opts))
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_BAD_OPTIONS, tmp->name.str);
      DBUG_RETURN(-1);
    }

    /*
      We adjust the default value to account for the hardcoded exceptions
      we have set for the federated and ndbcluster storage engines.
    */
    if (tmp->load_option != PLUGIN_FORCE &&
        tmp->load_option != PLUGIN_FORCE_PLUS_PERMANENT)
      opts[0].def_value= opts[1].def_value= plugin_load_option;

    error= handle_options(argc, &argv, opts, check_if_option_is_deprecated);
    (*argc)++; /* add back one for the program name */

    if (error)
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_PARSING_OPTIONS_FAILED, tmp->name.str);
      goto err;
    }
    /*
     Set plugin loading policy from option value. First element in the option
     list is always the <plugin name> option value.
    */
    if (tmp->load_option != PLUGIN_FORCE &&
        tmp->load_option != PLUGIN_FORCE_PLUS_PERMANENT)
      plugin_load_option= (enum_plugin_load_option) *(ulong*) opts[0].value;
  }

  disable_plugin= (plugin_load_option == PLUGIN_OFF);
  tmp->load_option= plugin_load_option;

  /*
    If the plugin is disabled it should not be initialized.
  */
  if (disable_plugin)
  {
    LogErr(INFORMATION_LEVEL, ER_PLUGIN_DISABLED, tmp->name.str);
    if (opts)
      my_cleanup_options(opts);
    DBUG_RETURN(1);
  }

  if (!my_strcasecmp(&my_charset_latin1, tmp->name.str, "NDBCLUSTER"))
  {
    plugin_name.str= const_cast<char*>("ndb"); // Use legacy "ndb" prefix
    plugin_name.length= 3;
  }
  else
    plugin_name= tmp->name;

  error= 1;
  for (opt= tmp->plugin->system_vars; opt && *opt; opt++)
  {
    SYS_VAR *o;
    const my_option** optp= (const my_option**)&opts;
    if (((o= *opt)->flags & PLUGIN_VAR_NOSYSVAR))
      continue;
    if ((var= find_bookmark(plugin_name.str, o->name, o->flags)))
      v= new (mem_root) sys_var_pluginvar(&chain, var->key + 1, o);
    else
    {
      len= plugin_name.length + strlen(o->name) + 2;
      varname= (char*) alloc_root(mem_root, len);
      strxmov(varname, plugin_name.str, "-", o->name, NullS);
      my_casedn_str(&my_charset_latin1, varname);
      convert_dash_to_underscore(varname, len-1);
      v= new (mem_root) sys_var_pluginvar(&chain, varname, o);
    }
    DBUG_ASSERT(v); /* check that an object was actually constructed */

    if (findopt((char*)o->name, strlen(o->name), optp))
      v->set_arg_source((*optp)->arg_source);
  } /* end for */
  if (chain.first)
  {
    chain.last->next = NULL;
    if (mysql_add_sys_var_chain(chain.first))
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_HAS_CONFLICTING_SYSTEM_VARIABLES,
             tmp->name.str);
      goto err;
    }
    tmp->system_vars= chain.first;
  }

  /*
    Once server is started and if there are few persisted plugin variables
    which needs to be handled, we do it here.
  */
  if (mysqld_server_started)
  {
    Persisted_variables_cache *pv= Persisted_variables_cache::get_instance();
    if (pv && pv->set_persist_options(true))
    {
      LogErr(ERROR_LEVEL, ER_PLUGIN_CANT_SET_PERSISTENT_OPTIONS,
             tmp->name.str);
      goto err;
    }
  }
  DBUG_RETURN(0);

err:
  if (opts)
    my_cleanup_options(opts);
  DBUG_RETURN(error);
}


/****************************************************************************
  Help Verbose text with Plugin System Variables
****************************************************************************/


void add_plugin_options(std::vector<my_option> *options, MEM_ROOT *mem_root)
{
  my_option *opt;

  if (!initialized)
    return;

  for (st_plugin_int **it= plugin_array->begin();
       it != plugin_array->end(); ++it)
  {
    st_plugin_int *p= *it;

    if (!(opt= construct_help_options(mem_root, p)))
      continue;

    /* Only options with a non-NULL comment are displayed in help text */
    for (;opt->name; opt++)
      if (opt->comment)
        options->push_back(*opt);
  }
}

/**
  Searches for a correctly loaded plugin of a particular type by name

  @param plugin   the name of the plugin we're looking for
  @param type     type of the plugin (0-MYSQL_MAX_PLUGIN_TYPE_NUM)
  @return plugin, or NULL if not found
*/
st_plugin_int *plugin_find_by_type(const LEX_CSTRING &plugin, int type)
{
  st_plugin_int *ret;
  DBUG_ENTER("plugin_find_by_type");

  ret= plugin_find_internal(plugin, type);
  DBUG_RETURN(ret && ret->state == PLUGIN_IS_READY ? ret : NULL);
}


/**
  Locks the plugin strucutres so calls to plugin_find_inner can be issued.

  Must be followed by unlock_plugin_data.
*/
int lock_plugin_data()
{
  DBUG_ENTER("lock_plugin_data");
  DBUG_RETURN(mysql_mutex_lock(&LOCK_plugin));
}


/**
  Unlocks the plugin strucutres as locked by lock_plugin_data()
*/
int unlock_plugin_data()
{
  DBUG_ENTER("unlock_plugin_data");
  DBUG_RETURN(mysql_mutex_unlock(&LOCK_plugin));
}


bool Sql_cmd_install_plugin::execute(THD *thd)
{
  bool st= mysql_install_plugin(thd, &m_comment, &m_ident);
  if (!st)
    my_ok(thd);
  mysql_audit_release(thd);
  return st;
}


bool Sql_cmd_uninstall_plugin::execute(THD *thd)
{
  bool st= mysql_uninstall_plugin(thd, &m_comment);
  if (!st)
    my_ok(thd);
  mysql_audit_release(thd);
  return st;
}
