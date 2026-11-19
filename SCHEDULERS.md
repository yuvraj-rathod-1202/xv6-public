# Pluggable Schedulers for xv6: RR, FIFO and CFS

This fork of xv6 (x86) replaces the fixed round-robin scheduler with three
schedulers that can be switched **at runtime, without rebooting**:

| Scheduler | `schedctl` name | Preemptive | Picks next | Cost per pick |
|---|---|---|---|---|
| Round Robin | `rr` | yes, every tick | next RUNNABLE slot after the last one picked | O(NPROC) |
| FIFO | `fcfs` | **no** | the process that became RUNNABLE earliest | O(NPROC) |
| Completely Fair Scheduler | `cfs` | yes, every tick | smallest virtual runtime (leftmost red-black tree node) | O(log n) |

CFS supports **nice values** (-20 to 19) using the Linux weight table, so a
process's share of the CPU is proportional to its weight.

The system boots with RR, so stock xv6 behaviour is unchanged until you switch.

---

## 1. Quick start

```sh
make qemu-nox                     # 2 CPUs (default)
make qemu-nox CPUS=1              # 1 CPU: scheduling effects are easiest to see
```

For **benchmarks**, also make guest time deterministic (see section 6.1):

```sh
make qemu-nox CPUS=1 QEMUEXTRA="-icount shift=3,sleep=off"
```

Inside xv6:

```
$ schedctl get              # show the active scheduler
rr
$ schedctl cfs              # switch to CFS (also: rr, fcfs)
Scheduler set to cfs
$ nice 5 ps                 # run a command at nice 5
$ renice -3 7               # change the nice value of pid 7
$ ps                        # processes with nice, vruntime, run/wait/sleep time
$ schedtest                 # correctness tests (expects: ALL TESTS PASSED)
$ schedbench                # compare RR / FIFO / CFS on several workloads
$ schedbench nice           # CPU share vs nice value under CFS
```

### User programs

| Program | Purpose |
|---|---|
| `schedctl get\|rr\|fcfs\|cfs` | Show or change the active scheduler. |
| `nice <n> <cmd> [args]` | Run `cmd` at nice `n`. The value survives `exec` and is inherited by children. |
| `renice <n> <pid>...` | Change the nice value of running processes. |
| `ps` | Show the scheduler and, per process: PID, PPID, state, nice, vruntime, CPU / wait / sleep ticks, name. |
| `schedbench [cpu\|io\|mix\|nice\|all]` | Run workloads under each scheduler and report turnaround, waiting and response time. |
| `schedtest` | Automated tests for runtime switching, CFS fairness, FIFO order and nice handling. |

### System calls

| # | Call | Description |
|---|---|---|
| 22 | `int setscheduler(int s)` | `SCHED_RR`=0, `SCHED_FCFS`=1, `SCHED_CFS`=2. Returns -1 if `s` is invalid. |
| 23 | `int getscheduler(void)` | Returns the active scheduler. |
| 24 | `int setnice(int n)` | Set the caller's nice value (-20..19). |
| 25 | `int getnice(void)` | Return the caller's nice value. |
| 26 | `int getpinfo(struct pinfo *buf, int max)` | Copy up to `max` process records; returns the count. |
| 27 | `int waitx(struct waitinfo *wi)` | Like `wait()`, and also fills in the child's timing statistics. |
| 28 | `int renice(int pid, int n)` | Set another process's nice value. |

`struct pinfo`, `struct waitinfo` and the `SCHED_*` and `NICE_*` constants are in
**`sched.h`**, which is shared by the kernel and user programs.

---

## 2. Architecture

### 2.1 One run queue that all three policies read

Switching scheduler must not lose, duplicate or strand a process. So every
RUNNABLE process is kept in **every** scheduler's data structure all the time,
whichever scheduler is active:

* it is a node in the CFS **red-black tree** (`ptable.cfs_root`), and
* it has a **FIFO sequence number** (`p->fifo_seq`), stamped when it became
  RUNNABLE.

RR needs no extra structure; it scans `ptable` from a rotating cursor.

Every transition into RUNNABLE goes through two helpers in `proc.c`:

| Helper | Used by | Effect |
|---|---|---|
| `make_runnable(p)` | `userinit`, `fork`, `wakeup1`, `kill` | lifts `vruntime` to `min_vruntime`, sets RUNNABLE, calls `runqueue_add` |
| `runqueue_add(p)` | `sched()` on yield, `make_runnable` | stamps `fifo_seq`, inserts into the tree |

When `scheduler()` dispatches a process it removes it from the tree
(`rb_delete`). So the invariant is: **a process is in the tree if and only if it
is RUNNABLE.** `rb_insert` and `rb_delete` panic if this is ever violated.

### 2.2 Choosing and dispatching

```
scheduler():                                   (each CPU, forever)
  acquire(ptable.lock)
  p = select_process()        -> scheduler_RR / scheduler_FCFS / scheduler_CFS
  if p:
      rb_delete(p); p->state = RUNNING
      p->run_start = rdtsc(); p->cfs_accounting = (current_scheduler == CFS)
      swtch(...)              -> p runs until it yields, sleeps or exits
  release(ptable.lock)

sched():                                       (p is giving up the CPU)
  elapsed = rdtsc() - p->run_start;  p->exec_time += elapsed
  if p->cfs_accounting:  p->vruntime += elapsed * 1024 / weight(nice)
  if p is RUNNABLE (preempted): runqueue_add(p)
  if CFS: advance min_vruntime
```

`current_scheduler` is read under `ptable.lock`, so a switch takes effect at the
next scheduling decision on every CPU. A process that is already running is
never interrupted by a switch.

### 2.3 Switching at runtime

`setscheduler(s)` → `set_scheduler()` (`proc.c`), under `ptable.lock`:

1. Validate `s`.
2. If switching **into CFS from another scheduler**, call `cfs_reset_vruntime()`
   (section 3.3).
3. Set `current_scheduler = s`.

Preemption is decided in `trap.c`: on a timer tick the running process yields
unless the active scheduler is FIFO.

---

## 3. The schedulers

### 3.1 Round Robin

`scheduler_RR()` scans `ptable` circularly starting at `rr_next` and picks the
first RUNNABLE process; `rr_next` is then set to the slot after it. Each process
runs for at most one timer tick (the time slice) before `trap()` preempts it.

> Keeping the cursor matters. The original version restarted the scan at slot
> 0 on every decision, so processes in low slots starved those in higher slots.

### 3.2 FIFO (first ready, first run)

* **Order:** `scheduler_FCFS()` picks the RUNNABLE process with the smallest
  `fifo_seq`. `fifo_seq` is a 64-bit counter stamped each time a process joins
  the run queue (fork, wakeup, kill, preemption). Ties are impossible, and reused
  `ptable` slots don't affect the order.
* **Non-preemptive:** the timer does not force a yield while FIFO is active, so a
  process runs until it blocks or exits.
* **Blocking:** a process that sleeps goes to the **back** of the queue when it
  wakes, like Linux `SCHED_FIFO`.

> The original version ordered by creation time in ticks. Processes created in
> the same tick were therefore ordered by slot index, and the timer still
> preempted the running process every tick (only to pick it again).

### 3.3 Completely Fair Scheduler

**Virtual runtime.** Each process has `vruntime`, its CPU time scaled by its
weight:

```
vruntime += cycles_run × NICE_0_LOAD / weight(nice)          NICE_0_LOAD = 1024
```

The scheduler always runs the process with the **smallest** `vruntime`. A
process with twice the weight accumulates `vruntime` half as fast, so it gets
twice the CPU.

**Weights.** These are the Linux `sched_prio_to_weight` table (nice 0 = 1024,
each nice step ≈ ×1.25):

```
nice  -20: 88761   -10: 9548   -5: 3121   0: 1024   5: 335   10: 110   19: 15
```

**Red-black tree.** RUNNABLE processes are kept in a red-black tree embedded in
`struct proc` (`rb_left`, `rb_right`, `rb_parent`, `rb_color`). It is ordered by
(`vruntime`, `pid`); `pid` breaks ties so the order is total. Picking the next
process means taking the leftmost node; insert and delete are O(log n) and
rebalance as in CLRS. A process's key never changes while it is in the tree:
`vruntime` only changes while it is RUNNING, SLEEPING or being created.

**Measuring run time in CPU cycles.** Run time is read from the time-stamp
counter (`rdtsc`, `x86.h`), not timer ticks. With ticks, a process that blocked
before the next tick was charged nothing. A process that always slept just
before a tick boundary could therefore run for free and take more than its
share. A process starts and stops a run on the same CPU, so differences between
CPUs' counters cancel out.

**Fixed-point arithmetic.** The xv6 kernel is not linked with libgcc, so a
64-bit `/` fails to link (`__udivdi3`). The charge is computed instead as:

```
factor    = (1024 << 16) / weight           // 32-bit division, done once
vruntime += (cycles * factor) >> 16         // 64-bit multiply and shift are inline
```

At nice 0 one cycle adds exactly 1. Even at nice −20 the factor (756) keeps
three significant digits. (The original code divided `1 tick × 1024 / weight`
in integers, which is 0 for every negative nice value. Those processes' vruntime
never grew, so they starved everyone else.)

**`min_vruntime` and placement.** `ptable.min_vruntime` is a floor that never
decreases. After each charge it is advanced to the leftmost `vruntime` in the
tree (O(log n)). When a process wakes, is killed while asleep, or is forked, its
`vruntime` is raised to at least `min_vruntime` (`make_runnable`). Without this,
a process that slept for a long time would return with a very small `vruntime`
and hold the CPU until it caught up. A forked child starts at
`max(parent's vruntime, min_vruntime)`.

**Normalizing on a switch into CFS.** While RR or FIFO is active, `vruntime`
doesn't advance. Values left over from an earlier CFS period are then stale
relative to each other: a process that slept through that period could come
back far behind and take over the CPU. So when the system switches *into* CFS,
`cfs_reset_vruntime()` removes every node from the tree, sets every
`vruntime` (and `min_vruntime`) to 0, cancels any CFS charge still in progress,
and reinserts the nodes. All processes start the new CFS period equal.
`schedtest` test 2 reproduces this situation: with the reset and placement
disabled, the late process took 99 of 100 ticks; with them, the split is 50/50.

### 3.4 Nice values

* Range −20 (most CPU) to 19 (least); default 0. Values outside the range are
  rejected.
* `setnice` changes the caller's value; `renice` changes another process's.
  The value is inherited by `fork` and kept across `exec`, which is how the
  `nice` command works.
* Nice only affects CFS. RR and FIFO ignore it.
* Changing nice doesn't move a process in the tree (the key is `vruntime`). The
  new weight applies from its next charge.

---

## 4. Statistics

| Field | How it is measured |
|---|---|
| `ctime`, `etime`, `first_run` | the tick at creation, at exit, and at the first dispatch |
| `rtime` (CPU time) | **exact:** TSC cycles accumulated in `sched()`, converted to ticks using the cycles-per-tick rate that CPU 0 measures on each timer interrupt |
| `wtime` (waiting) | sampled each tick: +1 if RUNNABLE |
| `stime` (sleeping) | sampled each tick: +1 if SLEEPING |

`schedbench` derives:

* **turnaround** = `etime − ctime`
* **waiting** = `wtime`
* **response** = `first_run − ctime`

Sampling happens in `trap()` **before** `wakeup(&ticks)`. Otherwise every
process in `sleep()` would have just been marked RUNNABLE when sampled and would
count as waiting on every tick.

`rtime` uses cycles rather than samples for the same reason as CFS: an I/O-bound
process that runs for a quarter of a tick just after each tick is never
RUNNING when a sample is taken, so sampling would report 0 CPU time.

---

## 5. Files

| File | Change |
|---|---|
| `proc.c` | the three scheduler policies, `select_process`, red-black tree, run-queue helpers, CFS accounting, `min_vruntime`, vruntime reset on switch, `waitx`, `getpinfo`, `renice`, `update_times` |
| `proc.h` | scheduling fields in `struct proc` |
| `sched.h` *(new)* | shared constants and `struct pinfo` / `struct waitinfo` |
| `trap.c` | no timer preemption under FIFO; per-tick statistics sampling |
| `x86.h` | `rdtsc()` |
| `sysproc.c`, `syscall.c`, `syscall.h`, `usys.S`, `user.h`, `defs.h` | system calls 22–28 |
| `types.h` | `uint64` |
| `schedctl.c`, `nice.c`, `renice.c`, `ps.c`, `schedbench.c`, `schedtest.c` *(new)* | user programs |
| `sh.c` | `runcmd`/`panic` declared `noreturn`, so GCC ≥ 12 builds with `-Werror` |
| `Makefile` | new programs; `-Werror -Wno-error=array-bounds` |
| `param.h` | `FSSIZE` 1000 → 2000 blocks, so `usertests` still has room with the new programs |

---

## 6. Evaluation

### 6.1 Method

Under plain QEMU (TCG) the guest's speed varies with host load. The same
CPU-bound job measured anywhere from 87 to 132 ticks across runs. QEMU's
`-icount` option ties guest time to instructions executed, which makes
measurements **reproducible**: two consecutive runs of the `cpu` workload with
`-icount` produced identical numbers, and each job's measured CPU time matched
its nominal length exactly. `shift=3` (one instruction = 8 ns of guest time)
keeps the virtual CPU fast enough that kernel overheads such as `fork()` stay
small compared with a tick. Hardware acceleration (`-enable-kvm`) is another option if your user
can access `/dev/kvm`.

All results below were measured with
`make qemu-nox CPUS=1 QEMUEXTRA="-icount shift=3,sleep=off"`. All times are in
ticks.

**Workloads** (`schedbench`): CPU jobs burn a calibrated number of ticks of
work. I/O jobs repeat 15 times: work for ¼ tick, then `sleep(2)`.

* `cpu`: one 100-tick job followed by four 20-tick jobs, arriving in that order
* `io`: four I/O jobs
* `mix`: two 60-tick CPU jobs and two I/O jobs

### 6.2 Results

**cpu** (long job first)

| Scheduler | Avg turnaround | Avg waiting | Avg response | Makespan |
|---|---|---|---|---|
| RR | 119.6 | 78.4 | 7.4 | 190 |
| FIFO | 146.4 | 108.6 | **110.0** | 189 |
| CFS | 118.4 | 76.4 | 6.2 | 190 |

**io**

| Scheduler | Avg turnaround | Avg waiting | Avg response | Makespan |
|---|---|---|---|---|
| RR | 32.5 | 0.7 | 2.5 | 37 |
| FIFO | 34.5 | 2.7 | 4.0 | 37 |
| CFS | 32.5 | 0.7 | 2.5 | 37 |

**mix** (2 CPU-bound + 2 I/O-bound)

| Scheduler | Avg turnaround | Avg waiting | Avg response | Makespan | I/O jobs' turnaround |
|---|---|---|---|---|---|
| RR | 85.7 | 42.0 | 5.7 | 136 | 40, 38 |
| FIFO | 125.2 | 78.2 | 79.5 | 158 | 155, 154 |
| CFS | 85.7 | **34.5** | 4.2 | 136 | **39, 34** (waiting 2, 0) |

**nice** (CFS, three CPU-bound processes for 300 ticks)

| Nice | Weight | CPU ticks | Share | Expected (weight / total weight) |
|---|---|---|---|---|
| −5 | 3121 | 207 | 69.9 % | 69.6 % |
| 0 | 1024 | 67 | 22.6 % | 22.8 % |
| 5 | 335 | 22 | 7.4 % | 7.4 % |

### 6.3 Discussion

* **FIFO shows the convoy effect.** Short jobs stuck behind the 100-tick job
  wait for all of it, so average response is ~110 ticks against ~7 for the
  preemptive schedulers. In `mix`, the I/O jobs wait behind both CPU jobs
  (turnaround ~155 against ~35). FIFO has the lowest overhead: a job runs
  without interruption once started, as job 0's waiting time of 6 shows.
* **RR and CFS behave the same for equal-nice CPU-bound jobs.** With a 1-tick
  slice, both rotate fairly between processes with equal weight.
* **CFS helps interactive and I/O-bound processes.** A process that sleeps
  accumulates little `vruntime`, so when it wakes it is at or near the left of
  the tree and runs almost immediately. In `mix`, CFS's I/O jobs waited 0–2
  ticks in total, against 17–19 under RR, where they queue behind the CPU hogs
  every time they wake.
* **Nice is proportional.** Measured CPU shares are within 0.3 percentage
  points of the weight ratios.
* **Makespan is about the same for all three** when the total work is the same:
  scheduling changes *who* waits, not how much work there is to do.

### 6.4 Tests

* `schedtest`: **all checks pass** on 1 CPU and on 2 CPUs. It covers 8
  scheduler switches while processes are running, fairness after switching back
  into CFS, FIFO dispatch order and non-preemption, and nice validation,
  inheritance and `renice`.
* xv6 `usertests`: **ALL TESTS PASSED** under CFS (2 CPUs) and RR (1 CPU).
  Under FIFO, `usertests`' `preempt` test cannot finish by design: it spins
  three children forever and expects the parent to be preempted back in to kill
  them.

---

## 7. Simplifications and limitations

Compared with Linux CFS:

* **Time slice is one tick.** A running process is preempted every tick (about
  10 ms). Linux instead computes a slice from `sched_latency` and
  `min_granularity`, and preempts on wakeup when the woken task is sufficiently
  behind.
* **One global run queue.** All CPUs share one tree under `ptable.lock`. Linux
  keeps one run queue per CPU and balances load between them. On SMP,
  `min_vruntime` follows the tree's leftmost node and ignores processes running
  on other CPUs, so it can be slightly ahead of their `vruntime`.
* **No sleeper credit.** Linux places a waking task slightly *below*
  `min_vruntime` (by half the latency period). Here it is placed exactly at it.
* **No group scheduling** (cgroups) or autogroup.

Other limitations:

* **FIFO can lock out the shell.** With FIFO active and as many CPU-bound
  processes as CPUs, nothing else runs, including the shell you would use to
  switch back, until one of them blocks or exits. This is inherent to a
  non-preemptive policy; Linux limits it with RT throttling.
* **Switching into CFS discards vruntime history.** This is deliberate (section
  3.3): each CFS period starts fair.
* **No permission checks.** Any process may change the scheduler or renice any
  other process; xv6 has no users.
* **Assumes a constant-rate TSC**, which is true for QEMU and for modern x86. A
  single uninterrupted run longer than about 2^42 cycles (roughly 20 minutes at
  3 GHz, possible only under FIFO) could overflow the CFS charge.
* Wait and sleep times are sampled once per tick, so they are accurate to
  ±1 tick. CPU time is exact.

---

## 8. Bugs fixed during development

| Problem | Fix |
|---|---|
| Kernel failed to link: `undefined reference to __udivdi3` (64-bit division) | fixed-point charge using 32-bit division (`cfs_charge`) |
| Negative nice values added 0 to vruntime, starving other processes | finer units: cycles × 2^16 fixed-point factor |
| RR always picked the lowest slot, starving others | rotating `rr_next` cursor |
| Stale vruntime when switching back into CFS | `cfs_reset_vruntime()` and placement at `min_vruntime` |
| FIFO tie-breaking by slot; FIFO still preempted every tick | `fifo_seq` ordering; no timer yield under FIFO |
| Tick-based charging let short-running processes go uncharged | TSC-based run time |
| Finding the minimum `vruntime` scanned the whole process table on every wakeup | `min_vruntime` maintained from the tree |
| Scheduler changed without holding `ptable.lock` | `set_scheduler()` / `get_scheduler()` take the lock |
| `-Werror` removed to work around GCC 13's `-Winfinite-recursion` in `sh.c` | `noreturn` declarations; `-Werror` restored |
