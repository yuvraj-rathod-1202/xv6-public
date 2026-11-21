// schedbench: compare RR, FIFO and CFS on the same workloads.
//
//   schedbench [cpu|io|mix|nice|all] [rr|fifo|cfs]
//     (default: all workloads, each under all three schedulers; naming a
//      scheduler runs only that one, e.g. schedtrace schedbench cpu fifo)
//
// cpu, io, mix: run the workload once under each scheduler and report, per
//   process and on average (all in timer ticks):
//     turnaround = exit - creation
//     waiting    = time spent RUNNABLE (ready but not running)
//     response   = first time on a CPU - creation
//   plus the makespan (first creation to last exit).
// nice: under CFS, run CPU-bound processes at different nice values for a
//   fixed time and compare each one's share of CPU time with the share
//   its weight entitles it to.
//
// Results are clearest on one CPU: make qemu-nox CPUS=1
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"

#define CPU 0   // burn `amount` ticks of CPU work, then exit
#define IO  1   // `amount` times: a quarter tick of work, then sleep 2 ticks

struct job {
  int kind;
  int amount;
  int nice;
};

struct result {
  int pid;
  struct waitinfo wi;
};

// A long job first, then short ones: shows FIFO's convoy effect.
static struct job cpu_jobs[] = {
  {CPU, 100, 0}, {CPU, 20, 0}, {CPU, 20, 0}, {CPU, 20, 0}, {CPU, 20, 0},
};
static struct job io_jobs[] = {
  {IO, 15, 0}, {IO, 15, 0}, {IO, 15, 0}, {IO, 15, 0},
};
static struct job mix_jobs[] = {
  {CPU, 60, 0}, {CPU, 60, 0}, {IO, 15, 0}, {IO, 15, 0},
};

#define NJOBS(a) (sizeof(a) / sizeof((a)[0]))
#define MAXJOBS 8

static int sched_ids[] = { SCHED_RR, SCHED_FCFS, SCHED_CFS };
static char *sched_names[] = { "RR", "FIFO", "CFS" };

static volatile uint sink;
static uint loops_per_tick;

static void
burn(uint n)
{
  uint i;

  for(i = 0; i < n; i++)
    sink += i;
}

// Measure how many burn() iterations fit in one timer tick. Times one large
// uninterrupted burn (no syscalls inside the timed work), doubling it until
// it spans at least 20 ticks, so the error is under 5%.
static void
calibrate(void)
{
  uint n = 1000000, t0, t;

  for(;;){
    t0 = uptime();
    while(uptime() == t0)
      ;
    t0 = uptime();
    burn(n);
    t = uptime() - t0;
    if(t >= 20 || n >= 0x40000000){
      loops_per_tick = t ? n / t : n;
      return;
    }
    n *= 2;
  }
}

static void
run_job(struct job *j)
{
  int i;

  setnice(j->nice);
  if(j->kind == CPU){
    for(i = 0; i < j->amount; i++)
      burn(loops_per_tick);
  } else {
    for(i = 0; i < j->amount; i++){
      burn(loops_per_tick / 4);
      sleep(2);
    }
  }
  exit();
}

// xv6's printf has no field widths, so format numbers by hand.
// Append the decimal digits of v to buf at *n.
static void
put_digits(char *buf, int *n, uint v)
{
  char tmp[12];
  int i = 0;

  do {
    tmp[i++] = '0' + v % 10;
    v /= 10;
  } while(v);
  while(i)
    buf[(*n)++] = tmp[--i];
}

static void
print_padded(char *buf, int n, int w)
{
  buf[n] = 0;
  for(; n < w; n++)
    printf(1, " ");
  printf(1, "%s", buf);
}

// Print integer v right-aligned in width w.
static void
print_int(int v, int w)
{
  char buf[16];
  int n = 0;

  if(v < 0){
    buf[n++] = '-';
    v = -v;
  }
  put_digits(buf, &n, v);
  print_padded(buf, n, w);
}

// Print v/10 with one decimal place, right-aligned in width w.
static void
print_tenths(int v, int w)
{
  char buf[16];
  int n = 0;

  if(v < 0){
    buf[n++] = '-';
    v = -v;
  }
  put_digits(buf, &n, v / 10);
  buf[n++] = '.';
  buf[n++] = '0' + v % 10;
  print_padded(buf, n, w);
}

struct summary {
  int turnaround10, waiting10, response10, makespan;
};

static struct summary
run_workload(int sched, struct job *jobs, int n, int verbose)
{
  struct result res[MAXJOBS];
  struct waitinfo wi;
  struct summary s;
  int i, k, pid, tsum = 0, wsum = 0, rsum = 0;
  uint first_c = 0xffffffff, last_e = 0;

  setscheduler(sched);
  sleep(2);

  for(i = 0; i < n; i++){
    pid = fork();
    if(pid == 0)
      run_job(&jobs[i]);
    res[i].pid = pid;
  }
  for(k = 0; k < n; k++){
    pid = waitx(&wi);
    for(i = 0; i < n; i++)
      if(res[i].pid == pid)
        res[i].wi = wi;
  }

  if(verbose)
    printf(1, "  pid  kind  amount  turnaround   waiting  response   cpu time\n");
  for(i = 0; i < n; i++){
    struct waitinfo *w = &res[i].wi;
    int t = w->etime - w->ctime;
    int r = w->first_run - w->ctime;
    tsum += t;
    wsum += w->wtime;
    rsum += r;
    if(w->ctime < first_c)
      first_c = w->ctime;
    if(w->etime > last_e)
      last_e = w->etime;
    if(verbose){
      printf(1, "  ");
      print_int(res[i].pid, 3); 
      printf(1, "  %s  ", jobs[i].kind == CPU ? "cpu" : "io ");
      print_int(jobs[i].amount, 6);
      print_int(t, 10);
      print_int(w->wtime, 8);
      print_int(r, 8);
      print_int(w->rtime, 8);
      printf(1, "\n");
    }
  }
  s.turnaround10 = tsum * 10 / n;
  s.waiting10 = wsum * 10 / n;
  s.response10 = rsum * 10 / n;
  s.makespan = last_e - first_c;
  return s;
}

static void
compare(char *name, struct job *jobs, int n, int only)
{
  struct summary s[3];
  int i;

  printf(1, "\n=== workload: %s (%d processes) ===\n", name, n);
  for(i = 0; i < 3; i++){
    if(only >= 0 && sched_ids[i] != only)
      continue;
    printf(1, "-- %s --\n", sched_names[i]);
    s[i] = run_workload(sched_ids[i], jobs, n, 1);
  }
  printf(1, "\nsummary (%s), averages in ticks:\n", name);
  printf(1, "  sched   turnaround   waiting  response  makespan\n");
  for(i = 0; i < 3; i++){
    if(only >= 0 && sched_ids[i] != only)
      continue;
    int pad = 6 - strlen(sched_names[i]);
    printf(1, "  %s", sched_names[i]);
    while(pad-- > 0)
      printf(1, " ");
    print_tenths(s[i].turnaround10, 10);
    print_tenths(s[i].waiting10, 10);
    print_tenths(s[i].response10, 10);
    print_int(s[i].makespan, 8);
    printf(1, "\n");
  }
}

// CFS weight table (same as the kernel's), indexed by nice - NICE_MIN.
static uint weights[] = {
  88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
   9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
   1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,    87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static void
nice_test(void)
{
  int nices[] = { -5, 0, 5 };
  int n = 3, i, k, pid, pids[3];
  uint rt[3], total = 0, wsum = 0, stop;
  struct waitinfo wi;

  printf(1, "\n=== nice: CPU share under CFS, 300 ticks ===\n");
  setscheduler(SCHED_CFS);
  sleep(2);
  stop = uptime() + 300;
  for(i = 0; i < n; i++){
    pid = fork();
    if(pid == 0){
      setnice(nices[i]);
      while(uptime() < stop)
        burn(1000);
      exit();
    }
    pids[i] = pid;
  }
  for(k = 0; k < n; k++){
    pid = waitx(&wi);
    for(i = 0; i < n; i++)
      if(pids[i] == pid)
        rt[i] = wi.rtime;
  }
  for(i = 0; i < n; i++){
    total += rt[i];
    wsum += weights[nices[i] - NICE_MIN];
  }
  if(total == 0)
    total = 1;
  printf(1, "  nice  weight  run ticks   share  expected\n");
  for(i = 0; i < n; i++){
    uint w = weights[nices[i] - NICE_MIN];
    printf(1, "  ");
    print_int(nices[i], 3);
    print_int(w, 7);
    print_int(rt[i], 9);
    print_tenths(rt[i] * 1000 / total, 7);
    printf(1, "%%");
    print_tenths(w * 1000 / wsum, 8);
    printf(1, "%%\n");
  }
  printf(1, "  (shares match weights exactly only on one CPU)\n");
}

int
main(int argc, char *argv[])
{
  char *what = argc > 1 ? argv[1] : "all";
  int all = strcmp(what, "all") == 0;
  int orig = getscheduler();
  int only = -1;   // run under one scheduler only (e.g. under schedtrace)

  if(argc > 2){
    if(strcmp(argv[2], "rr") == 0)
      only = SCHED_RR;
    else if(strcmp(argv[2], "fifo") == 0 || strcmp(argv[2], "fcfs") == 0)
      only = SCHED_FCFS;
    else if(strcmp(argv[2], "cfs") == 0)
      only = SCHED_CFS;
  }
  if((!all && strcmp(what, "cpu") && strcmp(what, "io") &&
      strcmp(what, "mix") && strcmp(what, "nice")) ||
     (argc > 2 && only < 0)){
    printf(2, "usage: schedbench [cpu|io|mix|nice|all] [rr|fifo|cfs]\n");
    exit();
  }

  calibrate();
  printf(1, "schedbench: %d loop iterations per tick\n", loops_per_tick);
  tracectl(TRACE_RESTART);   // under schedtrace: leave calibration out of the trace

  if(all || strcmp(what, "cpu") == 0)
    compare("cpu", cpu_jobs, NJOBS(cpu_jobs), only);
  if(all || strcmp(what, "io") == 0)
    compare("io", io_jobs, NJOBS(io_jobs), only);
  if(all || strcmp(what, "mix") == 0)
    compare("mix", mix_jobs, NJOBS(mix_jobs), only);
  if(all || strcmp(what, "nice") == 0)
    nice_test();

  setscheduler(orig);
  exit();
}
