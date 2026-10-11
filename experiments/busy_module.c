#include "kernel/module.h"
static const struct module_api_v2 *services;
int module_init(const struct module_api_v2 *api) {
  if(!api||api->abi_version!=MODULE_API_V2||api->struct_size<sizeof(*api)||!api->test_try_unload||!api->test_try_call)return -1;
  services=api;return services->print("busytest: initialized");
}
int module_command(int argc,const char *const argv[]) {
  int result;(void)argv;if(!services||argc)return -1;
  result=services->test_try_unload("busytest");
  if(result==0)return services->print("BUSYTEST ERROR: active unload succeeded");
  return services->print("busytest: active unload rejected");
}
void module_exit(void) {
  int result;if(!services)return;
  result=services->test_try_call("busytest");
  if(result>=0)services->print("BUSYTEST ERROR: call admitted during unload");
  else services->print("busytest: new call rejected during unload");
  services->print("busytest: exit");
}
