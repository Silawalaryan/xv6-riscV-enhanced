#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  printf("My PID    : %d\n", getpid());
  printf("My PPID   : %d\n", getppid());
  exit(0);
}