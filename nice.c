// nice: run a command with a given nice value.
//   nice <value> <command> [args...]
// The nice value survives exec, so the command (and any children it
// forks) runs at that value. Only affects scheduling under CFS.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"

static int
parseint(char *s, int *out)
{
  int neg = 0, v = 0;

  if(*s == '-' || *s == '+'){
    neg = (*s == '-');
    s++;
  }
  if(*s == 0)
    return -1;
  for(; *s; s++){
    if(*s < '0' || *s > '9')
      return -1;
    v = v * 10 + (*s - '0');
  }
  *out = neg ? -v : v;
  return 0;
}

int
main(int argc, char *argv[])
{
  int value;

  if(argc < 3 || parseint(argv[1], &value) < 0){
    printf(2, "usage: nice <%d..%d> <command> [args...]\n", NICE_MIN, NICE_MAX);
    exit();
  }
  if(setnice(value) < 0){
    printf(2, "nice: value must be between %d and %d\n", NICE_MIN, NICE_MAX);
    exit();
  }
  exec(argv[2], argv + 2);
  printf(2, "nice: exec %s failed\n", argv[2]);
  exit();
}
