// schedoverhead: how much does each scheduling decision cost, and how does
// it grow with the number of runnable processes?
//
//   schedoverhead [maxN]      (default 240; NPROC is 256)
//
// For N = 1, 2, 4, ... maxN CPU-bound processes, under RR and under CFS,
// measure (in TSC cycles, averaged over a 50-tick window):
//   pick    - choosing the next process and removing it from the run queue
//             (RR: unlink the list head, O(1); CFS: leftmost node of the
//             red-black tree + erase, O(log n))
//   enqueue - putting a process back (RR: append, O(1); CFS: tree insert
//             with rebalancing, O(log n))
//   scan    - CFS only: what a pick would cost WITHOUT the tree, i.e. a
//             linear scan of the process table for the smallest vruntime
//             (O(NPROC)). The kernel performs this scan alongside every
//             real pick and also checks that both agree.
// Run with the deterministic clock for stable numbers:
//   make qemu-bench CPUS=1
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"
#include "param.h"

static int pids[NPROC];

// 64-by-32 division without libgcc (see kernel sched.c).
static uint
div64(uint64 n, uint d)
{
  uint hi = n >> 32, lo = (uint)n, r, q;

  if(d == 0)
    return 0;
  if(hi >= d)
    return 0xffffffff;   // would overflow; never happens for these averages
  r = hi;
  asm("divl %4" : "=a" (q), "=d" (r) : "0" (lo), "1" (r), "rm" (d));
  return q;
}

static void
pad(int v, int w)
{
  int digits = 1, t = v;

  while(t >= 10){
    t /= 10;
    digits++;
  }
  while(digits++ < w)
    printf(1, " ");
  printf(1, "%d", v);
}

struct result {
  uint pick, enq, scan, picks;
};

static struct result
measure(int policy, int n)
{
  struct schedstat st;
  struct result r;
  int i;

  setscheduler(policy);
  schedstat(policy == SCHED_CFS ? SS_SCAN_ON : SS_SCAN_OFF, &st);
  for(i = 0; i < n; i++){
    pids[i] = fork();
    if(pids[i] == 0)
      for(;;)
        ;
  }
  sleep(5);                    // let the queue reach steady state
  schedstat(SS_RESET, &st);
  sleep(50);
  schedstat(SS_GET, &st);
  for(i = 0; i < n; i++)
    if(pids[i] > 0)
      kill(pids[i]);
  for(i = 0; i < n; i++)
    if(pids[i] > 0)
      wait();
  schedstat(SS_SCAN_OFF, &st);

  r.pick = div64(st.pick_cycles, st.picks);
  r.enq = div64(st.enqueue_cycles, st.enqueues);
  r.scan = div64(st.scan_cycles, st.scans);
  r.picks = st.picks;
  return r;
}

int
main(int argc, char *argv[])
{
  int maxn = argc > 1 ? atoi(argv[1]) : 240;
  int ns[] = { 1, 2, 4, 8, 16, 32, 64, 128, 240 };
  int orig = getscheduler(), k;
  struct schedstat st;

  if(maxn > NPROC - 16)
    maxn = NPROC - 16;
  schedstat(SS_GET, &st);
  printf(1, "schedoverhead: cycles per operation (TSC), %d cpu(s), NPROC %d\n",
         st.ncpu, NPROC);
  printf(1, "     N |   RR pick    RR enq |  CFS pick   CFS enq  CFS scan | scan/pick\n");
  printf(1, "       |    O(1)      O(1)   | O(log n)  O(log n)  O(NPROC) |\n");
  for(k = 0; k < sizeof(ns) / sizeof(ns[0]) && ns[k] <= maxn; k++){
    struct result rr = measure(SCHED_RR, ns[k]);
    struct result cfs = measure(SCHED_CFS, ns[k]);
    pad(ns[k], 6);
    printf(1, " |");
    pad(rr.pick, 9);
    pad(rr.enq, 10);
    printf(1, " |");
    pad(cfs.pick, 9);
    pad(cfs.enq, 10);
    pad(cfs.scan, 10);
    printf(1, " |");
    if(cfs.pick){
      uint r10 = cfs.scan * 10 / cfs.pick;
      pad(r10 / 10, 6);
      printf(1, ".%dx", r10 % 10);
    }
    printf(1, "\n");
  }
  setscheduler(orig);
  exit();
}
