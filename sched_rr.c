// Round Robin and FIFO scheduling classes.
//
// Both keep RUNNABLE processes in a doubly linked queue (O(1) enqueue,
// dequeue and pick). They differ only in preemption:
//   RR   - the timer preempts the running process every tick (quantum = 1
//          tick); a preempted process goes to the tail, so every runnable
//          process gets one quantum per round.
//   FIFO - never preempted by the timer: a process runs until it blocks or
//          exits, and a process that wakes up joins the tail. "First ready,
//          first run", like Linux SCHED_FIFO within one priority.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "sched_class.h"

struct runlist {
  struct proc *head;
  struct proc *tail;
  int nr;
};

static struct runlist rr_queue;
static struct runlist fifo_queue;

static void
list_push(struct runlist *q, struct proc *p)
{
  p->rq_next = 0;
  p->rq_prev = q->tail;
  if(q->tail)
    q->tail->rq_next = p;
  else
    q->head = p;
  q->tail = p;
  q->nr++;
}

static void
list_del(struct runlist *q, struct proc *p)
{
  if(p->rq_prev)
    p->rq_prev->rq_next = p->rq_next;
  else
    q->head = p->rq_next;
  if(p->rq_next)
    p->rq_next->rq_prev = p->rq_prev;
  else
    q->tail = p->rq_prev;
  p->rq_next = 0;
  p->rq_prev = 0;
  q->nr--;
}

static struct proc*
list_pop(struct runlist *q)
{
  struct proc *p = q->head;

  if(p)
    list_del(q, p);
  return p;
}

// Walk the list checking links, length and that every member is RUNNABLE.
static int
list_check(struct runlist *q, char *name)
{
  struct proc *p, *prev = 0;
  int n = 0;

  for(p = q->head; p; prev = p, p = p->rq_next){
    if(p->rq_prev != prev){
      cprintf("%s queue: pid %d has a bad prev link\n", name, p->pid);
      return -1;
    }
    if(p->state != RUNNABLE){
      cprintf("%s queue: pid %d is queued but in state %d\n",
              name, p->pid, p->state);
      return -1;
    }
    if(++n > NPROC){
      cprintf("%s queue: cycle in list\n", name);
      return -1;
    }
  }
  if(q->tail != prev || n != q->nr){
    cprintf("%s queue: tail/length mismatch (walked %d, nr %d)\n",
            name, n, q->nr);
    return -1;
  }
  return n;
}

// ---- Round Robin ----

static void rr_enqueue(struct proc *p, int wakeup) { list_push(&rr_queue, p); }
static void rr_dequeue(struct proc *p)             { list_del(&rr_queue, p); }
static struct proc* rr_pick_next(void)             { return list_pop(&rr_queue); }
static void rr_set_curr(struct proc *p)            { }
static void rr_put_prev(struct proc *p, uint64 c)  { }
static int  rr_tick(struct proc *p)                { return 1; }
static void rr_switched_to(void)                   { }
static int  rr_check(void)                         { return list_check(&rr_queue, "rr"); }

struct sched_class rr_sched_class = {
  .name = "rr",
  .enqueue = rr_enqueue,
  .dequeue = rr_dequeue,
  .pick_next = rr_pick_next,
  .set_curr = rr_set_curr,
  .put_prev = rr_put_prev,
  .tick = rr_tick,
  .switched_to = rr_switched_to,
  .check = rr_check,
};

// ---- FIFO ----

static void fifo_enqueue(struct proc *p, int wakeup) { list_push(&fifo_queue, p); }
static void fifo_dequeue(struct proc *p)             { list_del(&fifo_queue, p); }
static struct proc* fifo_pick_next(void)             { return list_pop(&fifo_queue); }
static int  fifo_tick(struct proc *p)                { return 0; }
static int  fifo_check(void)                         { return list_check(&fifo_queue, "fifo"); }

struct sched_class fifo_sched_class = {
  .name = "fifo",
  .enqueue = fifo_enqueue,
  .dequeue = fifo_dequeue,
  .pick_next = fifo_pick_next,
  .set_curr = rr_set_curr,
  .put_prev = rr_put_prev,
  .tick = fifo_tick,
  .switched_to = rr_switched_to,
  .check = fifo_check,
};
