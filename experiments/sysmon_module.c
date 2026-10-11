#include "module_helpers.h"
static const struct module_api_v2 *services;
int module_init(const struct module_api_v2 *api) {
  if(!api || api->abi_version!=MODULE_API_V2 || api->struct_size<sizeof(*api) ||
     (api->capabilities&(MODULE_CAP_PRINT|MODULE_CAP_SYSTEM))!=(MODULE_CAP_PRINT|MODULE_CAP_SYSTEM)) return -1;
  services=api; return services->print("sysmon: initialized");
}
int module_command(int argc,const char *const argv[]) {
  struct module_system_stats s; (void)argv;
  if(!services || argc || services->system_snapshot(&s)<0)return -1;
  if(module_emit_number(services,"sysmon uptime_ticks=",s.uptime_ticks)<0 ||
     module_emit_number(services,"sysmon total_pages=",s.total_pages)<0 ||
     module_emit_number(services,"sysmon free_pages=",s.free_pages)<0 ||
     module_emit_number(services,"sysmon active_processes=",s.active_processes)<0 ||
     module_emit_number(services,"sysmon process_slots=",s.proc_slots)<0)return -1;
  return 0;
}
void module_exit(void) { if(services)services->print("sysmon: exit"); services=0; }
