/*
 * atrfs
 *
 * Atari disk image (ATR file) mounted as a Linux file system.
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
// upcase_path() Create a copy of the path that is optionally converted to upper case
#define upcase_path(_path) \
   char _path ## _copy[PATH_MAX]; \
   strcpy_upcase( _path ## _copy, _path); \
   _path = _path ## _copy
#define lowcase_path(_path) \
   char _path ## _copy[PATH_MAX]; \
   strcpy_lowcase( _path ## _copy, _path); \
   _path = _path ## _copy

/*
 * Data types
 */

struct atr_head {
   unsigned char h0; /* 0x96 */
   unsigned char h1; /* 0x02 */
   unsigned char bytes16count[2];
   unsigned char secsize[2];
   unsigned char hibytes16count[2];
   unsigned char unused[8];
};

/*
 * Global variables
 */

struct atrfs master_atrfs;
struct options options;
const struct fs_ops *fs_ops[ATR_MAXFSTYPE] = {
   [ATR_SPECIAL] = &special_ops,
   [ATR_DOS1] = &dos1_ops,
   [ATR_DOS2] = &dos2_ops,
   [ATR_DOS20D] = &dos20d_ops,
   [ATR_DOS25] = &dos25_ops,
   [ATR_MYDOS] = &mydos_ops,
   [ATR_SPARTA] = &sparta_ops,
   [ATR_DOS3] = &dos3_ops,
   [ATR_DOS4] = &dos4_ops,
   [ATR_DOSXE] = &dosxe_ops,
   [ATR_LITEDOS] = &litedos_ops,
   [ATR_APT] = &apt_ops,
   [ATR_UNKNOWN] = &unknown_ops,
};
char *cwd; // FUSE sets working director to '/' before we open the ATR file

/*
 * MyDOS functions
 */

/*
 * General functions
 */
int valid_atr_file(struct atr_head *head)
{
   if ( head->h0 != 0x96 || head->h1 != 0x02 )
   {
      fprintf(stderr,"File does not have ATR signature\n");
      return 0;
   }
   master_atrfs.sectorsize=BYTES2(head->secsize);
   switch(master_atrfs.sectorsize)
   {
      case 128:
      case 256:
      case 512:
         break; // Those are valid
      default:
         fprintf(stderr,"Sector size in ATR file is invalid: %d\n",master_atrfs.sectorsize);
         return 0;
   }
   int atrbytes = (BYTES2(head->bytes16count) + (BYTES2(head->hibytes16count) << 16)) * 16;
   if ( atrbytes % master_atrfs.sectorsize && master_atrfs.sectorsize == 256 )
   {
      master_atrfs.shortsectors=3; // First three sectors are optionally 128 bytes
   }
   master_atrfs.ssbytes = master_atrfs.shortsectors * (master_atrfs.sectorsize-128);
   master_atrfs.sectors = (atrbytes + master_atrfs.ssbytes) / master_atrfs.sectorsize;

   size_t expected = sizeof(struct atr_head) + master_atrfs.sectors*master_atrfs.sectorsize - master_atrfs.ssbytes;
   if ( master_atrfs.atrsize != expected )
   {
      fprintf(stderr,"ATR file size unexpected value, expected %lu bytes, found %lu bytes\n",expected,master_atrfs.atrsize);
      if ( master_atrfs.atrsize < expected ) return 0;
      fprintf(stderr,"There is more data in the file than the ATR header indicates should be present.\n");
      // else it's just a warning
   }

   return 1;
}

/*
 * atr_preinit()
 *
 * Runs before fuse_main, so the cwd hasn't changed yet.
 * This allows relative paths for the ATR files
 */
int atr_preinit(void)
{
   int r;

   // Set file system type
   master_atrfs.fstype = ATR_UNKNOWN;
   if ( options.fstype )
   {
      for (int i=0;i<ATR_MAXFSTYPE;++i)
      {
         if ( fs_ops[i] && fs_ops[i]->fstype && strcmp(options.fstype,fs_ops[i]->fstype) == 0 )
         {
            master_atrfs.fstype = i;
            break;
         }
      }
      if ( master_atrfs.fstype == ATR_UNKNOWN ) {
         fprintf(stderr,"ERROR: Invalid file system type: %s\n",options.fstype);
         return 1;
      }
   }

   if ( options.create )
   {
      switch (options.secsize)
      {
         case 0:
            options.secsize = 128; // Default to single density
            break;
         case 128:
         case 256:
         case 512:
            break; // Legal
         default:
            fprintf(stderr,"Creating new image: Invalid sectors size: %d\n",options.secsize);
            return 1;
      }
      if ( options.sectors == 0 ) options.sectors = 720;
      if ( options.sectors < 4 || options.sectors > 0xffff )
      {
         fprintf(stderr,"Creating new image: Invalid sectors count: %d\n",options.sectors);
         return 1;
      }
      master_atrfs.fd=open(options.filename,O_CREAT|O_RDWR|O_EXCL,0644);
      if ( master_atrfs.fd < 0 )
      {
         fprintf(stderr,"creation of %s failed\n",options.filename);
         return 1;
      }
      // truncate to set the file size
      master_atrfs.atrsize = options.secsize*options.sectors+sizeof(struct atr_head);
      master_atrfs.sectorsize = options.secsize;
      master_atrfs.shortsectors = 0;
      master_atrfs.ssbytes = 0;
      master_atrfs.sectors = options.sectors;
      if ( master_atrfs.sectorsize == 256 && master_atrfs.sectors > 3 ) // This is optional, but may make emulators work better
      {
         master_atrfs.ssbytes = 128 * 3;
         master_atrfs.shortsectors = 3;
         master_atrfs.atrsize -= 128 * 3;
      }
      r=ftruncate(master_atrfs.fd,master_atrfs.atrsize);
      if ( r != 0 )
      {
         fprintf(stderr,"Failed to set size of new image: %s\n",options.filename);
         return 1;
      }
      // memmap
      master_atrfs.atrmem = mmap(NULL,master_atrfs.atrsize,PROT_READ|PROT_WRITE,MAP_SHARED,master_atrfs.fd,0);
      if ( master_atrfs.atrmem == MAP_FAILED )
      {
         perror("mmap of new image file failed");
         return 1;
      }
      master_atrfs.mem=((char *)master_atrfs.atrmem)+sizeof(struct atr_head);
      // Create ATR Head
      struct atr_head *head = master_atrfs.atrmem;
      head->h0 = 0x96;
      head->h1 = 0x02;
      head->secsize[0] = options.secsize & 0xff;
      head->secsize[1] = options.secsize >> 8;
      int imagesize16 = (options.secsize*options.sectors - master_atrfs.ssbytes) >> 4;
      head->bytes16count[0] = imagesize16 & 0xff;
      head->bytes16count[1] = (imagesize16 >> 8 ) & 0xff;
      head->hibytes16count[0] = (imagesize16 >> 16 ) & 0xff;
      head->hibytes16count[1] = (imagesize16 >> 24 ) & 0xff;

      // Save stat of image file
      r=fstat(master_atrfs.fd,&master_atrfs.atrstat); // Before opening for r/w
      if ( r!=0 )
      {
         fprintf(stderr,"Unable to stat new image file\n");
         return 1;
      }

      // Validate file system type
      if ( master_atrfs.fstype == ATR_UNKNOWN)
      {
         if ( options.fstype )
         {
            fprintf(stderr,"Unable to create specified image type: %s\n",options.fstype);
            return 1;
         }
         if ( master_atrfs.sectors < 368 ) master_atrfs.fstype = ATR_SPARTA; // Too short for DOS 2
         else if ( master_atrfs.sectors <= 720 && master_atrfs.sectorsize == 128 ) master_atrfs.fstype = ATR_DOS2;
         else if ( master_atrfs.sectors == 1040 && master_atrfs.sectorsize == 128 ) master_atrfs.fstype = ATR_DOS25;
         else if ( master_atrfs.sectors == 1024 && master_atrfs.sectorsize == 128 ) master_atrfs.fstype = ATR_DOS25; // short enhanced-density image
         else master_atrfs.fstype = ATR_MYDOS;
      }
      if ( fs_ops[master_atrfs.fstype] && fs_ops[master_atrfs.fstype]->fs_newfs )
      {
         (fs_ops[master_atrfs.fstype]->fs_newfs)(&master_atrfs);
         return 0;
      }
      fprintf(stderr,"Unable to create new file system\n");
      return 1;
   }
   else
   {
      r=stat(options.filename,&master_atrfs.atrstat); // Before opening for r/w
      if ( r )
      {
         fprintf(stderr,"stat of %s failed: %d\n",options.filename,r);
         return 1;
      }
      // Even if mounted read-only, try to open with write permissions.
      // This allows a remount without any action.
      // Opening for read-write doesn't impact time stamps until an
      // actual write is made, so this is harmless.
      // Also, I can't figure out how to tell if it's mounted read-only, and
      // we would have to intercept any request to remount read-write.
      master_atrfs.fd=open(options.filename,O_RDWR);
   }
   if ( master_atrfs.fd < 0 )
   {
      // Try read-only image file.  Will fail any write attempts with EROFS
      // FIXME: Find a way to tell FUSE to force this to be a read-only mount
      master_atrfs.fd=open(options.filename,O_RDONLY);
      if ( master_atrfs.fd >= 0 )
      {
         fprintf(stderr,"NOTE: Image %s is read-only\n",options.filename);
         master_atrfs.readonly = 1;
      }
   }
   if ( master_atrfs.fd < 0 )
   {
      fprintf(stderr,"Failed to open %s; check permissions (read-write and read-only both failed)\n",options.filename);
      return 1;
   }
   master_atrfs.atrsize = master_atrfs.atrstat.st_size;
   if ( master_atrfs.atrsize < 16 )
   {
      fprintf(stderr,"File is too small to be a valid ATR file: %lu bytes in %s\n",master_atrfs.atrsize,options.filename);
      return 1;
   }
   master_atrfs.atrmem = mmap(NULL,master_atrfs.atrsize,PROT_READ|(master_atrfs.readonly?0:PROT_WRITE),MAP_SHARED,master_atrfs.fd,0);
   if ( master_atrfs.atrmem == MAP_FAILED )
   {
      fprintf(stderr,"mmap of %s failed\n",options.filename);
      return 1;
   }
   if ( master_atrfs.atrmem ) master_atrfs.mem=((char *)master_atrfs.atrmem)+sizeof(struct atr_head);

   // Special case for disk image files with partition tables
   struct atr_head *head = master_atrfs.atrmem;
   if ( ( head->h0 != 0x96 || head->h1 != 0x02 ) && master_atrfs.atrstat.st_size > 128*720 )
   {
      for (int i = ATR_APT;i<=ATR_APT;++i) // Currently only APT, but code for other options
      if ( fs_ops[i] && fs_ops[i]->fs_sanity )
      {
         if ( (fs_ops[i]->fs_sanity)(&master_atrfs) == 0 )
         {
            master_atrfs.fstype = i;
            if ( options.debug ) fprintf(stderr,"DEBUG: %s detected %s image\n",__FUNCTION__,fs_ops[i]->name);
            return 0;
         }
      }
   }
   
   // Validate that this is an ATR file and fill in struct values
   if ( !valid_atr_file(master_atrfs.atrmem) )
   {
      // If sector size specified, use that
      if ( options.secsize == 128 || options.secsize == 256 || options.secsize == 512 )
      {
         master_atrfs.mem = master_atrfs.atrmem;
         master_atrfs.ssbytes = 0;
         master_atrfs.sectorsize = options.secsize;
         master_atrfs.sectors = master_atrfs.atrstat.st_size / master_atrfs.sectorsize;
      }
      // Check for raw SD disk (i.e., XFD file)
      else if ( master_atrfs.atrstat.st_size == 128 * 720 )
      {
         fprintf(stderr,"Assuming this is a raw 90K image from the file size (perhaps XFD image)\n");
         master_atrfs.mem = master_atrfs.atrmem;
         master_atrfs.ssbytes = 0;
         master_atrfs.sectorsize = 128;
         master_atrfs.sectors = 720;
      }
      else
      {
         return 1;
      }
   }

   // Determine file system type
   for (int i=0;i<ATR_MAXFSTYPE;++i)
   {
      if ( fs_ops[i] && fs_ops[i]->fs_sanity )
      {
         if ( (fs_ops[i]->fs_sanity)(&master_atrfs) == 0 )
         {
            if ( master_atrfs.fstype == ATR_UNKNOWN) master_atrfs.fstype = i;
            if ( master_atrfs.fstype != (enum atrfstype)i ) {
               fprintf(stderr,"Warning: Detected file system type %s, command-line specified: %s\n",fs_ops[i]->fstype,fs_ops[master_atrfs.fstype]->fstype);
            }
            if ( options.debug ) fprintf(stderr,"DEBUG: %s detected %s image\n",__FUNCTION__,fs_ops[i]->name);
            return 0;
         }
      }
   }
   if (master_atrfs.fstype == ATR_UNKNOWN) {
      fprintf(stderr,"Unable to determine file system type; expose raw non-zero sectors\n");
   }
   return 0;
}

/*
 * FUSE file operations
 */
#if (FUSE_USE_VERSION >= 30)
void *atr_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
   (void)conn; // unused parameter
   if ( options.lowcase ) options.upcase = 1;
   cfg->use_ino = 1; // Use sector numbers as inode number
   return NULL;
}
#endif

int atr_getattr(const char *path, struct stat *stbuf
#if (FUSE_USE_VERSION >= 30)
                , struct fuse_file_info *fi
#endif
   )
{
#if (FUSE_USE_VERSION >= 30)
   (void)fi;
#endif
   upcase_path(path);
   memset(stbuf,0,sizeof(*stbuf));

   if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);

   // Copy time stamps from image file; adjust if SpartaDOS or other file system supports time stamps
   stbuf->st_atim = master_atrfs.atrstat.st_atim;
   stbuf->st_mtim = master_atrfs.atrstat.st_mtim;
   stbuf->st_ctim = master_atrfs.atrstat.st_ctim;
   stbuf->st_uid = master_atrfs.atrstat.st_uid;
   stbuf->st_gid = master_atrfs.atrstat.st_gid;
   stbuf->st_blksize = master_atrfs.sectorsize;
   stbuf->st_nlink = 1;
   stbuf->st_mode = MODE_FILE(master_atrfs.atrstat.st_mode & 0777);

   return (generic_ops.fs_getattr)(&master_atrfs,path, stbuf);
}

static fuse_fill_dir_t save_filler;
int atrfs_filler(void *buf,const char *name,
                 const struct stat *stbuf, off_t off
#if (FUSE_USE_VERSION >= 30)
                 ,enum fuse_fill_dir_flags flags
#endif
   )
{
   lowcase_path(name);
   return save_filler(buf,name,stbuf,off
#if (FUSE_USE_VERSION >= 30)
                      ,flags
#endif
      );
}

int atr_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi
#if (FUSE_USE_VERSION >= 30)
                , enum fuse_readdir_flags flags
#endif
   )
{
   (void)fi;
#if (FUSE_USE_VERSION >= 30)
   (void)flags;
#endif
   if ( options.lowcase )
   {
      save_filler = filler;
      filler = atrfs_filler;
   }
   upcase_path(path);
#if 0 // Have FUSE call getattr() for stat information
   // No subdirectories yet
   struct stat st;
   memcpy(&st,&atrfs.atrstat,sizeof(st)); // Default values are to match image file
   st.st_dev = 0; // FUSE should fill this in
   st.st_ino = 0; // Fill it in with the fs-specific sector number
   st.st_nlink = 1;
   st.st_rdev = 0; // Not used
   st.st_size = 0; // Fill in below
   st.st_blksize = master_atrfs.sectorsize;
   st.st_blocks = 0; // Fill in below
   st.st_mode = MODE_DIR(master_atrfs.atrstat.st_mode); // dir; adjust below
#endif

   if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);
   // Standard directories
   filler(buf, ".", FILLER_NULL);
   filler(buf, "..", FILLER_NULL);

   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);
   return (generic_ops.fs_readdir)(&master_atrfs,path, buf, filler,offset);
}

int atr_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s %ld bytes at %lu\n",__FUNCTION__,path,size,offset);
   return (generic_ops.fs_read)(&master_atrfs,path,buf,size,offset);
}

int atr_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s %ld bytes at %lu\n",__FUNCTION__,path,size,offset);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_write)(&master_atrfs,path,buf,size,offset);
}

int atr_mkdir(const char *path,mode_t mode)
{
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_mkdir)(&master_atrfs,path,mode);
}

int atr_rmdir(const char *path)
{
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_rmdir)(&master_atrfs,path);
}

int atr_unlink(const char *path)
{
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_unlink)(&master_atrfs,path);
}

int atr_rename(const char *path1, const char *path2
#if (FUSE_USE_VERSION >= 30)
               , unsigned int flags
#endif
   )
{
#if (FUSE_USE_VERSION < 30)
   unsigned int flags = 0; // Default value if not supported
#endif
   
   upcase_path(path1);
   upcase_path(path2);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_rename)(&master_atrfs,path1,path2,flags);
}

int atr_chmod(const char *path, mode_t mode
#if (FUSE_USE_VERSION >= 30)
              , struct fuse_file_info *fi
#endif
   )
{
#if (FUSE_USE_VERSION >= 30)
   (void)fi;
#endif
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_chmod)(&master_atrfs,path,mode);
}

int atr_readlink(const char *path, char *buf, size_t size )
{
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   return (generic_ops.fs_readlink)(&master_atrfs,path,buf,size);
}

int atr_statfs(const char *path, struct statvfs *stfsbuf)
{
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   return (generic_ops.fs_statfs)(&master_atrfs,path,stfsbuf);
}

int atr_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_create)(&master_atrfs,path,mode);
}

int atr_truncate(const char *path,
                 off_t size
#if (FUSE_USE_VERSION >= 30)
                 , struct fuse_file_info *fi
#endif
   )
{
#if (FUSE_USE_VERSION >= 30)
   (void)fi;
#endif   
   upcase_path(path);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( master_atrfs.readonly ) return -EROFS;
   return (generic_ops.fs_truncate)(&master_atrfs,path,size);
}

#if (FUSE_USE_VERSION >= 30)
int atr_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
   if ( master_atrfs.readonly ) return -EROFS;
   
   if ( options.debug )
   {
      fprintf(stderr,"DEBUG: %s: Times in seconds: ",__FUNCTION__);
      if ( tv[0].tv_nsec == UTIME_NOW ) fprintf(stderr,"NOW, ");
      else if ( tv[0].tv_nsec == UTIME_OMIT ) fprintf(stderr,"OMIT, ");
      else fprintf(stderr,"%lu, ",tv[0].tv_sec);
      if ( tv[1].tv_nsec == UTIME_NOW ) fprintf(stderr,"NOW, ");
      else if ( tv[1].tv_nsec == UTIME_OMIT ) fprintf(stderr,"OMIT, ");
      else fprintf(stderr,"%lu, ",tv[1].tv_sec);
   }
   return (generic_ops.fs_utimens)(&master_atrfs,path,tv);
}
#else
int atr_utime(const char *path, struct utimbuf *utimbuf)
{
   upcase_path(path);
   if ( master_atrfs.readonly ) return -EROFS;
   if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);

   return (generic_ops.fs_utime)(&master_atrfs,path,utimbuf);
}
#endif

int atr_chown(const char *path, uid_t uid, gid_t gid
#if (FUSE_USE_VERSION >= 30)
              , struct fuse_file_info *fi
#endif
   )
{
   if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);
   (void)path;
   (void)uid;
   (void)gid;
#if (FUSE_USE_VERSION >= 30)
   (void)fi;
#endif
   return 0; // Fake it to avoid stupid not-supported error messages
}

static const struct fuse_operations atr_oper = {
#if (FUSE_USE_VERSION >= 30)
        .init           = atr_init,
#endif
	.getattr	= atr_getattr,
	.readdir	= atr_readdir,
	.read		= atr_read,
        .write          = atr_write,
        .mkdir          = atr_mkdir,
        .rmdir          = atr_rmdir,
        .unlink         = atr_unlink,
        .rename         = atr_rename,
        .chmod          = atr_chmod,
        .readlink       = atr_readlink,
        .create         = atr_create,
        .truncate       = atr_truncate,
#if (FUSE_USE_VERSION >= 30)
        .utimens        = atr_utimens,
#else
        .utime          = atr_utime,
#endif
        .chown          = atr_chown,
        .statfs         = atr_statfs,
};

/*
 * atrfs_info()
 *
 * Display information about the image
 */
void atrfs_info(void)
{
   struct atr_head *head = master_atrfs.atrmem;
   unsigned long bytes16 = (BYTES2(head->bytes16count)|(BYTES2(head->hibytes16count) << 16));

   printf("\n\n"
          "Image information\n"
          "\n"
          "ATR Headder:\n"
          "  Sector size:  %d\n"
          "  Size in 16-byte units:   %lu\n"
          "  Size in bytes:           %lu (plus 16-byte header)\n",
          BYTES2(head->secsize),bytes16,bytes16*16
      );
   printf("  Computed sectors:        %d\n",master_atrfs.sectors);
   if ( master_atrfs.shortsectors )
   {
      printf("The first three sectors in the image are only 128 bytes each\n");
   }
   if ( master_atrfs.atrstat.st_size != (off_t)bytes16*16+16 )
   {
      printf("The raw file size is %lu, not %lu as would be expected from the header\n",master_atrfs.atrstat.st_size,bytes16*16+16);
      int exp_sectors;
      int exp_short_sectors = 0;
      int exp_garbage;
      if ( master_atrfs.sectorsize == 256 && ((master_atrfs.atrstat.st_size - 16) % master_atrfs.sectorsize) == 128 ) exp_short_sectors = 3;
      exp_sectors = (master_atrfs.atrstat.st_size - 16 + 128 * exp_short_sectors) / master_atrfs.sectorsize;
      exp_garbage = (master_atrfs.atrstat.st_size - 16 + 128 * exp_short_sectors) % master_atrfs.sectorsize;
      printf("  Sectors from file size:  %d\n",exp_sectors);
      if ( exp_short_sectors ) printf("  First three sectors are 128-bytes long\n");
      if ( exp_garbage ) printf("  Stray bytes at end of image: %d\n", exp_garbage);
      
      
   }
   printf("\n");
   char *buf = fsinfo_textdata(&master_atrfs);
   if ( buf )
   {
      printf("%s",buf);
      free(buf);
   }
}

/*
 * Command line options
 */

#define OPTION(t, p)   { t, offsetof(struct options, p), 1 }
const struct fuse_opt option_spec[] = {
   OPTION("--name=%s", filename),
   OPTION("--nodotfiles", nodotfiles),
   OPTION("--atrdebug", debug),
   OPTION("--atrdebug=%d", debug),
   OPTION("-h", help),
   OPTION("--help", help),
   OPTION("--info", info),
   OPTION("-i", info),
   OPTION("--create", create),
   OPTION("--upcase", upcase),
   OPTION("--lowcase", lowcase),
   OPTION("--secsize=%u", secsize),
   OPTION("--sectors=%u", sectors),
   OPTION("--fs=%s", fstype),
   OPTION("--volname=%s", volname),
   OPTION("--cluster=%u", clustersize),
   FUSE_OPT_END
};

/*
 * main()
 *
 * Start things up
 */
int main(int argc,char *argv[])
{
   int ret;

   // Defaults
   options.secsize=128;
   options.sectors=720;

   // Mangle options for no '--nmae=' option with a mount point
   int mp = 0;
   for ( int i=argc-1;i>0;--i )
   {
      if ( argv[i][0] == '-' ) continue;
      if ( !mp )
      {
         mp = 1;
         continue;
      }
      char *dummy = malloc(strlen(argv[i])+10);
      sprintf(dummy,"--name=%s",argv[i]);
      argv[i] = dummy;
      break;
   }

   struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
   if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
      return 1;
   
   if ( !options.filename || options.help )
   {
      printf("usage:\n"
             " %s [options] <mountpoint>\n"
             "   or\n"
             " %s [options] <atrfile> <mountpoint>\n"
             "\n",argv[0],argv[0]);
      printf("fuse options:\n"
             "    -d   (debugging output; implies -f)\n"
             "    -f   (do not fork into background)\n"
             "    -s   (disable multithreading; may fix concurrency bugs)\n"
             "    -oro (read-only mount)\n"
             "\n");
      printf("atrfs options:\n"
             "    --name=<atr file path>\n"
             "    --nodotfiles  (no special dot files in root directory)\n"
             "    --atrdebug    (extra debugging from atrfs)\n"
             "    --info        (display image info)\n"
             "    --create      (create new image)\n"
             "    --upcase      (new files are create uppercase; operations are case insensitive)\n"
             "    --lowcase     (present all files as lower-case; implies --upcase)\n"
             "    --secsize=<#> (sector size to use if no ATR header is present)\n"
             "    --fs=<type>   (override auto-detect file system type)\n"
             " Options used with --create:\n"
             "    --secsize=<#> (sector size if creating; default 128)\n"
             "    --sectors=<#> (number of sectors in image; default 720)\n"
             "    --fs=<type>   (type of file system: " );
      int comma=0;
      for (int i=0;i<ATR_MAXFSTYPE;++i)
      {
         if ( fs_ops[i] && fs_ops[i]->fstype )
         {
            if ( comma ) printf(", ");
            printf("%s",fs_ops[i]->fstype);
            comma=1;
         }
      }
      printf(")\n");

      printf("    --volname=<>  (set volume name for new SpartaDOS image)\n"
             "    --cluster=<#> (minimum cluster size for LiteDOS)\n"
         );
      return 0;
   }

   ret = atr_preinit();
   if ( ret ) return ret;

   if ( options.create && args.argc == 1 )
   {
      printf("Image created.  No mount point specified.\n");
      return 0;
   }

   if ( options.info ) atrfs_info();

   if ( options.info && args.argc == 1 ) return 0;

   ret = fuse_main(args.argc, args.argv, &atr_oper
#if (FUSE_USE_VERSION >= 26)
                   , NULL
#endif
      );
   fuse_opt_free_args(&args);
   return ret;
}
