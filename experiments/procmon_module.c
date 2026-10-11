#include "kernel/param.h"
#include "module_helpers.h"
static const struct module_api_v2 *services;
static struct module_process_info processes[NPROC];
static const char *state_name(uint s) {
  if(s==MODULE_PROC_USED)return "used";
  if(s==MODULE_PROC_SLEEPING)return "sleeping";
  if(s==MODULE_PROC_RUNNABLE)return "runnable";
  if(s==MODULE_PROC_RUNNING)return "running";
  if(s==MODULE_PROC_ZOMBIE)return "zombie";
  return "unknown";
}
int module_init(const struct module_api_v2 *api) {
  if(!api || api->abi_version!=MODULE_API_V2 || api->struct_size<sizeof(*api) ||
     (api->capabilities&(MODULE_CAP_PRINT|MODULE_CAP_PROCESSES))!=(MODULE_CAP_PRINT|MODULE_CAP_PROCESSES))return -1;
  services=api; return services->print("procmon: initialized");
}
int module_command(int argc,const char *const argv[]) {
  uint32 count=0,i; (void)argv;
  if(!services || argc || services->process_snapshot(processes,NPROC,&count)<0)return -1;
  if(module_emit_number(services,"procmon active_processes=",count)<0)return -1;
  for(i=0;i<count;i++) {
    char line[MODULE_PRINT_MAX]; uint n=0; line[0]=0;
    if(module_add_field(line,sizeof(line),&n,"procmon pid=",(uint64)processes[i].pid)<0 ||
       module_append(line,sizeof(line),&n," state=")<0 ||
       module_append(line,sizeof(line),&n,state_name(processes[i].state))<0 ||
       module_append(line,sizeof(line),&n," name=")<0 ||
       module_append(line,sizeof(line),&n,processes[i].name)<0 || services->print(line)<0)return -1;
  }
  return 0;
}
void module_exit(void) { if(services)services->print("procmon: exit"); services=0; }
