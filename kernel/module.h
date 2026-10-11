#ifndef MODULE_H
#define MODULE_H
#include "types.h"
#define MODULE_API_V1 1
#define MODULE_CAP_PRINT (1UL << 0)
#define MODULE_NAME_MAX 15
#define MODULE_MAX_ARGS 6
#define MODULE_ARG_MAX 64
struct module_api_v1 {
  uint32 abi_version;
  uint32 struct_size;
  uint64 capabilities;
  int (*print)(const char *message);
};
typedef int (*module_init_fn)(const struct module_api_v1 *api);
typedef int (*module_command_fn)(int argc, const char *const argv[]);
typedef void (*module_exit_fn)(void);
#endif
