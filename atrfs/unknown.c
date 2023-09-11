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

#include FUSE_INCLUDE
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
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
int unknown_getattr(const char *path, struct stat *stbuf);
int unknown_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);

/*
 * Global variables
 */
const struct fs_ops unknown_ops = {
   .name = "Unknown Disk Format",
   // .fs_sanity = unknown_sanity,
   .fs_getattr = unknown_getattr,
   .fs_readdir = unknown_readdir,
   // .fs_read = unknown_read,
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
int unknown_getattr(const char *path, struct stat *stbuf)
{
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   if ( strcmp(path,"/") == 0 )
   {
      stbuf->st_ino = 0x10000;
      stbuf->st_size = 0;
      stbuf->st_mode = MODE_DIR(stbuf->st_mode);
      stbuf->st_mode = MODE_RO(stbuf->st_mode);
      return 0;
   }
   // Presumably the dummy files, but who cares?
   stbuf->st_ino = 0x10001;
   stbuf->st_size = 0;
   return 0; // Whatever, don't really care
}

int unknown_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
   (void)path; // Always "/"
   (void)offset;
   (void)fi;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   filler(buf, "UNKNOWN_DISK_FORMAT", FILLER_NULL);
   filler(buf, "USE_.sector#_for_raw_nonzero_sectors", FILLER_NULL);

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
      filler(buf,name,FILLER_NULL);
   }
   free(zero);
#endif
   return 0;
}
