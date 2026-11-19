// schedtrace: record which process ran on which CPU, and when.
//
//   schedtrace <command> [args...]      run command, then draw a Gantt chart
//   schedtrace -c <command> [args...]   dump the raw trace as CSV instead
//                                        (for tools/gantt.py on the host)
//
// The kernel logs an event every time a process is dispatched or stops
// running (preempted / blocked / exited) and every scheduler switch. In the
// chart, each row is a process and each column a slice of time:
//   '#' ran for most of that slice, '+' about half, '-' briefly, ' ' not at all.
// The per-process summary shows how many separate runs it had and their
// average length, which is the effective time slice.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"

#define COLS     60
#define MAXROWS  24
#define MAXCPU    8

struct row {
  int pid;
  char name[16];
  uint total;          // centiticks run
  uint runs;           // number of dispatches
  uint bucket[COLS];   // centiticks run in each column
};

static struct row rows[MAXROWS];
static int nrows;
static char sched_marks[COLS];
static char *policy_names[] = { "rr", "fifo", "cfs" };

static int
row_index(struct trace_event *e)
{
  int i;

  for(i = 0; i < nrows; i++)
    if(rows[i].pid == e->pid){
      // keep the latest name (it changes at exec)
      memmove(rows[i].name, e->name, sizeof(rows[i].name));
      return i;
    }
  if(nrows == MAXROWS)
    return -1;
  rows[nrows].pid = e->pid;
  memmove(rows[nrows].name, e->name, sizeof(rows[nrows].name));
  return nrows++;
}

static void
add_interval(int r, uint a, uint b, uint bs)
{
  uint col, lo, hi;

  if(r < 0 || b <= a)
    return;
  rows[r].total += b - a;
  for(col = a / bs; col <= (b - 1) / bs && col < COLS; col++){
    lo = col * bs > a ? col * bs : a;
    hi = (col + 1) * bs < b ? (col + 1) * bs : b;
    rows[r].bucket[col] += hi - lo;
  }
}

// "pid name" padded to 14 columns.
static void
print_label(int r)
{
  int i;

  printf(1, "%d", rows[r].pid);
  for(i = rows[r].pid < 10 ? 1 : rows[r].pid < 100 ? 2 : 3; i < 4; i++)
    printf(1, " ");
  for(i = 0; i < 10 && rows[r].name[i]; i++)
    printf(1, "%c", rows[r].name[i]);
  for(; i < 10; i++)
    printf(1, " ");
}

// Print centiticks as ticks with one decimal.
static void
print_ticks(uint ct)
{
  printf(1, "%d.%d", ct / 100, (ct % 100) / 10);
}

static void
chart(struct trace_event *ev, int n, int dropped)
{
  int running[MAXCPU], i, r, c;
  uint start[MAXCPU], tend, bs;

  if(n == 0){
    printf(1, "schedtrace: no events recorded\n");
    return;
  }
  tend = ev[n-1].t;
  bs = (tend + COLS - 1) / COLS;
  if(bs == 0)
    bs = 1;
  for(i = 0; i < MAXCPU; i++)
    running[i] = -2;   // nothing running
  for(i = 0; i < COLS; i++)
    sched_marks[i] = ' ';

  for(i = 0; i < n; i++){
    struct trace_event *e = &ev[i];
    int cpu = e->cpu < MAXCPU ? e->cpu : 0;
    if(e->type == TR_SCHED){
      c = e->t / bs < COLS ? e->t / bs : COLS - 1;
      sched_marks[c] = e->arg == SCHED_RR ? 'R' : e->arg == SCHED_FCFS ? 'F' : 'C';
      continue;
    }
    r = row_index(e);
    if(e->type == TR_RUN){
      running[cpu] = r;
      start[cpu] = e->t;
      if(r >= 0)
        rows[r].runs++;
    } else if(e->type == TR_STOP && running[cpu] == r){
      add_interval(r, start[cpu], e->t, bs);
      running[cpu] = -2;
    }
  }
  for(i = 0; i < MAXCPU; i++)       // still running when tracing stopped
    if(running[i] != -2)
      add_interval(running[i], start[i], tend, bs);

  printf(1, "\ntrace: %d events over ", n);
  print_ticks(tend);
  printf(1, " ticks; one column = ");
  print_ticks(bs);
  printf(1, " ticks%s\n", dropped ? " (buffer full: trace truncated)" : "");
  printf(1, "scheduler switches: R = rr, F = fifo, C = cfs\n\n");

  printf(1, "pid name      |");
  for(c = 0; c < COLS; c++)
    printf(1, "%c", sched_marks[c]);
  printf(1, "|\n");
  for(r = 0; r < nrows; r++){
    if(rows[r].runs == 0)
      continue;
    print_label(r);
    printf(1, "|");
    for(c = 0; c < COLS; c++){
      uint v = rows[r].bucket[c];
      printf(1, "%c", v * 4 >= bs * 3 ? '#' : v * 4 >= bs ? '+' : v ? '-' : ' ');
    }
    printf(1, "|\n");
  }

  printf(1, "\npid name        ran(ticks)  runs  avg run (= effective slice)\n");
  for(r = 0; r < nrows; r++){
    if(rows[r].runs == 0)
      continue;
    print_label(r);
    printf(1, "  ");
    print_ticks(rows[r].total);
    printf(1, "\t   %d\t ", rows[r].runs);
    print_ticks(rows[r].total / rows[r].runs);
    printf(1, "\n");
  }
}

int
main(int argc, char *argv[])
{
  struct trace_event *ev;
  int csv = 0, first = 1, pid, n, i, dropped;

  if(argc > 1 && strcmp(argv[1], "-c") == 0){
    csv = 1;
    first = 2;
  }
  if(first >= argc){
    printf(2, "usage: schedtrace [-c] <command> [args...]\n");
    exit();
  }

  tracectl(TRACE_START);
  pid = fork();
  if(pid == 0){
    exec(argv[first], argv + first);
    printf(2, "schedtrace: exec %s failed\n", argv[first]);
    exit();
  }
  if(pid > 0)
    wait();
  n = tracectl(TRACE_STOP);
  dropped = tracectl(TRACE_DROPPED);

  ev = malloc(n > 0 ? n * sizeof(*ev) : 1);
  if(ev == 0){
    printf(2, "schedtrace: out of memory\n");
    exit();
  }
  n = traceread(ev, 0, n);

  if(csv){
    struct schedstat st;
    schedstat(SS_GET, &st);
    printf(1, "TRACE-BEGIN events=%d dropped=%d ncpu=%d\n", n, dropped, st.ncpu);
    for(i = 0; i < n; i++)
      printf(1, "E,%d,%d,%d,%d,%d,%s\n", ev[i].t, ev[i].cpu, ev[i].pid,
             ev[i].type, ev[i].arg, ev[i].pid < 0 ?
             policy_names[ev[i].arg < 3 ? ev[i].arg : 0] : ev[i].name);
    printf(1, "TRACE-END\n");
  } else {
    chart(ev, n, dropped);
  }
  exit();
}
