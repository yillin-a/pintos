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
#ifdef USERPROG
#include "userprog/process.h"
#endif

// 全局变量声明
fixed_point_t load_avg;
extern bool is_hier;
bool thread_mlfqs;
bool schedule_started;

#define THREAD_MAGIC 0xcd6abf4b
#define TIMESLICE_TICKS 4

// 就绪队列和线程列表
static struct list fifo_ready_list;
static struct list all_list;

// 特殊线程指针
static struct thread* idle_thread;
static struct thread* initial_thread;

// 线程ID锁
static struct lock tid_lock;

// 内核线程栈帧结构
struct kernel_thread_frame {
  void* eip;
  thread_func* function;
  void* aux;
};

// 统计信息
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;
static unsigned thread_ticks;

// 函数声明
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

// 调度策略
enum sched_policy active_sched_policy;

typedef struct thread* scheduler_func(void);

scheduler_func* scheduler_jump_table[8] = {
    thread_schedule_fifo,     thread_schedule_prio,
    thread_schedule_fair,     thread_schedule_mlfqs,
    thread_schedule_reserved, thread_schedule_reserved,
    thread_schedule_reserved, thread_schedule_reserved
};

// 初始化线程系统
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

// 启动线程调度
void thread_start(void) {
  struct semaphore idle_started;
  load_avg = FP_CONST(0);
  schedule_started = true;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  intr_enable();

  sema_down(&idle_started);
}

// 时钟滴答处理
void thread_tick(void) {
  struct thread* t = thread_current();

  // 更新统计信息
  if (t == idle_thread)
    idle_ticks++;
  else if (t->pcb != NULL)
    user_ticks++;
  else
    kernel_ticks++;

  // 检查时间片是否用完
  thread_ticks++;
  if (thread_ticks >= TIMESLICE_TICKS)
    intr_yield_on_return();
}

// 打印线程统计信息
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", 
         idle_ticks, kernel_ticks, user_ticks);
}

// 创建新线程
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  // 分配页面
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  // 初始化线程
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();
  t->ticks_blocked = 0;

  // 设置内核线程栈帧
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  // 设置切换入口栈帧
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  // 设置线程切换栈帧
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  // 解除线程阻塞
  thread_unblock(t);

  // 如果新线程优先级更高，让出CPU
  struct thread* cur = thread_current();
  if (cur->priority < priority) {
    thread_yield();
  }

  return tid;
}

// 阻塞当前线程
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

// 将线程加入就绪队列
static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

  if (active_sched_policy == SCHED_FIFO)
    list_push_back(&fifo_ready_list, &t->elem);
  else
    PANIC("Unimplemented scheduling policy value: %d", active_sched_policy);
}

// 解除线程阻塞
void thread_unblock(struct thread *t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  
  // 按优先级插入就绪队列
  list_insert_ordered(&fifo_ready_list, &t->elem, 
                      (list_less_func *)&thread_cmp_priority, NULL);
  t->status = THREAD_READY;
  
  intr_set_level(old_level);
}

// 获取当前线程名称
const char* thread_name(void) { 
  return thread_current()->name; 
}

// 获取当前运行线程
struct thread* thread_current(void) {
  struct thread* t = running_thread();

  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

// 获取当前线程ID
tid_t thread_tid(void) { 
  return thread_current()->tid; 
}

// 终止当前线程
void thread_exit(void) {
  ASSERT(!intr_context());

  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

// 让出CPU
void thread_yield(void) {
  if (!schedule_started)
    return;
    
  struct thread* cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  
  // 将当前线程按优先级插入就绪队列
  if (cur != idle_thread) {
    list_insert_ordered(&fifo_ready_list, &cur->elem, 
                        (list_less_func *)&thread_cmp_priority, NULL);
  }
  
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

// 遍历所有线程
void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

// 设置线程优先级
void thread_set_priority(int new_priority) {
  // 公平调度下不允许手动设置优先级
  if (active_sched_policy == SCHED_FAIR)
    return;

  enum intr_level old_level = intr_disable();

  struct thread *current_thread = thread_current();
  int old_priority = current_thread->priority;
  current_thread->base_priority = new_priority;

  // 如果没有持有锁或新优先级更高，直接更新
  bool should_update = list_empty(&current_thread->locks) || (new_priority > old_priority);
  if (should_update) {
    current_thread->priority = new_priority;
    thread_yield();
  }

  intr_set_level(old_level);
}

// 获取线程优先级
int thread_get_priority(void) { 
  return thread_current()->priority; 
}

// 设置nice值
void thread_set_nice(int nice UNUSED) { 
  thread_current()->nice = nice;
  mlfqs_update_priority(thread_current());
  thread_yield();
}

// 获取nice值
int thread_get_nice(void) {
  return thread_current()->nice;
}

// 获取系统负载平均值
int thread_get_load_avg(void) {
  return FP_ROUND(FP_MULT_MIX(load_avg, 100));
}

// 获取最近CPU使用率
int thread_get_recent_cpu(void) {
  return FP_ROUND(FP_MULT_MIX(thread_current()->recent_cpu, 100));
}

// 空闲线程函数
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

// 内核线程基础函数
static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);

  intr_enable();
  function(aux);
  thread_exit();
}

// 获取正在运行的线程
struct thread* running_thread(void) {
  uint32_t* esp;
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

// 检查是否为有效线程
static bool is_thread(struct thread* t) { 
  return t != NULL && t->magic == THREAD_MAGIC; 
}

// 初始化线程结构
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
  t->pcb = NULL;
  t->magic = THREAD_MAGIC;

  t->base_priority = priority;
  list_init(&t->locks);
  t->lock_waiting = NULL;
  t->ticks = 0;

  // 根据模式设置nice值
  if (is_hier) {
    t->nice = 0;
  } else {
    // 根据优先级映射nice值
    int priority_map[][2] = {
        {56, 0}, {48, 1}, {40, 2}, {32, 3},
        {24, 4}, {16, 5}, {8, 6}, {0, 7}
    };
    
    t->nice = 0;
    for (int i = 0; i < 8; i++) {
      if (priority == priority_map[i][0]) {
        t->nice = priority_map[i][1];
        break;
      }
    }
  }
  
  t->recent_cpu = FP_CONST(0);

  old_level = intr_disable();
  list_insert_ordered(&all_list, &t->allelem, 
                      (list_less_func *)&thread_cmp_priority, NULL);
  intr_set_level(old_level);
}

// 分配栈帧
static void* alloc_frame(struct thread* t, size_t size) {
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

// FIFO调度算法
static struct thread* thread_schedule_fifo(void) {
  bool has_ready = !list_empty(&fifo_ready_list);
  return has_ready ? list_entry(list_pop_front(&fifo_ready_list), struct thread, elem) 
                   : idle_thread;
}

// 优先级调度算法
static struct thread* thread_schedule_prio(void) {
  bool has_ready = !list_empty(&fifo_ready_list);
  return has_ready ? list_entry(list_pop_front(&fifo_ready_list), struct thread, elem) 
                   : idle_thread;
}

// 公平调度算法
static struct thread* thread_schedule_fair(void) {
  bool has_ready = !list_empty(&fifo_ready_list);
  return has_ready ? list_entry(list_pop_front(&fifo_ready_list), struct thread, elem) 
                   : idle_thread;
}

// 多级反馈队列调度
static struct thread* thread_schedule_mlfqs(void) {
  bool has_ready = !list_empty(&fifo_ready_list);
  return has_ready ? list_entry(list_pop_front(&fifo_ready_list), struct thread, elem) 
                   : idle_thread;
}

// 保留调度算法
static struct thread* thread_schedule_reserved(void) {
  bool has_ready = !list_empty(&fifo_ready_list);
  return has_ready ? list_entry(list_pop_front(&fifo_ready_list), struct thread, elem) 
                   : idle_thread;
}

// 选择下一个运行的线程
static struct thread* next_thread_to_run(void) {
  bool has_ready = !list_empty(&fifo_ready_list);
  return has_ready ? list_entry(list_pop_front(&fifo_ready_list), struct thread, elem) 
                   : idle_thread;
}

// 线程切换尾处理
void thread_switch_tail(struct thread* prev) {
  struct thread* cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  cur->status = THREAD_RUNNING;
  thread_ticks = 0;

#ifdef USERPROG
  process_activate();
#endif

  // 释放已终止线程的资源
  bool should_free = (prev != NULL) && (prev->status == THREAD_DYING) && (prev != initial_thread);
  if (should_free) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

// 执行调度
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

// 分配线程ID
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

// 检查并唤醒阻塞线程
void blocked_thread_check(struct thread *t, void *aux UNUSED) {
  bool is_blocked = (t->status == THREAD_BLOCKED);
  bool has_ticks = (t->ticks_blocked > 0);
  
  if (is_blocked && has_ticks) {
    t->ticks_blocked--;
    if (t->ticks_blocked == 0) {
      thread_unblock(t);
    }
  }
}

// 比较线程优先级
bool thread_cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *ta = list_entry(a, struct thread, elem);
  struct thread *tb = list_entry(b, struct thread, elem);
  return ta->priority > tb->priority;
}

// 优先级捐赠
void thread_donate_priority(struct thread *thread) {
  enum intr_level old = intr_disable();
  thread_update_priority(thread);

  // 如果线程在就绪队列，重新排序
  if (thread->status == THREAD_READY) {
    list_remove(&thread->elem);
    list_insert_ordered(&fifo_ready_list, &thread->elem, thread_cmp_priority, NULL);
  }
  
  intr_set_level(old);
}

// 持有锁时的处理
void thread_hold_the_lock(struct lock *lock) {
  enum intr_level old_level = intr_disable();
  
  list_insert_ordered(&thread_current()->locks, &lock->elem, lock_cmp_priority, NULL);

  // 如果锁的最大优先级更高，提升当前线程优先级
  if (lock->max_priority > thread_current()->priority) {
    thread_current()->priority = lock->max_priority;
    thread_yield();
  }
  
  intr_set_level(old_level);
}

// 更新负载平均值和最近CPU使用
void mlfqs_update_load_avg_and_recent_cpu() {
  ASSERT(active_sched_policy == SCHED_FAIR);
  ASSERT(intr_context());

  // 计算就绪线程数
  size_t cnt = list_size(&fifo_ready_list);
  if (thread_current() != idle_thread) {
    cnt++;
  }
  
  // 更新负载平均值: load_avg = (59/60)*load_avg + (1/60)*ready_threads
  fixed_point_t term1 = FP_DIV_MIX(FP_MULT_MIX(load_avg, 59), 60);
  fixed_point_t term2 = FP_DIV_MIX(FP_CONST(cnt), 60);
  load_avg = FP_ADD(term1, term2);

  // 更新所有线程的recent_cpu
  struct thread *thread;
  struct list_elem *e;
  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    thread = list_entry(e, struct thread, allelem);
    if (thread != idle_thread) {
      // recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice
      fixed_point_t coef = FP_DIV(FP_MULT_2_F(load_avg, 1), 
                                   FP_ADD_MIX(FP_MULT_2_F(load_avg, 1), 1));
      thread->recent_cpu = FP_ADD_MIX(FP_MULT(coef, thread->recent_cpu), 0);
      mlfqs_update_priority(thread);
    }
  }
}

// 释放锁时的处理
void thread_remove_lock(struct lock *lock) {
  enum intr_level old = intr_disable();
  list_remove(&lock->elem);
  thread_update_priority(thread_current());
  intr_set_level(old);
}

// 更新线程优先级
void thread_update_priority(struct thread *t) {
  enum intr_level old = intr_disable();
  int max_priority = t->base_priority;

  // 检查持有的锁，获取最大优先级
  if (!list_empty(&t->locks)) {
    list_sort(&t->locks, lock_cmp_priority, NULL);
    struct lock *max_lock = list_entry(list_front(&t->locks), struct lock, elem);
    int lock_priority = max_lock->max_priority;
    if (lock_priority > max_priority)
      max_priority = lock_priority;
  }

  t->priority = max_priority;
  intr_set_level(old);
}

// 比较锁的优先级
bool lock_cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct lock *la = list_entry(a, struct lock, elem);
  struct lock *lb = list_entry(b, struct lock, elem);
  return la->max_priority > lb->max_priority;
}

// 增加最近CPU使用
void mlfqs_inc_recent_cpu() {
  ASSERT(active_sched_policy == SCHED_FAIR);
  ASSERT(intr_context());

  struct thread *cur = thread_current();
  if (cur == idle_thread)
    return;
  cur->recent_cpu = FP_ADD_MIX(cur->recent_cpu, 1);
}

// 更新MLFQS优先级
void mlfqs_update_priority(struct thread *t) {
  ASSERT(active_sched_policy == SCHED_FAIR);

  if (t == idle_thread)
    return;

  // priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
  fixed_point_t base = FP_CONST(PRI_MAX);
  fixed_point_t cpu_term = FP_DIV_MIX(t->recent_cpu, 4);
  
  if (is_hier) {
    t->priority = FP_INT_PART(FP_SUB_MIX(FP_SUB(base, cpu_term), t->nice));
  } else {
    t->priority = FP_INT_PART(FP_SUB_MIX(FP_SUB(base, cpu_term), 0));
  }

  // 限制优先级范围
  if (t->priority < PRI_MIN)
    t->priority = PRI_MIN;
  else if (t->priority > PRI_MAX)
    t->priority = PRI_MAX;
}

// 公平调度更新时间片
void thread_fair_update_ticks(void) {
  struct thread* current_thread = thread_current();
  if (current_thread == idle_thread)
    return;

  current_thread->ticks++;
  
  // 时间片用完，重新调度
  if (current_thread->ticks >= current_thread->priority) {
    current_thread->ticks = 0;
    if (!intr_context())
      thread_yield();
    else
      intr_yield_on_return();
  }
}

uint32_t thread_stack_ofs = offsetof(struct thread, stack);