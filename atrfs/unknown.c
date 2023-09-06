/*
 * unknown.c
 *
 * Functions for accessing images with unknown formats.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
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

/*
 * File System Structures
 */

/*
 * Function prototypes
 */
int unknown_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int unknown_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int unknown_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);

/*
 * Global variables
 */
const struct fs_ops unknown_ops = {
   .name = "Unknown Disk Format",
   // .fs_sanity = unknown_sanity,
   .fs_getattr = unknown_getattr,
   .fs_readdir = unknown_readdir,
   .fs_read = unknown_read,
   // .fs_write = unknown_write,
   // .fs_mkdir = unknown_mkdir,
   // .fs_rmdir = unknown_rmdir,
   // .fs_unlink = unknown_unlink,
   // .fs_rename = unknown_rename,
   // .fs_chmod = unknown_chmod,
   // .fs_create = unknown_create,
   // .fs_truncate = unknown_truncate,
   // .fs_utimens = unknown_utimens,
   // .fs_statfs = unknown_statfs,
   // .fs_newfs = unknown_newfs,
   // .fs_fsinfo = unknown_fsinfo,
};

/*
 * Functions
 */
int unknown_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
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
   if ( strncmp(path,"/.sector",sizeof("/.sector")-1) == 0 )
   {
      int sec = atoi(path+sizeof("/.sector")-1);
      if ( sec > 0 && sec <= atrfs.sectors )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = 128;
         stbuf->st_ino = sec;
         if ( !atrfs.ssbytes || sec > 3 ) stbuf->st_size = atrfs.sectorsize;
         return 0; // Good, can read this sector
      }
   }
   stbuf->st_ino = 0x10001;
   stbuf->st_size = 0;
   return 0; // Whatever, don't really care
}

int unknown_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
   (void)path; // Always "/"
   (void)offset;
   (void)fi;
   (void)flags;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   filler(buf, "UNKNOWN_DISK_FORMAT", NULL, 0, 0);
   filler(buf, "USE_.sector#_for_raw_nonzero_sectors", NULL, 0, 0);

#if 1 // This will work even if we skip the entries
   // Create .sector4 ... .sector720 as appropriate
   struct sector1 *s1 = SECTOR(1);
   unsigned char *zero = calloc(1,atrfs.sectorsize);
   if ( !zero ) return -ENOMEM; // Weird
   char name[32];
   int digits = sprintf(name,"%d",atrfs.sectors);
   for (int sec=s1->boot_sectors + 1; sec<=atrfs.sectors; ++sec)
   {
      unsigned char *s = SECTOR(sec);
      if ( memcmp(s,zero,atrfs.sectorsize) == 0 ) continue; // Skip empty sectors
      sprintf(name,".sector%0*d",digits,sec);
      filler(buf,name,NULL,0,0);
   }
   free(zero);
#endif
   return 0;
}

int unknown_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;
   if ( strncmp(path,"/.sector",sizeof("/.sector")-1) != 0 ) return -ENOENT;

   int sec = atoi(path+sizeof("/.sector")-1);
   if ( sec <= 0 || sec > atrfs.sectors ) return -ENOENT;

   int bytes = 128;
   if ( !atrfs.ssbytes || sec > 3 ) bytes = atrfs.sectorsize;

   if (offset >= bytes ) return -EOF;
   unsigned char *s = SECTOR(sec);
   bytes -= offset;
   s += offset;
   if ( (size_t)bytes > size ) bytes = size;
   memcpy(buf,s,bytes);
   return bytes;
}
