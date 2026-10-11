#include "kernel/module.h"
static const struct module_api_v2 *services;
int module_init(const struct module_api_v2 *api) { services=api;if(services&&services->print)services->print("failtest: init returns failure");return -1; }
int module_command(int argc,const char *const argv[]) {(void)argc;(void)argv;return -1;}
void module_exit(void) {if(services&&services->print)services->print("FAILTEST ERROR: exit called after failed init");}
