#include "module_helpers.h"
static const struct module_api_v2 *services;
static struct module_log_snapshot snapshot;
static uint text_length(const char *s) { uint n=0; while(s[n])n++; return n; }
static int append_message(char *out,uint cap,int argc,const char *const argv[]) {
  int i; uint used=0; out[0]=0;
  for(i=2;i<argc;i++) { uint j,n=text_length(argv[i]);
    if(i>2 && module_append(out,cap,&used," ")<0)return -1;
    for(j=0;j<n;j++){char c[2]={argv[i][j],0};if(module_append(out,cap,&used,c)<0)return -1;}
  }
  return used?0:-1;
}
static uint parse_level(const char *s) {
  if(s[0]=='I'&&s[1]=='N'&&s[2]=='F'&&s[3]=='O'&&!s[4])return MODULE_LOG_INFO;
  if(s[0]=='W'&&s[1]=='A'&&s[2]=='R'&&s[3]=='N'&&!s[4])return MODULE_LOG_WARN;
  if(s[0]=='E'&&s[1]=='R'&&s[2]=='R'&&s[3]=='O'&&s[4]=='R'&&!s[5])return MODULE_LOG_ERROR;
  return 0;
}
static const char *level_name(uint l) { return l==MODULE_LOG_INFO?"INFO":l==MODULE_LOG_WARN?"WARN":"ERROR"; }
int module_init(const struct module_api_v2 *api) {
  if(!api || api->abi_version!=MODULE_API_V2 || api->struct_size<sizeof(*api) ||
     (api->capabilities&(MODULE_CAP_PRINT|MODULE_CAP_LOG))!=(MODULE_CAP_PRINT|MODULE_CAP_LOG))return -1;
  services=api; return services->print("klogger: initialized");
}
int module_command(int argc,const char *const argv[]) {
  if(!services || argc<1 || !argv)return -1;
  if(argv[0][0]=='a'&&argv[0][1]=='d'&&argv[0][2]=='d'&&!argv[0][3]) {
    char message[MODULE_LOG_MESSAGE_MAX]; uint level;
    if(argc<3 || !(level=parse_level(argv[1])) || append_message(message,sizeof(message),argc,argv)<0 ||
       services->log_append(level,message)<0) { services->print("klogger usage: add INFO|WARN|ERROR message"); return -1; }
    return services->print("klogger: entry added");
  }
  if(argv[0][0]=='s'&&argv[0][1]=='h'&&argv[0][2]=='o'&&argv[0][3]=='w'&&!argv[0][4]) {
    uint32 i;
    if(argc!=1 || services->log_snapshot(&snapshot)<0)return -1;
    { char line[MODULE_PRINT_MAX];uint n=0;line[0]=0;
      if(module_add_field(line,sizeof(line),&n,"klogger entries=",snapshot.count)<0 ||
         module_append(line,sizeof(line),&n," dropped=")<0 ||
         module_append_number(line,sizeof(line),&n,snapshot.dropped)<0 || services->print(line)<0)return -1;
    }
    for(i=0;i<snapshot.count;i++) {
      char line[MODULE_PRINT_MAX];uint n=0;line[0]=0;
      if(module_add_field(line,sizeof(line),&n,"klogger seq=",snapshot.records[i].sequence)<0 ||
         module_append(line,sizeof(line),&n," tick=")<0 ||
         module_append_number(line,sizeof(line),&n,snapshot.records[i].tick)<0 ||
         module_append(line,sizeof(line),&n," level=")<0 ||
         module_append(line,sizeof(line),&n,level_name(snapshot.records[i].level))<0 ||
         module_append(line,sizeof(line),&n," message=")<0 ||
         module_append(line,sizeof(line),&n,snapshot.records[i].message)<0 || services->print(line)<0)return -1;
    }
    return 0;
  }
  if(argv[0][0]=='c'&&argv[0][1]=='l'&&argv[0][2]=='e'&&argv[0][3]=='a'&&argv[0][4]=='r'&&!argv[0][5]) {
    if(argc!=1 || services->log_clear()<0)return -1;
    return services->print("klogger: log cleared");
  }
  services->print("klogger usage: add, show, or clear"); return -1;
}
void module_exit(void) { if(services)services->print("klogger: exit"); services=0; }
