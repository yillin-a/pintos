#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h" /* 必须包含，用于访问 thread_current()->cwd */
#include "threads/malloc.h"

/* Partition that contains the file system. */
struct block* fs_device;

static void do_format(void);

/* 内部辅助函数：路径解析
   功能：给定路径 path，找到其父目录，并返回文件名。
   返回：父目录的 struct dir*。如果失败返回 NULL。
   副作用：file_name 被填充为路径的最后一部分。
   
   修改：添加 static 关键字
*/
static struct dir* parse_path(const char* path_orig, char* file_name) {
  struct dir* dir;
  
  if (path_orig == NULL || strlen(path_orig) == 0)
    return NULL;

  char* path = malloc(strlen(path_orig) + 1);
  if (path == NULL) return NULL;
  strlcpy(path, path_orig, strlen(path_orig) + 1);

  /* 1. 决定起始目录 */
  if (path[0] == '/') {
    dir = dir_open_root();
  } else {
    struct thread* t = thread_current();
    if (t->cwd == NULL) 
      dir = dir_open_root();
    else
      dir = dir_reopen(t->cwd);
  }

  if (dir == NULL) {
    free(path);
    return NULL;
  }

  /* 2. 遍历路径 */
  char *token, *next_token, *save_ptr;
  token = strtok_r(path, "/", &save_ptr);
  next_token = strtok_r(NULL, "/", &save_ptr);

  /* 如果路径本身就是 "/" 或为空 */
  if (token == NULL) {
    strlcpy(file_name, ".", NAME_MAX + 1);
    free(path);
    return dir;
  }

  while (next_token != NULL) {
    struct inode* inode = NULL;
    if (!dir_lookup(dir, token, &inode)) {
        dir_close(dir);
        free(path);
        return NULL; /* 路径中间节点不存在 */
    }

    struct dir* next_dir = dir_open(inode);
    dir_close(dir);
    if (next_dir == NULL) {
        free(path);
        return NULL; /* 路径中间节点不是目录 */
    }

    dir = next_dir;
    token = next_token;
    next_token = strtok_r(NULL, "/", &save_ptr);
  }

  /* 3. 输出文件名 */
  strlcpy(file_name, token, NAME_MAX + 1);
  free(path);
  return dir;
}

void filesys_init(bool format) {
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system found!");

  inode_init();
  free_map_init();

  if (format)
    do_format();

  free_map_open();
}

void filesys_done(void) { free_map_close(); }

/* 创建普通文件 */
bool filesys_create(const char* name, off_t initial_size) {
  char file_name[NAME_MAX + 1];
  struct dir* dir = parse_path(name, file_name);
  
  if (dir == NULL) return false;

  block_sector_t inode_sector = 0;
  /* 参数 false 表示创建文件而非目录 */
  bool success = (free_map_allocate(1, &inode_sector) &&
                  inode_create(inode_sector, initial_size, false) &&
                  dir_add(dir, file_name, inode_sector));
  
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
    
  dir_close(dir);
  return success;
}

/* 创建目录 */
bool filesys_mkdir(const char* name) {
  char file_name[NAME_MAX + 1];
  struct dir* dir = parse_path(name, file_name);
  
  if (dir == NULL) return false;

  block_sector_t inode_sector = 0;
  /* 参数 true 表示创建目录 */
  bool success = (free_map_allocate(1, &inode_sector) &&
                  dir_create(inode_sector, 16) &&
                  dir_add(dir, file_name, inode_sector));

  if (success) {
      /* 如果创建成功，需要为新目录添加 . 和 .. */
      struct dir* new_dir = dir_open(inode_open(inode_sector));
      if (new_dir) {
          dir_add(new_dir, ".", inode_sector);
          /* ".." 指向父目录的 inode sector */
          dir_add(new_dir, "..", inode_get_inumber(dir_get_inode(dir)));
          dir_close(new_dir);
      } else {
          success = false; // 理论上不应发生
      }
  }

  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
    
  dir_close(dir);
  return success;
}

struct file* filesys_open(const char* name) {
  if (strlen(name) == 0) return NULL;

  char file_name[NAME_MAX + 1];
  struct dir* dir = parse_path(name, file_name);
  struct inode* inode = NULL;

  if (dir != NULL) {
    if (strcmp(file_name, ".") == 0) {
        /* 打开目录本身 */
        inode = inode_reopen(dir_get_inode(dir));
    } else if (strcmp(file_name, "..") == 0) {
        /* 打开父目录，利用 dir_lookup 处理 */
        dir_lookup(dir, "..", &inode);
    } else {
        dir_lookup(dir, file_name, &inode);
    }
    dir_close(dir);
  }

  return file_open(inode);
}

bool filesys_remove(const char* name) {
  char file_name[NAME_MAX + 1];
  struct dir* dir = parse_path(name, file_name);
  
  bool success = dir != NULL && dir_remove(dir, file_name);
  dir_close(dir);

  return success;
}

bool filesys_chdir(const char* name) {
  /* 修复：删除未使用的变量 'dir' */
  char file_name[NAME_MAX + 1];
  
  /* 解析路径 */
  struct dir* parent = parse_path(name, file_name);
  
  if (parent != NULL) {
      struct inode* inode = NULL;
      if (strcmp(file_name, ".") == 0) {
          inode = inode_reopen(dir_get_inode(parent));
      } else if (strcmp(file_name, "..") == 0) {
          dir_lookup(parent, "..", &inode);
      } else {
          dir_lookup(parent, file_name, &inode);
      }
      
      dir_close(parent);

      if (inode != NULL) {
          /* 确保切换到的是目录 */
          if (inode_is_dir(inode)) {
              struct thread* t = thread_current();
              if (t->cwd) dir_close(t->cwd);
              t->cwd = dir_open(inode);
              return true;
          }
          inode_close(inode);
      }
  }
  return false;
}

static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  
  /* 为根目录添加 . 和 .. */
  struct dir* root = dir_open_root();
  dir_add(root, ".", ROOT_DIR_SECTOR);
  dir_add(root, "..", ROOT_DIR_SECTOR);
  dir_close(root);

  free_map_close();
  printf("done.\n");
}