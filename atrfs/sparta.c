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
 *
 * This handles time stamps slightly differently from SpartaDOS.  The time
 * stamp in entry 0 for a directory is the creation time of the directory,
 * as in SpartaDOS.  The time stamp in the entry for the subdirectory in
 * the parent directory is updated when the directory is modified as would
 * be expected in Linux, while SpartaDOS never changes directory time stamps.
 *
 * Also, it appears that in SDFS 2.1 (SpartaDOS X), an extra blank entry is not
 * created at the end of directories, but the code here will do so.
 *
 * When SpartaDOS extends a directory, the size is only inceased in entry 0 of
 * the directory.  This code updates the size in both entry 0 and in the parent
 * directory entry so that they match.
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
#define SIZE_TO_SECTORS(n)          ((int)(((n)+atrfs.sectorsize - 1)/atrfs.sectorsize))
#define ENTRIES_PER_MAP_SECTOR      (atrfs.sectorsize/2-2)
#define SIZE_TO_MAP_SECTORS(n)      ((SIZE_TO_SECTORS(n)+ENTRIES_PER_MAP_SECTOR-1)/ENTRIES_PER_MAP_SECTOR)
#define BITMAPBYTE(n)   (n/8)
#define BITMAPMASK(n)   (1<<(7-(n%8)))

/*
 * File System Structures
 */

// https://atariwiki.org/wiki/attach/SpartaDOS/SpartaDOS%20X%204.48%20User%20Guide.pdf
// Disk structure is at page 151
struct sector1_sparta {
   // Initial fields used by the OS to boot
   unsigned char pad_zero; // Usually 00, could be 0x53 for 'S' for SpartaDOS
   unsigned char boot_sectors; // Usually 3, 1 if 512-byte sectors
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
   unsigned char track_count; // high bit indicates double-sided; '01' indicates non-floppy
   unsigned char sector_size; // 0x00: 256-bytes, 0x01: 512-bytes, 0x80: 128-bytes
   unsigned char revision; // 0x11: SD1.1; 0x20: 2.x, 3.x, 4.1x, 4.2x; 0x21: SDSF 2.1
   unsigned char rev_specific[5]; // depends on the revision; not important here
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
   unsigned char parent_dir_map[2];
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
int sparta_alloc_any_sector(void);
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
   .fs_write = sparta_write,
   .fs_mkdir = sparta_mkdir,
   .fs_rmdir = sparta_rmdir,
   .fs_unlink = sparta_unlink,
   .fs_rename = sparta_rename,
   .fs_chmod = sparta_chmod,
   .fs_create = sparta_create,
   .fs_truncate = sparta_truncate,
   .fs_utimens = sparta_utimens,
   .fs_statfs = sparta_statfs,
   .fs_newfs = sparta_newfs,
   .fs_fsinfo = sparta_fsinfo,
};

/*
 * Boot Sectors
 */
static const char bootsectors[1][3*128] =
{
#if 0
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
#endif
   // SpartaDOS FS 2.1 (SpartaDOS X)
   [0] = {
 0x00, 0x03, 0x00, 0x30, 0xe0, 0x07, 0x4c, 0x80, 0x30, 0x06, 0x00, 0x40, 0x0b, 0xbe, 0x03, 0x02, 
 0x04, 0x00, 0x82, 0x07, 0xf1, 0x02, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xd0, 0x00, // Volume name is spaces
 0x21, 0x00, 0x01, 0x7e, 0x00, 0x01, 0xde, 0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
 0x00, 0x00, 0x00, 0x00, 0x45, 0x72, 0x72, 0x6f, 0x72, 0x3a, 0x20, 0x4e, 0x6f, 0x20, 0x44, 0x4f, //|....Error: No DO|
 0x53, 0x9b, 0xad, 0x28, 0x30, 0x8d, 0x0a, 0x03, 0xad, 0x29, 0x30, 0x8d, 0x0b, 0x03, 0xa9, 0x00, //|S..(0....)0.....|
 0xa2, 0x2f, 0x20, 0xf1, 0x30, 0xad, 0x00, 0x2f, 0x8d, 0x28, 0x30, 0xad, 0x01, 0x2f, 0x8d, 0x29, 
 0x30, 0xa0, 0x04, 0x84, 0x91, 0xa4, 0x91, 0xcc, 0x1f, 0x30, 0xf0, 0xd6, 0xb9, 0x00, 0x2f, 0x8d, 
 0x0a, 0x03, 0xb9, 0x01, 0x2f, 0x8d, 0x0b, 0x03, 0xc8, 0xc8, 0x84, 0x91, 0x60, 0x6c, 0xe2, 0x02, 
 0xa2, 0x00, 0xad, 0x1f, 0x30, 0x85, 0x91, 0x85, 0x90, 0x8d, 0x08, 0x03, 0xd0, 0x01, 0xe8, 0x8e, 
 0x09, 0x03, 0x20, 0x0a, 0x31, 0x85, 0x96, 0x20, 0x0a, 0x31, 0x25, 0x96, 0xc9, 0xff, 0xd0, 0x37, 
 0xa9, 0x7c, 0x8d, 0xe2, 0x02, 0xa9, 0x30, 0x8d, 0xe3, 0x02, 0x20, 0x0a, 0x31, 0x85, 0x92, 0x20, 
 0x0a, 0x31, 0x85, 0x93, 0x05, 0x92, 0xf0, 0x1c, 0x20, 0x0a, 0x31, 0x38, 0xe5, 0x92, 0x48, 0x08, 
 0x20, 0x0a, 0x31, 0x28, 0xe5, 0x93, 0x85, 0x95, 0x68, 0x85, 0x94, 0x20, 0x6b, 0x31, 0x20, 0x7d, 
 0x30, 0x4c, 0xa0, 0x30, 0x6c, 0xe0, 0x02, 0xa9, 0x34, 0xa2, 0x30, 0x8d, 0x44, 0x03, 0x8e, 0x45, 
 0x03, 0x8e, 0x48, 0x03, 0xa9, 0x09, 0x8d, 0x42, 0x03, 0xa2, 0x00, 0x20, 0x56, 0xe4, 0x4c, 0xee, 
 0x30, 0xa0, 0x40, 0x8c, 0x03, 0x03, 0x8d, 0x04, 0x03, 0x8e, 0x05, 0x03, 0xad, 0x0a, 0x03, 0x0d, 
 0x0b, 0x03, 0xf0, 0xd3, 0x20, 0x59, 0xe4, 0x30, 0xce, 0x60, 0xa9, 0x00, 0x85, 0x95, 0x85, 0x94, 
 0xa6, 0x90, 0xec, 0x1f, 0x30, 0xf0, 0x06, 0xbd, 0x00, 0x2e, 0xe6, 0x90, 0x60, 0x20, 0x65, 0x30, 
 0xa5, 0x95, 0xd0, 0x17, 0xad, 0x1f, 0x30, 0xf0, 0x04, 0xa5, 0x94, 0x30, 0x0e, 0xa9, 0x00, 0xa2, 
 0x2e, 0x20, 0xf1, 0x30, 0x38, 0x26, 0x90, 0xad, 0x00, 0x2e, 0x60, 0xa5, 0x92, 0xa6, 0x93, 0x20, 
 0xf1, 0x30, 0xa5, 0x92, 0x18, 0x6d, 0x08, 0x03, 0x85, 0x92, 0xa5, 0x93, 0x6d, 0x09, 0x03, 0x85, 
 0x93, 0x38, 0xa5, 0x94, 0xed, 0x08, 0x03, 0x85, 0x94, 0xa5, 0x95, 0xed, 0x09, 0x03, 0x85, 0x95, 
 0x4c, 0x1d, 0x31, 0xa5, 0x94, 0xd0, 0x02, 0xc6, 0x95, 0xc6, 0x94, 0x20, 0x10, 0x31, 0xa0, 0x00, 
 0x91, 0x92, 0xe6, 0x92, 0xd0, 0x02, 0xe6, 0x93, 0xa5, 0x94, 0x05, 0x95, 0xd0, 0xe5, 0x60, 0x00, 
   },
};


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
int sparta_get_sector(int inode,int sequence,int allocate)
{
   //if ( options.debug ) fprintf(stderr,"DEBUG: %s: inode %d sequence %d alloc %d\n",__FUNCTION__,inode,sequence,allocate);
   if ( inode >= atrfs.sectors || inode <= 0 ) return -EIO; // Out of range
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
   if ( r==0 && allocate )
   {
      // Allocate a new sector, store it in the map and return it
      r = sparta_alloc_any_sector();
      if ( r<0 ) return r;
      s[4+sequence*2] = r & 0xff;
      s[4+sequence*2+1] = r >> 8;
   }
   else if ( r < 1 || r > atrfs.sectors )
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

   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset),0);
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
   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset)+1,0);
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

   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset),0);
   if ( sector < 0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: Failed to get sector %d in %d\n",__FUNCTION__,OFFSET_TO_SECTOR(offset),dirinode);
      return sector;
   }
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
   sector = sparta_get_sector(dirinode,OFFSET_TO_SECTOR(offset)+1,0);
   if ( sector < 0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: Failed to get second sector %d in %d\n",__FUNCTION__,OFFSET_TO_SECTOR(offset)+1,dirinode);
      return sector;
   }
   s = SECTOR(sector);
   memcpy(s,dirent,bytes);
   return 0;
}

/*
 * sparta_dirent_time_set()
 *
 * Set time based on time_t seconds or now if zero.
 */
void sparta_dirent_time_set(struct sparta_dir_entry *dir_entry,time_t secs)
{
   if ( !secs ) secs = time(NULL);
   struct tm *tm;
   tm = localtime(&secs);

   dir_entry->file_time[2]=tm->tm_sec;
   dir_entry->file_time[1]=tm->tm_min;
   dir_entry->file_time[0]=tm->tm_hour;
   dir_entry->file_date[0]=tm->tm_mday;
   dir_entry->file_date[1]=tm->tm_mon+1; // Sparta: 1-12, struct tm is 0-11
   dir_entry->file_date[2]=(tm->tm_year%100); // Two-digit date
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
   *size = -1;
   *entry = -1;
   *locked=0;
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
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: size from directory header %d\n",__FUNCTION__,*size);
         return 0;
      }

      // If it's just ".info" then it's for the directory
      if ( strcmp(path,".info") == 0 )
      {
         r = sparta_get_dirent((void *)&dir_header,*inode,0);
         if ( r < 0 ) return r;
         *size = BYTES3(dir_header.dir_length_bytes);
         *isinfo = 1;
         *entry = 0;
         *parent_dir_inode = *inode;
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
         if ( !(dir_entry.status & FLAGS_IN_USE) )  // Directories should always end with a zero entry
         {
            if ( firstfree < 0 ) firstfree = i;
            break; // Should hit the loop condition if continuing; break to be safe
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
               // *size = BYTES3(dir_entry.file_size_bytes); // Wrong; use size in directory header
               *isdir = 1;
               *inode = BYTES2(dir_entry.sector_map);
               unsigned char *s = SECTOR(*inode);
               if ( !s ) return -EIO;
               int dirsec = BYTES2(s+4);
               struct sparta_dir_header *head = SECTOR(dirsec);
               if ( !head ) return -EIO;
               *size = BYTES3(head->dir_length_bytes);
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
      return 1; // Directory length limit enforced when extending; might get -ENOSPC
   }
   return -ENOENT; // Shouldn't be reached
}

/*
 * sparta_bitmap_status()
 *
 * Return the bit from the bitmap for the sector (0 if allocated)
 *
 * This is easy, as bitmap sectors or contiguous with nothing but
 * bitmaps in them.
 */
int sparta_bitmap_status(int sector)
{
   struct sector1_sparta *sec1 = SECTOR(1);
   unsigned char *bitmap = SECTOR(BYTES2(sec1->first_bitmap)); // sanity at mount verified this
   unsigned char mask = 1<<(7-(sector & 0x07));
   return (bitmap[sector/8] & mask) != 0; // 0 or 1
}

/*
 * sparta_bitmap()
 *
 * Set or clear bitmap for a given sector.
 * If 'allocate' is non-zero, change from free to used; otherwise reverse
 * Return 0 if changed, 1 if already at target value.
 */
int sparta_bitmap(int sector,int allocate)
{
   struct sector1_sparta *sec1 = SECTOR(1);
   unsigned char *bitmap = SECTOR(BYTES2(sec1->first_bitmap)); // sanity at mount verified this
   unsigned char mask = 1<<(7-(sector & 0x07));
   unsigned char old_value = bitmap[sector/8];
   if ( allocate )
   {
      bitmap[sector/8] &= ~mask; // Set to zero (used)
   }
   else
   {
      bitmap[sector/8] |= mask; // Set to one (free)
   }
   if ( old_value != bitmap[sector/8] ) return 0;
   return 1;
}

/*
 * sparta_free_sector()
 */
int sparta_free_sector(int sector)
{
   int r;
   if ( !sector ) return 0; // Free non-sector; ignore it
   r = sparta_bitmap(sector,0);

   // Only update the free sector count if the bitmap was modified
   if ( r == 0 )
   {
      struct sector1_sparta *sec1 = SECTOR(1);
      if ( sec1->free[0] < 0xff ) ++sec1->free[0];
      else
      {
         ++sec1->free[1];
         sec1->free[0] = 0;
      }
   }
   return r;
}

/*
 * sparta_alloc_sector()
 */
int sparta_alloc_sector(int sector)
{
   int r;
   r = sparta_bitmap(sector,1);

   // Only update the free sector count if the bitmap was modified
   if ( r == 0 )
   {
      struct sector1_sparta *sec1 = SECTOR(1);
      if ( sec1->free[0] ) --sec1->free[0];
      else
      {
         --sec1->free[1];
         sec1->free[0] = 0xff;
      }
   }
   return r;
}

/*
 * sparta_alloc_any_sector()
 *
 * Allocate the first free sector.  This is inefficient, but easy to code.
 */
int sparta_alloc_any_sector(void)
{
   int r;
   for ( int i=2;i<=atrfs.sectors; ++i )
   {
      r=sparta_alloc_sector(i);
      if ( r==0 )
      {
         char *s = SECTOR(i);
         memset(s,0,atrfs.sectorsize); // New sectors are zeroed out
         return i;
      }
   }
   return -1;
}

/*
 * sparta_add_map_sectors()
 *
 * Expand the sector map
 */
int sparta_add_map_sectors(int inode,int add)
{
   int r=0;
   unsigned char *s;
   int next;

   while ( 1 )
   {
      if ( inode < 1 || inode > atrfs.sectors ) return -EIO; // corrupt file system
      s=SECTOR(inode);
      next=BYTES2(s);
      if ( !next ) break;  // Add from here
      inode = next;
   }
   while ( add )
   {
      int newsec = sparta_alloc_any_sector();
      if ( newsec < 0 ) return newsec;
      s[0] = newsec & 0xff;
      s[1] = newsec >> 8;
      s = SECTOR(newsec);
      memset(s,0,atrfs.sectorsize);
      s[2] = inode & 0xff;
      s[3] = inode >> 8;
      inode = newsec;
      --add;
   }

   return r;
}

/*
 * sparta_alloc_sector_in_map()
 *
 * Alloc sector 'sequence' in the map.  This might not be the end of the file.
 * This might imply allocating a new sector or sectors to the map.
 * This is used by the write and mkdir code.
 *
 * Returns 0 on success, 1 if the sector was already allocated, and -ERROR on error.
 */
int sparta_alloc_sector_in_map(int inode,int sequence)
{
   unsigned char *s;

   // Move forward in the map to the right sector, allocating as needed
   while ( sequence >= atrfs.sectorsize/2-2 )
   {
      s = SECTOR(inode);
      int next = BYTES2(s);
      if ( !next )
      {
         next = sparta_alloc_any_sector();
         if ( next < 0 ) return -ENOSPC;
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: %d Adding map sector %d\n",__FUNCTION__,inode,next);
         unsigned char *n;
         n = SECTOR(next);
         memset(n,0,atrfs.sectorsize);
         n[2] = inode & 0xff;
         n[3] = inode >> 8;
         s[0] = next & 0xff;
         s[1] = next >> 8;
      }
      inode = next;
      sequence -= atrfs.sectorsize/2-2;
   }

   s = SECTOR(inode);
   int target = BYTES2(s + 4 + sequence*2);
   if ( target ) return 1; // Already there
   target = sparta_alloc_any_sector();
   if ( target < 0 ) return -ENOSPC;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %d Adding sector %d as sequence %d\n",__FUNCTION__,inode,target,sequence);
   (s + 4 + sequence*2)[0] = target & 0xff;
   (s + 4 + sequence*2)[1] = target >> 8;
   return 0;
}

/*
 * sparta_extend_directory()
 *
 * Add a new blank entry to the end of the directory so the previous one can be used
 * for a new file or subdirectory.
 *
 * The index of the new entry is returned in 'newentry' if a non-NULL pointer is passed in.
 */
int sparta_extend_directory(int inode,int *newentry)
{
   struct sparta_dir_header dir_header;
   struct sparta_dir_entry dir_entry;
   int parent_dir_inode;
   int parent_dir_entry;
   int r;
   int size;
   int junk;
   if ( !newentry ) newentry=&junk; // Avoid pointer check later

   r = sparta_get_dirent((void *)&dir_header,inode,0);
   if ( r<0 ) return r;
   size = BYTES3(dir_header.dir_length_bytes);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %d extend from %d bytes to %ld\n",__FUNCTION__,inode,size,size+sizeof(struct sparta_dir_entry));

   // If extending exceeds arbitrary directory length limits, return -ENOSPC
   {
      int dir_entry_limit;
      struct sector1_sparta *sec1 = SECTOR(1);

      // SpartaDOS X: 1423 entries (32K); 1424 with the header (entry 0) for 32K size limit
      // Older SpartaDOS: 126 entries (23 sectors with one ending blank entry and the header)
      if ( sec1->revision <= 0x20 ) dir_entry_limit = 126;
      else dir_entry_limit = 1423;

      if ( size / (int)sizeof(struct sparta_dir_entry) >= dir_entry_limit+2 ) return -ENOSPC; // Size for entries, header, zero at end
   }

   parent_dir_inode = BYTES2(dir_header.parent_dir_map);
   if ( parent_dir_inode ) // None if main directory
   {
      for ( parent_dir_entry = 1; ; ++parent_dir_entry )
      {
         r = sparta_get_dirent((void *)&dir_entry,parent_dir_inode,parent_dir_entry);
         if ( r < 0 ) return r;
         if ( dir_entry.status == 0 ) return -EIO; // Bad directory links
         if ( dir_entry.status & FLAGS_DELETED ) continue;
         if ( !(dir_entry.status & FLAGS_DIR) ) continue;
         if ( memcmp(dir_header.dir_name,dir_entry.file_name,8+3) != 0 ) continue;
         break; // Found it
      }
   }

   // Allocate new sector if needed
   if ( SIZE_TO_SECTORS(size) != SIZE_TO_SECTORS(size + sizeof(struct sparta_dir_entry)) )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %d Adding sector\n",__FUNCTION__,inode);
      r = sparta_alloc_sector_in_map(inode,OFFSET_TO_SECTOR(size + sizeof(struct sparta_dir_entry)));
   }

   // Write zeros to new entry
   struct sparta_dir_entry null_entry;
   memset(&null_entry,0,sizeof(null_entry));
   r = sparta_put_dirent(&null_entry,inode,size/sizeof(struct sparta_dir_entry));
   if ( r<0 ) return r;
   *newentry = size/sizeof(struct sparta_dir_entry);

   // Update file size
   size += sizeof(struct sparta_dir_entry);
   if ( parent_dir_inode )
   {
      dir_entry.file_size_bytes[0] = size & 0xff;
      dir_entry.file_size_bytes[1] = (size>>8) & 0xff;
      dir_entry.file_size_bytes[2] = (size>>16) & 0xff;
      r = sparta_put_dirent(&dir_entry,parent_dir_inode,parent_dir_entry);
      if ( r<0 ) return r;
   }
   dir_header.dir_length_bytes[0] = size & 0xff;
   dir_header.dir_length_bytes[1] = (size>>8) & 0xff;
   dir_header.dir_length_bytes[2] = (size>>16) & 0xff;
   r = sparta_put_dirent((void *)&dir_header,inode,0);
   if ( r<0 ) return r;
   return 0;
}

/*
 * sparta_touch_parent_dir()
 *
 * Set the time stamp for a directory to now.
 *
 * Used by mkdir, rmdir, rename, unlink, etc.
 */
int sparta_touch_parent_dir(const char *path,int dir_inode)
{
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: path %s inode %d\n",__FUNCTION__,path,dir_inode);
   // Remove last component from path
   char *p = strdup(path);
   char *slash = strrchr(p,'/');
   if ( slash )
   {
      if ( slash == p ) ++slash; // Keep leading '/'
      *slash=0;
   }

   // Look up parent directory
   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(p,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
//   free(p);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( !isdir ) return -EIO; // Not good

   if ( inode != dir_inode && dir_inode > 0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: path %s found inode %d expected %d\n",__FUNCTION__,p,inode,dir_inode);
      return -EIO; // Inode doesn't match what it must match
   }

   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;

   sparta_dirent_time_set(&dir_entry,time(NULL));
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   return 0;
}

/*
 * sparta_info()
 *
 * Return the buffer containing information specific to this file.
 *
 * The pointer returned should be 'free()'d after use.
 */
char *sparta_info(const char *path,int parent_dir_inode,int entry,int inode,int filesize)
{
   char *buf,*b;
   int r;

   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r < 0 ) return NULL;

   buf = malloc(64*1024);
   b = buf;
   *b = 0;
   b+=sprintf(b,"File information and analysis\n\n  %.*s\n  %d bytes\n\n",(int)(strrchr(path,'.')-path),path,filesize);
   b+=sprintf(b,
              "Directory entry internals:\n"
              "  Name in entry: %11.11s\n"
              "  Entry %d in directory with sector map at %d\n"
              "  Status: $%02x\n"
              "  %sSector Map at: %d\n\n",
              dir_entry.file_name,
              entry,parent_dir_inode,
              dir_entry.status,entry?"":"Parent ",BYTES2(dir_entry.sector_map));
   b+=sprintf(b,
              "  Date (d/m/y): %d/%d/%d\n",dir_entry.file_date[0],dir_entry.file_date[1],dir_entry.file_date[2]);
   b+=sprintf(b,
              "  Time: %d:%02d:%02d\n",dir_entry.file_time[0],dir_entry.file_time[1],dir_entry.file_time[2]);

   if ( inode )
   {
      b+=sprintf(b,"\nSector chain:\n");
      int s;
      int prev = -1,pprint = -1;
      for ( int i=0;(s=sparta_get_sector(inode,i,0))>0;++i )
      {
         if ( s == prev+1 )
         {
            ++prev;
            continue;
         }
         if ( prev > 0 )
         {
            if ( pprint != prev )
            {
               b+=sprintf(b," -- %d",prev);
            }
            b+=sprintf(b,"\n");
         }
         b+=sprintf(b,"  %d",s);
         prev = s;
         pprint = s;
      }
      if ( prev > 0 && pprint != prev )
      {
         b+=sprintf(b," -- %d",prev);
      }
      b+=sprintf(b,"\n\n");
   }
   else
   {
      b+=sprintf(b,"\nNo sector chain; invalid sector map pointer\n");
   }
   // Generic info for the file type, but only if it's not a directory
   if ( !(dir_entry.status & FLAGS_DIR) )
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
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s parent: %d inode %d size %d entry %d\n",__FUNCTION__,path,parent_dir_inode,inode,size,entry);

   // Get time stamp
   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry>0?entry:0); //isdir?0:entry);
   if ( r<0 ) return r;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s dir entry read\n",__FUNCTION__,path);
   struct tm tm;
   time_t secs;
   memset(&tm,0,sizeof(tm));
   tm.tm_sec=dir_entry.file_time[2];
   tm.tm_min=dir_entry.file_time[1];
   tm.tm_hour=dir_entry.file_time[0];
   tm.tm_mday=dir_entry.file_date[0];
   tm.tm_mon=dir_entry.file_date[1]-1; // Want 0-11, not 1-12
   tm.tm_year=dir_entry.file_date[2];
   if ( tm.tm_year < 78 ) tm.tm_year += 100;
   // tm.tm_year += 1900; // Spec: Year - 1900, so don't add 1900
   tm.tm_isdst = -1; // Have the system determine if DST is in effect
   secs = mktime(&tm);

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

      char *info = sparta_info(path,parent_dir_inode,entry,inode,size);
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
   return 0;
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
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s size %lu offset %ld\n",__FUNCTION__,path,size,offset);
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
      char *info = sparta_info(path,parent_dir_inode,entry,inode,filesize);
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
      int sector = sparta_get_sector(inode,sequence,0);
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

int sparta_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s size %lu offset %ld\n",__FUNCTION__,path,size,offset);
   int inode=0,parent_dir_inode,filesize,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&filesize,&locked,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -ENOENT; // These aren't writable

   // Quick lazy hack: Extend file if needed first
   if ( offset + size > (size_t)filesize )
   {
      r = sparta_truncate(path,offset+size,fi);
      if ( r<0 ) return r;
   }

   // Now we know the file is the right length
   int written=0;
   while ( size )
   {
      int bytes,sector;
      int seq = OFFSET_TO_SECTOR(offset);
      int localoffset = OFFSET_TO_SECTOR_OFFSET(offset);
      unsigned char *s;
      sector = sparta_get_sector(inode,seq,1); // Allocate a new sector if file is sparse
      if ( sector<0 ) return sector;
      s = SECTOR(sector);
      bytes = atrfs.sectorsize - localoffset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(s+localoffset,buf,bytes);
      written += bytes;
      offset += bytes;
      buf += bytes;
      size -= bytes;
   }

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s success: %d\n",__FUNCTION__,path,written);

   // Update file timestamp
   
   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   sparta_dirent_time_set(&dir_entry,time(NULL));
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   return written;
}

/*
 * sparta_mkdir()
 */
int sparta_mkdir(const char *path,mode_t mode)
{
   (void)mode; // Always create read-write, but allow chmod to lock

   // Filter out illegal directory names that would cause serious problems
   if ( strchr(path,'>') || strchr(path,'\\') )
   {
      return -EINVAL; // '>' and '\' are the directory separator characters in SpartaDOS
   }

   struct sparta_dir_entry dir_entry;
   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r == 0 ) return -EEXIST;

   if ( entry == -1 )
   {
      r = sparta_extend_directory(parent_dir_inode,&entry);
      if ( r<0 ) return r;
   }

   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   if ( dir_entry.status == 0 )
   {
      // Add another blank entry at the end
      r = sparta_extend_directory(parent_dir_inode,NULL);
      if ( r<0 ) return r;
   }

   memset(&dir_entry,0,sizeof(dir_entry)); // Clear any old data
   dir_entry.status = FLAGS_IN_USE | FLAGS_DIR;
   dir_entry.file_size_bytes[0] = sizeof(struct sparta_dir_entry) * 2; // header and blank to start
   sparta_dirent_time_set(&dir_entry,0);

   // Set name
   const char *n = strrchr(path,'/');
   if (n) ++n;
   else n=path; // Shouldn't happen
   for (int i=0;i<8;++i)
   {
      dir_entry.file_name[i]=' ';
      if ( *n && *n != '.' )
      {
         dir_entry.file_name[i]=*n;
         ++n;
      }
   }
   if ( *n == '.' ) ++n;
   for (int i=0;i<3;++i)
   {
      dir_entry.file_ext[i]=' ';
      if ( *n )
      {
         dir_entry.file_ext[i]=*n;
         ++n;
      }
   }

   // Allocate sector map
   int dirmap = sparta_alloc_any_sector();
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s allocated sector map %d\n",__FUNCTION__,path,dirmap);
   if ( dirmap < 0 )
   {
      // Full; nothing written yet
      return -ENOSPC;
   }
   unsigned char *map = SECTOR(dirmap);

   // Allocate a sector in the sector map
   int dirsec = sparta_alloc_any_sector();
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s allocated directory sector %d\n",__FUNCTION__,path,dirsec);
   if ( dirsec < 0 )
   {
      sparta_free_sector(dirmap);
      return -ENOSPC;
   }
   map[4]=dirsec & 0xff;
   map[5]=dirsec >> 8;

   // Point the directory entry at the sector map
   dir_entry.sector_map[0] = dirmap & 0xff;
   dir_entry.sector_map[1] = dirmap >> 8;
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r; // Should be impossible

   // Create header entry in new directory
   struct sparta_dir_header *head = (void *)&dir_entry; // Keep name and size
   head->pad_0 = 0;
   head->parent_dir_map[0] = parent_dir_inode & 0xff;
   head->parent_dir_map[1] = parent_dir_inode >> 8;

   r = sparta_put_dirent(&dir_entry,dirmap,0);
   if ( r<0 ) return r;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s success\n",__FUNCTION__,path);

   // Update time stamp on parent directory
   r = sparta_touch_parent_dir(path,parent_dir_inode);
   if ( r<0 ) return r;
   return 0;
}

/*
 * sparta_freemap()
 *
 * Free all sectors in the sector map, and then the map itself.
 * Used in unlink, truncate, and rmdir
 */
int sparta_freemap(int inode)
{
   int r;
   while ( inode )
   {
      unsigned char *s = SECTOR(inode);
      for ( int i=2;i<atrfs.sectorsize/2;++i )
      {
         int sector = BYTES2(s+i*2);
         if ( sector )
         {
            r = sparta_free_sector(sector);
            if ( r )
            {
               if ( options.debug ) fprintf(stderr,"DEBUG: %s: Freeing data free sector %d\n",__FUNCTION__,sector);
            }
         }
      }

      r = sparta_free_sector(inode);
      if ( r )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: Freeing free map sector %d\n",__FUNCTION__,inode);
      }

      inode = BYTES2(s); // Next map sector
   }
   return 0;
}

/*
 * sparta_rmdir()
 */
int sparta_rmdir(const char *path)
{
   struct sparta_dir_entry dir_entry;
   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( !isdir ) return -ENOTDIR;
   if ( isinfo ) return -ENOENT;
   if ( locked ) return -EACCES;

   // Make sure directory is empty
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s inode %d in dir %d\n",__FUNCTION__,path,inode,parent_dir_inode);

   r = sparta_get_dirent(&dir_entry,inode,0);
   if ( r<0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s get dir header failed\n",__FUNCTION__,path);
      return r;
   }
   if ( BYTES3(dir_entry.file_size_bytes) != size )
   {
      if ( options.debug ) fprintf(stderr,"ERROR: %s: %s internal size %d parent reports size %d\n",__FUNCTION__,path,BYTES3(dir_entry.file_size_bytes),size);
   }
   for ( unsigned int e=1;e < size/sizeof(struct sparta_dir_entry); ++e)
   {
      r = sparta_get_dirent(&dir_entry,inode,e);
      if ( r<0 )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s get entry %d failed: %d\n",__FUNCTION__,path,e,r);
         return r;
      }
      if ( dir_entry.status == 0 ) break;
      if ( dir_entry.status & FLAGS_DELETED ) continue;
      return -ENOTEMPTY;
   }

   // Mark the entry in the parent directory as deleted
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s get entry %d failed: %d\n",__FUNCTION__,path,entry,r);
      return r;
   }

   dir_entry.status = FLAGS_DELETED;
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;

   // Free the sectors
   sparta_freemap(inode);

   // Update time stamp on parent directory
   r = sparta_touch_parent_dir(path,parent_dir_inode);
   if ( r<0 ) return r;
   return 0;
}

/*
 * sparta_unlink()
 */
int sparta_unlink(const char *path)
{
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isdir ) return -EISDIR;
   if ( isinfo ) return -ENOENT;
   if ( locked ) return -EACCES;

   // Flag file as deleted
   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s get entry failed: %d\n",__FUNCTION__,path,r);
      return r;
   }
   dir_entry.status = FLAGS_DELETED;
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;

   // Free all data sectors and the map
   sparta_freemap(BYTES2(dir_entry.sector_map));

   // Update time stamp on parent directory
   r = sparta_touch_parent_dir(path,parent_dir_inode);
   if ( r<0 ) return r;
   return 0;
}

/*
 * sparta_rename()
 */
int sparta_rename(const char *old, const char *new, unsigned int flags)
{
   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int inode2=0,parent_dir_inode2,size2,locked2,entry2,isdir2;
   int r,r2;
   struct sparta_dir_entry dir_entry,dir_entry2;
   r = sparta_path(old,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -EACCES;
   if ( isdir ) if ( strncmp(old,new,strlen(old)) == 0 ) return -EINVAL; // attempt to make a directory a subdirectory of itself
   r2 = r = sparta_path(new,&inode2,&parent_dir_inode2,&size2,&locked2,&entry2,&isdir2,&isinfo);
   if ( r<0 ) return r;
   if ( isinfo ) return -EACCES;
   if ( r==0 && (flags & RENAME_NOREPLACE) ) return -EEXIST;
   if ( r!=0 && (flags & RENAME_EXCHANGE) ) return -ENOENT;
   if ( isdir2 && !(flags & RENAME_EXCHANGE) ) return -EISDIR;
   if ( r==0 && locked2 ) return -EACCES;
   if ( r==0 && inode==inode2 ) return -EINVAL; // Rename to self
   if ( (flags & RENAME_EXCHANGE) && isdir2 && strncmp(old,new,strlen(new)) == 0 ) return -EINVAL;

   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;

   if ( r2 == 0 )
   {
      r = sparta_get_dirent(&dir_entry2,parent_dir_inode2,entry2);
      if ( r<0 ) return r;
   }

   // Handle weird exchange case
   // Handle the move/replace case; same thing; plus unlink old
   if ( (flags & RENAME_EXCHANGE) || r2 == 0 )
   {
      char copy[8+3];

      if ( options.debug ) fprintf(stderr,"DEBUG: %s Exchange %s and %s\n",__FUNCTION__,new,old);
      // Names stay the same, but everything else swaps
      memcpy(copy,dir_entry.file_name,8+3);
      memcpy(dir_entry.file_name,dir_entry2.file_name,8+3);
      memcpy(dir_entry2.file_name,copy,8+3);
      r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
      if ( r<0 ) return r;
      r = sparta_put_dirent(&dir_entry2,parent_dir_inode2,entry2);
      if ( r<0 ) return r;

      // Update time stamp on parent directories
      r = sparta_touch_parent_dir(old,parent_dir_inode);
      if ( r<0 ) return r;
      r = sparta_touch_parent_dir(new,parent_dir_inode2);
      if ( r<0 ) return r;
      
      // Rename in directory header as well
      if ( isdir )
      {
         struct sparta_dir_header head;
         r = sparta_get_dirent((void *)&head,inode,0);
         if ( r<0 ) return r;
         memcpy(head.dir_name,dir_entry2.file_name,8+3);
         r = sparta_put_dirent((void *)&head,inode,0);
         if ( r<0 ) return r;
      }
      if ( isdir2 )
      {
         struct sparta_dir_header head;
         r = sparta_get_dirent((void *)&head,inode2,0);
         if ( r<0 ) return r;
         memcpy(head.dir_name,dir_entry.file_name,8+3);
         r = sparta_put_dirent((void *)&head,inode2,0);
         if ( r<0 ) return r;
      }

      if ( flags & RENAME_EXCHANGE ) return 0;
      r = sparta_unlink(old);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s unlink removed file returned %d\n",__FUNCTION__,r);
      return r;
   }

   // Rename in place?
   if ( r2!=0 && parent_dir_inode2 == parent_dir_inode )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s Rename in-place (same dir): %s -> %s\n",__FUNCTION__,old,new);
      const char *n = strrchr(new,'/');
      if (n) ++n;
      else n=new; // Shouldn't happen
      for (int i=0;i<8;++i)
      {
         dir_entry.file_name[i]=' ';
         if ( *n && *n != '.' )
         {
            dir_entry.file_name[i]=*n;
            ++n;
         }
      }
      if ( *n == '.' ) ++n;
      for (int i=0;i<3;++i)
      {
         dir_entry.file_ext[i]=' ';
         if ( *n )
         {
            dir_entry.file_ext[i]=*n;
            ++n;
         }
      }
      r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
      if ( r<0 ) return r;

      // Rename in directory header as well
      if ( isdir )
      {
         struct sparta_dir_header head;
         r = sparta_get_dirent((void *)&head,inode,0);
         if ( r<0 ) return r;
         memcpy(head.dir_name,dir_entry.file_name,8+3);
         r = sparta_put_dirent((void *)&head,inode,0);
         if ( r<0 ) return r;
      }

      // Update time stamp on parent directory
      r = sparta_touch_parent_dir(old,parent_dir_inode);
      if ( r<0 ) return r;

      return 0;
   }

   // Move file (or directory) to new directory, possibly with new name
   if ( 1 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s Rename to new dir: %s -> %s\n",__FUNCTION__,old,new);
      if ( parent_dir_inode == inode2 ) return -EINVAL; // e.g., mv dir/file dir/

      if ( entry2 == -1 )
      {
         r = sparta_extend_directory(parent_dir_inode2,&entry2);
         if ( r<0 ) return r;
      }

      if ( entry2 < 1 ) return -EIO; // Should be impossible.
      r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
      if ( r<0 ) return r;
      if ( dir_entry.status == 0 )
      {
         // Add another blank entry at the end
         r = sparta_extend_directory(parent_dir_inode,NULL);
         if ( r<0 ) return r;
      }

      // Change name in dir_entry to match target
      const char *n = strrchr(new,'/');
      if (n) ++n;
      else n=new; // Shouldn't happen
      for (int i=0;i<8;++i)
      {
         dir_entry.file_name[i]=' ';
         if ( *n && *n != '.' )
         {
            dir_entry.file_name[i]=*n;
            ++n;
         }
      }
      if ( *n == '.' ) ++n;
      for (int i=0;i<3;++i)
      {
         dir_entry.file_ext[i]=' ';
         if ( *n )
         {
            dir_entry.file_ext[i]=*n;
            ++n;
         }
      }

      // Save new entry
      r = sparta_put_dirent(&dir_entry,parent_dir_inode2,entry2);
      if ( r<0 ) return r;

      // Flag old entry as deleted (will also change name, but who cares?)
      dir_entry.status = FLAGS_DELETED;
      r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
      if ( r<0 ) return r;

      // Update name in directory head
      if ( isdir )
      {
         struct sparta_dir_header head;
         r = sparta_get_dirent((void *)&head,inode,0);
         if ( r<0 ) return r;
         memcpy(head.dir_name,dir_entry.file_name,8+3);
         r = sparta_put_dirent((void *)&head,inode,0);
         if ( r<0 ) return r;
      }

      // Update time stamp on parent directories
      r = sparta_touch_parent_dir(old,parent_dir_inode);
      if ( r<0 ) return r;
      r = sparta_touch_parent_dir(new,parent_dir_inode2);
      if ( r<0 ) return r;

      return 0;
   }

   // Should have covered all cases by this point
   return -EIO;
}

/*
 * sparta_chmod()
 */
int sparta_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
   (void)fi;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s.%d\n",__FUNCTION__,__LINE__);

   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -EACCES;

   if ( locked && !(mode & 0200) ) return 0; // Already read-only
   if ( !locked && (mode & 0200) ) return 0; // Already has write permissions

   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   if ( mode & 0200 ) // make writable; unlock
   {
      dir_entry.status &= ~FLAGS_LOCKED;
   }
   else
   {
      dir_entry.status |= FLAGS_LOCKED;
   }
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   return 0;
}

/*
 * sparta_create()
 */
int sparta_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
   (void)fi;
   (void)mode; // Always create read-write, but allow chmod to lock it
   // Create a file
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   const char *n = strrchr(path,'/');
   if ( n ) ++n; else n=path; // else never happens
   char name[8+3+1];
   name[11]=0;
   for (int i=0;i<8;++i)
   {
      name[i]=' ';
      if ( *n && *n != '.' )
      {
         name[i]=*n;
         ++n;
      }
   }
   if ( *n == '.' ) ++n;
   for (int i=0;i<3;++i)
   {
      name[8+i]=' ';
      if ( *n )
      {
         name[8+i]=*n;
         ++n;
      }
   }
   if ( *n )
   {
      return -ENAMETOOLONG;
   }

   int inode=0,parent_dir_inode,filesize,locked,entry,isdir,isinfo;
   int r;
   struct sparta_dir_entry dir_entry;
   r = sparta_path(path,&inode,&parent_dir_inode,&filesize,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r==0 ) return -EEXIST;

   if ( entry == -1 )
   {
      r = sparta_extend_directory(parent_dir_inode,&entry);
      if ( r<0 ) return r;
   }

   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   if ( dir_entry.status == 0 )
   {
      // Add another blank entry at the end
      r = sparta_extend_directory(parent_dir_inode,NULL);
      if ( r<0 ) return r;
   }

   memset(&dir_entry,0,sizeof(dir_entry)); // Clear any old data
   dir_entry.status = FLAGS_IN_USE;
   sparta_dirent_time_set(&dir_entry,0);

   // Set name
   memcpy(dir_entry.file_name,name,8+3);

   // Allocate sector map
   int dirmap = sparta_alloc_any_sector();
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s allocated sector map %d\n",__FUNCTION__,path,dirmap);
   if ( dirmap < 0 )
   {
      // Full; nothing written yet
      return -ENOSPC;
   }

   // Point the directory entry at the sector map
   dir_entry.sector_map[0] = dirmap & 0xff;
   dir_entry.sector_map[1] = dirmap >> 8;
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r; // Should be impossible

   // Update time stamp on parent directory
   r = sparta_touch_parent_dir(path,parent_dir_inode);
   if ( r<0 ) return r;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s success\n",__FUNCTION__,path);
   return 0;
}

/*
 * sparta_truncate()
 */
int sparta_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
   (void)fi;
   int inode=0,parent_dir_inode,filesize,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&filesize,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -EACCES;
   if ( isdir ) return -EISDIR;
   if ( size == filesize ) return 0; // No change; easy!

   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;

   if ( size > filesize )
   {
      /*
       * Grow the file
       *
       * This is easy, as SpartaDOS allows sparse files.  Just grow the sector maps.
       * To be safe, zero out previously unallocated space in the last sector.
       */
      // Zero blank space at the end, if any
      int last;
      last = sparta_get_sector(inode,OFFSET_TO_SECTOR(filesize),0);
      if ( last > 0 )
      {
         int offset = OFFSET_TO_SECTOR_OFFSET(filesize);
         int bytes = atrfs.sectorsize - offset;
         if ( bytes )
         {
            memset(SECTOR(last)+offset,0,bytes);
         }
      }
      r = sparta_add_map_sectors(inode,SIZE_TO_MAP_SECTORS(size)-SIZE_TO_MAP_SECTORS(filesize));
      if ( r<0 ) return r;
   }
   else
   {
      /*
       * Shrink the file
       */
      int newsectors = (size+atrfs.sectorsize-1)/atrfs.sectorsize; // 0, 1, 2, ...
      char *s;
      while ( newsectors > ENTRIES_PER_MAP_SECTOR )
      {
         s = SECTOR(inode);
         inode = BYTES2(s);
         newsectors -= ENTRIES_PER_MAP_SECTOR;
      }
      for (int seq=newsectors;seq<ENTRIES_PER_MAP_SECTOR;++seq)
      {
         int sec = BYTES2(s+seq*2+2);
         r = sparta_free_sector(sec);
         if ( r < 0 ) return r;
         s[seq*2+2] = 0;
         s[seq*2+2+1] = 0;
      }
      s = SECTOR(inode);
      int next = BYTES2(s);
      if ( next )
      {
         s[0] = s[1] = 0; // Clear next pointer
         s=SECTOR(next);
         s[2] = s[3] = 0; // Clear previous pointer;
         r = sparta_freemap(next);
         if ( r<0 ) return r;
      }
   }

   // Update file size
   dir_entry.file_size_bytes[0] = size & 0xff;
   dir_entry.file_size_bytes[1] = (size>>8) & 0xff;
   dir_entry.file_size_bytes[2] = (size>>16) & 0xff;

   // Update time stamp
   sparta_dirent_time_set(&dir_entry,time(NULL));
   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   return 0;
}

int sparta_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
   (void)fi;
   int inode=0,parent_dir_inode,size,locked,entry,isdir,isinfo;
   int r;
   r = sparta_path(path,&inode,&parent_dir_inode,&size,&locked,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -EACCES;

   struct sparta_dir_entry dir_entry;
   r = sparta_get_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;

   time_t secs = tv[1].tv_sec;
   if ( tv[1].tv_nsec == UTIME_OMIT ) return 0;
   if ( tv[1].tv_nsec == UTIME_NOW )
   {
      secs = time(NULL);
   }
   sparta_dirent_time_set(&dir_entry,secs);

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: "
                                "  Date (d/m/y): %d/%d/%d\n",__FUNCTION__,dir_entry.file_date[0],dir_entry.file_date[1],dir_entry.file_date[2]);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: "
                                "  Time: %d:%02d:%02d\n",__FUNCTION__,dir_entry.file_time[0],dir_entry.file_time[1],dir_entry.file_time[2]);

   r = sparta_put_dirent(&dir_entry,parent_dir_inode,entry);
   if ( r<0 ) return r;
   return 0;
}

/*
 * sparta_statfs()
 */
int sparta_statfs(const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = atrfs.sectorsize;
   stfsbuf->f_frsize = atrfs.sectorsize;
   stfsbuf->f_blocks = atrfs.sectors;
   struct sector1_sparta *sec1 = SECTOR(1);
   stfsbuf->f_bfree = BYTES2((sec1->free));
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = 0; // Meaningless for Sparta
   stfsbuf->f_ffree = 0;
   stfsbuf->f_namemax = 12; // 8.3, plus ".info" if enabled
   // if ( !options.noinfofiles ) stfsbuf->f_namemax += 5; // Don't report this
   return 0;
}

/*
 * sparta_newfs()
 */
int sparta_newfs(void)
{
   struct sector1_sparta *sec1 = SECTOR(1);
   int next_sector;

   // Boot sectors: from table
   if ( atrfs.sectorsize <= 256 )
   {
      for (int i=1; i<=3; ++i)
      {
         unsigned char *s = SECTOR(i);
         memcpy(s,bootsectors[0]+128*(i-1),128);
      }
      next_sector = 4;
   }
   else
   {
      // FIXME: Verify that this is still right; we might need a different boot sector for 512-bytes
      unsigned char *s = SECTOR(1);
      memcpy(s,bootsectors[0],128*3);
      sec1->boot_sectors = 1;
      next_sector = 2;
   }

   // Sector 1
   sec1->sectors[0] = atrfs.sectors & 0xff;
   sec1->sectors[1] = atrfs.sectors >> 8;
   sec1->bitmap_sectors = (atrfs.sectors+1)/(8*atrfs.sectorsize);
   sec1->first_bitmap[0] = next_sector & 0xff;
   sec1->first_bitmap[1] = next_sector >> 8; // Always zero
   next_sector += sec1->bitmap_sectors;
   sec1->dir[0] = next_sector & 0xff;
   sec1->dir[0] = next_sector >> 8; // Always zero
   switch (atrfs.sectorsize)
   {
      case 128: sec1->sector_size = 128; break;
      case 256: sec1->sector_size = 0; break;
      case 512: sec1->sector_size = 1; break;
      default: return -EIO; // Illegal sector size
   }
   switch (atrfs.sectors)
   {
      case 720: // Single sided, single- or double-density
      case 1040: // 1050 enhanced density
         sec1->track_count = 40;
         break;
      case 1440:
         sec1->track_count = 40 & 0x80; // 360K, 40 tracks, double-sided
         break;
      case 2880:
         sec1->track_count = 80 & 0x80; // 720K, 80 tracks, double-sided
         break;
      default:
         sec1->track_count = 1; // Non-floppy
   }
   sec1->revision = 0x21; // SpartaDOS X; allows large directories
   sec1->vol_sequence = 1;
   srandom(time(NULL));
   sec1->vol_random = random() & 0xff;
   sec1->sec_num_file_load_map[0] = 0;
   sec1->sec_num_file_load_map[1] = 0;

   // Set volume name
   {
      const char *v = options.volname;
      if ( !v ) v="";
      for ( int i=0;i<8;++i )
      {
         if (*v)
         {
            sec1->volume_name[i] = *v;
            ++v;
         }
         else sec1->volume_name[i] = ' ';
      }
   }
   
   ++next_sector; // Bitmap sector is now used

   // Set main directory:
   unsigned char *s = SECTOR(BYTES2(sec1->dir));
   s[4] = next_sector & 0xff;
   s[5] = next_sector >> 8;
   struct sparta_dir_header *head = SECTOR(next_sector);
   // parent_dir_map[] is zero for MAIN
   head->dir_length_bytes[0]=sizeof(*head)*2; // Head and blank
   memcpy(head->dir_name,"MAIN       ",8+3);
   sparta_dirent_time_set((void *)head,0);
   ++next_sector; // MAIN directory now used

   // Allocation hints
   sec1->sec_num_allocation[0] = next_sector & 0xff;
   sec1->sec_num_allocation[1] = next_sector >> 8;
   sec1->sec_num_dir_alloc[0] = next_sector & 0xff;
   sec1->sec_num_dir_alloc[1] = next_sector >> 8;

   // Free bitmap and free count
   sec1->free[0] = sec1->free[1] = 0; // Add in as we free
   for ( int i=next_sector; i<=atrfs.sectors; ++i )
   {
      sparta_free_sector(i);
   }
   
   // Done
   return 0;
}

/*
 * sparta_fsinfo()
 */
char *sparta_fsinfo(void)
{
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;

   struct sector1_sparta *sec1 = SECTOR(1);

   b+=sprintf(b,"Volume Name:          ");
   int reverse=0;
   for ( unsigned int i=0;i<sizeof(sec1->volume_name);++i )
   {
      b+=sprintf(b,"%c",sec1->volume_name[i]&0x7f); // Print without reverse video
      if ( sec1->volume_name[i] & 0x80 ) ++reverse;
   }
   if ( reverse ) b+=sprintf(b," (reverse video stripped)");
   b+=sprintf(b,"\n");

   b+=sprintf(b,"Total sectors:        %d\n",BYTES2(sec1->sectors));
   b+=sprintf(b,"Free sectors:         %d\n",BYTES2(sec1->free));
   b+=sprintf(b,"Track count:          %d%s%s\n",sec1->track_count&0x7f,(sec1->track_count&0x80)?" (double-sided)":"",(sec1->track_count == 0x01)?" (non-floppy)":"");
   switch ( sec1->sector_size )
   {
      case 128: b+=sprintf(b,"Sector Size:          128\n"); break;
      case 0:   b+=sprintf(b,"Sector Size:          256\n"); break;
      case 1:   b+=sprintf(b,"Sector Size:          512\n"); break;
      default:  b+=sprintf(b,"Sector Size:          invalid (%d)\n",sec1->sector_size); break;
   }
   if ( sec1->bitmap_sectors < 2 )
   {
      b+=sprintf(b,"Bitmap sector:        %d\n",BYTES2(sec1->first_bitmap));
   }
   else
   {
      b+=sprintf(b,"Bitmap sectors:       %d -- %d\n",BYTES2(sec1->first_bitmap),BYTES2(sec1->first_bitmap)+sec1->bitmap_sectors-1);
   }
   b+=sprintf(b,"SpartaDOS revision:   %02x\n",sec1->revision);
   b+=sprintf(b,"Volume sequence:      %02x\n",sec1->vol_sequence);
   b+=sprintf(b,"Volume random:        %02x\n",sec1->vol_random);

   if ( sec1->revision == 0x11 )
   {
      b+=sprintf(b,"Sectors in DOS boot:  %d\n",sec1->rev_specific[4]);
   }
   // FIXME: Is the sec_num_file_load_map field valid with SD1.1?  If not, then add an else below
   if ( BYTES2(sec1->sec_num_file_load_map) == 0 )
   {
      b+=sprintf(b,"Map to load at boot:  none (0)\n");
   }
   else if ( BYTES2(sec1->sec_num_file_load_map) <= sec1->boot_sectors || BYTES2(sec1->sec_num_file_load_map) > atrfs.sectors )
   {
      b+=sprintf(b,"Map to load at boot:  invalid (%d)\n",BYTES2(sec1->sec_num_file_load_map));
   }
   else
   {
      b+=sprintf(b,"Map to load at boot:  %d\n",BYTES2(sec1->sec_num_file_load_map));
   }
   

   // Free bit map; generally not useful and may be too much data
#if 1
   b+=sprintf(b,"\nFree Map:\n");
   int prev=2;
   int start= -1;
   for ( int i=0;i<=atrfs.sectors;++i )
   {
      int r = sparta_bitmap_status(i);

      if ( r == prev ) continue;
      // Finish the previous line
      if ( start >= 0 )
      {
         if ( start != i-1 ) 
            b+=sprintf(b," -- %d",i-1);
         b+=sprintf(b,"\n");
      }
      // Start new line
      b+=sprintf(b,"%s: %d",r?"Free":"Used",i);
      start=i;
      prev=r;
   }
   if ( start != atrfs.sectors ) 
      b+=sprintf(b," -- %d",atrfs.sectors);
   b+=sprintf(b,"\n");
#endif
   return buf;
}
