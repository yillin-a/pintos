#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

struct bitmap;

void inode_init(void);
/* 修改：增加 is_dir 参数 */
bool inode_create(block_sector_t, off_t, bool is_dir);
struct inode* inode_open(block_sector_t);
struct inode* inode_reopen(struct inode*);
block_sector_t inode_get_inumber(const struct inode*);
void inode_close(struct inode*);
void inode_remove(struct inode*);
off_t inode_read_at(struct inode*, void*, off_t size, off_t offset);
off_t inode_write_at(struct inode*, const void*, off_t size, off_t offset);
void inode_deny_write(struct inode*);
void inode_allow_write(struct inode*);
off_t inode_length(const struct inode*);

/* 新增：判断是否为目录 */
bool inode_is_dir(const struct inode*);
/* 新增：获取该 inode 是否被标记为移除 (用于调试或检查) */
bool inode_is_removed(const struct inode*);

#endif /* filesys/inode.h */