/*
 * sparta.c
 *
 * Functions for accessing SpartaDOS file systems.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 *
 * Notes:
 *
 * SpartaDOS uses a list of the sectors that comprise a file instead of
 * using a sector chain at the end of each sector.  Becuase the code
 * overuses the term "sector," I've tried to consistently refer to the
 * first sector that contains the list of sectors comprising a file as the
 * "inode" of the file, as it has the index of sectors of the file.
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
#define OFFSET_TO_SECTOR(n)         ((n)/atrfs.sectorsize)
#define OFFSET_TO_SECTOR_OFFSET(n)  ((n)%atrfs.sectorsize)

/*
 * File System Structures
 */

// https://atariwiki.org/wiki/attach/SpartaDOS/SpartaDOS%20X%204.48%20User%20Guide.pdf
// Disk structure is at page 151
struct sector1_sparta {
   // Initial fields used by the OS to boot
   unsigned char pad_zero; // Usually 00, could be 0x53 for 'S' for SD
   unsigned char boot_sectors; // Usually 3
   unsigned char boot_addr[2]; // Where to load the boot sectors in memory
   unsigned char dos_ini[2]; // JSR here at the end
   unsigned char jmp; // Normally 0x4C, but could be start of code (MyDOS/DOS 2)
   unsigned char exec_addr[2]; // execute address after boot sectors loaded ([0] is 8)
   // Fields used by DOS, so specific to the version
   unsigned char dir[2]; // sector start of main directory sector map
   unsigned char sectors[2]; // number of sectors; should match atr_head
   unsigned char free[2]; // number of free sectors
   unsigned char bitmap_sectors; // number of bit map sectors
   unsigned char first_bitmap[2]; // first bit map sector
   unsigned char sec_num_allocation[2];
   unsigned char sec_num_dir_alloc[2];
   unsigned char volume_name[8];
   unsigned char track_count; // high bit indicates doulbe-sided; '01' indicates non-floppy
   unsigned char sector_size; // 0x00: 256-bytes, 0x01: 512-bytes, 0x80: 128-bytes
   unsigned char revision; // 0x11: SD1.1; 0x20: 2.x, 3.x, 4.1x, 4.2x; 0x21: SDSF 2.1
   unsigned char rev_specific[4]; // depends on the revision; not important here
   unsigned char vol_sequence;
   unsigned char vol_random;
   unsigned char sec_num_file_load_map[2];
   unsigned char lock; // Not used in 2.1; 0xff for lock in older versions
   // Boot code follows
};

// Each file or directory has a sector map; this is like the map in a Unix inode
// Sector maps are an array of sector numbers
// Entry 0 is the next sector of the sector map (if needed; otherwise zero)
// Entry 1 is the previous sector map of the file (zero if the first)
// Entries 2..end are the sequence of sectors; 0x00 indicates data is all zeros (sparse file)
// The Sparta directory header; the first 23 bytes
struct sparta_dir_header {
   unsigned char pad_0; // status not used for first entry
   unsigned char parent_dir_inode_map[2];
   unsigned char dir_length_bytes[3];
   unsigned char dir_name[8];
   unsigned char dir_ext_pad[3];
   unsigned char file_date[3]; // dd/mm/yy (directory creation date)
   unsigned char file_time[3]; // hh/mm/ss
};
struct sparta_dir_entry {
   unsigned char status; // bit 3: in use; bit 5: subdirectory
   unsigned char sector_map[2];
   unsigned char file_size_bytes[3];
   unsigned char file_name[8];
   unsigned char file_ext[3];
   unsigned char file_date[3]; // dd/mm/yy
   unsigned char file_time[3]; // hh/mm/ss
};
enum sparta_dir_status {
   FLAGS_LOCKED  = 0x01,
   FLAGS_IN_USE  = 0x08,
   FLAGS_DELETED = 0x10,
   FLAGS_DIR     = 0x20,
};

/*
 * Function prototypes
 */
int sparta_sanity(void);
int sparta_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int sparta_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int sparta_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int sparta_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int sparta_mkdir(const char *path,mode_t mode);
int sparta_rmdir(const char *path);
int sparta_unlink(const char *path);
int sparta_rename(const char *path1, const char *path2, unsigned int flags);
int sparta_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int sparta_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int sparta_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int sparta_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);
int sparta_statfs(const char *path, struct statvfs *stfsbuf);
int sparta_newfs(void);
char *sparta_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops sparta_ops = {
   .name = "SpartaDOS",
   .fs_sanity = sparta_sanity,
   .fs_getattr = sparta_getattr,
   .fs_readdir = sparta_readdir,
   .fs_read = sparta_read,
   //.fs_write = sparta_write,
   //.fs_mkdir = sparta_mkdir,
   //.fs_rmdir = sparta_rmdir,
   //.fs_unlink = sparta_unlink,
   //.fs_rename = sparta_rename,
   //.fs_chmod = sparta_chmod,
   //.fs_create = sparta_create,
   //.fs_truncate = sparta_truncate,
   //.fs_statfs = sparta_statfs,
   //.fs_newfs = sparta_newfs,
   //.fs_fsinfo = sparta_fsinfo,
};

/*
 * Boot Sectors
 */
#if 0 // FIXME: Enable when implementing newfs
static const char bootsectors[ATR_MAXFSTYPE][3*128] =
{
   // SpartaDOS 1.1
   [ATR_SPARTA] = {
 0x00, 0x03, 0x00, 0x40, 0x94, 0x18, 0x4c, 0x80, 0x40, 0x35, 0x00, 0xd0, 0x02, 0xf9, 0x00, 0x01, 
 0x34, 0x00, 0xe3, 0x01, 0x3b, 0x00, 0xa0, 0xcd, 0xe1, 0xf3, 0xf4, 0xe5, 0xf2, 0xa0, 0x28, 0x80, 
 0x11, 0x06, 0x01, 0x80, 0x80, 0x30, 0x02, 0x19, 0x1d, 0x19, 0x28, 0xff, 0x00, 0x00, 0x00, 0x00, 
 0x7d, 0x1d, 0x20, 0x20, 0x20, 0x20, 0x20, 0x53, 0x70, 0x61, 0x72, 0x74, 0x61, 0x44, 0x4f, 0x53, //|}.     SpartaDOS|
 0x20, 0x20, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x20, 0x31, 0x2e, 0x31, 0x20, 0x48, 0x53, //|  Version 1.1 HS|
 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x43, 0x6f, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, //|        Copyrigh|
 0x74, 0x20, 0x28, 0x43, 0x29, 0x20, 0x31, 0x39, 0x38, 0x34, 0x20, 0x20, 0x62, 0x79, 0x20, 0x49, //|t (C) 1984  by I|
 0x43, 0x44, 0x2c, 0x20, 0x49, 0x4e, 0x43, 0x2e, 0x9b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //|CD, INC.........|
 0xa9, 0x04, 0x8d, 0x0a, 0x03, 0xa2, 0x00, 0xad, 0x1f, 0x40, 0x8d, 0x08, 0x03, 0xd0, 0x01, 0xe8, 
 0x8e, 0x09, 0x03, 0xa9, 0x00, 0x8d, 0x04, 0x03, 0xa9, 0x06, 0x8d, 0x05, 0x03, 0xa9, 0x40, 0x8d, 
 0x03, 0x03, 0xad, 0x2b, 0x40, 0x10, 0x10, 0xad, 0x05, 0x03, 0xc9, 0x09, 0xd0, 0x03, 0xee, 0x2b, 
 0x40, 0x20, 0x59, 0xe4, 0x4c, 0xc9, 0x40, 0x20, 0x30, 0x07, 0xad, 0x2b, 0x40, 0xd0, 0x09, 0xee, 
 0x2b, 0x40, 0xad, 0xef, 0x1c, 0x8d, 0x2a, 0x40, 0x98, 0x30, 0x21, 0xad, 0x2a, 0x40, 0x8d, 0xef, 
 0x1c, 0x18, 0xad, 0x04, 0x03, 0x6d, 0x08, 0x03, 0x8d, 0x04, 0x03, 0xad, 0x05, 0x03, 0x6d, 0x09, 
 0x03, 0x8d, 0x05, 0x03, 0xee, 0x0a, 0x03, 0xce, 0x25, 0x40, 0x10, 0x02, 0x38, 0x60, 0xd0, 0xad, 
 0xad, 0x21, 0x40, 0x8d, 0xcc, 0x1e, 0xad, 0x22, 0x40, 0x09, 0x30, 0x8d, 0xcd, 0x1d, 0xa9, 0x3e, 
 0x85, 0x0a, 0xa9, 0x1d, 0x85, 0x0b, 0xa0, 0x00, 0xb9, 0x1a, 0x03, 0xc9, 0x45, 0xf0, 0x05, 0xc8, 
 0xc8, 0xc8, 0xd0, 0xf4, 0xb9, 0x1b, 0x03, 0x85, 0x45, 0xb9, 0x1c, 0x03, 0x85, 0x46, 0x8c, 0xda, 
 0x1f, 0xa0, 0x0b, 0xb1, 0x45, 0x99, 0xce, 0x1f, 0x88, 0x10, 0xf8, 0xa0, 0x03, 0xb9, 0x26, 0x40, 
 0x99, 0xd2, 0x1f, 0x88, 0x10, 0xf7, 0xa0, 0x04, 0xa2, 0x01, 0x20, 0x6d, 0x41, 0xa0, 0x06, 0xa2, 
 0x04, 0x20, 0x6d, 0x41, 0xe6, 0x09, 0x20, 0x94, 0x18, 0xa9, 0x30, 0x8d, 0x44, 0x03, 0xa9, 0x40, 
 0x8d, 0x45, 0x03, 0xa9, 0x50, 0x8d, 0x48, 0x03, 0xa9, 0x09, 0x8d, 0x42, 0x03, 0xa2, 0x00, 0x8e, 
 0xdb, 0x1f, 0x20, 0x56, 0xe4, 0xa0, 0x01, 0x20, 0x6a, 0x19, 0x4c, 0xac, 0x19, 0xb1, 0x45, 0x18, 
 0x69, 0x01, 0x9d, 0x40, 0x19, 0xc8, 0xb1, 0x45, 0x69, 0x00, 0x9d, 0x41, 0x19, 0x60, 0x00, 0x00, 
   },
};
#endif

/*
 * Functions
 */

/*
 * sparta_get_sector()
 *
 * Given an inode number (first sector map sector number)
 * and a relative sector number in the file, return the
 * absolute sector number.
 *
 * Sequence numbers start at zero for this function.
 *
 * If negative, it's an error.
 */
int sparta_get_sector(int inode,int sequence)
{
   unsigned char *s=SECTOR(inode);

   while ( sequence > atrfs.sectorsize/2-2 )
   {
      int next = BYTES2(s);
      if ( next > atrfs.sectors ) return -EIO; // Corrupt chain
      if ( next == 0 ) return -EOF; // Past end of file
      unsigned char *n = SECTOR(next);
      if ( BYTES2(n+2) != inode ) // Verify back pointer
      {
         return -EIO; // Bad linked list
      }
      inode = next;
      s = n;
      sequence -= atrfs.sectorsize/2-2;
   }
   int r = BYTES2(s+4+sequence*2);
   if ( r < 1 || r > atrfs.sectors )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: inode sector %d, sequence %d, invalid sector %d\n",__FUNCTION__,inode,sequence,r);
      r = -EIO;
   }
   return r;
}

/*
 * sparta_get_dirent()
 *
 * Fill in a buffer with the 'entry'th directory entry.  This may span
 * two non-contiguous sectors, so we can't just return a pointer.
 *
 * Returns zero on success, negative errno on error
 */
int sparta_get_dirent(struct sparta_dir_entry *dirent,int dirinode,int entry)
{
   int sector;
   int offset = entry * sizeof(*dirent);
   unsigned char *s;
   int bytes;

   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset));
   if ( sector < 0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: dirinode sector %d, entry %d, offset %d, relative sector %d+1, invalid sector %d\n",__FUNCTION__,dirinode,entry,offset,OFFSET_TO_SECTOR(offset),sector);
      return sector;
   }
   s = SECTOR(sector);
   bytes = sizeof(*dirent);
   int secoff = OFFSET_TO_SECTOR_OFFSET(offset);
   if ( bytes + secoff > atrfs.sectorsize )
   {
      bytes = atrfs.sectorsize - secoff;
   }
   memcpy(dirent,s+secoff,bytes);
   dirent = (void *)(((char *)dirent) + bytes);
   bytes = sizeof(*dirent) - bytes;
   if ( bytes == 0 ) return 0;
   // Read remainder from next sector
   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset)+1);
   if ( sector < 0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: dirinode sector %d, entry %d, offset %d, relative sector %d+1, invalid sector %d\n",__FUNCTION__,dirinode,entry,offset,OFFSET_TO_SECTOR(offset),sector);
      return sector;
   }
   s = SECTOR(sector);
   memcpy(dirent,s,bytes);
   return 0;
}

/*
 * sparta_put_dirent()
 *
 * Copy a buffer to the 'entry'th directory entry.  This may span
 * two non-contiguous sectors.
 *
 * Returns zero on success, negative errno on error
 */
int sparta_put_dirent(struct sparta_dir_entry *dirent,int dirinode,int entry)
{
   int sector;
   int offset = entry * sizeof(*dirent);
   unsigned char *s;
   int bytes;

   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset));
   if ( sector < 0 ) return sector;
   s = SECTOR(sector);
   bytes = sizeof(*dirent);
   int secoff = OFFSET_TO_SECTOR_OFFSET(offset);
   if ( bytes + secoff > atrfs.sectorsize )
   {
      bytes = atrfs.sectorsize - secoff;
   }
   memcpy(s+secoff,dirent,bytes);
   dirent = (void *)(((char *)dirent) + bytes);
   bytes = sizeof(*dirent) - bytes;
   if ( bytes == 0 ) return 0;
   // Read remainder from next sector
   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset)+1);
   if ( sector < 0 ) return sector;
   s = SECTOR(sector);
   memcpy(s,dirent,bytes);
   return 0;
}

/*
 * sparta_sanity()
 *
 * Return 0 if this is a valid Sparta file system
 */
int sparta_sanity(void)
{
   if ( atrfs.sectors < 6*1024/atrfs.sectorsize ) return 1; // I think 6K is the minimum to have bitmaps, directory, and a one-byte file
   struct sector1_sparta *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 3 ) return 1; // Must have 3 boot sectors

   if ( BYTES2(sec1->dir) > atrfs.sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: Main directory sector map > sector count: %d > %d\n",BYTES2(sec1->dir), atrfs.sectors);
      return 1;
   }
   if ( BYTES2(sec1->sectors) != atrfs.sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: Sparta sector count != image sector count %d != %d\n",BYTES2(sec1->sectors), atrfs.sectors);
      return 1;
   }
   if ( BYTES2(sec1->free) >= atrfs.sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: Free sector count >= sector count %d != %d\n",BYTES2(sec1->free), atrfs.sectors);
      return 1;
   }
   if ( !sec1->bitmap_sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: No bitmap sectors\n");
      return 1;
   }
   if ( (sec1->bitmap_sectors-1)*8*atrfs.sectorsize >= atrfs.sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: Too many bitmap sectors %d\n",sec1->bitmap_sectors);
      return 1;
   }
   if ( BYTES2(sec1->first_bitmap) >= atrfs.sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: first bitmap >= sector count %d != %d\n",BYTES2(sec1->first_bitmap), atrfs.sectors);
      return 1;
   }
   if ( BYTES2(sec1->sec_num_allocation) >= atrfs.sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: sector number alloc >= sector count %d != %d\n",BYTES2(sec1->sec_num_allocation), atrfs.sectors);
      return 1;
   }
   if ( BYTES2(sec1->sec_num_dir_alloc) >= atrfs.sectors )
   {
      if ( options.debug ) printf("Not SpartaDOS: sector number dir alloc >= sector count %d != %d\n",BYTES2(sec1->sec_num_dir_alloc), atrfs.sectors);
      return 1;
   }
   return 0;
}

/*
 * sparta_path()
 *
 * Given a path, return the starting sector number.
 * 
 * Return value:
 *   0 File found
 *   1 File not found, but could be created in parent_dir
 *   -x return this error (usually -ENOENT)
 */
int sparta_path(const char *path,int *inode,int *parent_dir_inode,int *size,int *locked,int *entry,int *isdir,int *isinfo)
{
   unsigned char name[8+3+1]; // 8+3+NULL
   struct sparta_dir_header dir_header;
   struct sector1_sparta *sec1 = SECTOR(1);
   int r;

   if ( !*inode )
   {
      *parent_dir_inode = BYTES2(sec1->dir);
      *inode = BYTES2(sec1->dir);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s default dir %d\n",__FUNCTION__,path,*parent_dir_inode);
   }
   else
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s recursing on dir %d\n",__FUNCTION__,path,*parent_dir_inode);
   }
   *isdir=0;
   *isinfo=0;
   while ( *path )
   {
      while ( *path == '/' ) ++path;
      if ( ! *path )
      {
         *isdir = 1;
         // Get size from directory header
         r = sparta_get_dirent((void *)&dir_header,*inode,0);
         if ( r < 0 ) return r;
         *size = BYTES3(dir_header.dir_length_bytes);
         return 0;
      }

      // Extract the file name up to the trailing slash
      memset(name,' ',8+3);
      name[8+3]=0;
      int i;
      for ( i=0;i<8;++i)
      {
         if ( *path && *path != '.' && *path != '/' )
         {
            name[i]=*path;
            ++path;
         }
      }
      if ( !options.noinfofiles && strncmp(path,".info",5)==0 )
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
         if ( *path && *path != '.' && *path != '/' )
         {
            name[i]=*path;
            ++path;
         }
      }
      if ( strncmp(path,".info",5)==0 )
      {
         *isinfo=1;
         path += 5;
      }
      if ( *path && *path != '/' )
      {
         return -ENOENT; // Name too long
      }
      if ( *path == '/' ) ++path;

      if ( options.debug ) fprintf(stderr,"DEBUG: %s: Look for %11.11s in dir at sector %d\n",__FUNCTION__,name,*inode);
      // Scan parent_dir_inode for file name
      int firstfree = -1;
      r = sparta_get_dirent((void *)&dir_header,*inode,0);
      if ( r < 0 ) return r;
      int dir_size = BYTES3(dir_header.dir_length_bytes);
      for ( i=1;i<(int)(dir_size/sizeof(dir_header));++i )
      {
         struct sparta_dir_entry dir_entry;
         r = sparta_get_dirent(&dir_entry,*inode,i);
         if ( r < 0 ) return r;

         //if ( options.debug ) fprintf(stderr,"DEBUG: %s: entry %d:%d %11.11s flags %02x\n",__FUNCTION__,i,j,dirent[j].file_name,dirent[j].flags);
         if ( dir_entry.status & FLAGS_DELETED )
         {
            if ( firstfree < 0 ) firstfree = i;
            continue;
         }
         if ( !(dir_entry.status & FLAGS_IN_USE) )  // FIXME: Not sure if this happens or if the directory size grows as more are added
         {
            if ( firstfree < 0 ) firstfree = i;
            continue;
         }
         if ( strncmp((char *)dir_entry.file_name,(char *)name,8+3) != 0 ) continue;
         *entry = i;
         // subdirectories
         //if ( atrfs.fstype == ATR_SPARTA )
         if ( ! *isinfo )
         {
            if ( dir_entry.status & FLAGS_DIR )
            {
               if ( *path )
               {
                  *parent_dir_inode = *inode;
                  *inode = BYTES2(dir_entry.sector_map);
                  if ( options.debug ) fprintf(stderr,"DEBUG: %s: recurse in with dir %d path %s\n",__FUNCTION__,*parent_dir_inode,path);
                  return sparta_path(path,inode,parent_dir_inode,size,locked,entry,isdir,isinfo);
               }
               *isdir = 1;
               *inode = BYTES2(dir_entry.sector_map);
               return 0;
            }
         }
         if ( *path ) return -ENOTDIR; // Should have been a directory
         *size = BYTES3(dir_entry.file_size_bytes);
         *parent_dir_inode = *inode;
         *inode = BYTES2(dir_entry.sector_map);
         if ( dir_entry.status & FLAGS_LOCKED ) *locked=1;
         return 0;
      }
      if ( *path ) return -ENOENT; // Not found in directory scan
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: Did not find %11.11s in dir at sector %d; could create\n",__FUNCTION__,name,*inode);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: set dir sector %d\n",__FUNCTION__,*parent_dir_inode);
      *parent_dir_inode = *inode;
      *inode = 0;
      *entry = firstfree; // Might be -1 indicating extend the file
      *size = 0;
      return 1; // FIXME: What is the directly length limit? Enforce it!
   }
   return -ENOENT; // Shouldn't be reached
}

/*
 * sparta_getattr()
 */
int sparta_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
   (void)fi;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;

   // Set time stamp
   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,isdir?0:entry);
   if ( r<0 ) return r;
   struct tm tm;
   memset(&tm,0,sizeof(tm));
   tm.tm_sec=dir_entry.file_time[2];
   tm.tm_min=dir_entry.file_time[1];
   tm.tm_hour=dir_entry.file_time[0];
   tm.tm_mday=dir_entry.file_date[0];
   tm.tm_mon=dir_entry.file_date[1]-1; // Want 0-11, not 1-12
   tm.tm_year=dir_entry.file_date[2];
   if ( tm.tm_year < 78 ) tm.tm_year += 100; // FIXME: Is there a standard for this?
   // tm.tm_year += 1900; // Spec: Year - 1900, so don't add 1900
   time_t secs = mktime(&tm);
   struct timespec ts;
   ts.tv_nsec = 0;
   ts.tv_sec = secs;
   if ( secs != -1 )
   {
      stbuf->st_mtim = ts;
      stbuf->st_ctim = ts;
      stbuf->st_atim = ts;
   }

   if ( isinfo )
   {
      stbuf->st_mode = MODE_RO(stbuf->st_mode); // These files are never writable
      stbuf->st_ino = inode + 0x10000;

      // FIXME
      // char *info = sparta_info(path,dirent,parent_dir_inode,entry,inode,sectors,filesize);
      char *info = strdup("Detailed file information not available for SpartaDOS yet\n");
      stbuf->st_size = strlen(info);
      free(info);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s info\n",__FUNCTION__,path);
      return 0;
   }

   if ( isdir )
   {
      ++stbuf->st_nlink;
      stbuf->st_mode = MODE_DIR(atrfs.atrstat.st_mode & 0777);
   }
   if ( locked ) stbuf->st_mode = MODE_RO(stbuf->st_mode);
   stbuf->st_size = size;
   stbuf->st_ino = inode;
   stbuf->st_blocks = (size + atrfs.sectorsize -1) / atrfs.sectorsize + 1; // Approx; accuracy isn't important
   return 0; // FIXME
}

/*
 * sparta_readdir()
 */
int sparta_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
   (void)offset; // FUSE will always read directories from the start in our use
   (void)fi;
   (void)flags;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   int sector=0,parent_dir_inode,dir_size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&sector,&parent_dir_inode,&dir_size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( !isdir ) return -ENOTDIR;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s dir with map at sector %d; %d bytes\n",__FUNCTION__,path,sector,dir_size);

   char name[8+1+3+1];
   struct sparta_dir_entry dir_entry;
   for ( int i=1;i<(int)(dir_size/sizeof(dir_entry));++i )
   {
      r = sparta_get_dirent(&dir_entry,sector,i);
      if ( r < 0 ) return r;
      if ( dir_entry.status & FLAGS_DELETED ) continue;
      if ( !(dir_entry.status & FLAGS_IN_USE) ) continue;
      memcpy(name,dir_entry.file_name,8);
      int k;
      for (k=0;k<8;++k)
      {
         if ( dir_entry.file_name[k] == ' ' ) break;
      }
      name[k]=0;
      if ( dir_entry.file_ext[0] != ' ' )
      {
         name[k]='.';
         ++k;
         for (int l=0;l<3;++l)
         {
            if ( dir_entry.file_ext[l] == ' ' ) break;
            name[k+l]=dir_entry.file_ext[l];
            name[k+l+1]=0;
         }
      }
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s %s\n",__FUNCTION__,path,name);
      filler(buf, name, NULL, 0, 0);
   }
   return 0;
}

/*
 * sparta_read()
 */
int sparta_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;
   int inode=0,parent_dir_inode,filesize,locked,entry,isdir,isinfo;
   int r;
   struct sparta_dir_entry dir_entry;
   r = sparta_path(path,&inode,&parent_dir_inode,&filesize,&locked,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;

   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r < 0 ) return r;

   if ( isinfo )
   {
#if 0 // FIXME
      char *info = sparta_info(path,&dir_entry,parent_dir_inode,entry,inode,filesize);
#else
      char *info = strdup("File info not implemented yet for SpartaDOS\n");
#endif
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

   if ( offset >= filesize ) return -EOF;

   r=0;
   while ( size && offset < filesize )
   {
      int sequence = offset / atrfs.sectorsize;
      int sector = sparta_get_sector(inode,sequence);
      int bytes;
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s inode sector %d, size %ld, offset %ld, relative sector %d\n",__FUNCTION__,path,inode,size,offset,sequence);
      if ( sector < 0 ) return sector;
      char *s = SECTOR(sector);
      bytes = atrfs.sectorsize;
      if ( sequence * atrfs.sectorsize + bytes > filesize ) bytes = filesize - sequence * atrfs.sectorsize;
      bytes -= offset%atrfs.sectorsize;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(buf,s+offset%atrfs.sectorsize,bytes);
      buf+=bytes;
      size-=bytes;
      r+=bytes;
      offset+=bytes;
   }
   if ( r == 0 && size ) r=-EOF;
   return r;
}
