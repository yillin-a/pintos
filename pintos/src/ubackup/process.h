#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <stdint.h>
#include <list.h>

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

//typedef thread_metainfo_list to a list which acts as a Pintos list for the children's shared data
typedef struct list thread_metainfo_list;

//creating the file descriptor table struct
typedef struct FD {
  int fd;                 //file descriptor
  const char* file_name;  //name of the file
  struct file* open_file; //open file*
  struct list_elem elem;  //pintos list elem
} thread_FD;

//making a new defintion for list
typedef struct list fd_list;

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */
  //Global lock for filesystem syscalls
  struct lock* global_prog_lock;

  //Pointer to the executable so we can close it in process exit
  struct file* executable;

  /*Adding a struct list child to act as a Pintos list 
  for all the shared datas of the children in its PCB*/
  thread_metainfo_list* child_thread;

  //file descriptor table struct
  fd_list* fd_blocks;

  //File descriptor counter
  int fd_next;

};

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(int exit_code);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

/* Initialize a fd table list. */
void init_fdtable(fd_list* fdlist);

/* add a fd in a fd list. */
thread_FD* add_FD(fd_list* fdlist, int fd, struct file* curfile,
                          const char* name);

/* Find a fd in a fd list. */
thread_FD* find_fd(fd_list* fdlist, int fd);

//Find the file name in the fd table
thread_FD* FD_find_filename(fd_list* fdlist, const char* name);

//Close syscall
int process_file_close(int args);

#endif /* userprog/process.h */
