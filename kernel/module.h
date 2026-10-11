#ifndef MODULE_H
#define MODULE_H
#include "types.h"
#define MODULE_API_V2 2
#define MODULE_CAP_PRINT       (1UL << 0)
#define MODULE_CAP_SYSTEM     (1UL << 1)
#define MODULE_CAP_PROCESSES  (1UL << 2)
#define MODULE_CAP_LOG        (1UL << 3)
#define MODULE_NAME_MAX 15
#define MODULE_MAX_ARGS 6
#define MODULE_ARG_MAX 64
#define MODULE_PRINT_MAX 192
#define MODULE_PROC_NAME_MAX 16
#define MODULE_PROC_STATE_COUNT 6
#define MODULE_LOG_CAPACITY 32
#define MODULE_LOG_MESSAGE_MAX 64

enum module_proc_state {
  MODULE_PROC_UNUSED = 0,
  MODULE_PROC_USED,
  MODULE_PROC_SLEEPING,
  MODULE_PROC_RUNNABLE,
  MODULE_PROC_RUNNING,
  MODULE_PROC_ZOMBIE
};

enum module_log_level {
  MODULE_LOG_INFO = 1,
  MODULE_LOG_WARN = 2,
  MODULE_LOG_ERROR = 3
};

struct module_system_stats {
  uint64 uptime_ticks;
  uint64 total_pages;
  uint64 free_pages;
  uint32 proc_slots;
  uint32 active_processes;
  uint32 state_count[MODULE_PROC_STATE_COUNT];
};

struct module_process_info {
  int pid;
  uint32 state;
  char name[MODULE_PROC_NAME_MAX];
};

struct module_log_record {
  uint64 sequence;
  uint64 tick;
  uint32 level;
  char message[MODULE_LOG_MESSAGE_MAX];
};

struct module_log_snapshot {
  uint32 count;
  uint32 dropped;
  struct module_log_record records[MODULE_LOG_CAPACITY];
};

struct module_api_v2 {
  uint32 abi_version;
  uint32 struct_size;
  uint64 capabilities;
  int (*print)(const char *message);
  int (*system_snapshot)(struct module_system_stats *result);
  int (*process_snapshot)(struct module_process_info *entries,
                         uint32 capacity, uint32 *count);
  int (*log_append)(uint32 level, const char *message);
  int (*log_snapshot)(struct module_log_snapshot *result);
  int (*log_clear)(void);
#ifdef MODULE_TESTS
  /* Test-only lifecycle probes; absent from normal kernel builds. */
  int (*test_try_unload)(const char *name);
  int (*test_try_call)(const char *name);
#endif
};
typedef int (*module_init_fn)(const struct module_api_v2 *api);
typedef int (*module_command_fn)(int argc, const char *const argv[]);
typedef void (*module_exit_fn)(void);
#endif
