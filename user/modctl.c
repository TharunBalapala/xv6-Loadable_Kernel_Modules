#include "kernel/types.h"
#include "user/user.h"

static void
usage(void)
{
  printf("usage: modctl load file.o | unload name | list | call name [args...]\n");
  exit(1);
}

int
main(int argc, char **argv)
{
  int result;
  if (argc == 2 && strcmp(argv[1], "list") == 0) {
    result = modlist();
    if (result < 0)
      printf("modctl: list failed\n");
    exit(result < 0);
  }
  if (argc == 3 && strcmp(argv[1], "load") == 0) {
    result = modload(argv[2]);
    if (result < 0)
      printf("modctl: load failed\n");
    exit(result < 0);
  }
  if (argc == 3 && strcmp(argv[1], "unload") == 0) {
    result = modunload(argv[2]);
    if (result < 0)
      printf("modctl: unload failed or module is busy\n");
    exit(result < 0);
  }
  if (argc >= 3 && strcmp(argv[1], "call") == 0) {
    result = modcall(argv[2], argc - 3,
                         (const char *const *)&argv[3]);
    if (result < 0)
      printf("modctl: call failed\n");
    else
      printf("modctl: command returned %d\n", result);
    exit(result < 0);
  }
  usage();
  return 1;
}
