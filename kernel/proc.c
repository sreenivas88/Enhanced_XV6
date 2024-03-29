#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "queue.h"

uint64 sys_uptime();
struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S
// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
uint64
random(void)
{
  static uint64 z1 = 5234254; // assuming a random seed to generate random numbers
  static uint64 z2 = 1764237; // static to ensure same number is not generated all the time
  static uint64 z3 = 3986790;
  static uint64 z4 = 9823476;
  static uint64 b;
  b = ((z1 << 6) ^ z1) >> 5;
  z1 = ((z1 & 75643U) << 13) ^ b;
  b = ((z2 << 23) ^ z2) >> 12;
  z2 = ((z2 & 873256U) << 17) ^ b;
  b = ((z3 << 13) ^ z3) >> 19;
  z3 = ((z3 & 71549U) << 7) ^ b;
  b = ((z4 << 3) ^ z4) >> 11;
  z4 = ((z4 & 326565U) << 13) ^ b;
  return (z1 ^ z2 ^ z3 ^ z4) / 2;
}

void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  ///////////////// IMPLEMENTED FOR SIGALARM /////////////////
  if((p->trapframe_copy = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  ///////////////////////////////////////////////////////////////

  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  /////////////////////// IMPLEMENTED FOR SCHEDULER TESTING /////////////////
  p->rtime = 0;
  p->etime = 0;
  p->ctime = sys_uptime();

  //////////////////////////////////////////////////////////////////////////

  p->birth_time = sys_uptime(); // sys_uptime - gives number of ticks since start
  p->num_tickets = 1;           // # tickets = 1 by default for every process
  p->static_priority = 60;      // priority = 60 by default
  p->sleep_start = 0;
  p->sleep_time = 0;
  p->running_time = 0;
  p->proc_queue = 0;

  ///////////////// IMPLEMENTED FOR SIGALARM /////////////////
  p->alarm_is_set=0;  // initialising to 0 as alarm is not set yet
  p->num_ticks=0;     // number of ticks to pass = 0 (no alarm)
  p->curr_ticks=0;    // number of ticks passed = 0 (its just initialized)
  p->sig_handler=0;   // there is no handler associated to teh alarm handler (copyin/copyout handles invalid addresses)
  ///////////////////////////////////////////////////////////
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  /////////// implemented for sigalarm /////////////
  if(p->trapframe_copy)   // freeing the allocated memory
    kfree((void*)p->trapframe_copy);
  /////////////////////////////////////////////////
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->strace_bit = 0;
  p->birth_time = __INT_MAX__;
  p->num_tickets = 0;
  p->static_priority = 0; // for PBS
  p->dynamic_priority = 0;
  p->sleep_start = 0;
  p->sleep_time = 0;
  p->running_time = 0;
  p->proc_queue = 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);
  np->strace_bit = p->strace_bit;
  np->birth_time = p->birth_time;           // check if this and equalities below this are required
  np->num_tickets = np->num_tickets;        // ensuring # tickets of parent = # tickets of child
  np->static_priority = p->static_priority; // check if this is required
  np->dynamic_priority = p->dynamic_priority;
  np->sleep_time = p->sleep_time;
  np->running_time = p->running_time;
  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  //////////////// IMPLEMENTED FOR SCHEDULER TESTING ////////////////////////
  p->etime = sys_uptime();
  ///////////////////////////////////////////////////////////////////////

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

///////////////////////// SCHEDULERS ////////////////////////////////
/////////////////////// ROUND ROBIN - original implementation //////////////
void roundRobin(struct cpu *c)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == RUNNABLE)
    {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      p->state = RUNNING;
      c->proc = p;
      swtch(&c->context, &p->context);

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&p->lock);
  }
  return;
}

///////////////////////////////////////////////////////////////////////////

/////////////////////// FCFS implementation ///////////////////
void fcfs(struct cpu *c)
{
  struct proc *oldestproc = 0;
  uint64 oldesttime = 0;

  for (int i = 0; i < NPROC; i++)
  {
    struct proc *p = &proc[i];
    acquire(&p->lock);

    if (oldestproc)
      oldesttime = oldestproc->birth_time; // save the time of the oldest proc

    if ((oldesttime > p->birth_time) || !oldestproc)
    {
      if (p->state == RUNNABLE)
      {
        if (oldestproc)
          release(&oldestproc->lock); // release the prev selected proc, and lock newly selected proc

        oldestproc = p;
      }
    }

    if (oldestproc != p) // if the selected proc is not the last proc, release last proc
      release(&p->lock);
  }

  if (!oldestproc) // change state of the newly selected process
    return;

  if (oldestproc->state == RUNNABLE)
  {
    // Switch to chosen process.  It is the process's job
    // to release its lock and then reacquire it
    // before jumping back to us.
    oldestproc->state = RUNNING;
    c->proc = oldestproc;
    swtch(&c->context, &oldestproc->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&oldestproc->lock);
  }
  return;
}

/////////////////////////////////////////////////////////////////

//////////////////// LOTTERY BASED implementation /////////////////
// The probability that a process assumes the given time schedule is the
// the number of tickets owned by the proc
void lotteryBased(struct cpu *c)
{
  uint64 totalNumTickets = 0, ticketCnt = 0;

  for (int i = 0; i < NPROC; i++)
  {
    struct proc *p = &proc[i];
    acquire(&p->lock);

    if (p->state == RUNNABLE)
      totalNumTickets += p->num_tickets;

    release(&p->lock);
  }

  struct proc *chosenproc = 0;
  uint64 randNum = random() % totalNumTickets;

  for (int i = 0; i < NPROC; i++)
  {
    struct proc *p = &proc[i];
    acquire(&p->lock);

    if (p->state != RUNNABLE)
    {
      release(&p->lock);
      continue;
    }

    if (chosenproc)
      release(&chosenproc->lock);

    chosenproc = p;
    ticketCnt += p->num_tickets;
    if (ticketCnt >= randNum)
    {
      chosenproc = p;
      break;
    }
  }

  if (chosenproc)
  {
    // Switch to chosen process.  It is the process's job
    // to release its lock and then reacquire it
    // before jumping back to us.
    chosenproc->state = RUNNING;
    c->proc = chosenproc;
    swtch(&c->context, &chosenproc->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&chosenproc->lock);
  }
  return;
}

//////////////////////////////////////////////////////////////////

////////////////// PRIORITY BASED SCHEDULING /////////////////////
// save the time when the process goes ot sleep and the time when the process wakes up
// For running time, myproc gives the currently running process, just increment the
// running time for it whenever you increment the number of ticks

void priority_based(struct cpu *c)
{
  struct proc *chosenproc = 0;

  for (int i = 0; i < NPROC; i++)
  {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    if (p->state != RUNNABLE)
    {
      release(&p->lock);
      continue;
    }

    p->dynamic_priority = calcDP(p);
    if (!chosenproc || (p->dynamic_priority < chosenproc->dynamic_priority)) // this works because of short-circuiting
    {
      if (chosenproc)
        release(&chosenproc->lock);

      chosenproc = p;
    }

    if (p != chosenproc)
      release(&p->lock);
  }

  if (chosenproc)
  {
    // Switch to chosen process.  It is the process's job
    // to release its lock and then reacquire it
    // before jumping back to us.
    chosenproc->state = RUNNING;
    chosenproc->sleep_time = 0;
    c->proc = chosenproc;
    swtch(&c->context, &chosenproc->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&chosenproc->lock);
  }
  return;
}

//////////////////////////////////////////////////////////////////

////////////////////// MLFQ SCHEDULING ///////////////////////////
// void mlfq(struct cpu* c) /////////////// i guess this MLFQ is breaking somewhere
// {
//   struct proc* chosenproc = 0;
//   for(int i=0; i<NQUEUE; i++)
//   {
//     while (queue_info.num_procs[i])
//     {
//       struct proc* p = queue_pop(i);
//       acquire(&p->lock);
//       if (!p)
//         return;
      
//       if (p->state == RUNNABLE)
//       {
//         chosenproc = p;
//         break;
//       }
//       release(&p->lock);
//     }
//     if (chosenproc)
//       break;      
//   }

  
//   if (chosenproc)
//   {
//     // Switch to chosen process.  It is the process's job
//     // to release its lock and then reacquire it
//     // before jumping back to us.
//     chosenproc->state = RUNNING;
//     chosenproc->sleep_time = 0;
//     c->proc = chosenproc;
//     swtch(&c->context, &chosenproc->context);

//     // Process is done running for now.
//     // It should have changed its p->state before coming back.
//     c->proc = 0;
//     release(&chosenproc->lock);
//   }
//   return;
// }

//////////////////////////////////////////////////////////////////

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

#ifdef RR
    roundRobin(c);
#endif

#ifdef FCFS
    fcfs(c);
#endif

#ifdef PBS
    priority_based(c);
#endif

#ifdef MLFQ
    mlfq(c);
#endif

  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  if (p->state != SLEEPING)        // added for PBS
    p->sleep_start = sys_uptime(); // added for PBS

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;
  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;

        if (p->sleep_start != 0)                         // added for PBS
          p->sleep_time = sys_uptime() - p->sleep_start; // added for PBS
        p->sleep_start = 0;                              // added for PBS
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

void strace(int strace_mask)
{
  struct proc *p;
  p = myproc();
  if (!p)
    return;

  myproc()->strace_bit = strace_mask;
  return;
}

int settickets(int numTickets)
{
  struct proc *p;
  p = myproc();
  if (!p)
    return -1;

  myproc()->num_tickets = numTickets;
  return numTickets;
}

int calcDP(struct proc *p)
{
  int sleep_time = p->sleep_time;
  int running_time = p->running_time;
  int SP = p->static_priority;

  if ((sleep_time + running_time) == 0)
    return 5; // assume normal niceness

  int niceness = 10 * ((int)sleep_time / (sleep_time + running_time));
  int DP = ((SP - niceness + 5) < 100) ? (SP - niceness + 5) : 100;

  return (DP > 0) ? DP : 0;
}

int set_priority(int new_priority, int pid)
{
  struct proc *chosen = 0;
  for (int i = 0; i < NPROC; i++)
  {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    if (pid == p->pid)
    {
      chosen = p;
      break;
    }
    release(&p->lock);
  }

  int prevSP = -1;
  if (chosen)
  {
    prevSP = chosen->static_priority;
    chosen->static_priority = new_priority;
    chosen->dynamic_priority = calcDP(chosen);
    release(&chosen->lock);
  }
  else
  {
    printf("Given pid does not exist\n");
  }
  yield(); // reschedule once set_priority is done
  return prevSP;
}

void PBS_find_times()
{
  // struct proc *p;
  // p = myproc();
  // if (!p)
  //   return;

  // (myproc()->running_time)++;
  return;
}

  ///////////////// IMPLEMENTED FOR SIGALARM /////////////////
uint64 sys_sigalarm(void)
{
  int this_ticks;
  argint(0, &this_ticks);
  uint64 handler;
  argaddr(1, &handler);
  myproc()->sig_handler = 0;
  myproc()->num_ticks = this_ticks;
  myproc()->curr_ticks = 0;
  myproc()->sig_handler = handler;
  return 0; 
}
//////////////////////////////////////////////

////////////// IMPLEMENTED FOR SCHEDULER TESTING //////////////////////
int
waitx(uint64 addr, uint* wtime, uint* rtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
  return 0;
}

void
update_time()
{
  struct proc* p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->rtime++;
    }
    release(&p->lock); 
  }
}
////////////////////////////////////////////////////////////////