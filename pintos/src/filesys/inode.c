#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t start; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  uint32_t is_dir;      /* 新增：1 表示目录，0 表示文件 */
  uint32_t unused[124]; /* 减少 1 个 uint32_t 以保持结构体大小不变 */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

/* Returns the block device sector that contains byte offset POS
   within INODE. */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

static struct list open_inodes;

void inode_init(void) { list_init(&open_inodes); }

/* 修改：实现带有 is_dir 参数的创建 */
bool inode_create(block_sector_t sector, off_t length, bool is_dir) {
  struct inode_disk* disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->is_dir = is_dir ? 1 : 0; // 设置目录标记
    
    if (free_map_allocate(sectors, &disk_inode->start)) {
      block_write(fs_device, sector, disk_inode);
      if (sectors > 0) {
        static char zeros[BLOCK_SECTOR_SIZE];
        size_t i;
        for (i = 0; i < sectors; i++)
          block_write(fs_device, disk_inode->start + i, zeros);
      }
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;

  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read(fs_device, inode->sector, &inode->data);
  return inode;
}

struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

void inode_close(struct inode* inode) {
  if (inode == NULL)
    return;

  if (--inode->open_cnt == 0) {
    list_remove(&inode->elem);

    if (inode->removed) {
      free_map_release(inode->sector, 1);
      free_map_release(inode->data.start, bytes_to_sectors(inode->data.length));
    }

    free(inode);
  }
}

void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t* bounce = NULL;

  while (size > 0) {
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;
    int chunk_size = size < min_left ? size : min_left;
    
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      block_read(fs_device, sector_idx, buffer + bytes_read);
    } else {
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL) break;
      }
      block_read(fs_device, sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);
  return bytes_read;
}

off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t* bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) {
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;
    int chunk_size = size < min_left ? size : min_left;
    
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      block_write(fs_device, sector_idx, buffer + bytes_written);
    } else {
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL) break;
      }
      if (sector_ofs > 0 || chunk_size < sector_left)
        block_read(fs_device, sector_idx, bounce);
      else
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      block_write(fs_device, sector_idx, bounce);
    }

    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);
  return bytes_written;
}

void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

off_t inode_length(const struct inode* inode) { return inode->data.length; }

/* 新增实现 */
bool inode_is_dir(const struct inode* inode) {
    return inode->data.is_dir != 0;
}

bool inode_is_removed(const struct inode* inode) {
    return inode->removed;
}