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
// MD5 library functions
#if defined(__APPLE__)
#  define COMMON_DIGEST_FOR_OPENSSL
#  include <CommonCrypto/CommonDigest.h>
#  define SHA1 CC_SHA1
#else
#  include <openssl/md5.h>
#endif

#include "atrfs.h"

/*
 * Macros and defines
 */

/*
 * File System Structures
 */
struct disk_checksums {
   int start_sector;
   int sector_count;
   char md5sum_hexstring[33];
   char *name;
};

/*
 * Function prototypes
 */
int unknown_sanity(void);
int unknown_getattr(const char *path, struct stat *stbuf);
int unknown_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int unknown_statfs(const char *path, struct statvfs *stfsbuf);
int unknown_newfs(void);

/*
 * Global variables
 */
const struct fs_ops unknown_ops = {
   .name = "Unknown Disk Format",
   .fstype = "blank",
   .fs_sanity = unknown_sanity,
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
   .fs_statfs = unknown_statfs,
   .fs_newfs = unknown_newfs,
   // .fs_fsinfo = unknown_fsinfo,
};

/*
 * Table of checksums for specific disks
 *
 * This is not intended to be a comprehensive table for all commercial disks.
 * If false positives or negatives arise, then we need to find different sector ranges to check.
 * Ideally this should exclude sectors most often hacked to bypass copy protection.
 */
const struct disk_checksums disk_checksums[] = {
   {2,3,"eab06eb5d8ca1b612a6bb30cb6fa8171","Ultima IV Program Disk (Disk 1, Side A)"}, // Noted copies with differences in sector 1
   {1,3,"beade5644d8b2c83698149e46b7c80c8","Ultima IV Towne Disk (Disk 1, Side B)"},
   {1,3,"05c8594da63bd65e1a21930d66ea36e5","Ultima IV Britannia Disk (Disk 2, Side A)"}, // Also player disk
   {1,3,"fe7ce56a369b0366eb9941f46aad2e43","Ultima IV Underworld Disk (Disk 2, Side B)"},
};

int matching_checksum;
int match_found;
const char *match_disk_name;

/*
 * find_checksum_match()
 *
 * Check to see if the disk image matches any of the known checksums
 * Note: This may not work correctly on images that are not single density
 */
void find_checksum_match(void)
{
   match_found = 0;
   unsigned char digest[16];
   for (unsigned int i=0;i<sizeof(disk_checksums)/sizeof(disk_checksums[0]);++i)
   {
      // Convert the hex string into a 16-byte binary blob
      unsigned char match_digest[16];
      for (int j=0;j<16;++j)
      {
         char hex[3];
         hex[0] = disk_checksums[i].md5sum_hexstring[j*2];
         hex[1] = disk_checksums[i].md5sum_hexstring[j*2+1];
         hex[2] = 0;
         match_digest[j] = strtol(hex,NULL,16);
      }

      // Compute MD5 checksum on the specified sector range in the image
      {
         MD5_CTX md5;
         MD5_Init(&md5);
         // FIXME: Make this a loop for boot sectors in case it's DD
         // FIXME: Use the sector size instead of 128 to support DD disks
         MD5_Update(&md5,SECTOR(disk_checksums[i].start_sector),disk_checksums[i].sector_count*128);
         MD5_Final(digest,&md5);
      }

      // Do they match?
      if (0==memcmp(digest,match_digest,16))
      {
         matching_checksum = i;
         match_found = 1;
         match_disk_name = disk_checksums[i].name;
         return;
      }
   }
   match_disk_name = "UNKNOWN_DISK_FORMAT";
}

/*
 * Functions
 */
int unknown_sanity(void)
{
   find_checksum_match();
   if ( match_found ) return 0;
   return 1;
}

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
   if ( 0 == strcmp(path,"/USE_.sector#_for_raw_nonzero_sectors") ||
        0 == strcmp(path+1,match_disk_name) )
   {
      stbuf->st_ino = 0x10001;
      stbuf->st_size = 0;
      return 0; // Whatever, don't really care
   }
   return 1;
}

int unknown_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)path; // Always "/"
   (void)offset;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   filler(buf, match_disk_name, FILLER_NULL);
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

/*
 * unknown_statfs()
 *
 * Report empty sectors as free.
 */
int unknown_statfs(const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless

   // Find count of empty sectors
   int free_sectors=0;
   struct sector1 *s1 = SECTOR(1);
   unsigned char *zero = calloc(1,atrfs.sectorsize);
   if ( !zero ) return -ENOMEM; // Weird
   for (int sec=s1->boot_sectors + 1; sec<=atrfs.sectors; ++sec)
   {
      unsigned char *s = SECTOR(sec);
      if ( memcmp(s,zero,atrfs.sectorsize) == 0 ) continue;
      ++free_sectors;
   }
   free(zero);

   stfsbuf->f_bsize = atrfs.sectorsize;
   stfsbuf->f_frsize = atrfs.sectorsize;
   stfsbuf->f_blocks = atrfs.sectors;
   stfsbuf->f_bfree = free_sectors;
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = 0; // no file system, no files
   stfsbuf->f_ffree = 0;
   stfsbuf->f_namemax = 12;
   return 0;
}

/*
 * unknown_newfs()
 *
 * Seems like a good place for creating blank images
 */
int unknown_newfs(void)
{
   return 0;
}
