// renice: change the nice value of running processes.
//   renice <value> <pid> [pid...]
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
  int value, pid, i;

  if(argc < 3 || parseint(argv[1], &value) < 0){
    printf(2, "usage: renice <%d..%d> <pid> [pid...]\n", NICE_MIN, NICE_MAX);
    exit();
  }
  if(value < NICE_MIN || value > NICE_MAX){
    printf(2, "renice: value must be between %d and %d\n", NICE_MIN, NICE_MAX);
    exit();
  }
  for(i = 2; i < argc; i++){
    if(parseint(argv[i], &pid) < 0 || renice(pid, value) < 0)
      printf(2, "renice: no such process: %s\n", argv[i]);
    else
      printf(1, "%d: nice set to %d\n", pid, value);
  }
  exit();
}
