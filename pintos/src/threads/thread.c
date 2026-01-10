#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "filesys/directory.h" 
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list fifo_ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread* idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread* initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
  void* eip;             /* Return address. */
  thread_func* function; /* Function to call. */
  void* aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

static void init_thread(struct thread*, const char* name, int priority);
static bool is_thread(struct thread*) UNUSED;
static void* alloc_frame(struct thread*, size_t size);
static void schedule(void);
static void thread_enqueue(struct thread* t);
static tid_t allocate_tid(void);
void thread_switch_tail(struct thread* prev);

static void kernel_thread(thread_func*, void* aux);
static void idle(void* aux UNUSED);
static struct thread* running_thread(void);

static struct thread* next_thread_to_run(void);
static struct thread* thread_schedule_fifo(void);
static struct thread* thread_schedule_prio(void);
static struct thread* thread_schedule_fair(void);
static struct thread* thread_schedule_mlfqs(void);
static struct thread* thread_schedule_reserved(void);

/* Determines which scheduler the kernel should use.
   Controlled by the kernel command-line options. 
   Variable definition (declaration is in thread.h) */
enum sched_policy active_sched_policy;

/* Selects a thread to run from the ready list according to
   some scheduling policy, and returns a pointer to it. */
typedef struct thread* scheduler_func(void);

/* Jump table for dynamically dispatching the current scheduling
   policy in use by the kernel. */
scheduler_func* scheduler_jump_table[8] = {
    thread_schedule_fifo,     
    thread_schedule_prio,
    thread_schedule_fair,     
    thread_schedule_mlfqs,
    thread_schedule_reserved, 
    thread_schedule_reserved,
    thread_schedule_reserved, 
    thread_schedule_reserved
};

/* Initializes the threading system by transforming the code
   that's currently running into a thread. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&fifo_ready_list);
  list_init(&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick. */
void thread_tick(void) {
  struct thread* t = thread_current();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks,
         user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial PRIORITY. */
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  /* Inherit current working directory from parent */
  {
      struct thread *cur = thread_current();
      if (cur->cwd != NULL) {
          t->cwd = dir_reopen(cur->cwd);
      }
      /* t is zeroed, so t->cwd is NULL if cur->cwd is NULL */
  }

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Need to save the current FPU State on the stack. */
  uint8_t fpu_old_thread[108];
  asm volatile("fsave %0" : "=m"(fpu_old_thread));

  /* Now we can initialize the FPU since we already saved the current state. */
  asm volatile("fninit");
  asm volatile("fsave %0" : "=m"(sf->fpu));

  /* Now we can restore the current state. */
  asm volatile("frstor %0" : : "m"(fpu_old_thread));

  /* Add to run queue. */
  thread_unblock(t);

  return tid;
}

/* Puts the current thread to sleep. */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Places a thread on the ready structure appropriate for the
   current active scheduling policy. */
static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

  if (active_sched_policy == SCHED_FIFO)
    list_push_back(&fifo_ready_list, &t->elem);
  else
    PANIC("Unimplemented scheduling policy value: %d", active_sched_policy);
}

/* Transitions a blocked thread T to the ready-to-run state. */
void thread_unblock(struct thread* t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  thread_enqueue(t);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char* thread_name(void) { return thread_current()->name; }

/* Returns the running thread. */
struct thread* thread_current(void) {
  struct thread* t = running_thread();
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);
  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it. */
void thread_exit(void) {
  ASSERT(!intr_context());

  /* Close current working directory to prevent memory leak */
  if (thread_current()->cwd != NULL) {
      dir_close(thread_current()->cwd);
      thread_current()->cwd = NULL;
  }

  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* Yields the CPU. */
void thread_yield(void) {
  struct thread* cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    thread_enqueue(cur);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'. */
void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) { thread_current()->priority = new_priority; }

/* Returns the current thread's priority. */
int thread_get_priority(void) { return thread_current()->priority; }

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) { /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  /* Not yet implemented. */
  return 0;
}

/* Idle thread. */
static void idle(void* idle_started_ UNUSED) {
  struct semaphore* idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    intr_disable();
    thread_block();
    asm volatile("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);

  intr_enable(); 
  function(aux); 
  thread_exit(); 
}

/* Returns the running thread. */
struct thread* running_thread(void) {
  uint32_t* esp;
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool is_thread(struct thread* t) { return t != NULL && t->magic == THREAD_MAGIC; }

/* Does basic initialization of T as a blocked thread named NAME. */
static void init_thread(struct thread* t, const char* name, int priority) {
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t*)t + PGSIZE;
  t->priority = priority;
  
  t->magic = THREAD_MAGIC;

  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack. */
static void* alloc_frame(struct thread* t, size_t size) {
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* First-in first-out scheduler */
static struct thread* thread_schedule_fifo(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Strict priority scheduler */
static struct thread* thread_schedule_prio(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=prio\"");
}

/* Fair priority scheduler */
static struct thread* thread_schedule_fair(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=fair\"");
}

/* Multi-level feedback queue scheduler */
static struct thread* thread_schedule_mlfqs(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=mlfqs\"");
}

/* Not an actual scheduling policy */
static struct thread* thread_schedule_reserved(void) {
  PANIC("Invalid scheduler policy value: %d", active_sched_policy);
}

/* Chooses and returns the next thread to be scheduled. */
static struct thread* next_thread_to_run(void) {
  return (scheduler_jump_table[active_sched_policy])();
}

/* Completes a thread switch. */
void thread_switch_tail(struct thread* prev) {
  struct thread* cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  cur->status = THREAD_RUNNING;
  thread_ticks = 0;

#ifdef USERPROG
  process_activate();
#endif

  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new thread. */
static void schedule(void) {
  struct thread* cur = running_thread();
  struct thread* next = next_thread_to_run();
  struct thread* prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next);
  thread_switch_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

uint32_t thread_stack_ofs = offsetof(struct thread, stack);