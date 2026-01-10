#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h> /* 修复：uint32_t 需要此头文件 */

void syscall_init(void);

//Including the two new functions we created for verifying pointers
bool is_valid_ptr(char* ptr);
bool is_valid_int(uint32_t* ptr);

#endif /* userprog/syscall.h */