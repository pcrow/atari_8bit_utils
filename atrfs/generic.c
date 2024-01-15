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

int generic_readdir(struct atrfs *atrfs,const char *path,void *buf,fuse_fill_dir_t filler,off_t offset)
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
   st.st_blksize = atrfs->sectorsize;
   st.st_blocks = 0; // Fill in below
   st.st_mode = MODE_DIR(atrfs->atrstat.st_mode); // dir; adjust below
#endif

   if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);
   // Standard directories
   filler(buf, ".", FILLER_NULL);
   filler(buf, "..", FILLER_NULL);

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

int generic_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
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

int generic_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
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

int generic_mkdir(const char *path,mode_t mode)
{
   upcase_path(path);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_mkdir )
   {
      return (fs_ops[atrfs->fstype]->fs_mkdir)(atrfs,path,mode);
   }
   return -EPERM; // mkdir(2) man page says EPERM if directory creation not supported
}

int generic_rmdir(const char *path)
{
   upcase_path(path);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_rmdir )
   {
      return (fs_ops[atrfs->fstype]->fs_rmdir)(atrfs,path);
   }
   return -EIO; // Seems like the right error for not supported
}

int generic_unlink(const char *path)
{
   upcase_path(path);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_unlink )
   {
      return (fs_ops[atrfs->fstype]->fs_unlink)(atrfs,path);
   }
   return -EIO; // Seems like the right error for not supported
}

int generic_rename(const char *path1, const char *path2
#if (FUSE_USE_VERSION >= 30)
               , unsigned int flags
#endif
   )
{
   upcase_path(path1);
   upcase_path(path2);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;

   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_rename )
   {
      return (fs_ops[atrfs->fstype]->fs_rename)(atrfs,path1,path2,
#if (FUSE_USE_VERSION >= 30)
                                               flags
#else
                                               0
#endif
         );
   }
   return -EIO; // Seems like the right error for not supported
}
int generic_chmod(const char *path, mode_t mode
#if (FUSE_USE_VERSION >= 30)
              , struct fuse_file_info *fi
#endif
   )
{
#if (FUSE_USE_VERSION >= 30)
   (void)fi;
#endif
   upcase_path(path);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_chmod )
   {
      return (fs_ops[atrfs->fstype]->fs_chmod)(atrfs,path,mode);
   }
   return 0; // Fake success if not implemented
}
int generic_readlink(const char *path, char *buf, size_t size )
{
   upcase_path(path);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_readlink )
   {
      return (fs_ops[atrfs->fstype]->fs_readlink)(atrfs,path,buf,size);
   }
   return -ENOENT; // Not implemented; shouldn't be reached
}
int generic_statfs(const char *path, struct statvfs *stfsbuf)
{
   upcase_path(path);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_statfs )
   {
      return (fs_ops[atrfs->fstype]->fs_statfs)(atrfs,path,stfsbuf);
   }
   return -EIO; // Seems like the right error for not supported
}
int generic_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_create )
   {
      return (fs_ops[atrfs->fstype]->fs_create)(atrfs,path,mode);
   }
   return -EIO; // Seems like the right error for not supported
}
int generic_truncate(const char *path,
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
   if ( options.debug > GENERIC_DEBUG_THRESHOLD ) fprintf(stderr,"DEBUG: %s\n",__FUNCTION__);
   if ( atrfs->readonly ) return -EROFS;
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_truncate )
   {
      return (fs_ops[atrfs->fstype]->fs_truncate)(atrfs,path,size);
   }
   return -EIO; // Seems like the right error for not supported
}


#if (FUSE_USE_VERSION >= 30)
int generic_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
   (void)fi;
   upcase_path(path);
   if ( atrfs->readonly ) return -EROFS;
   
   if ( options.debug > GENERIC_DEBUG_THRESHOLD )
   {
      fprintf(stderr,"DEBUG: %s: Times in seconds: ",__FUNCTION__);
      if ( tv[0].tv_nsec == UTIME_NOW ) fprintf(stderr,"NOW, ");
      else if ( tv[0].tv_nsec == UTIME_OMIT ) fprintf(stderr,"OMIT, ");
      else fprintf(stderr,"%lu, ",tv[0].tv_sec);
      if ( tv[1].tv_nsec == UTIME_NOW ) fprintf(stderr,"NOW, ");
      else if ( tv[1].tv_nsec == UTIME_OMIT ) fprintf(stderr,"OMIT, ");
      else fprintf(stderr,"%lu, ",tv[1].tv_sec);
   }
   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_utimens )
   {
      return (fs_ops[atrfs->fstype]->fs_utimens)(atrfs,path,tv);
   }
   return 0; // Fake success on file systems that don't have time stamps
}
#else
int generic_utime(const char *path, struct utimbuf *utimbuf)
{
   upcase_path(path);
   if ( atrfs->readonly ) return -EROFS;
   if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s %s\n",__FUNCTION__,path);

   if ( fs_ops[atrfs->fstype] && fs_ops[atrfs->fstype]->fs_utime )
   {
      return (fs_ops[atrfs->fstype]->fs_utime)(atrfs,path,utimbuf);
   }
   return 0; // Fake success on file systems that don't have time stamps
}
#endif

int generic_chown(const char *path, uid_t uid, gid_t gid
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
   struct atr_head *head = atrfs->atrmem;
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
   printf("  Computed sectors:        %d\n",atrfs->sectors);
   if ( atrfs->shortsectors )
   {
      printf("The first three sectors in the image are only 128 bytes each\n");
   }
   if ( atrfs->atrstat.st_size != (off_t)bytes16*16+16 )
   {
      printf("The raw file size is %lu, not %lu as would be expected from the header\n",atrfs->atrstat.st_size,bytes16*16+16);
      int exp_sectors;
      int exp_short_sectors = 0;
      int exp_garbage;
      if ( atrfs->sectorsize == 256 && ((atrfs->atrstat.st_size - 16) % atrfs->sectorsize) == 128 ) exp_short_sectors = 3;
      exp_sectors = (atrfs->atrstat.st_size - 16 + 128 * exp_short_sectors) / atrfs->sectorsize;
      exp_garbage = (atrfs->atrstat.st_size - 16 + 128 * exp_short_sectors) % atrfs->sectorsize;
      printf("  Sectors from file size:  %d\n",exp_sectors);
      if ( exp_short_sectors ) printf("  First three sectors are 128-bytes long\n");
      if ( exp_garbage ) printf("  Stray bytes at end of image: %d\n", exp_garbage);
      
      
   }
   printf("\n");
   char *buf = fsinfo_textdata(atrfs);
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
