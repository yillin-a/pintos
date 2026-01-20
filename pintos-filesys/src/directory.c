#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"

/* A directory. */
struct dir {
  struct inode* inode; /* Backing store. */
  off_t pos;           /* Current position. */
};

/* A single directory entry. */
struct dir_entry {
  block_sector_t inode_sector; /* Sector number of header. */
  char name[NAME_MAX + 1];     /* Null terminated file name. */
  bool in_use;                 /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool dir_create(block_sector_t sector, size_t entry_cnt) {
  /* 修改：传递 true 给 is_dir 参数 */
  return inode_create(sector, entry_cnt * sizeof(struct dir_entry), true);
}

struct dir* dir_open(struct inode* inode) {
  struct dir* dir = calloc(1, sizeof *dir);
  if (inode != NULL && dir != NULL) {
    dir->inode = inode;
    dir->pos = 0;
    return dir;
  } else {
    inode_close(inode);
    free(dir);
    return NULL;
  }
}

struct dir* dir_open_root(void) {
  return dir_open(inode_open(ROOT_DIR_SECTOR));
}

struct dir* dir_reopen(struct dir* dir) {
  return dir_open(inode_reopen(dir->inode));
}

void dir_close(struct dir* dir) {
  if (dir != NULL) {
    inode_close(dir->inode);
    free(dir);
  }
}

struct inode* dir_get_inode(struct dir* dir) {
  return dir->inode;
}

static bool lookup(const struct dir* dir, const char* name, struct dir_entry* ep, off_t* ofsp) {
  struct dir_entry e;
  size_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (e.in_use && !strcmp(name, e.name)) {
      if (ep != NULL)
        *ep = e;
      if (ofsp != NULL)
        *ofsp = ofs;
      return true;
    }
  return false;
}

bool dir_lookup(const struct dir* dir, const char* name, struct inode** inode) {
  struct dir_entry e;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  if (lookup(dir, name, &e, NULL))
    *inode = inode_open(e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

bool dir_add(struct dir* dir, const char* name, block_sector_t inode_sector) {
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  if (*name == '\0' || strlen(name) > NAME_MAX)
    return false;

  if (lookup(dir, name, NULL, NULL))
    goto done;

  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (!e.in_use)
      break;

  e.in_use = true;
  strlcpy(e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/* 原始的 remove 功能被保留，但需要调用者确保逻辑正确。
   我们在 filesys_remove 中处理复杂逻辑，但也可以在这里加防线 */
bool dir_remove(struct dir* dir, const char* name) {
  struct dir_entry e;
  struct inode* inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  if (!lookup(dir, name, &e, &ofs))
    goto done;

  inode = inode_open(e.inode_sector);
  if (inode == NULL)
    goto done;
    
  /* 新增：如果删除的是目录，必须确保为空 */
  if (inode_is_dir(inode)) {
      struct dir* target = dir_open(inode);
      bool empty = dir_is_empty(target);
      dir_close(target); // 关闭 target dir wrapper，但 inode 因为被上面 inode_open 引用了，所以这里只是关掉视图
      /* 注意：dir_close 内部会 inode_close。我们需要确保 inode 仍然有效或者重新 open */
      /* Pintos 的 dir_close 会做 inode_close。所以上面的逻辑会导致 inode 引用计数减1。
         为了安全，dir_open 会增加引用计数，dir_close 减少。
         所以 target = dir_open(inode) 会调用 inode_reopen(inode)，计数+1。
         dir_close(target) 计数-1。
         原始 inode 变量保持不变。逻辑是安全的。
      */
      
      if (!empty) {
          /* 目录不为空，无法删除 */
          goto done; 
      }
  }

  e.in_use = false;
  if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e)
    goto done;

  inode_remove(inode);
  success = true;

done:
  inode_close(inode);
  return success;
}

bool dir_readdir(struct dir* dir, char name[NAME_MAX + 1]) {
  struct dir_entry e;

  while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
    dir->pos += sizeof e;
    /* 忽略 . 和 ..，使得 ls 命令不显示它们，或者根据需求决定 */
    if (e.in_use && strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0) {
      strlcpy(name, e.name, NAME_MAX + 1);
      return true;
    }
  }
  return false;
}

/* 新增实现：检查目录是否为空 */
bool dir_is_empty(struct dir* dir) {
  struct dir_entry e;
  off_t ofs;

  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e) {
    if (e.in_use) {
      if (strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0) {
        return false;
      }
    }
  }
  return true;
}