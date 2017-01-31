#include "mysql/psi/psi_thread.h"
#include "my_global.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "my_thread.h"
#include "psi_base.h"
#include "my_psi_config.h"
typedef unsigned int PSI_mutex_key;
typedef unsigned int PSI_rwlock_key;
typedef unsigned int PSI_cond_key;
typedef unsigned int PSI_thread_key;
typedef unsigned int PSI_file_key;
typedef unsigned int PSI_stage_key;
typedef unsigned int PSI_statement_key;
typedef unsigned int PSI_socket_key;
struct PSI_placeholder
{
  int m_placeholder;
};
struct opaque_THD
{
  int dummy;
};
typedef struct opaque_THD THD;
