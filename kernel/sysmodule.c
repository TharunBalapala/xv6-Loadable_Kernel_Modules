#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "module.h"
#include "proc.h"
#include "defs.h"

uint64
sys_modload(void)
{
  char path[MAXPATH];
  if (argstr(0, path, sizeof(path)) < 0)
    return -1;
  return module_load(path);
}

uint64
sys_modunload(void)
{
  char name[MODULE_NAME_MAX + 1];
  if (argstr(0, name, sizeof(name)) < 0)
    return -1;
  return module_unload(name);
}

uint64
sys_modlist(void)
{
  return module_list();
}

uint64
sys_modcall(void)
{
  char name[MODULE_NAME_MAX + 1];
  char storage[MODULE_MAX_ARGS][MODULE_ARG_MAX];
  const char *arguments[MODULE_MAX_ARGS];
  uint64 user_argv, user_string;
  int argc, i;

  if (argstr(0, name, sizeof(name)) < 0)
    return -1;
  argint(1, &argc);
  argaddr(2, &user_argv);
  if (argc < 0 || argc > MODULE_MAX_ARGS)
    return -1;
  for (i = 0; i < argc; i++) {
    uint64 offset = (uint64)i * sizeof(uint64);
    if (user_argv > ~(uint64)0 - offset ||
        fetchaddr(user_argv + offset, &user_string) < 0 ||
        fetchstr(user_string, storage[i], sizeof(storage[i])) < 0)
      return -1;
    arguments[i] = storage[i];
  }
  return module_call(name, argc, arguments);
}
