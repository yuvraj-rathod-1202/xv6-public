#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Set the scheduler type
int
sys_setscheduler(void)
{
  int scheduler;

  if(argint(0, &scheduler) < 0)
    return -1;

  return set_scheduler(scheduler);
}

// get the current scheduler type
int
sys_getscheduler(void)
{
  return get_scheduler();
}

int
sys_setnice(void)
{
  int nice_value;

  if(argint(0, &nice_value) < 0)
    return -1;
  if(nice_value < NICE_MIN || nice_value > NICE_MAX)
    return -1;

  myproc()->nice_value = nice_value;
  return 0;
}

int
sys_getnice(void)
{
  return myproc()->nice_value;
}

// getpinfo(struct pinfo *buf, int max): snapshot up to max processes.
int
sys_getpinfo(void)
{
  struct pinfo *buf;
  int max;

  if(argint(1, &max) < 0 || max < 0 || max > NPROC)
    return -1;
  if(argptr(0, (char**)&buf, max * sizeof(struct pinfo)) < 0)
    return -1;
  return getpinfo(buf, max);
}

// waitx(struct waitinfo *wi): wait() that also reports the child's timings.
int
sys_waitx(void)
{
  struct waitinfo *wi;

  if(argptr(0, (char**)&wi, sizeof(*wi)) < 0)
    return -1;
  return waitx(wi);
}

// renice(int pid, int nice): change another process's nice value.
int
sys_renice(void)
{
  int pid, nice;

  if(argint(0, &pid) < 0 || argint(1, &nice) < 0)
    return -1;
  return renice(pid, nice);
}

// schedtune(int latency, int min_granularity): CFS tunables in ticks;
// 0 leaves a value unchanged.
int
sys_schedtune(void)
{
  int latency, gran;

  if(argint(0, &latency) < 0 || argint(1, &gran) < 0)
    return -1;
  return schedtune(latency, gran);
}

// schedstat(int cmd, struct schedstat *st): read/reset scheduler counters.
int
sys_schedstat(void)
{
  int cmd;
  struct schedstat *st;

  if(argint(0, &cmd) < 0 || argptr(1, (char**)&st, sizeof(*st)) < 0)
    return -1;
  return schedstat(cmd, st);
}

// tracectl(int cmd): start/stop the scheduling trace.
int
sys_tracectl(void)
{
  int cmd;

  if(argint(0, &cmd) < 0)
    return -1;
  return tracectl(cmd);
}

// traceread(struct trace_event *buf, int start, int n): copy trace events.
int
sys_traceread(void)
{
  struct trace_event *buf;
  int start, n;

  if(argint(1, &start) < 0 || argint(2, &n) < 0 || n < 0 || n > TRACE_MAX)
    return -1;
  if(argptr(0, (char**)&buf, n * sizeof(struct trace_event)) < 0)
    return -1;
  return traceread(buf, start, n);
}
