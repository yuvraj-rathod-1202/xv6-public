// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

#include "sched.h"   // SCHED_*, NICE_*, struct pinfo, struct waitinfo

#define NICE_0_LOAD 1024

extern int current_scheduler;

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  uint ctime;                  // Creation time (ticks)
  uint etime;                  // Exit time (ticks)
  uint first_run;              // Tick of first dispatch (valid if has_run)
  int has_run;                 // Has been dispatched at least once
  uint wtime;                  // Ticks spent RUNNABLE
  uint stime;                  // Ticks spent SLEEPING
  uint64 fifo_seq;             // Order in which it last became RUNNABLE (FIFO)
  uint64 vruntime;             // Virtual runtime for CFS (weighted CPU cycles)
  uint64 exec_time;            // Total CPU cycles spent RUNNING (any scheduler)
  uint64 run_start;            // TSC value when the current run started
  int cfs_accounting;          // Whether the current run interval is CFS-accounted
  int nice_value;              // User nice value, from NICE_MIN through NICE_MAX
  struct proc *rb_left;
  struct proc *rb_right;
  struct proc *rb_parent;
  int rb_color;
  int rb_in_tree;
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
