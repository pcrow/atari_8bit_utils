/*
 * generic.c
 *
 * Middle layer to call the code for whatever specific file system is being used.
 * This is useful as a separate layer when putting a file system below the main
 * directory, such as with APT images.
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
#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>
#include "atrfs.h"

/*
 * Macros and defines
 */
#define GENERIC_DEBUG_THRESHOLD 4 // Only display messages from this layer if at least this debug level

/*
 * Data types
 */

/*
 * Function prototypes
 */
int generic_readdir(struct atrfs *atrfs,const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int generic_getattr(struct atrfs *atrfs,const char *path, struct stat *stbuf);
int generic_read(struct atrfs *atrfs,const char *path, char *buf, size_t size, off_t offset);
int generic_write(struct atrfs *atrfs,const char *path, const char *buf, size_t size, off_t offset);
int generic_mkdir(struct atrfs *atrfs,const char *path,mode_t mode);
int generic_rmdir(struct atrfs *atrfs,const char *path);
int generic_unlink(struct atrfs *atrfs,const char *path);
int generic_rename(struct atrfs *atrfs,const char *path1, const char *path2, unsigned int flags);
int generic_chmod(struct atrfs *atrfs,const char *path, mode_t mode);
int generic_create(struct atrfs *atrfs,const char *path, mode_t mode);
int generic_truncate(struct atrfs *atrfs,const char *path, off_t size);
#if (FUSE_USE_VERSION >= 30)
int generic_utimens(struct atrfs *atrfs,const char *path, const struct timespec tv[2]);
#else
int generic_utime(struct atrfs *atrfs,const char *path, struct utimbuf *utimbuf);
#endif
int generic_statfs(struct atrfs *atrfs,const char *path, struct statvfs *stfsbuf);

/*
 * Global variables
 */
const struct fs_ops generic_ops = {
   .name = "Generic File System Layer", // Never used
   .fstype = "generic", // Not used
   // .fs_sanity = generic_sanity,
   .fs_getattr = generic_getattr,
   .fs_readdir = generic_readdir,
   .fs_read = generic_read,
   .fs_write = generic_write,
   .fs_mkdir = generic_mkdir,
   .fs_rmdir = generic_rmdir,
   .fs_unlink = generic_unlink,
   .fs_rename = generic_rename,
   .fs_chmod = generic_chmod,
   .fs_create = generic_create,
   .fs_truncate = generic_truncate,
#if (FUSE_USE_VERSION >= 30)
   .fs_utimens = generic_utimens,
#else
   .fs_utime = generic_utime,
#endif
   .fs_statfs = generic_statfs,
   //.fs_newfs = generic_newfs, // Only called from atrfs.c when creating new images; doesn't make sense here
   //.fs_fsinfo = generic_fsinfo, // Only called from special.c, bypassing this layer
};

/*
 * General functions
 */

/*
 * generic_getattr()
 *
 * Handle special per-filesystem files as well as for the specific file system
 */
int generic_getattr(struct atrfs *atrfs,const char *path,struct stat *stbuf)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);

   // Magic ".sector###" files
   if ( strncasecmp(path,"/.sector",sizeof("/.sector")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec > 0 && sec <= master_atrfs.sectors )
      {
         // stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = 128;
         stbuf->st_ino = sec;
         if ( !master_atrfs.ssbytes || sec > 3 ) stbuf->st_size = master_atrfs.sectorsize;
         return 0; // Good, can read this sector
      }
   }
   if ( fs_ops[ATR_SPECIAL] && fs_ops[ATR_SPECIAL]->fs_getattr )
   {
      int r = (fs_ops[ATR_SPECIAL]->fs_getattr)(atrfs,path,stbuf);
      if ( r == 0 ) return r;
   }
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_getattr )
   {
      return (fs_ops[atrfs->fstype]->fs_getattr)(atrfs,path,stbuf);
   }
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s failure: EIO\n",__FUNCTION__,path);
   return -EIO;
}

/*
 * generic_readdir()
 *
 * The "." and ".." entries are inserted by atrfs_readdir()
 */
int generic_readdir(struct atrfs *atrfs,const char *path,void *buf,fuse_fill_dir_t filler,off_t offset)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);
   if ( fs_ops[ATR_SPECIAL] && fs_ops[ATR_SPECIAL]->fs_readdir )
   {
      (fs_ops[ATR_SPECIAL]->fs_readdir)(atrfs,path, buf, filler,offset);
   }
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_readdir )
   {
      return (fs_ops[atrfs->fstype]->fs_readdir)(atrfs,path, buf, filler,offset);
   }
   return 0; // At least the standard files work
}

/*
 * generic_read()
 *
 * Handle reads including magic .sector### files.
 */
int generic_read(struct atrfs *atrfs,const char *path, char *buf, size_t size, off_t offset)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s %ld bytes at %lu\n",__FUNCTION__,path,size,offset);

   // Magic .sector### files: Read a raw sector
   if ( strncasecmp(path,"/.sector",sizeof("/.sector")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec <= 0 || sec > atrfs->sectors ) return -ENOENT;

      int bytes = 128;
      if ( !atrfs->ssbytes || sec > 3 ) bytes = atrfs->sectorsize;

      if (offset >= bytes ) return -EOF;
      unsigned char *s = MSECTOR(sec);
      bytes -= offset;
      s += offset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(buf,s,bytes);
      return bytes;
   }
 
   if ( fs_ops[ATR_SPECIAL] && fs_ops[ATR_SPECIAL]->fs_read )
   {
      int r;
      r = (fs_ops[ATR_SPECIAL]->fs_read)(atrfs,path,buf,size,offset);
      if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s special returned %d\n",__FUNCTION__,path,r);
      if ( r != 0 ) return r;
      // If 'r' is zero, then it wasn't handled.
   }
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_read )
   {
      return (fs_ops[atrfs->fstype]->fs_read)(atrfs,path,buf,size,offset);
   }
   return -ENOENT;
}

/*
 * generic_write()
 *
 * Handle writes including magic .sector### files.
 */
int generic_write(struct atrfs *atrfs,const char *path, const char *buf, size_t size, off_t offset)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s %ld bytes at %lu\n",__FUNCTION__,path,size,offset);
   if ( atrfs->readonly ) return -EROFS;

   // Magic .sector### files: Write a raw sector
   if ( strncasecmp(path,"/.sector",sizeof("/.sector")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec <= 0 || sec > atrfs->sectors ) return -ENOENT;

      int bytes = 128;
      if ( !atrfs->ssbytes || sec > 3 ) bytes = atrfs->sectorsize;

      if (offset >= bytes ) return -ENOSPC; // -EOF doesn't stop 'dd' from writing
      unsigned char *s = MSECTOR(sec);
      bytes -= offset;
      s += offset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(s,buf,bytes);
      return bytes;
   }

   if ( fs_ops[ATR_SPECIAL] && fs_ops[ATR_SPECIAL]->fs_write )
   {
      int r;
      r = (fs_ops[ATR_SPECIAL]->fs_write)(atrfs,path,buf,size,offset);
      if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s special returned %d\n",__FUNCTION__,path,r);
      if ( r != 0 ) return r;
      // If 'r' is zero, then it wasn't handled.
   }
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_write )
   {
      return (fs_ops[atrfs->fstype]->fs_write)(atrfs,path,buf,size,offset);
   }
   return -ENOENT;
}

int generic_mkdir(struct atrfs *atrfs,const char *path,mode_t mode)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_mkdir )
   {
      return (fs_ops[atrfs->fstype]->fs_mkdir)(atrfs,path,mode);
   }
   return -EPERM; // mkdir(2) man page says EPERM if directory creation not supported
}

int generic_rmdir(struct atrfs *atrfs,const char *path)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_rmdir )
   {
      return (fs_ops[atrfs->fstype]->fs_rmdir)(atrfs,path);
   }
   return -EIO; // Seems like the right error for not supported
}

int generic_unlink(struct atrfs *atrfs,const char *path)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_unlink )
   {
      return (fs_ops[atrfs->fstype]->fs_unlink)(atrfs,path);
   }
   return -EIO; // Seems like the right error for not supported
}

/*
 * generic_rename()
 *
 * Flags are zero if older fuse version
 */
int generic_rename(struct atrfs *atrfs,const char *path1, const char *path2,unsigned int flags)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;

   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_rename )
   {
      return (fs_ops[atrfs->fstype]->fs_rename)(atrfs,path1,path2,flags);
   }
   return -EIO; // Seems like the right error for not supported
}

int generic_chmod(struct atrfs *atrfs,const char *path, mode_t mode)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_chmod )
   {
      return (fs_ops[atrfs->fstype]->fs_chmod)(atrfs,path,mode);
   }
   return 0; // Fake success if not implemented
}

int generic_readlink(struct atrfs *atrfs,const char *path, char *buf, size_t size )
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_readlink )
   {
      return (fs_ops[atrfs->fstype]->fs_readlink)(atrfs,path,buf,size);
   }
   return -ENOENT; // Not implemented; shouldn't be reached
}

int generic_statfs(struct atrfs *atrfs,const char *path, struct statvfs *stfsbuf)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_statfs )
   {
      return (fs_ops[atrfs->fstype]->fs_statfs)(atrfs,path,stfsbuf);
   }
   return -EIO; // Seems like the right error for not supported
}

int generic_create(struct atrfs *atrfs,const char *path, mode_t mode)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_create )
   {
      return (fs_ops[atrfs->fstype]->fs_create)(atrfs,path,mode);
   }
   return -EIO; // Seems like the right error for not supported
}

int generic_truncate(struct atrfs *atrfs,const char *path,off_t size)
{
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_truncate )
   {
      return (fs_ops[atrfs->fstype]->fs_truncate)(atrfs,path,size);
   }
   return -EIO; // Seems like the right error for not supported
}

#if (FUSE_USE_VERSION >= 30)
int generic_utimens(struct atrfs *atrfs,const char *path, const struct timespec tv[2])
{
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_utimens )
   {
      return (fs_ops[atrfs->fstype]->fs_utimens)(atrfs,path,tv);
   }
   return 0; // Fake success on file systems that don't have time stamps
}
#else
int generic_utime(struct atrfs *atrfs,const char *path, struct utimbuf *utimbuf)
{
   if ( atrfs->readonly ) return -EROFS;
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);

   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_utime )
   {
      return (fs_ops[atrfs->fstype]->fs_utime)(atrfs,path,utimbuf);
   }
   return 0; // Fake success on file systems that don't have time stamps
}
#endif
