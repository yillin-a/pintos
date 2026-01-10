#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* 全局文件系统锁，防止多进程同时操作文件系统导致竞争 */
struct lock sys_file_lock;

static thread_func start_process_worker NO_RETURN;
static void start_pthread_worker(void* exec_) NO_RETURN; /* 前向声明匹配 */
static bool load_executable(const char* file_name, void (**eip)(void), void** esp);

/* ------------------ 文件描述符管理辅助函数 ------------------ */

// 初始化文件描述符链表
void init_fd_list(fd_list* list) {
  if (list) {
    list_init(list);
  }
}

// 根据 FD 编号查找对应的结构体
thread_FD* get_fd_struct(fd_list* list, int target_fd) {
  if (list == NULL) return NULL;

  struct list_elem* iter = list_begin(list);
  while (iter != list_end(list)) {
    thread_FD* entry = list_entry(iter, thread_FD, elem);
    if (entry->fd == target_fd) {
      return entry;
    }
    iter = list_next(iter);
  }
  return NULL;
}

// 根据文件名查找对应的文件描述符结构体
thread_FD* get_fd_by_name(fd_list* list, const char* fname) {
  if (!list || !fname) return NULL;

  struct list_elem* iter;
  for (iter = list_begin(list); iter != list_end(list); iter = list_next(iter)) {
    thread_FD* entry = list_entry(iter, thread_FD, elem);
    if (strcmp(entry->file_name, fname) == 0) {
      return entry;
    }
  }
  return NULL;
}

// 向表中注册一个新的打开文件
thread_FD* register_fd(fd_list* list, int fd_id, struct file* file_ptr, const char* name) {
  if (!list || !file_ptr) return NULL;

  thread_FD* new_entry = (thread_FD*)malloc(sizeof(thread_FD));
  if (!new_entry) return NULL;

  // 复制文件名并设置字段
  int name_len = strlen(name) + 1;
  new_entry->file_name = (const char*)malloc(name_len);
  strlcpy((char*)new_entry->file_name, name, name_len);
  
  new_entry->fd = fd_id;
  new_entry->open_file = file_ptr;

  list_insert(list_end(list), &(new_entry->elem));
  return new_entry;
}

// 关闭指定 FD 并清理资源 (供 process_exit 使用)
int process_close_file_wrapper(int fd_id) {
  struct thread* curr = thread_current();

  // 加锁保护文件系统操作
  if (curr->pcb && curr->pcb->global_prog_lock)
      lock_acquire(curr->pcb->global_prog_lock);

  thread_FD* target = get_fd_struct(curr->pcb->fd_blocks, fd_id);

  if (target == NULL) {
    if (curr->pcb && curr->pcb->global_prog_lock)
        lock_release(curr->pcb->global_prog_lock);
    return -1;
  }

  // 释放文件资源、文件名内存及结构体内存
  file_close(target->open_file);
  free((void*)target->file_name);
  list_remove(&(target->elem));
  free(target);

  if (curr->pcb && curr->pcb->global_prog_lock)
      lock_release(curr->pcb->global_prog_lock);
  return 0;
}

/* ------------------ 进程管理核心函数 ------------------ */

/* 初始化进程控制块及相关同步原语 */
void userprog_init(void) {
  struct thread* curr = thread_current();

  /* 分配 PCB 内存，使用 calloc 确保 pagedir 初始为 NULL */
  curr->pcb = calloc(1, sizeof(struct process));
  
  if (curr->pcb == NULL) {
    PANIC("Memory allocation failed for PCB"); 
  }

  // 初始化文件描述符表
  curr->pcb->fd_blocks = (fd_list*)malloc(sizeof(fd_list));
  init_fd_list(curr->pcb->fd_blocks);
  curr->pcb->fd_next = 2; // 0 和 1 预留给标准输入输出

  // 初始化子进程链表
  curr->pcb->child_thread = (thread_metainfo_list*)malloc(sizeof(thread_metainfo_list));
  list_init(curr->pcb->child_thread);

  // 初始化全局锁
  lock_init(&sys_file_lock);
  curr->pcb->global_prog_lock = &sys_file_lock;

  // 初始化共享元数据
  curr->shared = (thread_metainfo*)malloc(sizeof(thread_metainfo));
  if (curr->shared) {
    curr->shared->share_count = 1; // 初始引用计数
    curr->shared->tid = curr->tid;
    curr->shared->is_loaded = false;
    lock_init(&(curr->shared->tlock));
    sema_init(&(curr->shared->finish), 0);
  }
}

/* 创建新线程执行用户程序 file_name */
tid_t process_execute(const char* file_name) {
  struct thread* curr = thread_current();
  char* cmd_copy;
  tid_t new_tid;

  // 拷贝命令行参数，防止竞争条件
  cmd_copy = palloc_get_page(0);
  if (cmd_copy == NULL) return TID_ERROR;
  strlcpy(cmd_copy, file_name, PGSIZE);

  // 解析第一个 token 作为线程名
  char *fn_copy = malloc(strlen(file_name) + 1);
  strlcpy(fn_copy, file_name, strlen(file_name) + 1);
  char *save_ptr;
  char *real_name = strtok_r(fn_copy, " ", &save_ptr);

  // 准备传递给新线程的元数据
  thread_metainfo* meta = (thread_metainfo*)malloc(sizeof(thread_metainfo));
  if (!meta) {
      palloc_free_page(cmd_copy);
      free(fn_copy);
      return TID_ERROR;
  }
  
  meta->file_copy = cmd_copy; 
  sema_init(&(meta->load), 0); // 用于同步加载过程

  // 创建线程
  new_tid = thread_create(real_name, PRI_DEFAULT, start_process_worker, (void*)meta);
  
  free(fn_copy);

  if (new_tid == TID_ERROR) {
      free(meta);
      palloc_free_page(cmd_copy);
      return TID_ERROR;
  }

  // 等待子线程加载完毕
  sema_down(&(meta->load));

  if (meta->is_loaded) {
    // 加载成功，加入子进程列表
    list_insert(list_end(curr->pcb->child_thread), &(meta->elem));
    return new_tid;
  } 
  
  // 加载失败，清理资源
  // 注意：meta 内存会在子进程退出时或者在此处由父进程清理，
  // 但根据引用计数逻辑，如果 start_process_worker 失败，它会处理自己的引用。
  // 此处我们需要根据协议判断谁负责释放。
  // 通常如果 is_loaded 为 false，子线程已经退出。
  // 按照 process_wait 的逻辑，我们应该在这里处理释放吗？
  // 如果子线程加载失败，它会调用 thread_exit，进而 sema_up(&finish)。
  // 但是在这里我们仅仅 sema_down(&load)。
  // 简单的做法：如果加载失败，返回 -1。
  // 子线程在加载失败时会释放它的资源吗？
  // 让我们看 start_process_worker：失败时 sema_up(&finish), thread_exit。
  // 这样父进程 process_wait 才能回收它。
  // 但这里我们不调用 process_wait，直接返回 -1。
  // 所以我们需要在这里清理掉 meta，因为我们没有把它加入 child_thread 列表。
  
  free(meta);
  return -1;
}

/* 新线程的入口函数，负责加载 ELF 并建立环境 */
static void start_process_worker(void* aux_data) {
  thread_metainfo* info = (thread_metainfo*)aux_data;
  char* cmd_line = (char*)info->file_copy;
  struct thread* curr = thread_current();
  struct intr_frame if_frame;
  bool load_ok = false;
  bool pcb_alloc_ok = false;

  // 分配并初始化 PCB
  struct process* pcb = malloc(sizeof(struct process));
  pcb_alloc_ok = (pcb != NULL);

  if (pcb_alloc_ok) {
    pcb->pagedir = NULL; // 关键：先置空
    curr->pcb = pcb;
    
    // 基础信息设置
    curr->pcb->main_thread = curr;
    strlcpy(curr->pcb->process_name, curr->name, sizeof curr->pcb->process_name);

    // 资源容器初始化
    curr->pcb->fd_blocks = malloc(sizeof(fd_list));
    init_fd_list(curr->pcb->fd_blocks);
    curr->pcb->fd_next = 2;

    curr->pcb->child_thread = malloc(sizeof(thread_metainfo_list));
    list_init(curr->pcb->child_thread);
    
    // 连接共享数据
    curr->shared = info;
    lock_init(&sys_file_lock);
    curr->pcb->global_prog_lock = &sys_file_lock;

    // 更新共享元数据状态
    curr->shared->share_count = 2; // 父子各持有一份引用
    curr->shared->tid = curr->tid;
    curr->shared->is_loaded = false;
    lock_init(&(curr->shared->tlock));
    sema_init(&(curr->shared->finish), 0);
  }

  // 设置中断帧并尝试加载 ELF
  if (pcb_alloc_ok) {
    memset(&if_frame, 0, sizeof if_frame);
    if_frame.gs = if_frame.fs = if_frame.es = if_frame.ds = if_frame.ss = SEL_UDSEG;
    if_frame.cs = SEL_UCSEG;
    if_frame.eflags = FLAG_IF | FLAG_MBS;
    
    // 保护 FPU 状态
    asm volatile("fsave %0" : "=m"(if_frame.fpu));
    
    load_ok = load_executable(cmd_line, &if_frame.eip, &if_frame.esp);
  }

  // 清理临时命令行拷贝 (由父进程 palloc)
  palloc_free_page(cmd_line);

  // 错误处理路径
  if (!load_ok) {
    if (pcb_alloc_ok) {
        // 若 PCB 已分配，需清理内部资源
        // 注意：load_executable 失败可能已经打开了 executable，需检查
        if (curr->pcb->executable) file_close(curr->pcb->executable);
        free(curr->pcb->child_thread);
        free(curr->pcb->fd_blocks);
        
        struct process* ptr = curr->pcb;
        curr->pcb = NULL; // 防止 use-after-free
        free(ptr);
    }
    
    // 通知父进程加载失败
    info->is_loaded = false;
    sema_up(&(info->load));
    
    // 退出
    thread_exit(); 
  }

  // 跳转到用户空间 (模拟中断返回)
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_frame) : "memory");
  NOT_REACHED();
}

/* 等待子进程退出并获取退出码 */
int process_wait(tid_t child_tid) {
  struct thread* curr = thread_current();
  struct list* children = curr->pcb->child_thread;
  
  if (list_empty(children)) return -1;

  struct list_elem* e;
  for (e = list_begin(children); e != list_end(children); e = list_next(e)) {
    thread_metainfo* child_info = list_entry(e, thread_metainfo, elem);
    
    if (child_info->tid == child_tid) {
      // 阻塞直到子进程结束
      sema_down(&(child_info->finish));
      
      int status = child_info->exit_code;
      
      // 安全地减少引用计数
      lock_acquire(&(child_info->tlock));
      child_info->share_count--;
      int ref_cnt = child_info->share_count;
      lock_release(&(child_info->tlock));
      
      // 从父进程列表中移除
      list_remove(e);
      
      // 如果引用归零，释放元数据
      if (ref_cnt == 0) {
        free(child_info);
      }
      
      // 处理异常退出码转换 (-2 -> -1)
      return status;
    }
  }
  
  return -1;
}

/* 进程退出清理函数 */
void process_exit(int status) {
  struct thread* curr = thread_current();
  uint32_t* pd_ptr;

  // 记录退出码
  if (curr->shared) {
      curr->shared->exit_code = status;
  }

  if (curr->pcb == NULL) {
    // 对于没有 PCB 的内核线程（非用户进程），直接退出
    thread_exit();
    NOT_REACHED();
  }

  // 打印退出日志
  printf("%s: exit(%d)\n", curr->pcb->process_name, status);

  // 1. 关闭自身可执行文件引用
  file_close(curr->pcb->executable);

  // 2. 关闭所有打开的文件描述符
  if (curr->pcb->fd_blocks) {
      while (!list_empty(curr->pcb->fd_blocks)) {
        struct list_elem* front = list_begin(curr->pcb->fd_blocks);
        thread_FD* entry = list_entry(front, thread_FD, elem);
        process_close_file_wrapper(entry->fd);
      }
      free(curr->pcb->fd_blocks);
  }

  // 3. 更新自身元数据引用计数
  if (curr->shared) {
      lock_acquire(&(curr->shared->tlock));
      curr->shared->share_count--;
      int self_ref = curr->shared->share_count;
      lock_release(&(curr->shared->tlock));

      if (self_ref == 0) {
        free(curr->shared);
      } else {
        // 通知父进程我已结束
        sema_up(&(curr->shared->finish));
      }
  }

  // 4. 断开与所有子进程的联系
  if (curr->pcb->child_thread) {
    struct list_elem *e = list_begin(curr->pcb->child_thread);
    while (e != list_end(curr->pcb->child_thread)) {
        thread_metainfo* child = list_entry(e, thread_metainfo, elem);
        struct list_elem* next_e = list_next(e);
        
        lock_acquire(&(child->tlock));
        child->share_count--;
        int child_ref = child->share_count;
        lock_release(&(child->tlock));
        
        if (child_ref == 0) {
            free(child);
        }
        e = next_e;
    }
    free(curr->pcb->child_thread);
  }

  // 5. 销毁页目录并切回内核空间
  pd_ptr = curr->pcb->pagedir;
  if (pd_ptr != NULL) {
    curr->pcb->pagedir = NULL; // 防止时钟中断访问无效页目录
    pagedir_activate(NULL);
    pagedir_destroy(pd_ptr);
  }

  // 6. 释放 PCB
  struct process* pcb_ptr = curr->pcb;
  curr->pcb = NULL;
  free(pcb_ptr);

  thread_exit();
}

/* 进程上下文切换激活函数 */
void process_activate(void) {
  struct thread* t = thread_current();

  // 激活页表
  pagedir_activate(t->pcb ? t->pcb->pagedir : NULL);

  // 更新 TSS 栈指针
  tss_update();
}

/* ------------------ ELF 加载模块 ------------------ */

// ELF 数据类型定义
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_STACK 0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_user_stack(void** esp);
static bool check_elf_segment(const struct Elf32_Phdr*, struct file*);
static bool load_elf_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);
static bool map_user_page(void* uaddr, void* kaddr, bool rw);

/* 加载 ELF 可执行文件 */
bool load_executable(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* curr = thread_current();
  struct Elf32_Ehdr elf_header;
  struct file* exec_file = NULL;
  off_t offset;
  bool is_success = false;
  int i;

  // 1. 命令行参数解析准备
  char* cmd_buf = malloc(strlen(file_name) + 1);
  if (!cmd_buf) return false;
  strlcpy(cmd_buf, file_name, strlen(file_name) + 1);

  int space_counter = 0;
  bool prev_is_space = false;

  // 估算参数个数 (通过计算空格)
  for (size_t k = 0; k < strlen(file_name); k++) {
    if (file_name[k] == ' ' && !prev_is_space) {
        space_counter++;
        prev_is_space = true;
    } else {
      prev_is_space = false;
    }
  }

  int arg_max = space_counter + 1;
  char** arg_vector = malloc((arg_max + 1) * sizeof(char*));
  if (!arg_vector) {
      free(cmd_buf);
      return false;
  }
  
  arg_vector[arg_max] = NULL;
  int arg_idx = 0;

  // 使用 strtok_r 分割参数
  char* store_ptr = NULL;
  char* token = strtok_r(cmd_buf, " ", &store_ptr);
  while (token != NULL) {
    int tok_len = strlen(token);
    arg_vector[arg_idx] = malloc(tok_len + 1);
    strlcpy(arg_vector[arg_idx], token, tok_len + 1);
    arg_idx++;
    token = strtok_r(NULL, " ", &store_ptr);
  }
  
  // 更新实际参数数量
  arg_max = arg_idx;

  if (arg_max == 0) {
    printf("load: Empty arguments\n");
    goto cleanup; 
  }

  // 设置进程名为第一个参数（可执行文件名）
  strlcpy(curr->pcb->process_name, arg_vector[0], sizeof curr->pcb->process_name);

  // 创建页目录
  curr->pcb->pagedir = pagedir_create();
  if (!curr->pcb->pagedir) goto cleanup;
  process_activate();

  // 打开文件
  exec_file = filesys_open(arg_vector[0]);
  if (!exec_file) {
    printf("load: %s: open failed\n", arg_vector[0]);
    goto cleanup;
  }
  
  // 保存文件指针并在执行期间禁止写入
  curr->pcb->executable = exec_file;
  file_deny_write(exec_file);

  // 校验 ELF 头
  if (file_read(exec_file, &elf_header, sizeof elf_header) != sizeof elf_header ||
      memcmp(elf_header.e_ident, "\177ELF\1\1\1", 7) || elf_header.e_type != 2 || 
      elf_header.e_machine != 3 || elf_header.e_version != 1 || 
      elf_header.e_phentsize != sizeof(struct Elf32_Phdr) || elf_header.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", arg_vector[0]);
    goto cleanup;
  }

  // 读取程序头表
  offset = elf_header.e_phoff;
  for (i = 0; i < elf_header.e_phnum; i++) {
    struct Elf32_Phdr ph;

    if (offset < 0 || offset > file_length(exec_file)) goto cleanup;
    
    file_seek(exec_file, offset);
    if (file_read(exec_file, &ph, sizeof ph) != sizeof ph) goto cleanup;
    
    offset += sizeof ph;

    switch (ph.p_type) {
      case PT_NULL: case PT_NOTE: case PT_PHDR: case PT_STACK:
        break; 
      case PT_DYNAMIC: case PT_INTERP: case PT_SHLIB:
        goto cleanup; 
      case PT_LOAD:
        if (check_elf_segment(&ph, exec_file)) {
          bool is_writable = (ph.p_flags & PF_W) != 0;
          uint32_t f_page = ph.p_offset & ~PGMASK;
          uint32_t m_page = ph.p_vaddr & ~PGMASK;
          uint32_t pg_ofs = ph.p_vaddr & PGMASK;
          uint32_t read_b, zero_b;

          if (ph.p_filesz > 0) {
            read_b = pg_ofs + ph.p_filesz;
            zero_b = (ROUND_UP(pg_ofs + ph.p_memsz, PGSIZE) - read_b);
          } else {
            read_b = 0;
            zero_b = ROUND_UP(pg_ofs + ph.p_memsz, PGSIZE);
          }
          
          if (!load_elf_segment(exec_file, f_page, (void*)m_page, read_b, zero_b, is_writable))
            goto cleanup;
        } else {
          goto cleanup;
        }
        break;
    }
  }

  // 建立用户栈
  if (!setup_user_stack(esp)) goto cleanup;

  // 设置入口点
  *eip = (void (*)(void))elf_header.e_entry;

  /* ------------------ 参数压栈逻辑 ------------------ */
  
  // 1. 压入所有参数字符串
  char* stack_addresses[arg_max];
  
  for (int j = arg_max - 1; j >= 0; j--) {
    size_t len = strlen(arg_vector[j]) + 1;
    *esp -= len;
    memcpy(*esp, arg_vector[j], len);
    stack_addresses[j] = *esp; 
  }

  // 2. 栈指针 4 字节对齐
  while ((uint32_t)*esp % 4 != 0) {
      *esp -= 1;
      *(uint8_t*)*esp = 0;
  }
  
  // 3. 压入 NULL 结束符
  *esp -= 4;
  *(char**)*esp = NULL;

  // 4. 压入参数指针 (argv[i])
  for (int j = arg_max - 1; j >= 0; j--) {
    *esp -= 4;
    *(char**)*esp = stack_addresses[j];
  }

  // 5. 压入 argv (指向 argv[0] 的指针)
  char** argv_ptr = (char**)*esp;
  *esp -= 4;
  *(char***)*esp = argv_ptr;

  // 6. 压入 argc
  *esp -= 4;
  *(int*)*esp = arg_max;

  // 7. 压入伪造的返回地址
  *esp -= 4;
  *(uint32_t*)*esp = 0;

  is_success = true;

cleanup:
  // 释放临时资源
  free(cmd_buf);
  for (int j = 0; j < arg_max; j++) {
    if (arg_vector[j]) free(arg_vector[j]);
  }
  free(arg_vector);

  // 同步状态到共享元数据
  curr->shared->is_loaded = is_success;
  sema_up(&(curr->shared->load));
  
  return is_success;
}

/* ------------------ 内存映射辅助函数 ------------------ */

// 检查段是否合法
static bool check_elf_segment(const struct Elf32_Phdr* ph, struct file* f) {
  if ((ph->p_offset & PGMASK) != (ph->p_vaddr & PGMASK)) return false;
  if (ph->p_offset > (Elf32_Off)file_length(f)) return false;
  if (ph->p_memsz < ph->p_filesz) return false;
  if (ph->p_memsz == 0) return false;
  
  if (!is_user_vaddr((void*)ph->p_vaddr)) return false;
  if (!is_user_vaddr((void*)(ph->p_vaddr + ph->p_memsz))) return false;

  if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) return false;
  if (ph->p_vaddr < PGSIZE) return false;

  return true;
}

// 加载段数据到内存
static bool load_elf_segment(struct file* f, off_t ofs, uint8_t* uaddr, uint32_t read_amt,
                         uint32_t zero_amt, bool rw) {
  ASSERT((read_amt + zero_amt) % PGSIZE == 0);
  ASSERT(pg_ofs(uaddr) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(f, ofs);
  
  while (read_amt > 0 || zero_amt > 0) {
    size_t chunk_read = read_amt < PGSIZE ? read_amt : PGSIZE;
    size_t chunk_zero = PGSIZE - chunk_read;

    uint8_t* frame = palloc_get_page(PAL_USER);
    if (!frame) return false;

    if (file_read(f, frame, chunk_read) != (int)chunk_read) {
      palloc_free_page(frame);
      return false;
    }
    
    memset(frame + chunk_read, 0, chunk_zero);

    if (!map_user_page(uaddr, frame, rw)) {
      palloc_free_page(frame);
      return false;
    }

    read_amt -= chunk_read;
    zero_amt -= chunk_zero;
    uaddr += PGSIZE;
  }
  return true;
}

// 建立用户栈
static bool setup_user_stack(void** esp) {
  uint8_t* frame;
  bool ok = false;

  frame = palloc_get_page(PAL_USER | PAL_ZERO);
  if (frame) {
    ok = map_user_page(((uint8_t*)PHYS_BASE) - PGSIZE, frame, true);
    if (ok)
      *esp = PHYS_BASE;
    else
      palloc_free_page(frame);
  }
  return ok;
}

// 在页表中添加映射
static bool map_user_page(void* uaddr, void* kaddr, bool rw) {
  struct thread* t = thread_current();
  
  if (pagedir_get_page(t->pcb->pagedir, uaddr) != NULL) {
      return false;
  }
  return pagedir_set_page(t->pcb->pagedir, uaddr, kaddr, rw);
}

/* ------------------ 占位函数 ------------------ */

// Project 2 预留接口
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }
static void start_pthread_worker(void* exec_ UNUSED) {}
tid_t pthread_join(tid_t tid UNUSED) { return -1; }
void pthread_exit(void) {}
void pthread_exit_main(void) {}