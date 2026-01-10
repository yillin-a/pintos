#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

/* 别名定义，匹配 process.c 中的用法 */
typedef struct list fd_list;
typedef struct list thread_metainfo_list;

/* 文件描述符结构体 */
typedef struct thread_FD {
    int fd;
    struct file* file_struct;
    const char* file_name;     /* 修改：改为 const char* 以匹配 process.c 的用法 */
    struct file* open_file;    /* 修改：process.c 中使用的是 open_file */
    struct list_elem elem;
} thread_FD;

/* 线程元信息结构体（父子共享） */
typedef struct thread_metainfo {
    tid_t tid;
    int exit_code;
    bool is_loaded;
    int share_count;
    struct semaphore load;
    struct semaphore finish;
    struct lock tlock;
    void* file_copy; /* 用于传递 cmd_line */
    struct list_elem elem;
} thread_metainfo;

/* 进程控制块 (PCB) */
struct process {
    struct thread* main_thread;
    char process_name[16];
    uint32_t* pagedir;
    struct file* executable;
    fd_list* fd_blocks;
    int fd_next;
    thread_metainfo_list* child_thread;
    struct lock* global_prog_lock;
};

/* 基本进程函数 */
void userprog_init(void); /* 新增：process.c 中有定义 */
tid_t process_execute(const char* file_name);
int process_wait(tid_t);
void process_exit(int status); /* 修改：参数匹配 process.c */
void process_activate(void);

/* 辅助函数声明 (解决 missing prototypes 警告) */
void init_fd_list(fd_list* list);
thread_FD* get_fd_struct(fd_list* list, int target_fd);
thread_FD* get_fd_by_name(fd_list* list, const char* fname);
thread_FD* register_fd(fd_list* list, int fd_id, struct file* file_ptr, const char* name);
int process_close_file_wrapper(int fd_id);

/* Project 2 预留接口 */
typedef void stub_fun(void);
typedef void pthread_fun(void*);
bool setup_thread(void (**eip)(void), void** esp);
tid_t pthread_execute(stub_fun sf, pthread_fun tf, void* arg);
tid_t pthread_join(tid_t tid);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */