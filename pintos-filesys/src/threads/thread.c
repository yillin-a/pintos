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

#define THREAD_MAGIC 0xcd6abf4b

static struct list fifo_ready_list;
static struct list all_list;
static struct thread* idle_thread;
static struct thread* initial_thread;
static struct lock tid_lock;

struct kernel_thread_frame {
  void* eip;
  thread_func* function;
  void* aux;
};

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define TIME_SLICE 4
static unsigned thread_ticks;

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

enum sched_policy active_sched_policy;

typedef struct thread* scheduler_func(void);

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

void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&fifo_ready_list);
  list_init(&all_list);

  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

void thread_start(void) {
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  intr_enable();
  sema_down(&idle_started);
}

void thread_tick(void) {
  struct thread* t = thread_current();

  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  /* 修改：通过 pcb 检查 pagedir */
  else if (t->pcb != NULL && t->pcb->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks,
         user_ticks);
}

tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  {
      struct thread *cur = thread_current();
      if (cur->cwd != NULL) {
          t->cwd = dir_reopen(cur->cwd);
      }
  }

  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  uint8_t fpu_old_thread[108];
  asm volatile("fsave %0" : "=m"(fpu_old_thread));
  asm volatile("fninit");
  asm volatile("fsave %0" : "=m"(sf->fpu));
  asm volatile("frstor %0" : : "m"(fpu_old_thread));

  thread_unblock(t);

  return tid;
}

void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

  if (active_sched_policy == SCHED_FIFO)
    list_push_back(&fifo_ready_list, &t->elem);
  else
    PANIC("Unimplemented scheduling policy value: %d", active_sched_policy);
}

void thread_unblock(struct thread* t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  thread_enqueue(t);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

const char* thread_name(void) { return thread_current()->name; }

struct thread* thread_current(void) {
  struct thread* t = running_thread();
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);
  return t;
}

tid_t thread_tid(void) { return thread_current()->tid; }

void thread_exit(void) {
  ASSERT(!intr_context());

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

void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

void thread_set_priority(int new_priority) { thread_current()->priority = new_priority; }
int thread_get_priority(void) { return thread_current()->priority; }
void thread_set_nice(int nice UNUSED) {}
int thread_get_nice(void) { return 0; }
int thread_get_load_avg(void) { return 0; }
int thread_get_recent_cpu(void) { return 0; }

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

static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);
  intr_enable(); 
  function(aux); 
  thread_exit(); 
}

struct thread* running_thread(void) {
  uint32_t* esp;
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

static bool is_thread(struct thread* t) { return t != NULL && t->magic == THREAD_MAGIC; }

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

static void* alloc_frame(struct thread* t, size_t size) {
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

static struct thread* thread_schedule_fifo(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

static struct thread* thread_schedule_prio(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=prio\"");
}

static struct thread* thread_schedule_fair(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=fair\"");
}

static struct thread* thread_schedule_mlfqs(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=mlfqs\"");
}

static struct thread* thread_schedule_reserved(void) {
  PANIC("Invalid scheduler policy value: %d", active_sched_policy);
}

static struct thread* next_thread_to_run(void) {
  return (scheduler_jump_table[active_sched_policy])();
}

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

static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

uint32_t thread_stack_ofs = offsetof(struct thread, stack);