/*
 * dos3.c
 *
 * Functions for accessing Atari DOS 3 file systems.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 *
 * Reference:
 *  https://forums.atariage.com/topic/102915-dos-3-history/page/3/#comment-4998383
 *
 *  Boot sectors: 1-15 (only uses 9)
 *  Directory: 16-23
 *  VTOC: 24
 *  Data blocks: 25-32,33-40,...,713-720,...,1033-1040
 *
 */

#define FUSE_USE_VERSION 30
#include <fuse3/fuse.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <linux/fs.h> // RENAME_NOREPLACE
#include <time.h>
#include "atrfs.h"

/*
 * Macros and defines
 */
#define CLUSTER(n) SECTOR((n)*8+25) // 1K blocks starting with '0' at sector 25

/*
 * File System Structures
 */
struct dos3_dir_head {
   unsigned char zeros[14];
   unsigned char blocks_free; // ((720-24)/8)==87 for SD, 127 for ED
   unsigned char dos_id; // 0xA5
};

struct dos3_dir_entry {
   unsigned char status; // Bit 7 valid, bit 6 exists, bit 1 locked, bit 0 open
   unsigned char file_name[8];
   unsigned char file_ext[3];
   unsigned char blocks;
   unsigned char start;
   unsigned char file_size[2]; // Low bytes of size for computing EOF in final block
};

enum dos3_dir_status {
   FLAGS_OPEN    = 0x01, // Open for write
   FLAGS_LOCKED  = 0x02,
   FLAGS_IN_USE  = 0x40, // This is a file
   FLAGS_ACTIVE  = 0x80, // Entry has been used
   // Deleted is ACTIVE but not IN_USE
};

// struct dos3_vtoc; // This is just 128 bytes for each cluster
enum dos3_vtoc_entry {
   // 00--7F: Link to next block in file
   // 80--FC: Reserved value
   VTOC_EOF   = 0xFD,
   VTOC_FREE  = 0xFE,
   VTOC_RESERVED = 0xFF, // Block is excluded from use, e.g., beyond end of disk for SD
};

/*
 * Function prototypes
 */
int dos3_sanity(void);
int dos3_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int dos3_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int dos3_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dos3_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dos3_mkdir(const char *path,mode_t mode);
int dos3_rmdir(const char *path);
int dos3_unlink(const char *path);
int dos3_rename(const char *path1, const char *path2, unsigned int flags);
int dos3_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos3_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos3_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int dos3_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);
int dos3_statfs(const char *path, struct statvfs *stfsbuf);
int dos3_newfs(void);
char *dos3_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops dos3_ops = {
   .name = "Atari DOS 3",
   .fs_sanity = dos3_sanity,
   .fs_getattr = dos3_getattr,
   .fs_readdir = dos3_readdir,
   .fs_read = dos3_read,
   // .fs_write = dos3_write,
   // .fs_mkdir = dos3_mkdir,
   // .fs_rmdir = dos3_rmdir,
   // .fs_unlink = dos3_unlink,
   // .fs_rename = dos3_rename,
   // .fs_chmod = dos3_chmod,
   // .fs_create = dos3_create,
   // .fs_truncate = dos3_truncate,
   // .fs_utimens = dos3_utimens,
   // .fs_statfs = dos3_statfs,
   // .fs_newfs = dos3_newfs,
   // .fs_fsinfo = dos3_fsinfo,
};

/*
 * Functions
 */


/*
 * dos3_sanity()
 *
 * Return 0 if this is a valid Atari DOS 3 file system
 */
int dos3_sanity(void)
{
   if ( atrfs.sectorsize != 128 ) return 1; // Must be SD
   struct sector1 *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 9 ) return 1; // Nice to have a different number
   // FIXME: Would be nice to have a little more structure verification on sector 1
   atrfs.readonly = 1; // FIXME: No write support yet
   struct dos3_dir_head *head = SECTOR(16);
   unsigned char zeros[14];
   memset(zeros,0,sizeof(zeros));
   if ( memcmp(zeros,head->zeros,sizeof(zeros)) != 0 ) return 1;
   if ( head->dos_id != 0xA5 ) return 1;
   return 0;
}


// Implement these
int dosxe_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int dosxe_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int dosxe_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dosxe_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dosxe_mkdir(const char *path,mode_t mode);
int dosxe_rmdir(const char *path);
int dosxe_unlink(const char *path);
int dosxe_rename(const char *path1, const char *path2, unsigned int flags);
int dosxe_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int dosxe_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int dosxe_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int dosxe_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);
int dosxe_statfs(const char *path, struct statvfs *stfsbuf);
int dosxe_newfs(void);
char *dosxe_fsinfo(void);

/*
 * Temporary
 */

int dos3_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
   (void)fi;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   if ( strcmp(path,"/") == 0 )
   {
      stbuf->st_ino = 0x10000;
      stbuf->st_size = 0;
      stbuf->st_mode = MODE_DIR(stbuf->st_mode);
      stbuf->st_mode = MODE_RO(stbuf->st_mode);
      return 0;
   }
   if ( strncmp(path,"/.block",sizeof("/.block")-1) == 0 )
   {
      int sec = atoi(path+sizeof("/.block")-1);
      if ( sec >= 0 && sec*8+25+7<=atrfs.sectors )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = 1024;
         stbuf->st_ino = sec*8+25;
         return 0; // Good, can read this sector
      }
   }
   // Presumably the dummy files, but who cares?
   stbuf->st_ino = 0x10001;
   stbuf->st_size = 0;
   return 0; // Whatever, don't really care
}

int dos3_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
   (void)path; // Always "/"
   (void)offset;
   (void)fi;
   (void)flags;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   filler(buf, "DOS_3_DISK_IMAGE", NULL, 0, 0);
   filler(buf, "NOT_YET_SUPPORTED", NULL, 0, 0);
   filler(buf, "USE_.block#_for_raw_nonzero_data_blocks", NULL, 0, 0);
   filler(buf, "USE_.sector#_for_raw_dir_and_vtoc_sectors", NULL, 0, 0);

#if 1 // This will work even if we skip the entries
   // Create .sector4 ... .sector24 as appropriate
   {
      struct sector1 *s1 = SECTOR(1);
      unsigned char *zero = calloc(1,atrfs.sectorsize);
      if ( !zero ) return -ENOMEM; // Weird
      char name[32];
      int digits = 2;
      for (int sec=s1->boot_sectors + 1; sec<=atrfs.sectors && sec< 25; ++sec)
      {
         unsigned char *s = SECTOR(sec);
         if ( memcmp(s,zero,atrfs.sectorsize) == 0 ) continue; // Skip empty sectors
         sprintf(name,".sector%0*d",digits,sec);
         filler(buf,name,NULL,0,0);
      }
      free(zero);
   }
#endif
#if 1 // Data clusters
   // Create .block001 ... .block126 as appropriate
   {
      unsigned char *zero = calloc(1,1024);
      if ( !zero ) return -ENOMEM; // Weird
      char name[32];
      int digits = 3;
      for (int sec=0; sec*8+25+7<=atrfs.sectors; ++sec)
      {
         unsigned char *s = CLUSTER(sec);
         if ( memcmp(s,zero,1024) == 0 ) continue; // Skip empty sectors
         sprintf(name,".block%0*d",digits,sec);
         filler(buf,name,NULL,0,0);
      }
      free(zero);
   }
#endif
   return 0;
}

int dos3_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;

   // Magic /.block### files
   if ( strncmp(path,"/.block",sizeof("/.block")-1) == 0 )
   {
      int sec = atoi(path+sizeof("/.block")-1);
      if ( sec < 0 || sec*8+25+7>atrfs.sectors ) return -ENOENT;

      int bytes = 1024;
      if (offset >= bytes ) return -EOF;
      unsigned char *s = CLUSTER(sec);
      bytes -= offset;
      s += offset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(buf,s,bytes);
      return bytes;
   }

   // Regular files
   // FIXME
   return -ENOENT;
}
