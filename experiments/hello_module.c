#include "kernel/module.h"

static const struct module_api_v1 *module_api;

int
module_init(const struct module_api_v1 *api)
{
  if (api == 0 || api->abi_version != MODULE_API_V1 ||
      api->struct_size < sizeof(*api) ||
      (api->capabilities & MODULE_CAP_PRINT) == 0 || api->print == 0)
    return -1;
  module_api = api;
  module_api->print("hello module initialized through API v1");
  return 0;
}

int
module_command(int argc, const char *const argv[])
{
  int i;
  if (module_api == 0 || argc < 0 || argc > MODULE_MAX_ARGS)
    return -1;
  module_api->print("hello module command invoked");
  for (i = 0; i < argc; i++) {
    if (argv == 0 || argv[i] == 0)
      return -1;
    module_api->print(argv[i]);
  }
  return argc;
}

void
module_exit(void)
{
  if (module_api != 0)
    module_api->print("hello module exiting");
}
