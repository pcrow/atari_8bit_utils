/*
 * dos4.c
 *
 * Functions for accessing Atari DOS 4 file systems.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 *
 * Reference:
 *  https://mcurrent.name/atari-8-bit/faq.txt
 *
 *  http://atariki.krap.pl/index.php/Format_AtariDOS_4
 *  (translate from Polish)
 *
 * The references have bugs.  Best answer is to test:
 *
 *  Cluster size is 768 (6 128-byte sectors or 3 256-byte sectors)
 *  or 1536 (6 256-byte sectors) for DS/DD 360K disks.
 *
 *  With two clusters for dirctories and one or two sectors for VTOC:
 *    SD: Max files: 88
 *    ED: Max files: 80
 *    DD: Max files: 80
 *    DSDD: Max files: 176
 *
 *  VTOC is one or two sectors, used like DOS 3
 *   SD: VTOC is on sector 360, directory is 349-359 (clusters 66-67)
 *   ED: VTOC is sectors 515-516, directory is 505-514 (clusters 92-93)
 *   DD: VTOC is clusters 126-127
 *   DSDD: clusters 66-67.
 *
 *  This is hard-coded because a DOS 4 data disk doesn't reserve boot sectors,
 *  so the first file will start on sector 1!
 *
 *  For double-density disks, cluster 8, which starts on sector 1, is not
 *  used.  This is because some drives will just send 128 bytes for the
 *  first three sectors even for DD disks.
 *
 *  When you write the DOS files, it will just put QDOS.SYS sequentially
 *  starting with cluster 8 (sector 1) if it's a SD disk.  If it's DD, then
 *  it writes a 2-sector boot loader to sectors 1 and 2 (or a 256-byte boot
 *  loader to sector 1 if it will take it).  Best to be sure that ATR files
 *  for DD images use 128-byte sectors for the first three here, or it
 *  might not work correctly.
 *
 *  DOS 4 has a bug in formatting.  When you format a disk, it may leave
 *  some pending writes for the VTOC sectors in a buffer.  In some cases
 *  with a SS/DD disk, you will see the last eight clusters in the free
 *  chain as zeros instead of their correct values.  In some cases, the
 *  VTOC might not be written at all.  If you do other operations after the
 *  format, it will usually complete the writes.  Just doing a directory
 *  listing is often enough.
 *
 *  I can't figure out why the directory and VTOC clusters are different on
 *  different disk layouts.  It seems like they're really trying for the
 *  middle-of-the-disk optimization, which never seemed like a good idea to
 *  begin with.
 *
 *  It seems that using DOS 4 with anything other than the four formats
 *  would be difficult.
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
// FIXME: Hopefully this can be found in the boot sectors instead
#define CLUSTER_SIZE                            \
   /* Always 6 for SD/ED */                     \
   (atrfs.sectorsize==128?6:                    \
    /* 3 for DD unless it's a big disk */       \
    (atrfs.sectors <= 720 ? 3 :                 \
     /* 6 for DS/DD or other large disks */    \
     6 ))
#define CLUSTER_BYTES (CLUSTER_SIZE * atrfs.sectorsize)
#define CLUSTER_TO_SEC(n) ((n-8)*CLUSTER_SIZE + 1) // Starts at 8, not zero
#define CLUSTER(n) SECTOR(CLUSTER_TO_SEC(n))
#define VTOC_CLUSTER                                    \
   /* SS/SD: 66 */                                      \
   (atrfs.sectorsize==128 && atrfs.sectors<=720 ? 66 :  \
    /* SS/ED: 92 */                                     \
    (atrfs.sectorsize==128 ? 92 :                       \
     /* SS/DD: 126 */                                   \
     (atrfs.sectors <= 720 ? 126 :                      \
      /* DS/DD: 66 */                                   \
      66 )))
#define VTOC_SECTOR_COUNT                               \
   /* SS/SD: 1 */                                       \
   (atrfs.sectorsize==128 && atrfs.sectors<=720 ? 1 :   \
    /* SS/ED: 2 */                                      \
    (atrfs.sectorsize==128 ? 2 :                        \
     /* SS/DD: 1 */                                     \
     (atrfs.sectors <= 720 ? 1 :                        \
      /* DS/DD: 1 */                                    \
      1 )))
#define VTOC_START SECTOR(CLUSTER_TO_SEC(VTOC_CLUSTER+2)-VTOC_SECTOR_COUNT)
#define MAX_CLUSTER (atrfs.sectors/CLUSTER_SIZE+8)
#define DIR_ENTRIES ((CLUSTER_BYTES*2-VTOC_SECTOR_COUNT*atrfs.sectorsize)/(int)sizeof(struct dos4_dir_entry))

/*
 * File System Structures
 */

struct dos4_dir_entry {
   unsigned char status; // Bit 7 valid, bit 6 exists, bit 1 locked, bit 0 open
   unsigned char blocks;
   unsigned char bytes_in_last_sector; // not last block
   unsigned char start;
   unsigned char reserved;
   unsigned char file_name[8];
   unsigned char file_ext[3];
};

enum dos4_dir_status {
   FLAGS_OPEN    = 0x01, // Open for write
   FLAGS_LOCKED  = 0x20,
   FLAGS_IN_USE  = 0x40, // This is a file
   FLAGS_DELETED = 0x80,
};

struct dos4_vtoc {
   unsigned char format; // 'R' for single-sided, 'C' for double-sided
   unsigned char first_free; // 00 if full, otherwise offset into VTOC of first free
   unsigned char reserved_02; // 00, not sure
   unsigned char free; // Number of free clusters
   unsigned char reserved[4]; // not sure
   unsigned char allocation_table[256]; // Actually shorter
};

/*
 * Allocation table:
 *
 * Clusters start with number 8, so that they index from 0 at the start of
 * the VTOC sector.
 *
 * For clusters that are not EOF, then it has the number of the next cluster.
 *
 * For the last cluster of the file, it's the number of full sectors used
 * in that cluster.  EOF is determined by counting clusters based on the
 * number used in the directory.
 *
 * Hence file size:
 *  (blocks - 1) * CLUSTER_SIZE_BYTES + last_vtoc_byte*sector_size + bytes_in_last_sector
 *
 * Free sectors are kept in a chain of free sectors.  This avoids searching.
 *
 */

/*
 * Function prototypes
 */
int dos4_sanity(void);
int dos4_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int dos4_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int dos4_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dos4_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dos4_mkdir(const char *path,mode_t mode);
int dos4_rmdir(const char *path);
int dos4_unlink(const char *path);
int dos4_rename(const char *path1, const char *path2, unsigned int flags);
int dos4_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos4_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos4_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int dos4_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);
int dos4_statfs(const char *path, struct statvfs *stfsbuf);
int dos4_newfs(void);
char *dos4_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops dos4_ops = {
   .name = "Atari DOS 4",
   .fs_sanity = dos4_sanity,
   .fs_getattr = dos4_getattr,
   .fs_readdir = dos4_readdir,
   .fs_read = dos4_read,
   // .fs_write = dos4_write,
   // .fs_mkdir = dos4_mkdir,
   // .fs_rmdir = dos4_rmdir,
   // .fs_unlink = dos4_unlink,
   // .fs_rename = dos4_rename,
   // .fs_chmod = dos4_chmod,
   // .fs_create = dos4_create,
   // .fs_truncate = dos4_truncate,
   // .fs_utimens = dos4_utimens,
   // .fs_statfs = dos4_statfs,
   // .fs_newfs = dos4_newfs,
   // .fs_fsinfo = dos4_fsinfo,
};

/*
 * Functions
 */


/*
 * dos4_sanity()
 *
 * Return 0 if this is a valid Atari DOS 4 file system
 */
int dos4_sanity(void)
{
   if ( atrfs.sectorsize != 128 &&  atrfs.sectorsize != 256 ) return 1; // Must be SD
   struct dos4_dir_entry *dirent = CLUSTER(VTOC_CLUSTER);
   struct dos4_vtoc *vtoc = VTOC_START;
   unsigned char *map = VTOC_START;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: Cluster size: %d  VTOC Clusters: %d-%d (sectors %d-%d)  VTOC Sector count: %d  First VTOC sector: %d Max DIR entries: %d Max Cluster: %d\n",__FUNCTION__,CLUSTER_SIZE,VTOC_CLUSTER,VTOC_CLUSTER+1,CLUSTER_TO_SEC(VTOC_CLUSTER),CLUSTER_TO_SEC(VTOC_CLUSTER+2)-1,VTOC_SECTOR_COUNT,CLUSTER_TO_SEC(VTOC_CLUSTER+2)-VTOC_SECTOR_COUNT,DIR_ENTRIES,MAX_CLUSTER);
   // Sector 1 is not special in DOS 4!
   // If DOS files are written, QDOS.SYS is simply written contiguously starting at
   // sector 1 (cluster 8)
   if ( vtoc->format != 'R' && vtoc->format != 'C' ) return 1; // FIXME: Check all formats
   if ( vtoc->reserved_02 != 00 ) return 1;
   if ( vtoc->reserved[0] != 00 ) return 1;
   if ( vtoc->reserved[1] != 00 ) return 1;
   if ( vtoc->reserved[2] != 00 ) return 1;
   if ( vtoc->reserved[3] != 00 ) return 1;
   if ( vtoc->first_free && !vtoc->free ) return 1; // If one is zero, the other must be
   if ( !vtoc->first_free && vtoc->free ) return 1;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: basic VTOC check passed\n",__FUNCTION__);

   // Make sure free list in VTOC is correct:
   int free = vtoc->free;
   int c = vtoc->first_free;
   while ( free )
   {
      if ( c > MAX_CLUSTER )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: VTOC free sanity failed %d > %d with %d free clusters left\n",__FUNCTION__,c,MAX_CLUSTER,free);
         return 1;
      }
      if ( free == 1 && map[c]==0 ) break; // Good ending
      if ( free == 1 )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: VTOC free sanity failed last free sector has chain: %02x\n",__FUNCTION__,map[c]);
         return 1; // Map has more
      }
      if ( map[c] <= c )
      {
         // Weird: On SS/DD image, we hit this with 00 entries at the end, being short 8 clusters
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: VTOC free sanity failed, free list goes backwards: %02x -> %02x, clusters remaining: %d\n",__FUNCTION__,c,map[c],free);
         if ( map[c] == 0 ) return 0; // Strange: I see this on a SS/DD image until things are written to it
         return 1; // Must always be increasing
      }
      c = map[c];
      --free;
   }
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: VTOC free list sane\n",__FUNCTION__);

   // Scan directory; At first zero status, everything should be zero
   int blank=0;
   struct dos4_dir_entry blank_entry;
   memset(&blank_entry,0,sizeof(blank_entry));

   for (int i=0;i<DIR_ENTRIES;++i)
   {
      if ( dirent[i].status == 0 ) blank=1;
      if ( blank && memcmp(&blank_entry,&dirent[i],sizeof(blank_entry)) != 0 ) return 1;
   }
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: End of DIR scan sane\n",__FUNCTION__);

   switch(dirent->status)
   {
      case 0:
      case FLAGS_DELETED:
      case FLAGS_IN_USE:
      case FLAGS_IN_USE|FLAGS_LOCKED:
         break;
      default:
         return 1;
   }
   if ( dirent->bytes_in_last_sector > atrfs.sectorsize ) return 1;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: First directory entry sane\n",__FUNCTION__);

   atrfs.readonly = 1; // FIXME: No write support yet
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

int dos4_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
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
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = atoi(path+sizeof("/.cluster")-1);
      if ( sec >= 0 && sec*CLUSTER_SIZE+1<=atrfs.sectors )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = CLUSTER_BYTES;
         stbuf->st_ino = sec*8+25;
         return 0; // Good, can read this sector
      }
   }
   // Presumably the dummy files, but who cares?
   stbuf->st_ino = 0x10001;
   stbuf->st_size = 0;
   return 0; // Whatever, don't really care
}

int dos4_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
   (void)path; // Always "/"
   (void)offset;
   (void)fi;
   (void)flags;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   filler(buf, "DOS_4_DISK_IMAGE", NULL, 0, 0);
   filler(buf, "NOT_YET_SUPPORTED", NULL, 0, 0);
   filler(buf, "USE_.cluster#_for_raw_nonzero_clusters", NULL, 0, 0);

#if 1 // Data clusters
   // Create .cluster000 ... .cluster255 as appropriate
   {
      unsigned char *zero = calloc(1,CLUSTER_BYTES);
      if ( !zero ) return -ENOMEM; // Weird
      char name[32];
      int digits = 3;
      for (int sec=8; sec*CLUSTER_SIZE+1<=atrfs.sectors; ++sec)
      {
         unsigned char *s = CLUSTER(sec);
         char *note="";
         if ( memcmp(s,zero,CLUSTER_BYTES) == 0 ) continue; // Skip empty sectors
         if ( sec == VTOC_CLUSTER || sec == VTOC_CLUSTER+1 ) note="-dir_vtoc";
         sprintf(name,".cluster%0*d%s",digits,sec,note);
         filler(buf,name,NULL,0,0);
      }
      free(zero);
   }
#endif
   return 0;
}

int dos4_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;

   // Magic /.cluster### files
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = atoi(path+sizeof("/.cluster")-1);
      if ( sec < 0 || sec*CLUSTER_SIZE+1>atrfs.sectors ) return -ENOENT;

      int bytes = CLUSTER_BYTES;
      if (offset >= bytes ) return -EOF;
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: read cluster %d starting at sector %d\n",__FUNCTION__,sec,CLUSTER_TO_SEC(sec));
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
