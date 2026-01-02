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

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp);

// 对file syscalls建立一个全局的锁
struct lock global_prog_lock;

// 初始化一个新的文件描述其器表
void init_fdtable(fd_list* fdlist) {
  if (fdlist == NULL)return;
  list_init(fdlist);
}

// 传入表格和描述器编号，返回相应描述器的指针，如果在表格中没有相应的描述器，则返回NULL
thread_FD* find_fd(fd_list* fdlist, int fd) {
  if (fdlist == NULL) return NULL;

  struct list_elem* e;
  for (e = list_begin(fdlist); e != list_end(fdlist); e = list_next(e)) {
    struct FD* curr_fd = list_entry(e, struct FD, elem);
    if (curr_fd->fd == fd) return curr_fd;
  }
  
  return NULL;
}

// 传入表格和文件名，返回相应描述器的指针，如果在表格中没有相应的描述器，则返回NULL
thread_FD* FD_find_filename(fd_list* fdlist, const char* name) {
  if (fdlist == NULL) return NULL;
  if (name == NULL) return NULL;

  struct list_elem* e;
  for (e = list_begin(fdlist); e != list_end(fdlist); e = list_next(e)) {
    struct FD* curr_fd = list_entry(e, struct FD, elem);
    if (strcmp(curr_fd->file_name, name) == 0) return curr_fd;
  }

  return NULL;
}

//在描述器表格中添加一个描述器
thread_FD* add_FD(fd_list* fdlist, int fd, struct file* curfile,
                          const char* name) {
  if (fdlist == NULL) return NULL;
  if (curfile == NULL) return NULL;
  thread_FD* new_FD = (thread_FD*)malloc(sizeof(thread_FD));
  if (new_FD == NULL) return NULL;

  // 拷贝结构体中的三个变量
  new_FD->fd = fd;
  new_FD->file_name = (const char*)malloc(sizeof(char) * (strlen(name) + 1));
  strlcpy((char*)new_FD->file_name, name, strlen(name) + 1);
  new_FD->open_file = curfile;

  list_insert(list_end(fdlist), &(new_FD->elem));
  return new_FD;
}

// 系统调用关闭过程的实现，process_exit可以访问它
int process_file_close(int fd_num) {
  struct thread* t = thread_current();

  // 获取全局的文件系统访问锁，保证原子操作
  lock_acquire(t->pcb->global_prog_lock);

  // 通过描述器的编号访问相应的描述器
  int fd = fd_num;
  thread_FD* found_descriptor = find_fd(t->pcb->fd_blocks, fd);

  if (found_descriptor == NULL) {
    lock_release(t->pcb->global_prog_lock);
    return (-1);
  } else {
    //清空文件、文件指针、列表元素，清空描述器
    file_close(found_descriptor->open_file);
    free((void*)found_descriptor->file_name);
    list_remove(&(found_descriptor->elem));
    free((void*)found_descriptor);
    lock_release(t->pcb->global_prog_lock);
    return (0);
  }
}

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  if (success) {
    t->pcb->fd_blocks = (fd_list*)malloc(sizeof(fd_list));

    init_fdtable(t->pcb->fd_blocks); // 初始化链表
    t->pcb->fd_next = 2;

    t->pcb->child_thread = (thread_metainfo_list*)malloc(sizeof(thread_metainfo_list)); // 初始化子进程链表
    list_init(t->pcb->child_thread);

    lock_init(&global_prog_lock); // 初始化全局锁
    t->pcb->global_prog_lock = &global_prog_lock;

    t->shared = (thread_metainfo*)malloc(sizeof(thread_metainfo));
    t->shared->share_count = 1;   // 程序执行的开始，只有一个活动的线程
    t->shared->tid = t->tid;      // 进程中当前正在运行的线程
    lock_init(&(t->shared->tlock));
    sema_init(&(t->shared->finish), 0);
    t->shared->is_loaded = false;
  }

  /* Kill the kernel if we did not succeed */
  ASSERT(success);
}

/* 启动一个新线程，运行从 FILENAME 加载的用户程序。
   新线程可能在 process_execute() 返回之前被调度（甚至退出）。
   返回新进程的进程 ID，如果无法创建线程则返回 TID_ERROR。 */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;
  struct thread* t = thread_current();

  /* 复制 FILE_NAME，以避免调用者和 load() 之间的竞争条件 */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL) return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  /* 分配并初始化线程元数据 */
  thread_metainfo* data = (thread_metainfo*)malloc(sizeof(thread_metainfo));
  data->file_copy = fn_copy; // 为线程启动作准备
  sema_init(&(data->load), 0);

  /* 创建一个新线程来执行 FILE_NAME */
  tid = thread_create(file_name, PRI_DEFAULT, start_process, (void*)data);
  sema_down(&(data->load));

  if (data->is_loaded && tid != TID_ERROR) {
    list_insert(list_end(t->pcb->child_thread), &(data->elem));
    return tid;
  } else {
    free(data);
    palloc_free_page(fn_copy);
    return -1;
  }
}

/* 一个线程函数，用于加载用户进程并开始运行 */
static void start_process(void* newmetadata) {
  thread_metainfo* data = (thread_metainfo*)newmetadata;
  char* file_name = (char*)data->file_copy;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* 分配进程控制块 */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* 初始化进程控制块 */
  if (success) {
    // 确保 timer_interrupt() -> schedule() -> process_activate()
    // 不会尝试激活未初始化的页目录
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    // 继续初始化进程控制块
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);

    t->pcb->fd_blocks = (fd_list*)malloc(sizeof(fd_list)); // 分配文件描述符列表
    init_fdtable(t->pcb->fd_blocks);   // 初始化文件描述符列表
    t->pcb->fd_next = 2;

    t->pcb->child_thread = (thread_metainfo_list*)malloc(sizeof(thread_metainfo_list));
    list_init(t->pcb->child_thread);
    t->shared = data;

    lock_init(&global_prog_lock);         // 初始化全局锁
    t->pcb->global_prog_lock = &global_prog_lock;

    // 初始化共享结构体的元素
    t->shared->share_count = 2;
    t->shared->tid = t->tid;
    lock_init(&(t->shared->tlock));
    sema_init(&(t->shared->finish), 0);
    t->shared->is_loaded = false;
  }

  /* 初始化中断帧并加载可执行文件 */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    /* 保存当前 FPU 的值 */
    asm volatile("fsave %0" : "=m"(if_.fpu));
    success = load(file_name, &if_.eip, &if_.esp);
  }

  /* 处理加载失败的情况 */
  if (!success && pcb_success) {
    // 避免在 t->pcb 被设置为 NULL 之前释放 PCB
    struct process* pcb_to_free = t->pcb;
    // 关闭可执行文件、释放子线程列表、释放文件描述符表
    file_close(t->pcb->executable);
    free(t->pcb->child_thread);
    free(t->pcb->fd_blocks);
    t->pcb = NULL;
    free(pcb_to_free);
  }

  if (!success) {
    // 通知父进程子进程加载失败
    sema_up(&(t->shared->finish));
    thread_exit();
  }

  /* 清理。失败时退出或跳转到用户空间 */
  palloc_free_page(file_name);

  /* 通过模拟中断返回开始用户进程，由 intr_exit (在 threads/intr-stubs.S) 实现。
     因为 intr_exit 通过堆栈接收参数，我们只需将堆栈指针 (%esp) 指向我们的堆栈帧并跳转 */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {
  struct thread* t = thread_current();
  struct list_elem* e;
  //iterate through child list and search for child with a given pid
  if (!list_empty(t->pcb->child_thread)) {
    for (e = list_begin(t->pcb->child_thread); e != list_end(t->pcb->child_thread); e = list_next(e)) {
      thread_metainfo* cur_md = list_entry(e, thread_metainfo, elem);
      if (cur_md->tid == child_pid) {
        //wait till child has finished by downing finish semaphore
        sema_down(&(cur_md->finish));
        //store exitcode from child's meta_info
        int exit = cur_md->exitcode;
        //acquire lock to decrement share_count to avoid race conditions
        lock_acquire(&(cur_md->tlock));
        cur_md->share_count -= 1;
        int temp = cur_md->share_count;
        lock_release(&(cur_md->tlock));
        //remove waited child from parent's child list
        list_remove(e);
        //if share_count is 0, free meta_info
        if (temp == 0) free(cur_md);
        //since we have a unique exit code for errors, we want to ensure that if -2 is returned, we turn it into -1 so it is handled properly.
        if (exit == -2) return -1;
        return exit;
      }
    }
  }
  return -1;
}

/* Free the current process's resources. */
//Changed process exit to take an exit code as an input
void process_exit(int exit_code) {
  struct thread* cur = thread_current();
  uint32_t* pd;
  //store exit code in thread's meta_info
  cur->shared->exitcode = exit_code;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  } else if (exit_code == -2) {
    //printing the exit code if we exited due to an error. Set to -2 because only excpetion should return
    //an exit code of -2
    printf("%s: exit(%d)\n", thread_current()->pcb->process_name, exit_code + 1);
  }

  //close the executable before we free the pcb
  file_close(cur->pcb->executable);

  //Iterate through the file descriptor list and close all of the file descriptors that are still open
  while (!list_empty(cur->pcb->fd_blocks)) {
    thread_FD* fd_entry = list_entry(list_begin(cur->pcb->fd_blocks), thread_FD, elem);
    process_file_close(fd_entry->fd);
  }
  //free the file descriptor list from the heap
  free((void*)cur->pcb->fd_blocks);

  //**********************************
  lock_acquire(&(cur->shared->tlock));
  cur->shared->share_count -= 1;
  int temp = cur->shared->share_count;
  lock_release(&(cur->shared->tlock));

  if (temp == 0) {
    free(cur->shared);
  }

  //iterate through all children and decrement their shared data's share_count
  struct list_elem* temp_next, *e;
  if (!list_empty(cur->pcb->child_thread)) {
    for (e = list_begin(cur->pcb->child_thread); e != list_end(cur->pcb->child_thread);) {
      thread_metainfo* cur_md = list_entry(e, thread_metainfo, elem);
      //acquire lock to decrement share_count to avoid race conditions and free if 0
      lock_acquire(&(cur_md->tlock));
      cur_md->share_count -= 1;
      int temp = cur_md->share_count;
      lock_release(&(cur_md->tlock));
      temp_next = list_next(e);
      if (temp == 0) free(cur_md);
      e = temp_next;
    }
  }
  //free threads list struct of children
  free((void*)cur->pcb->child_thread);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);
  //up on finish semaphore so waiting parent can continue
  sema_up(&(cur->shared->finish));

  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
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

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
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

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* 加载 ELF 可执行文件到当前线程中。
   将可执行文件的入口地址存储到 *EIP 中，
   并将初始栈指针存储到 *ESP 中。
   成功返回 true，失败返回 false。 */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* current_thread = thread_current();
  struct Elf32_Ehdr elf_header;
  struct file* executable_file = NULL;
  off_t file_offset;
  bool success = false;

  // 复制文件名并解析参数
  char* file_copy = strdup(file_name);
  int argument_count = count_arguments(file_name);
  char* arguments[argument_count + 1];
  parse_arguments(file_copy, arguments, argument_count);

  if (argument_count == 0) {
    printf("未提供任何参数。\n");
    free(file_copy);
    return false;
  }

  // 设置进程名称为第一个参数（通常是可执行文件名）
  strlcpy(current_thread->pcb->process_name, arguments[0], strlen(arguments[0]) + 1);

  // 分配并激活页目录
  current_thread->pcb->pagedir = pagedir_create();
  if (current_thread->pcb->pagedir == NULL)
    goto cleanup;

  process_activate();

  // 打开可执行文件
  executable_file = filesys_open(arguments[0]);
  if (executable_file == NULL) {
    printf("load: %s: 文件打开失败\n", file_name);
    goto cleanup;
  }
  current_thread->pcb->executable = executable_file;
  file_deny_write(executable_file);

  // 读取并验证 ELF 文件头
  if (!read_and_validate_elf_header(executable_file, &elf_header)) {
    printf("load: %s: 加载可执行文件时出错\n", file_name);
    goto cleanup;
  }

  // 读取程序头
  file_offset = elf_header.e_phoff;
  for (int i = 0; i < elf_header.e_phnum; i++) {
    struct Elf32_Phdr program_header;
    if (!read_program_header(executable_file, file_offset, &program_header))
      goto cleanup;
    file_offset += sizeof(program_header);

    if (!handle_program_header(&program_header, executable_file, current_thread))
      goto cleanup;
  }

  // 设置栈
  if (!setup_stack(esp))
    goto cleanup;

  // 设置入口地址
  *eip = (void (*)(void))elf_header.e_entry;

  // 将参数逆序压入栈中
  push_arguments_to_stack(arguments, argument_count, esp);

  // 栈对齐
  align_stack(esp, argument_count);

  // 压入参数指针、argc 和假返回地址
  push_argument_pointers(esp, arguments, argument_count);

  success = true;

cleanup:
  if (!success) {
    free(file_copy);
    for (int i = 0; i < argument_count; i++) {
      free(arguments[i]);
    }
  }

  // 将加载结果存入线程的共享数据中
  current_thread->shared->is_loaded = success;
  sema_up(&(current_thread->shared->load));
  return success;
}

// 辅助函数定义

// 计算传入参数的数量
static int count_arguments(const char* file_name) {
  int num_spaces = 0;
  bool last_space = false;
  for (unsigned i = 0; i < strlen(file_name); i++) {
    if (file_name[i] == ' ' && !last_space) {
      num_spaces++;
      last_space = true;
    } else {
      last_space = false;
    }
  }
  return num_spaces + 1;
}

// 解析命令行参数并保存到数组中
static void parse_arguments(char* file_copy, char* arguments[], int argument_count) {
  char* saveptr = NULL;
  int counter = 0;
  for (char* token = strtok_r(file_copy, " ", &saveptr); token != NULL; token = strtok_r(NULL, " ", &saveptr)) {
    arguments[counter++] = strdup(token);
  }
}

// 读取并验证 ELF 文件头
static bool read_and_validate_elf_header(struct file* file, struct Elf32_Ehdr* header) {
  if (file_read(file, header, sizeof(*header)) != sizeof(*header) ||
      memcmp(header->e_ident, "\177ELF\1\1\1", 7) || header->e_type != 2 || header->e_machine != 3 ||
      header->e_version != 1 || header->e_phentsize != sizeof(struct Elf32_Phdr) || header->e_phnum > 1024) {
    return false;
  }
  return true;
}

// 读取程序头
static bool read_program_header(struct file* file, off_t offset, struct Elf32_Phdr* header) {
  file_seek(file, offset);
  return file_read(file, header, sizeof(*header)) == sizeof(*header);
}

// 处理程序头
static bool handle_program_header(struct Elf32_Phdr* header, struct file* file, struct thread* thread) {
  switch (header->p_type) {
    case PT_NULL:
    case PT_NOTE:
    case PT_PHDR:
    case PT_STACK:
      return true;
    case PT_DYNAMIC:
    case PT_INTERP:
    case PT_SHLIB:
      return false;
    case PT_LOAD:
      if (validate_segment(header, file)) {
        bool writable = (header->p_flags & PF_W) != 0;
        uint32_t file_page = header->p_offset & ~PGMASK;
        uint32_t mem_page = header->p_vaddr & ~PGMASK;
        uint32_t page_offset = header->p_vaddr & PGMASK;
        uint32_t read_bytes, zero_bytes;
        if (header->p_filesz > 0) {
          read_bytes = page_offset + header->p_filesz;
          zero_bytes = (ROUND_UP(page_offset + header->p_memsz, PGSIZE) - read_bytes);
        } else {
          read_bytes = 0;
          zero_bytes = ROUND_UP(page_offset + header->p_memsz, PGSIZE);
        }
        return load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable);
      }
      return false;
    default:
      return false;
  }
}

// 将参数逆序压入栈中
static void push_arguments_to_stack(char* arguments[], int argument_count, void** esp) {
  for (int k = argument_count - 1; k >= 0; k--) {
    *esp -= strlen(arguments[k]) + 1;
    memcpy(*esp, arguments[k], strlen(arguments[k]) + 1);
    arguments[k] = *esp;
  }
}

// 栈对齐
static void align_stack(void** esp, int argument_count) {
  int numpointers = 3 + argument_count;
  int amount_to_decrement = ((unsigned)*esp - numpointers * 4) % 16;
  *esp -= amount_to_decrement;
}

// 压入参数指针、argc 和假返回地址
static void push_argument_pointers(void** esp, char* arguments[], int argument_count) {
  // 压入参数地址（逆序）
  for (int k = argument_count; k >= 0; k--) {
    *esp -= 4;
    *(uint32_t*)*esp = (int)(uint32_t*)arguments[k];
  }

  // 压入 **argv
  *esp -= 4;
  *(uint32_t*)*esp = (int)(*esp + 4);

  // 压入 argc
  *esp -= 4;
  *(uint32_t*)*esp = argument_count;

  // 压入假返回地址
  *esp -= 4;
  *(uint32_t*)*esp = 0;
}


/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) { return -1; }

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}
