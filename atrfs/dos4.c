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
 *
 *
 * Note that the field in the directory saying how many bytes are valid in
 * the last sector is not a count of bytes, but is saying that bytes are
 * valid through that index.  So a value of '0' still has one byte valid.
 * This makes it impossible to represent a zero-length file.
 *
 * Not supporting zero-length files makes adding write support here
 * difficult.  It wouldn't be impossible, but would present challenges.  It
 * might be necessary to leave the entry deleted in the directory until
 * something is written and just keep an array in memory of flags for
 * deleted entries that represent zero-length files.  They would be deleted on
 * unmount, which is valid as the file system doesn't support them.
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
// FIXME: Hopefully this can be found in the boot sectors instead
#define CLUSTER_SIZE                            \
   /* Always 6 for SD/ED */                     \
   (atrfs->sectorsize==128?6:                    \
    /* 3 for DD unless it's a big disk */       \
    (atrfs->sectors <= 720 ? 3 :                 \
     /* 6 for DS/DD or other large disks */     \
     6 ))
#define CLUSTER_BYTES (CLUSTER_SIZE * atrfs->sectorsize)
#define CLUSTER_TO_SEC(n) (((n)-8)*CLUSTER_SIZE + 1 - ((n)>128?CLUSTER_SIZE:0)) // Starts at 8, not zero, but skip checksum at 0x80
#define CLUSTER(n) SECTOR(CLUSTER_TO_SEC(n))
#define VTOC_CLUSTER                                    \
   /* SS/SD: 66 */                                      \
   (atrfs->sectorsize==128 && atrfs->sectors<=720 ? 66 :  \
    /* SS/ED: 92 */                                     \
    (atrfs->sectorsize==128 ? 92 :                       \
     /* SS/DD: 126 */                                   \
     (atrfs->sectors <= 720 ? 126 :                      \
      /* DS/DD: 66 */                                   \
      66 )))
#define VTOC_SECTOR_COUNT                               \
   /* SS/SD: 1 */                                       \
   (atrfs->sectorsize==128 && atrfs->sectors<=720 ? 1 :   \
    /* SS/ED: 2 */                                      \
    (atrfs->sectorsize==128 ? 2 :                        \
     /* SS/DD: 1 */                                     \
     (atrfs->sectors <= 720 ? 1 :                        \
      /* DS/DD: 1 */                                    \
      1 )))
#define DIR_START ((struct dos4_dir_entry *)CLUSTER(VTOC_CLUSTER))
#define VTOC_START SECTOR(CLUSTER_TO_SEC(VTOC_CLUSTER+2)-VTOC_SECTOR_COUNT)
#define REAL_MAX_CLUSTER (atrfs->sectors/CLUSTER_SIZE+8-1) // Pretend 0x80 is a normal cluster
#define MAX_CLUSTER (REAL_MAX_CLUSTER < 0x80 ? REAL_MAX_CLUSTER : REAL_MAX_CLUSTER + 1)
#define DIR_ENTRIES ((CLUSTER_BYTES*2-VTOC_SECTOR_COUNT*atrfs->sectorsize)/(int)sizeof(struct dos4_dir_entry))
#define TOTAL_CLUSTERS (REAL_MAX_CLUSTER-7-(atrfs->sectorsize == 256?1:0))

/*
 * File System Structures
 */

struct dos4_dir_entry {
   unsigned char status; // Bit 7 valid, bit 6 exists, bit 1 locked, bit 0 open
   unsigned char blocks;
   unsigned char bytes_in_last_sector; // (not last block) Actually: Valid through byte 'n', so add one
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
int dos4_sanity(struct atrfs *atrfs);
int dos4_getattr(struct atrfs *atrfs,const char *path, struct stat *stbuf);
int dos4_readdir(struct atrfs *atrfs,const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int dos4_read(struct atrfs *atrfs,const char *path, char *buf, size_t size, off_t offset);
int dos4_write(struct atrfs *atrfs,const char *path, const char *buf, size_t size, off_t offset);
int dos4_unlink(struct atrfs *atrfs,const char *path);
int dos4_rename(struct atrfs *atrfs,const char *path1, const char *path2, unsigned int flags);
int dos4_chmod(struct atrfs *atrfs,const char *path, mode_t mode);
int dos4_create(struct atrfs *atrfs,const char *path, mode_t mode);
int dos4_truncate(struct atrfs *atrfs,const char *path, off_t size);
int dos4_statfs(struct atrfs *atrfs,const char *path, struct statvfs *stfsbuf);
int dos4_newfs(struct atrfs *atrfs);
char *dos4_fsinfo(struct atrfs *atrfs);

/*
 * Global variables
 */
const struct fs_ops dos4_ops = {
   .name = "Atari DOS 4",
   .fstype = "dos4",
   .fs_sanity = dos4_sanity,
   .fs_getattr = dos4_getattr,
   .fs_readdir = dos4_readdir,
   .fs_read = dos4_read,
   // .fs_write = dos4_write,
   // .fs_unlink = dos4_unlink,
   // .fs_rename = dos4_rename,
   // .fs_chmod = dos4_chmod,
   // .fs_create = dos4_create,
   // .fs_truncate = dos4_truncate,
   .fs_statfs = dos4_statfs,
   .fs_newfs = dos4_newfs,
   .fs_fsinfo = dos4_fsinfo,
};

static const unsigned char bootsectors[2][128*2] = {
   [0] = { // SS/DD
      0x00, 0x02, 0x08, 0x1c, 0x0e, 0x1c, 0xa9, 0x31, 0x8d, 0x00, 0x03, 0xa9, 0x01, 0x8d, 0x01, 0x03, 
      0xa9, 0x4e, 0x8d, 0x02, 0x03, 0xa9, 0x40, 0x8d, 0x03, 0x03, 0xa9, 0xfc, 0x8d, 0x04, 0x03, 0xa9, 
      0x1b, 0x8d, 0x05, 0x03, 0xa9, 0x02, 0x8d, 0x06, 0x03, 0xa9, 0x0c, 0x8d, 0x08, 0x03, 0xa9, 0x00, 
      0x8d, 0x09, 0x03, 0xa9, 0x00, 0x8d, 0x0a, 0x03, 0xa9, 0x00, 0x8d, 0x0b, 0x03, 0x20, 0x59, 0xe4, 
      0x30, 0x23, 0xa9, 0x00, 0x8d, 0x00, 0x1c, 0xa9, 0x04, 0x8d, 0x01, 0x1c, 0xa9, 0x01, 0x8d, 0x02, // 0x00 for SS/SD
      0x1c, 0xa9, 0x00, 0x8d, 0x03, 0x1c, 0xa9, 0x4f, 0x8d, 0x02, 0x03, 0xa9, 0x80, 0x8d, 0x03, 0x03, 
      0x20, 0x59, 0xe4, 0x10, 0x02, 0x38, 0x60, 0xa9, 0x52, 0x8d, 0x02, 0x03, 0xa9, 0x40, 0x8d, 0x03, 
      0x03, 0xa9, 0x01, 0x8d, 0x04, 0x03, 0xa9, 0x07, 0x8d, 0x05, 0x03, 0xa9, 0x07, 0x8d, 0x06, 0x03, 
   },
   [1] = { // DS/DD
      0x00, 0x02, 0x08, 0x1c, 0x0e, 0x1c, 0xa9, 0x31, 0x8d, 0x00, 0x03, 0xa9, 0x01, 0x8d, 0x01, 0x03, 
      0xa9, 0x4e, 0x8d, 0x02, 0x03, 0xa9, 0x40, 0x8d, 0x03, 0x03, 0xa9, 0xfc, 0x8d, 0x04, 0x03, 0xa9, 
      0x1b, 0x8d, 0x05, 0x03, 0xa9, 0x02, 0x8d, 0x06, 0x03, 0xa9, 0x0c, 0x8d, 0x08, 0x03, 0xa9, 0x00, 
      0x8d, 0x09, 0x03, 0xa9, 0x00, 0x8d, 0x0a, 0x03, 0xa9, 0x00, 0x8d, 0x0b, 0x03, 0x20, 0x59, 0xe4, 
      0x30, 0x23, 0xa9, 0x01, 0x8d, 0x00, 0x1c, 0xa9, 0x04, 0x8d, 0x01, 0x1c, 0xa9, 0x01, 0x8d, 0x02, // 0x01 for DS/DD
      0x1c, 0xa9, 0x00, 0x8d, 0x03, 0x1c, 0xa9, 0x4f, 0x8d, 0x02, 0x03, 0xa9, 0x80, 0x8d, 0x03, 0x03, 
      0x20, 0x59, 0xe4, 0x10, 0x02, 0x38, 0x60, 0xa9, 0x52, 0x8d, 0x02, 0x03, 0xa9, 0x40, 0x8d, 0x03, 
      0x03, 0xa9, 0x01, 0x8d, 0x04, 0x03, 0xa9, 0x07, 0x8d, 0x05, 0x03, 0xa9, 0x07, 0x8d, 0x06, 0x03, 
   },
};

/*
 * Functions
 */

/*
 * dos4_set_vtoc_checksum()
 *
 * byte 128 of the VTOC (except on SS/SD) is a sum of all previous bytes
 * and carry bits.
 *
 * I don't think DOS 4 actually verifies this at any point.
 */
void dos4_set_vtoc_checksum(struct atrfs *atrfs)
{
   unsigned char *map = VTOC_START;

   if ( atrfs->sectorsize == 128 && VTOC_SECTOR_COUNT == 1 ) return; // No checksum without a byte 128

   int sum = 0;
   for (int i=0;i<128;++i) sum += map[i];
   sum = (sum&0xff) + (sum>>8);
   map[128]=sum;
}

/*
 * dos4_free_cluster()
 */
int dos4_free_cluster(struct atrfs *atrfs,int cluster)
{
   struct dos4_vtoc *vtoc = VTOC_START;
   unsigned char *map = VTOC_START;

   if ( options.debug>1 ) fprintf(stderr,"DEBUG: %s: Cluster %d (old map value: %d)\n",__FUNCTION__,cluster,map[cluster]);
   if ( cluster > MAX_CLUSTER || cluster < 8 || ( cluster==8 && atrfs->sectorsize==256 ) ) return -EIO;
   if ( cluster == VTOC_CLUSTER ) return -EIO;
   if ( cluster == VTOC_CLUSTER+1 ) return -EIO;

   if ( !vtoc->first_free || map[vtoc->first_free] > cluster )
   {
      map[cluster] = vtoc->first_free;
      vtoc->first_free = cluster;
      ++vtoc->free;
      dos4_set_vtoc_checksum(atrfs);
      return 0;
   }

   int c = vtoc->first_free;
   while ( c < cluster && map[c] > c && map[c] < cluster )
   {
      c=map[c];
   }
   if ( map[c] > cluster || map[c] == 0 )
   {
      map[cluster] = map[c];
      map[c] = cluster;
      ++vtoc->free;
      dos4_set_vtoc_checksum(atrfs);
      return 0;
   }
   fprintf(stderr,"DEBUG: %s: Bad free cluster chain: attempt to free cluster %d; map[%d]->%d\n",__FUNCTION__,cluster,c,map[c]);
   return -EIO; // Bad free list
}

/*
 * dos4_get_dir_entry()
 */
int dos4_get_dir_entry(struct atrfs *atrfs,const char *path,struct dos4_dir_entry **dirent_found,int *isinfo)
{
   struct dos4_dir_entry *junk;
   if ( !dirent_found ) dirent_found = &junk; // Avoid NULL check later
   int junkinfo;
   if ( !isinfo ) isinfo=&junkinfo;

   *dirent_found = NULL;
   *isinfo = 0;
   while (*path == '/') ++path;
   if ( strchr(path,'/') ) return -ENOENT; // No subdirectories alowed
   if ( atrfs_strcmp(path,".info")==0 )
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
   if ( atrfs_strcmp(path,".info")==0 )
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
   if ( atrfs_strcmp(path,".info")==0 )
   {
      *isinfo=1;
      path += 5;
   }
   if ( *path )
   {
      return -ENOENT; // Name too long
   }

   struct dos4_dir_entry *dirent = DIR_START;
   int firstfree = 0;
   for ( int i=0; i<DIR_ENTRIES; ++i ) // Entry 0 is info about the file system
   {
      if (!dirent[i].status)
      {
         if ( !firstfree ) firstfree = i;
         break;
      }
      if ( (dirent[i].status & FLAGS_DELETED) )
      {
         if ( !firstfree ) firstfree = i;
         continue; // Not in use
      }
      if ( (dirent[i].status & FLAGS_OPEN) ) continue; // Hidden; incomplete writes
      if ( atrfs_memcmp(name,dirent[i].file_name,8+3)!=0 ) continue;

      *dirent_found = &dirent[i];
      return 0;
   }
   if ( firstfree ) *dirent_found = &dirent[firstfree];
   return -ENOENT;
}

/*
 * dos4_get_file_size()
 *
 * This requires tracing the cluster chain
 */
int dos4_get_file_size(struct atrfs *atrfs,struct dos4_dir_entry *dirent)
{
   unsigned char *vtoc = VTOC_START;
   int clusters = 0;
   int c = dirent->start;
   while ( clusters < 0xff )
   {
      if ( vtoc[c] < 0x08 )
      {
         return clusters * CLUSTER_BYTES + atrfs->sectorsize * vtoc[c] + dirent->bytes_in_last_sector+1;
      }
      if ( vtoc[c] > MAX_CLUSTER ) break;
      ++clusters;
      c=vtoc[c];
   }
   fprintf(stderr,"Corrupted file: %.8s.%.3s: Bad cluster chain\n",dirent->file_name,dirent->file_ext);
   return -EIO;
}

/*
 * dos4_info()
 *
 * Return the buffer containing information specific to this file.
 *
 * This could be extended to do a full analysis of binary load files,
 * BASIC files, or any other recognizable file type.
 *
 * The pointer returned should be 'free()'d after use.
 */
char *dos4_info(struct atrfs *atrfs,const char *path,struct dos4_dir_entry *dirent)
{
   char *buf,*b;
   int filesize;

   if ( !dirent )
   {
      filesize = DIR_ENTRIES * sizeof(struct dos4_dir_entry);
   }
   else
   {
      filesize = dos4_get_file_size(atrfs,dirent);
      if ( filesize < 0 ) return NULL;
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
                 "  Cluster size: %d bytes, %d sectors\n"
                 "  Clusters: %d\n"
                 "  Starting cluster: %d\n\n",
                 dirent - DIR_START,
                 dirent->status,CLUSTER_BYTES,CLUSTER_SIZE,dirent->blocks,dirent->start);
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
 * dos4_sanity(atrfs)
 *
 * Return 0 if this is a valid Atari DOS 4 file system
 */
int dos4_sanity(struct atrfs *atrfs)
{
   if ( atrfs->sectorsize != 128 &&  atrfs->sectorsize != 256 ) return 1; // Must be SD
   if ( atrfs->sectors < (VTOC_CLUSTER+2)*CLUSTER_SIZE - 1 ) return 1; // Too small
   struct dos4_dir_entry *dirent = DIR_START;
   struct dos4_vtoc *vtoc = VTOC_START;
   unsigned char *map = VTOC_START;
   if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s: Cluster size: %d  VTOC Clusters: %d-%d (sectors %d-%d)  VTOC Sector count: %d  First VTOC sector: %d Max DIR entries: %d Max Cluster: %d\n",__FUNCTION__,CLUSTER_SIZE,VTOC_CLUSTER,VTOC_CLUSTER+1,CLUSTER_TO_SEC(VTOC_CLUSTER),CLUSTER_TO_SEC(VTOC_CLUSTER+2)-1,VTOC_SECTOR_COUNT,CLUSTER_TO_SEC(VTOC_CLUSTER+2)-VTOC_SECTOR_COUNT,DIR_ENTRIES,MAX_CLUSTER);
   // Sector 1 is not special in DOS 4!
   // If DOS files are written, QDOS.SYS is simply written contiguously starting at
   // sector 1 (cluster 8)
   if ( MAX_CLUSTER > 0xff ) return 1; // Too large (well, it could have unallocated space, but that's stupid)
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
         // Strange, on a SS/DD disk, DOS 4 creates a VTOC where cluster f7 points to f8 instead of being zero
         // Also on SS/ED, DOS 4 VTOC has cluster b4 pointing to b5 instead of being zero
         // On a DS/DD disk, it gets it right with the same number of clusters
         // But on DS/DD, the free count is one short.
         // On SS/SD, the chain is right and the free count is right.
         if ( c == MAX_CLUSTER+1 && map[c]==0 ) // Let it optionally chain to past the end
         {
            ; // OK, bug in DOS 4
         }
         else
         {
            if ( options.debug ) fprintf(stderr,"DEBUG: %s: VTOC free sanity failed %d > %d with %d free clusters left\n",__FUNCTION__,c,MAX_CLUSTER,free);
            //return 1;
         }
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
      if ( blank && atrfs_memcmp(&blank_entry,&dirent[i],sizeof(blank_entry)) != 0 ) return 1;
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
   if ( dirent->bytes_in_last_sector > atrfs->sectorsize ) return 1;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: First directory entry sane\n",__FUNCTION__);

   atrfs->readonly = 1; // FIXME: No write support yet
   return 0;
}

/*
 * dos4_getattr()
 */
int dos4_getattr(struct atrfs *atrfs,const char *path, struct stat *stbuf)
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
   if ( atrfs_strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec >= 0 && sec*CLUSTER_SIZE+1<=atrfs->sectors && sec != 0x80 )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = CLUSTER_BYTES;
         stbuf->st_ino = sec*8+25;
         return 0; // Good, can read this sector
      }
   }
   // Real or info files
   struct dos4_dir_entry *dirent;
   int isinfo;
   int r;
   r = dos4_get_dir_entry(atrfs,path,&dirent,&isinfo);
   if ( r ) return r;
   stbuf->st_ino = dirent ? CLUSTER_TO_SEC(dirent->start) : 0;
   if ( isinfo ) stbuf->st_ino += 0x10000;
   if ( isinfo )
   {
      char *info = dos4_info(atrfs,path,dirent);
      stbuf->st_size = strlen(info);
      free(info);
   }
   else
   {
      stbuf->st_size = dos4_get_file_size(atrfs,dirent);
   }
   return 0;
}

/*
 * dos4_readdir()
 */
int dos4_readdir(struct atrfs *atrfs,const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)path; // Always "/"
   (void)offset;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   struct dos4_dir_entry *dirent = DIR_START;
   for ( int i=0; i<DIR_ENTRIES; ++i )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: entry %d, status %02x: %.8s.%.3s\n",__FUNCTION__,i,dirent[i].status,dirent[i].file_name,dirent[i].file_ext);
      if (!dirent[i].status) break;
      if ( (dirent[i].status & FLAGS_DELETED) ) continue;
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

   // Create .cluster000 ... .cluster255 as appropriate
   // Only for debugging
   if ( options.debug > 1 )
   {
      unsigned char *zero = calloc(1,CLUSTER_BYTES);
      if ( !zero ) return -ENOMEM; // Weird
      char name[32];
      int digits = 3;
      for (int sec=8; sec*CLUSTER_SIZE+1<=atrfs->sectors; ++sec)
      {
         unsigned char *s = CLUSTER(sec);
         char *note="";
         if ( sec == 128 ) continue; // No such cluster
         if ( atrfs_memcmp(s,zero,CLUSTER_BYTES) == 0 ) continue; // Skip empty sectors
         if ( sec == VTOC_CLUSTER || sec == VTOC_CLUSTER+1 ) note="-dir_vtoc";
         sprintf(name,".cluster%0*d%s",digits,sec,note);
         filler(buf,name,FILLER_NULL);
      }
      free(zero);
   }

   return 0;
}

/*
 * dos4_read()
 */
int dos4_read(struct atrfs *atrfs,const char *path, char *buf, size_t size, off_t offset)
{

   // Magic /.cluster### files
   if ( atrfs_strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec < 0 || sec*CLUSTER_SIZE+1>atrfs->sectors || sec==128 ) return -ENOENT;

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

   // Look up file
   struct dos4_dir_entry *dirent;
   int isinfo;
   int r;
   r = dos4_get_dir_entry(atrfs,path,&dirent,&isinfo);
   if ( r ) return r;

   // Info files
   if ( isinfo )
   {
      char *info = dos4_info(atrfs,path,dirent);
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
   unsigned char *vtoc = VTOC_START;
   int cluster = dirent->start;
   int bytes_read = 0;
   while ( size )
   {
      int bytes = CLUSTER_BYTES;
      if ( vtoc[cluster] > MAX_CLUSTER ) return -EIO; // Bad chain
      if ( vtoc[cluster] < 0x08 )
      {
         bytes = atrfs->sectorsize * vtoc[cluster] + dirent->bytes_in_last_sector + 1;
      }
      unsigned char *s=CLUSTER(cluster);
      if ( !s ) return -EIO; // Invalid CLUSTER
      if ( offset < bytes )
      {
         bytes -= offset;
         s += offset;
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
      if ( vtoc[cluster] < 0x08 ) break; // EOF
      cluster=vtoc[cluster];
   }
   if ( bytes_read ) return bytes_read;
   return -EOF;
}

// Implement these for read-write support
int dos4_write(struct atrfs *atrfs,const char *path, const char *buf, size_t size, off_t offset);
int dos4_unlink(struct atrfs *atrfs,const char *path);
int dos4_rename(struct atrfs *atrfs,const char *path1, const char *path2, unsigned int flags);
int dos4_chmod(struct atrfs *atrfs,const char *path, mode_t mode);
int dos4_create(struct atrfs *atrfs,const char *path, mode_t mode);
int dos4_truncate(struct atrfs *atrfs,const char *path, off_t size);

/*
 * dos4_newfs()
 */
int dos4_newfs(struct atrfs *atrfs)
{
   if ( atrfs->sectorsize != 128 && atrfs->sectorsize != 256 )
   {
      fprintf(stderr,"Error: Atari DOS 4 only supports SD or DD sector sizes\n");
      return -EIO;
   }
   if ( atrfs->sectors > 1482 ) // MAX_CLUSTER <= 0xff in both DS/DD and SS/ED mode with cluster size 6
   {
      fprintf(stderr,"Error: Atari DOS 4 maximum size is 1484 sectors\n");
      return -EIO;
   }
   if ( CLUSTER_TO_SEC(VTOC_CLUSTER+2) - 1 > atrfs->sectors )
   {
      fprintf(stderr,"Error: Atari DOS 4 needs %d sectors minimum for this density\n",CLUSTER_TO_SEC(VTOC_CLUSTER+2) - 1);
      return -EIO;
   }

   // Warn about non-standard images
   if ( atrfs->sectors != 720 &&
        !(atrfs->sectors == 1040 && atrfs->sectorsize == 128) &&
        !(atrfs->sectors == 1440 && atrfs->sectorsize == 256) )
   {
      if ( atrfs->sectors < 720 && atrfs->sectorsize == 128 )
         fprintf(stderr,"Creating short non-standard DOS 4 image:  Tell DOS 4 that this is SS/SD\n");
      else if ( atrfs->sectors < 720 && atrfs->sectorsize == 256 )
         fprintf(stderr,"Creating short non-standard DOS 4 image:  Tell DOS 4 that this is SS/DD\n");
      else if ( atrfs->sectorsize == 128 )
         fprintf(stderr,"Creating non-standard DOS 4 image:  Tell DOS 4 that this is SS/2D (AT1050)\n");
      else
         fprintf(stderr,"Creating non-standard DOS 4 image:  Tell DOS 4 that this is DS/DD\n");
   }

   /*
    * Formatting a disk with DOS 4 creates the VTOC and nothing else.
    * No boot sectors!
    *
    * With DD disks, it will write boot sectors when writing the DOS files; perhaps we want to do that here, too?
    */

   /*
    * The formatting rules are non-intuitive and different for each of the four supported sizes:
    *  SS/SD
    *     720 sectors grouped into 120 clusters of 6 sectors (768 bytes) each.
    *     These are numbered 8--127 to cover sectors 1--720.
    *     Clusters 66-67; sectors 349--360 are the directory and vtoc
    *     118 clusters are initially free, reported correctly as 708 sectors.
    *
    *  SS/ED
    *     1040 sectors grouped into 173 clusters of 6 sectors each.
    *     Sectors 1039 and 1040 are unused.
    *     Clusters 92--93, sectors 505--516, are the directory and 2 vtoc sectors.
    *     Cluster 128 does not exist; cluster 129 starts with sector 721 where 128 should start
    *     The entry for cluster 128 is modified, I'm not clear on what they byte represents.
    *     Note: DOS 4 reports half the real number of sectors, so it treats it as if it were
    *     clusters of 3 256-byte sectors instead of 6 128-byte sectors.
    *
    *  SS/DD
    *     720 sectors grouped into 240 clusters of 3 sectors (768 bytes) each.
    *     These are numbered 8--248, skipping 128 to cover sectors 1--720.
    *     Cluster 8 is not used because the boot sectors may be short.
    *     Cluster 126-127; sectors 355--360; are the directory and vtoc
    *     Reports 711 sectors free.
    *
    *  DS/DD
    *     1440 sectors grouped into 240 clusters of 6 sectors (1536 bytes) each.
    *     These are numbered 8--248, skipping 128 to cover sectors 1--1440.
    *     Cluster 8 is not used because the boot sectors may be short.
    *     Clusters 66--67 (sectors 349--359) are the directory and vtoc
    *     Cluster 248 is not used, which is apparently a bug DOS 4.  I have tested using
    *     it, and DOS 4 seems happy to allocate, read, and write it like any other cluster.
    *     Reports 708 sectors free, dividing real sectors by 2 to keep it under 1000.
    *
    *  Except on SS/SD where there is no byte 128 of the VTOC, this byte is a checksum of
    *  the bytes 0--127.  Just add them up, plus all the carry bits, and it should match.
    *  I don't see any indications either in the DOS source code or testing that it ever
    *  verifies the value.
    */
   struct dos4_vtoc *vtoc = VTOC_START;
   vtoc->format = 'R';
   if ( atrfs->sectorsize==256 && atrfs->sectors > 720 ) vtoc->format = 'C';

   for ( int cluster = 8; cluster <= MAX_CLUSTER; ++cluster)
   {
      if ( cluster == 8 && atrfs->sectorsize==256 ) continue;
      if ( cluster == 0x80 ) continue; // This is skipped
      if ( cluster == VTOC_CLUSTER ) continue;
      if ( cluster == VTOC_CLUSTER+1 ) continue;
      // Uncomment the following for bug compatibility:
      // if ( cluster == MAX_CLUSTER && atrfs->sectors == 1440 ) continue;
      dos4_free_cluster(atrfs,cluster);
   }

   /*
    * Real DOS 4 does not write boot sectors.
    * For SD, the boot sectors are also data sectors.
    * For DD, writing DOS files writes out two sectors of boot code.
    * They are identical except byte 68 is 1 for DS/DD and 0 for SS/DD.
    * Might as well write them out now.
    *
    * If writing QDOS.SYS to the image, be aware that there are a number of
    * byte differences between the files depending on the drive settings.
    */
   if ( atrfs->sectorsize == 256 )
   {
      const unsigned char *b;
      unsigned char *s = SECTOR(1);
      if ( atrfs->sectors <= 720 ) b=bootsectors[0];
      else b=bootsectors[1];
      for (int i=0;i<2;++i)
      {
         s=SECTOR(i+1);
         memcpy(s,b+i*128,128);
      }
   }
   return 0;
}

/*
 * dos4_statfs()
 */
int dos4_statfs(struct atrfs *atrfs,const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = CLUSTER_BYTES;
   stfsbuf->f_frsize = CLUSTER_BYTES;
   stfsbuf->f_blocks = TOTAL_CLUSTERS;
   struct dos4_vtoc *vtoc = VTOC_START;
   stfsbuf->f_bfree = vtoc->free;
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = DIR_ENTRIES;
   stfsbuf->f_ffree = 0;
   struct dos4_dir_entry *dirent = DIR_START;
   for (int i=0;i<DIR_ENTRIES;++i)
   {
      if ( (dirent[i].status & FLAGS_DELETED) || (dirent[i].status == 0x00) )
      {
         ++stfsbuf->f_ffree;
      }
   }
   stfsbuf->f_namemax = 12; // 8.3 including '.'
   // stfsbuf->f_namemax += 5; // Don't report this for ".info" files; not needed by FUSE
   return 0;
}

/*
 * dos4_fsinfo()
 */
char *dos4_fsinfo(struct atrfs *atrfs)
{
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;
   
   struct dos4_vtoc *vtoc = VTOC_START;
   b+=sprintf(b,"Cluster size:        %d bytes, %d sectors\n",CLUSTER_BYTES,CLUSTER_SIZE);
   b+=sprintf(b,"Total data clusters: %d: %d--%d  sectors: %d--%d\n",TOTAL_CLUSTERS,(atrfs->sectorsize==128)?8:9,MAX_CLUSTER,CLUSTER_TO_SEC((atrfs->sectorsize==128)?8:9),CLUSTER_TO_SEC(MAX_CLUSTER+1)-1);
   b+=sprintf(b,"Free clusters:       %d\n",vtoc->free);
   b+=sprintf(b,"VTOC/DIR clusters:   %d--%d\n",VTOC_CLUSTER,VTOC_CLUSTER+1);
   b+=sprintf(b,"DIR sectors:         %d--%d\n",CLUSTER_TO_SEC(VTOC_CLUSTER),CLUSTER_TO_SEC(VTOC_CLUSTER+2)-VTOC_SECTOR_COUNT-1);   
   if ( VTOC_SECTOR_COUNT == 1 )
   {
      b+=sprintf(b,"VTOC sector:         %d\n",CLUSTER_TO_SEC(VTOC_CLUSTER+2)-VTOC_SECTOR_COUNT);
   }
   else
   {
      b+=sprintf(b,"VTOC sector:         %d--%d\n",CLUSTER_TO_SEC(VTOC_CLUSTER+2)-VTOC_SECTOR_COUNT,CLUSTER_TO_SEC(VTOC_CLUSTER+2)-1);
   }
   return buf;
}
