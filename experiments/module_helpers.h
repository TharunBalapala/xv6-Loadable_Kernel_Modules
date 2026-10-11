#ifndef MODULE_HELPERS_H
#define MODULE_HELPERS_H
#include "kernel/module.h"
static int module_append(char *b,uint cap,uint *n,const char *s) {
  uint i=0; if (*n>=cap) return -1;
  while(s[i]) { if(*n+1>=cap) return -1; b[(*n)++]=s[i++]; }
  b[*n]=0; return 0;
}
static int module_append_number(char *b,uint cap,uint *n,uint64 v) {
  char d[20]; uint count=0;
  if(v==0) return module_append(b,cap,n,"0");
  while(v) { d[count++]=(char)('0'+v%10); v/=10; }
  while(count) { char c[2]; c[0]=d[--count]; c[1]=0; if(module_append(b,cap,n,c)<0)return -1; }
  return 0;
}
static int module_add_field(char *b,uint cap,uint *n,const char *key,uint64 v) {
  return module_append(b,cap,n,key)<0 || module_append_number(b,cap,n,v)<0 ? -1:0;
}
static __attribute__((unused)) int module_emit_number(const struct module_api_v2 *api,const char *key,uint64 v) {
  char line[MODULE_PRINT_MAX]; uint n=0; line[0]=0;
  if(module_add_field(line,sizeof(line),&n,key,v)<0)return -1;
  return api->print(line);
}
#endif
