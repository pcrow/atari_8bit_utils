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

#include FUSE_INCLUDE
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#ifdef __linux__
#include <linux/fs.h> // Linux-only options; fix if supported elsewhere
#else
#define RENAME_NOREPLACE  0
#define RENAME_EXCHANGE 0
#endif
#include <time.h>
#include "atrfs.h"

/*
 * Macros and defines
 */
#define CLUSTER_TO_SECTOR(n)  ((n)*8+25) // 1K blocks starting with '0' at sector 25
#define CLUSTER(n)            SECTOR(CLUSTER_TO_SECTOR(n))
#define TOTAL_CLUSTERS        ((atrfs.sectors-24)/8) // Count of clusters
#define MAX_CLUSTER           ((atrfs.sectors-32)/8) // 0--MAX_CLUSTER are valid


/*
 * File System Structures
 */
struct dos3_dir_head {
   unsigned char zeros[14];
   unsigned char max_free_blocks; // ((720-24)/8)==87 for SD, 127 for ED
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
int dos3_getattr(const char *path, struct stat *stbuf);
int dos3_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int dos3_read(const char *path, char *buf, size_t size, off_t offset);
int dos3_write(const char *path, const char *buf, size_t size, off_t offset);
int dos3_unlink(const char *path);
int dos3_rename(const char *path1, const char *path2, unsigned int flags);
int dos3_chmod(const char *path, mode_t mode);
int dos3_create(const char *path, mode_t mode);
int dos3_truncate(const char *path, off_t size);
int dos3_statfs(const char *path, struct statvfs *stfsbuf);
int dos3_newfs(void);
char *dos3_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops dos3_ops = {
   .name = "Atari DOS 3",
   .fstype = "dos3",
   .fs_sanity = dos3_sanity,
   .fs_getattr = dos3_getattr,
   .fs_readdir = dos3_readdir,
   .fs_read = dos3_read,
   // .fs_write = dos3_write,
   // .fs_mkdir // not relevant
   // .fs_rmdir // not relevant
   // .fs_unlink = dos3_unlink,
   // .fs_rename = dos3_rename,
   // .fs_chmod = dos3_chmod,
   // .fs_create = dos3_create,
   // .fs_truncate = dos3_truncate,
   // .fs_utimens // not relevant
   .fs_statfs = dos3_statfs,
   .fs_newfs = dos3_newfs,
   .fs_fsinfo = dos3_fsinfo,
};

const unsigned char bootsectors[] = {
   0x01, 0x09, 0x00, 0x32, 0x06, 0x32, 0xa2, 0x5d, 0xbd, 0xfd, 0x35, 0x9d, 0x00, 0x01, 0xca, 0xe0, 
   0xff, 0xd0, 0xf5, 0x20, 0x1c, 0x32, 0x10, 0x01, 0x00, 0x4c, 0x00, 0x01, 0xa9, 0x42, 0x85, 0x43, 
   0xa9, 0x36, 0x85, 0x44, 0xa9, 0x00, 0x8d, 0x34, 0x36, 0x8d, 0x35, 0x36, 0x8d, 0x02, 0x36, 0x8d, 
   0x31, 0x36, 0x8d, 0x32, 0x36, 0x8d, 0x33, 0x36, 0x8d, 0x2e, 0x36, 0x8d, 0x2f, 0x36, 0x8d, 0x30, 
   0x36, 0xa9, 0x01, 0x8d, 0x36, 0x36, 0xa2, 0x0a, 0xbd, 0x5e, 0x32, 0x9d, 0x07, 0x36, 0xca, 0x10, 
   0xf7, 0x20, 0x64, 0x33, 0x20, 0x79, 0x33, 0x90, 0x10, 0xa0, 0xaa, 0x4c, 0x18, 0x32, 0x46, 0x4d, 
   0x53, 0x20, 0x20, 0x20, 0x20, 0x20, 0x53, 0x59, 0x53, 0x20, 0xa2, 0x33, 0x20, 0x2c, 0x34, 0x20, 
   0x73, 0x32, 0x60, 0xa9, 0xfc, 0x8d, 0xe0, 0x02, 0xa9, 0x32, 0x8d, 0xe1, 0x02, 0xa9, 0x02, 0xa2, 
   0x25, 0xa0, 0x36, 0x20, 0xfd, 0x32, 0xad, 0x25, 0x36, 0x2d, 0x26, 0x36, 0xc9, 0xff, 0xf0, 0x05, 
   0xa0, 0xaf, 0x4c, 0x18, 0x32, 0xa9, 0x02, 0xa2, 0x03, 0xa0, 0x36, 0x20, 0xfd, 0x32, 0xa9, 0x02, 
   0xa2, 0x05, 0xa0, 0x36, 0x20, 0xfd, 0x32, 0xa9, 0xfc, 0x8d, 0xe2, 0x02, 0xa9, 0x32, 0x8d, 0xe3, 
   0x02, 0x38, 0xad, 0x05, 0x36, 0xed, 0x03, 0x36, 0x8d, 0x05, 0x36, 0xad, 0x06, 0x36, 0xed, 0x04, 
   0x36, 0x8d, 0x06, 0x36, 0xee, 0x05, 0x36, 0xd0, 0x03, 0xee, 0x06, 0x36, 0x20, 0x10, 0x33, 0xc0, 
   0x03, 0xd0, 0x07, 0x20, 0xf6, 0x32, 0x20, 0xf9, 0x32, 0x60, 0x20, 0xf6, 0x32, 0xa9, 0x02, 0xa2, 
   0x03, 0xa0, 0x36, 0x20, 0xfd, 0x32, 0xad, 0x03, 0x36, 0x2d, 0x04, 0x36, 0xc9, 0xff, 0xd0, 0x03, 
   0x4c, 0x95, 0x32, 0x4c, 0x9e, 0x32, 0x6c, 0xe2, 0x02, 0x6c, 0xe0, 0x02, 0x60, 0x8d, 0x28, 0x36, 
   0x86, 0x45, 0x84, 0x46, 0xa9, 0x00, 0x8d, 0x29, 0x36, 0x20, 0x2d, 0x33, 0x20, 0x41, 0x34, 0x60, 
   0xad, 0x05, 0x36, 0x8d, 0x28, 0x36, 0xad, 0x06, 0x36, 0x8d, 0x29, 0x36, 0xad, 0x03, 0x36, 0x85, 
   0x45, 0xad, 0x04, 0x36, 0x85, 0x46, 0x20, 0x2d, 0x33, 0x20, 0x41, 0x34, 0x60, 0x38, 0xad, 0x3f, 
   0x36, 0xed, 0x31, 0x36, 0x8d, 0x25, 0x36, 0xad, 0x40, 0x36, 0xed, 0x32, 0x36, 0x8d, 0x26, 0x36, 
   0xad, 0x41, 0x36, 0xed, 0x33, 0x36, 0x8d, 0x27, 0x36, 0xad, 0x27, 0x36, 0xf0, 0x01, 0x60, 0x38, 
   0xad, 0x25, 0x36, 0xed, 0x28, 0x36, 0xad, 0x26, 0x36, 0xed, 0x29, 0x36, 0x90, 0x01, 0x60, 0xa0, 
   0xaf, 0x4c, 0x18, 0x32, 0xa9, 0x00, 0x8d, 0x3b, 0x36, 0x20, 0xc0, 0x33, 0xa9, 0xa5, 0xcd, 0x22, 
   0x36, 0xd0, 0x01, 0x60, 0xa0, 0xb0, 0x4c, 0x18, 0x32, 0x20, 0xfb, 0x33, 0xb0, 0x23, 0xad, 0x13, 
   0x36, 0x29, 0x80, 0xd0, 0x03, 0x38, 0xb0, 0x19, 0xad, 0x13, 0x36, 0x29, 0x40, 0xf0, 0xea, 0xa2, 
   0x0a, 0xbd, 0x07, 0x36, 0xc9, 0x3f, 0xf0, 0x05, 0xdd, 0x14, 0x36, 0xd0, 0xdc, 0xca, 0x10, 0xf1, 
   0x18, 0x60, 0xad, 0x1f, 0x36, 0x8d, 0x3e, 0x36, 0xad, 0x20, 0x36, 0x8d, 0x12, 0x36, 0xad, 0x21, 
   0x36, 0x8d, 0x3f, 0x36, 0xad, 0x22, 0x36, 0x8d, 0x40, 0x36, 0xa9, 0x00, 0x8d, 0x41, 0x36, 0x60, 
   0x20, 0xd7, 0x33, 0x20, 0x6d, 0x35, 0xa2, 0x00, 0xac, 0x3d, 0x36, 0xb1, 0x43, 0x9d, 0x13, 0x36, 
   0xc8, 0xe8, 0xe0, 0x10, 0x90, 0xf5, 0x60, 0xae, 0x3b, 0x36, 0x8a, 0x29, 0x07, 0x0a, 0x0a, 0x0a, 
   0x0a, 0x8d, 0x3d, 0x36, 0x8a, 0x29, 0x38, 0x4a, 0x4a, 0x4a, 0x8d, 0x3c, 0x36, 0x18, 0x69, 0x10, 
   0x8d, 0x23, 0x36, 0xa9, 0x00, 0x69, 0x00, 0x8d, 0x24, 0x36, 0x60, 0xee, 0x3b, 0x36, 0xad, 0x3b, 
   0x36, 0xc9, 0x40, 0xd0, 0x02, 0x38, 0x60, 0x20, 0xc0, 0x33, 0x18, 0x60, 0xad, 0x33, 0x36, 0xcd, 
   0x41, 0x36, 0x90, 0x16, 0xd0, 0x12, 0xad, 0x32, 0x36, 0xcd, 0x40, 0x36, 0x90, 0x0c, 0xd0, 0x08, 
   0xad, 0x31, 0x36, 0xcd, 0x3f, 0x36, 0x90, 0x02, 0x38, 0x60, 0x18, 0x60, 0xa9, 0xff, 0x8d, 0x3a, 
   0x36, 0xa9, 0x07, 0x8d, 0x39, 0x36, 0xa9, 0x80, 0x8d, 0x37, 0x36, 0xa9, 0x00, 0x8d, 0x2d, 0x36, 
   0x60, 0xad, 0x28, 0x36, 0x0d, 0x29, 0x36, 0xd0, 0x0a, 0xa0, 0x01, 0x20, 0x0c, 0x34, 0x90, 0x02, 
   0xa0, 0x03, 0x60, 0xad, 0x2d, 0x36, 0x29, 0x80, 0xd0, 0x2e, 0x20, 0x09, 0x35, 0xad, 0x29, 0x36, 
   0xd0, 0x55, 0xad, 0x28, 0x36, 0x30, 0x50, 0x20, 0x0c, 0x34, 0xb0, 0x03, 0x20, 0x6d, 0x35, 0xad, 
   0x23, 0x36, 0x8d, 0x34, 0x36, 0xad, 0x24, 0x36, 0x8d, 0x35, 0x36, 0xad, 0x2d, 0x36, 0x09, 0x80, 
   0x8d, 0x2d, 0x36, 0xa9, 0x00, 0x8d, 0x37, 0x36, 0xad, 0x29, 0x36, 0xd0, 0x21, 0x38, 0xa9, 0x80, 
   0xed, 0x37, 0x36, 0xcd, 0x28, 0x36, 0x90, 0x16, 0xad, 0x28, 0x36, 0x20, 0xd6, 0x34, 0xad, 0x37, 
   0x36, 0x10, 0x08, 0xad, 0x2d, 0x36, 0x29, 0x7f, 0x8d, 0x2d, 0x36, 0x4c, 0x41, 0x34, 0x38, 0xa9, 
   0x80, 0xed, 0x37, 0x36, 0x4c, 0x9b, 0x34, 0xa5, 0x45, 0x8d, 0x04, 0x03, 0xa5, 0x46, 0x8d, 0x05, 
   0x03, 0xae, 0x23, 0x36, 0xac, 0x24, 0x36, 0xad, 0x36, 0x36, 0x18, 0x20, 0xe3, 0x35, 0xa9, 0x80, 
   0x20, 0xa1, 0x35, 0x4c, 0x41, 0x34, 0x8d, 0x2c, 0x36, 0x38, 0xa5, 0x45, 0xed, 0x37, 0x36, 0x85, 
   0x45, 0xb0, 0x02, 0xc6, 0x46, 0xac, 0x37, 0x36, 0xae, 0x2c, 0x36, 0xb1, 0x43, 0x91, 0x45, 0xc8, 
   0xca, 0xd0, 0xf8, 0x18, 0xa5, 0x45, 0x6d, 0x37, 0x36, 0x85, 0x45, 0x90, 0x02, 0xe6, 0x46, 0x8c, 
   0x37, 0x36, 0xad, 0x2c, 0x36, 0x20, 0xa1, 0x35, 0x60, 0xee, 0x39, 0x36, 0xad, 0x39, 0x36, 0xc9, 
   0x08, 0xd0, 0x0e, 0xa9, 0x00, 0x8d, 0x39, 0x36, 0xee, 0x3a, 0x36, 0xad, 0x3a, 0x36, 0x20, 0x25, 
   0x35, 0x20, 0x3a, 0x35, 0x60, 0x20, 0x63, 0x35, 0xae, 0x3a, 0x36, 0xad, 0x12, 0x36, 0xa8, 0xb1, 
   0x43, 0xca, 0xe0, 0xff, 0xd0, 0xf8, 0x8c, 0x38, 0x36, 0x60, 0xad, 0x38, 0x36, 0x0a, 0x0a, 0x8d, 
   0x25, 0x36, 0xa9, 0x00, 0x2a, 0x8d, 0x26, 0x36, 0x0e, 0x25, 0x36, 0x2e, 0x26, 0x36, 0xad, 0x25, 
   0x36, 0x0d, 0x39, 0x36, 0x18, 0x69, 0x19, 0x8d, 0x23, 0x36, 0xad, 0x26, 0x36, 0x69, 0x00, 0x8d, 
   0x24, 0x36, 0x60, 0xa9, 0x18, 0x8d, 0x23, 0x36, 0xa9, 0x00, 0x8d, 0x24, 0x36, 0xad, 0x34, 0x36, 
   0xcd, 0x23, 0x36, 0xd0, 0x09, 0xad, 0x35, 0x36, 0xcd, 0x24, 0x36, 0xd0, 0x01, 0x60, 0xad, 0x23, 
   0x36, 0x8d, 0x34, 0x36, 0xad, 0x24, 0x36, 0x8d, 0x35, 0x36, 0xa5, 0x43, 0x8d, 0x04, 0x03, 0xa5, 
   0x44, 0x8d, 0x05, 0x03, 0xad, 0x36, 0x36, 0xae, 0x23, 0x36, 0xac, 0x24, 0x36, 0x18, 0x4c, 0xe3, 
   0x35, 0x8d, 0x2b, 0x36, 0x18, 0x65, 0x45, 0x85, 0x45, 0x90, 0x02, 0xe6, 0x46, 0xad, 0x2b, 0x36, 
   0x18, 0x6d, 0x2e, 0x36, 0x8d, 0x2e, 0x36, 0x90, 0x08, 0xee, 0x2f, 0x36, 0xd0, 0x03, 0xee, 0x30, 
   0x36, 0xad, 0x2e, 0x36, 0x8d, 0x31, 0x36, 0xad, 0x2f, 0x36, 0x8d, 0x32, 0x36, 0xad, 0x30, 0x36, 
   0x8d, 0x33, 0x36, 0x38, 0xad, 0x28, 0x36, 0xed, 0x2b, 0x36, 0x8d, 0x28, 0x36, 0xb0, 0x03, 0xce, 
   0x29, 0x36, 0x60, 0x8c, 0x0b, 0x03, 0x8e, 0x0a, 0x03, 0x8d, 0x01, 0x03, 0xa9, 0x52, 0x8d, 0x02, 
   0x03, 0x20, 0x53, 0xe4, 0xad, 0x03, 0x03, 0x30, 0x01, 0x60, 0x4c, 0x18, 0x32, 0xa9, 0x00, 0x8d, 
   0x5c, 0x01, 0xac, 0x5c, 0x01, 0xb9, 0x2f, 0x01, 0x8d, 0x54, 0x03, 0xb9, 0x32, 0x01, 0x8d, 0x55, 
   0x03, 0xa2, 0x10, 0xa9, 0x29, 0x9d, 0x42, 0x03, 0xa9, 0x00, 0x9d, 0x4a, 0x03, 0x20, 0x56, 0xe4, 
   0xee, 0x5c, 0x01, 0xad, 0x5c, 0x01, 0xc9, 0x03, 0xd0, 0xd8, 0x18, 0x60, 0x35, 0x3f, 0x4e, 0x01, 
   0x01, 0x01, 0x44, 0x3a, 0x4b, 0x43, 0x50, 0x2e, 0x53, 0x59, 0x53, 0x9b, 0x44, 0x3a, 0x48, 0x41, // |..D:KCP.SYS.D:HA|
   0x4e, 0x44, 0x4c, 0x45, 0x52, 0x53, 0x2e, 0x53, 0x59, 0x53, 0x9b, 0x44, 0x3a, 0x41, 0x55, 0x54, // |NDLERS.SYS.D:AUT|
   0x4f, 0x52, 0x55, 0x4e, 0x2e, 0x53, 0x59, 0x53, 0x9b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // |ORUN.SYS........|
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/*
 * Functions
 */

/*
 * dos3_get_dir_entry()
 */
int dos3_get_dir_entry(const char *path,struct dos3_dir_entry **dirent_found,int *isinfo)
{
   struct dos3_dir_entry *junk;
   if ( !dirent_found ) dirent_found = &junk; // Avoid NULL check later
   int junkinfo;
   if ( !isinfo ) isinfo=&junkinfo;

   *dirent_found = NULL;
   *isinfo = 0;
   while (*path == '/') ++path;
   if ( strchr(path,'/') ) return -ENOENT; // No subdirectories alowed
   if ( strcmp(path,".info")==0 )
   {
      *isinfo = 1;
      return 0;      
   }

   unsigned char name[8+3+1]; // 8+3+NULL
   memset(name,' ',8+3);

   int i;
   for ( i=0;i<8;++i)
   {
      if ( *path && *path != '.' )
      {
         name[i]=*path;
         ++path;
      }
   }
   if ( strcmp(path,".info")==0 )
   {
      *isinfo=1;
      path += 5;
   }
   if ( *path == '.' )
   {
      ++path;
   }
   for ( i=8;i<8+3;++i)
   {
      if ( *path && *path != '.' )
      {
         name[i]=*path;
         ++path;
      }
   }
   if ( strcmp(path,".info")==0 )
   {
      *isinfo=1;
      path += 5;
   }
   if ( *path )
   {
      return -ENOENT; // Name too long
   }

   struct dos3_dir_entry *dirent = SECTOR(16);
   int firstfree = 0;
   for ( int i=1; i<64; ++i ) // Entry 0 is info about the file system
   {
      if (!dirent[i].status)
      {
         if ( !firstfree ) firstfree = i;
         break;
      }
      if ( dirent[i].status == FLAGS_ACTIVE )
      {
         if ( !firstfree ) firstfree = i;
         continue; // Not in use
      }
      if ( (dirent[i].status & FLAGS_OPEN) ) continue; // Hidden; incomplete writes
      if ( memcmp(name,dirent[i].file_name,8+3)!=0 ) continue;

      *dirent_found = &dirent[i];
      return 0;
   }
   if ( firstfree ) *dirent_found = &dirent[firstfree];
   return -ENOENT;
}

/*
 * dos3_info()
 *
 * Return the buffer containing information specific to this file.
 *
 * This could be extended to do a full analysis of binary load files,
 * BASIC files, or any other recognizable file type.
 *
 * The pointer returned should be 'free()'d after use.
 */
char *dos3_info(const char *path,struct dos3_dir_entry *dirent)
{
   char *buf,*b;
   int filesize;

   if ( !dirent )
   {
      filesize = 8*128;
   }
   else
   {
      filesize = (dirent->blocks-1)*1024 + BYTES2(dirent->file_size)%1024;
   }

   buf = malloc(64*1024);
   b = buf;
   *b = 0;
   b+=sprintf(b,"File information and analysis\n\n  %.*s\n  %d bytes\n\n",(int)(strrchr(path,'.')-path),path,filesize);
   if ( dirent )
   {
      b+=sprintf(b,
                 "Directory entry internals:\n"
                 "  Entry %ld\n"
                 "  Flags: $%02x\n"
                 "  1K Clusters: %d\n"
                 "  Starting cluster: %d\n\n",
                 dirent - (struct dos3_dir_entry *)SECTOR(16),
                 dirent->status,dirent->blocks,dirent->start);
   }
   
   // Generic info for the file type
   if ( dirent )
   {
      char *moreinfo;
      moreinfo = atr_info(path,filesize);
      if ( moreinfo )
      {
         int off = b - buf;
         buf = realloc(buf,strlen(buf)+1+strlen(moreinfo));
         b = buf+off; // In case it moved
         b+=sprintf(b,"%s",moreinfo);
         free(moreinfo);
      }
   }

   buf = realloc(buf,strlen(buf)+1);
   return buf;
}

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
   struct dos3_dir_head *head = SECTOR(16);
   unsigned char zeros[14];
   memset(zeros,0,sizeof(zeros));
   if ( memcmp(zeros,head->zeros,sizeof(zeros)) != 0 ) return 1;
   if ( head->dos_id != 0xA5 ) return 1;

   if ( !atrfs.readonly ) // FIXME: No write support yet
   {
      fprintf(stderr,"DOS 3 write support not implemented; read-only mode forced\n");
      atrfs.readonly = 1;
   }
   return 0;
}

/*
 * dos3_getattr()
 */
int dos3_getattr(const char *path, struct stat *stbuf)
{
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   // The only directory option
   if ( strcmp(path,"/") == 0 )
   {
      stbuf->st_ino = 16; // Root dir starts on sector 16
      stbuf->st_size = 0;
      stbuf->st_mode = MODE_DIR(stbuf->st_mode);
      stbuf->st_mode = MODE_RO(stbuf->st_mode);
      return 0;
   }

   // Special files for 1K blocks or clusters
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec >= 0 && sec*8+25+7<=atrfs.sectors )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = 1024;
         stbuf->st_ino = sec*8+25;
         return 0; // Good, can read this sector
      }
   }

   // Real or info files
   struct dos3_dir_entry *dirent;
   int isinfo;
   int r;
   r = dos3_get_dir_entry(path,&dirent,&isinfo);
   if ( r ) return r;
   stbuf->st_ino = dirent ? CLUSTER_TO_SECTOR(dirent->start) : 0;
   if ( isinfo )
   {
      stbuf->st_ino += 0x10000;
      char *info = dos3_info(path,dirent);
      stbuf->st_size = strlen(info);
      free(info);
      return 0;
   }
   stbuf->st_size = (dirent->blocks-1)*1024 + BYTES2(dirent->file_size)%1024;
   return 0; // Whatever, don't really care
}

/*
 * dos3_readdir()
 */
int dos3_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)path; // Always "/"
   (void)offset;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   struct dos3_dir_entry *dirent = SECTOR(16);
   for ( int i=1; i<64; ++i ) // Entry 0 is info about the file system
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: entry %d, status %02x: %.8s.%.3s\n",__FUNCTION__,i,dirent[i].status,dirent[i].file_name,dirent[i].file_ext);
      if (!dirent[i].status) break;
      if ( dirent[i].status == FLAGS_ACTIVE ) continue; // Not in use
      if ( (dirent[i].status & FLAGS_OPEN) ) continue; // Hidden; incomplete writes
      char name[8+1+3+1];
      char *n = name;
      for (int j=0;j<8;++j)
      {
         if ( dirent[i].file_name[j] == ' ' ) break;
         *n = dirent[i].file_name[j];
         ++n;
      }
      if ( dirent[i].file_ext[0] != ' ' )
      {
         *n = '.';
         ++n;
         for (int j=0;j<3;++j)
         {
            if ( dirent[i].file_ext[j] == ' ' ) break;
            *n=dirent[i].file_ext[j];
            ++n;
         }
      }
      *n=0;
      filler(buf, name, FILLER_NULL);
   }
#if 0 // Reserved, DIR, and VTOC sectors
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
         filler(buf,name,FILLER_NULL);
      }
      free(zero);
   }
#endif
#if 0 // Data clusters
   // Create .cluster000 ... .cluster126 as appropriate
   {
      unsigned char *zero = calloc(1,1024);
      if ( !zero ) return -ENOMEM; // Weird
      char name[32];
      int digits = 3;
      for (int sec=0; sec*8+25+7<=atrfs.sectors; ++sec)
      {
         unsigned char *s = CLUSTER(sec);
         if ( memcmp(s,zero,1024) == 0 ) continue; // Skip empty sectors
         sprintf(name,".cluster%0*d",digits,sec);
         filler(buf,name,FILLER_NULL);
      }
      free(zero);
   }
#endif
   return 0;
}

/*
 * dos3_read()
 */
int dos3_read(const char *path, char *buf, size_t size, off_t offset)
{

   // Magic /.cluster### files
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
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

   // Look up file
   struct dos3_dir_entry *dirent;
   int isinfo;
   int r;
   r = dos3_get_dir_entry(path,&dirent,&isinfo);
   if ( r ) return r;

   // Info files
   if ( isinfo )
   {
      char *info = dos3_info(path,dirent);
      char *i = info;
      int bytes = strlen(info);
      if ( offset >= bytes )
      {
         free(info);
         return -EOF;
      }
      bytes -= offset;
      i += offset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(buf,i,bytes);
      free(info);
      return bytes;
   }
   
   // Regular files
   unsigned char *vtoc = SECTOR(24);
   int cluster = dirent->start;
   int bytes_read = 0;
   while ( size )
   {
      int bytes = 1024;
      if ( vtoc[cluster] == 0xff ) return -EIO; // Reserved entry
      if ( vtoc[cluster] == 0xfe ) return -EIO; // Free entry
      if ( vtoc[cluster] == 0xfd )
      {
         bytes = BYTES2(dirent->file_size)%1024;
      }
      unsigned char *s=CLUSTER(cluster);
      if ( !s ) return -EIO; // Invalid CLUSTER
      if ( offset < bytes )
      {
         bytes -= offset;
         s+=offset;
         offset = 0;
      }
      if ( offset > bytes )
      {
         offset -= bytes;
         bytes = 0;
      }
      if ( (size_t)bytes > size ) bytes = size;
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s read %d bytes from cluster %d offset %ld\n",__FUNCTION__,path,bytes,cluster,offset);
      if ( bytes )
      {
         memcpy(buf,s,bytes);
         buf += bytes;
         bytes_read += bytes;
         size -= bytes;
      }
      
      // Next cluster
      if ( !size ) break;
      if ( vtoc[cluster] == 0xFD ) break; // EOF
      cluster=vtoc[cluster];
   }
   if ( bytes_read ) return bytes_read;
   return -EOF;
}

// Implement these for read-write support
int dos3_write(const char *path, const char *buf, size_t size, off_t offset);
int dos3_unlink(const char *path);
int dos3_rename(const char *path1, const char *path2, unsigned int flags);
int dos3_chmod(const char *path, mode_t mode);
int dos3_create(const char *path, mode_t mode);
int dos3_truncate(const char *path, off_t size);

/*
 * dos3_newfs()
 */
int dos3_newfs(void)
{
   if ( atrfs.sectorsize != 128 )
   {
      fprintf(stderr,"ERROR: DOS 3 only works with SD disks\n");
      return -EINVAL;
   }
   if ( atrfs.sectors > 1048 ) // Yes, you can add one extra block and it will work.
   {
      fprintf(stderr,"ERROR: DOS 3 only supports up to 1048 sectors\n");
      return -EINVAL;
   }
   if ( atrfs.sectors < 32 )
   {
      fprintf(stderr,"ERROR: DOS 3 needs 24 sectors plus 8 sectors per data block\n");
      return -EINVAL;
   }

   unsigned char *vtoc = SECTOR(24);
   for (int i=0;i<=MAX_CLUSTER;++i) vtoc[i] = VTOC_FREE;
   for (int i=MAX_CLUSTER+1;i<128;++i) vtoc[i] = VTOC_RESERVED;
   struct dos3_dir_head *head = SECTOR(16);
   head->max_free_blocks = TOTAL_CLUSTERS;
   head->dos_id = 0xA5;

   // Write out boot sectors
   // Testing shows that there are no differences between SD and ED on the boot sectors
   unsigned char *s = SECTOR(1);
   memcpy(s,bootsectors,sizeof(bootsectors));
   return 0;
}

/*
 * dos3_statfs()
 */
int dos3_statfs(const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = 1024;
   stfsbuf->f_frsize = 1024;
   stfsbuf->f_blocks = (atrfs.sectors - 24) / 8;
   unsigned char *vtoc = SECTOR(24);
   stfsbuf->f_bfree = 0;
   for ( int i=0;i<0x80;++i )
   {
      if ( vtoc[i]==0xfe ) ++stfsbuf->f_bfree;
   }
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = 63;
   stfsbuf->f_ffree = 0;
   struct dos3_dir_entry *dirent = SECTOR(16);
   for (int i=1;i<64;++i)
   {
      if ( (dirent[i].status == FLAGS_ACTIVE) || (dirent[i].status == 0x00) )
      {
         ++stfsbuf->f_ffree;
      }
   }
   stfsbuf->f_namemax = 12; // 8.3 including '.'
   // stfsbuf->f_namemax += 5; // Don't report this for ".info" files; not needed by FUSE
   return 0;
}

/*
 * dos3_fsinfo()
 */
char *dos3_fsinfo(void)
{
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;
   
   struct dos3_dir_head *head = SECTOR(16);
   b+=sprintf(b,"Total data clusters: %d\n",head->max_free_blocks);
   int free_count=0;
   unsigned char *vtoc = SECTOR(24);
   for ( int i=0;i<0x80;++i )
   {
      if ( vtoc[i]==0xfe ) ++free_count;
   }
   b+=sprintf(b,"Free clusters:       %d\n",free_count);
   return buf;
}
