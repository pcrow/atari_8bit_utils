/*
 * apt.c
 *
 * Functions for accessing drive images using the Atari Partition Table format.
 *
 * Copyright 2024
 * Preston Crow
 *
 * Released under the GPL version 2.0
 *
 * Reference:
 *
 * MBR:
 *  https://en.wikipedia.org/wiki/Master_boot_record
 *  https://en.wikipedia.org/wiki/Extended_boot_record
 *
 * APT:
 *  http://drac030.krap.pl/APT_spec.pdf
 *
 * The general logic here is to accept either a MBR image or APT image.  If
 * it's a MBR image, scan the partition table for an APT (type 7f) entry,
 * and use that.
 *
 * This does not support having multiple APT partitions within the MBR
 * partitions.  I could add that in the future if that's a real use case.
 *
 * APT:
 *
 * The partition table consists of a doubly linked list of sectors, starting with sector 0.
 * (Typically this is inside a MBR partition, but that's irrelevant in this code.)
 * Each sector can be thought of as a collection of 16-byte records.
 * Each sector has a header struct, at offset 0 in the first sector, but it can be
 * elsewhere (the offset is saved along with the sector number for the links).
 * (I'm not clear on why it shouldn't always be entry 0 on each sector, but so goes the spec.)
 *
 * If you remove the headers and make it an array, the first 15 entries
 * (optionally more) are called mapping entries; the partition entries for
 * D1: through D15:, which are copies of subsequent entries.  These entries
 * are followed by upto 65536 partition entries.
 *
 * The mapping entries are implemented as symlinks to the partition entries.
 *
 *
 * Implementation strategy:
 *
 * For 512-byte sectors, it's still just blocks of the memory-mapped file
 * that get passed down to local file systems.  But for smaller sectors,
 * they may not be mapped in a linear fashion.  Sectors may be interleaved
 * on a byte basis, and it may just use one physical sector for each
 * logical sector.  The spec is confusing, so the implementation may only
 * cover what I have samples of.
 *
 * For smaller sectors, just dynamically allocate memory for two copies of
 * the partition, up to 65535 sectors (as no Atari file system can use
 * more).  Keep one copy as a reference copy, and pass the other in as the
 * active file system memory for file system operations.  Then after every
 * write operation, call a copy-back routine that will do a memcmp between
 * the working and reference copies and copy back anything that has
 * changed.
 *
 * Performance will be horrible, but it's probably acceptable for this use
 * case.  Performance would be better by simply calling atrfs again to map
 * the .raw partition file.
 *
 *
 * Thoughts:
 *
 * My overall impression of the APT spec is that it is overly complicated with
 * a bunch of features that aren't needed.  I'm guessing behind each one there's
 * some history to explain it.  Too many "normally zero" fields.
 *
 */

#include FUSE_INCLUDE
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
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
#define VALID_SECTOR(_s) ((_s) < atrfs->atrstat.st_size/512 )
#define SECTORMEM(_s) ((void *)((char *)(atrfs->atrmem)+512*(_s)))

/*
 * Data Types
 */

/*
 * MBR structs
 */
struct __attribute__((__packed__)) mbr_partition {
   unsigned char status; // 0x80 for active?
   unsigned char chs_first[3]; // head (1 byte), sector(6 low bits), cylinder (2 high bits + next 8 bits)
   unsigned char type; // 05 for CHS addressing, 0F for LBA
   unsigned char chs_last[3];
   uint32_t lba_first; // little endian
   uint32_t sectors;
};

struct __attribute__((__packed__)) mbr_partition_table {
   char bootstrap[446]; // May include disk timestamp and disk signature
   struct mbr_partition partition[4];
   unsigned char mbr_boot_signature[2]; // 0x55, 0xAA
};

/*
 * APT Partition Table Header
 *
 * There is one header per partition table sector.
 */
struct __attribute__((__packed__)) apt_partition_table_header {
   unsigned char global_flags;
   unsigned char signature[3];
   unsigned char boot_drive;  // 0 for unspecified, otherwise 1-15
   unsigned char current_table_sector_entries; // Valid: 1-32 (entries on this sector, including the header)
   unsigned char header_entry_offset; // valid: 0-31
   unsigned char header_entry_prev_offset; // valid: 0-31
   uint32_t next_sector_in_table_chain; // absolute LBA sector number; 0 for last header
   uint32_t prev_sector_in_table_chain; // 0 in first, absolute LBA sector number
};

/*
 * Partition Table Entry
 *
 * There are 0 to 31 of these on each partition table sector
 */
struct __attribute__((__packed__)) partition_table_entry {
   unsigned char access_flags;
   unsigned char partition_type;
   uint32_t starting_sector;
   uint32_t sector_count;
   uint16_t partition_id;
   unsigned char partition_type_details[4];
};

/*
 * Metadata Leader
 */
struct __attribute__((__packed__)) metadata_leader {
   unsigned char apt_signature[3]; // "APT"
   unsigned char revision; // 00
   unsigned char metadata_signature[4]; // "META"
   unsigned char reserved[8];
   char partition_name[40]; // ATASCII
   unsigned char reserved_zeros[456];
};

/*
 * Local Structs
 */
struct apt_partition {
   void *mem;
   void *working; // working copy to pass in to file system layer
   void *reference; // reference copy for detecting writes
   unsigned int start;
   unsigned int sectors;
   int bytes_per_sector;
   int bytes_access;
   int byte_interleave;
   int sectors_per_sector;
   int chunks;
   size_t chunk_size;
   int size_divisor;
   struct partition_table_entry *entry;
   struct metadata_leader *meta;
   char *name;
   int submount; // non-zero if mounting a file system
   struct atrfs atrfs; // If mounting a file system (e.g., SpartaDOS)
};

/*
 * Function prototypes
 */
int apt_sanity(struct atrfs *atrfs);
int apt_getattr(struct atrfs *atrfs,const char *path, struct stat *stbuf);
int apt_readdir(struct atrfs *atrfs,const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int apt_read(struct atrfs *atrfs,const char *path, char *buf, size_t size, off_t offset);
int apt_write(struct atrfs *atrfs,const char *path, const char *buf, size_t size, off_t offset);
int apt_unlink(struct atrfs *atrfs,const char *path);
int apt_rename(struct atrfs *atrfs,const char *path1, const char *path2, unsigned int flags);
int apt_chmod(struct atrfs *atrfs,const char *path, mode_t mode);
int apt_readlink(struct atrfs *atrfs,const char *path, char *buf, size_t size);
int apt_create(struct atrfs *atrfs,const char *path, mode_t mode);
int apt_truncate(struct atrfs *atrfs,const char *path, off_t size);
#if (FUSE_USE_VERSION >= 30)
int apt_utimens(struct atrfs *atrfs,const char *path, const struct timespec tv[2]);
#else
int apt_utime(struct atrfs *atrfs,const char *path, struct utimbuf *utimbuf);
#endif
int apt_statfs(struct atrfs *atrfs,const char *path, struct statvfs *stfsbuf);
int apt_newfs(struct atrfs *atrfs);
char *apt_fsinfo(struct atrfs *atrfs);

/*
 * Global variables
 */
const struct fs_ops apt_ops = {
   .name = "Disk Image using Atari Partition Table",
   .fstype = "apt",
   .fs_sanity = apt_sanity,
   .fs_getattr = apt_getattr,
   .fs_readdir = apt_readdir,
   .fs_read = apt_read,
   .fs_write = apt_write,
   .fs_unlink = apt_unlink,
   .fs_rename = apt_rename,
   .fs_chmod = apt_chmod,
   .fs_readlink = apt_readlink,
   .fs_create = apt_create,
   .fs_truncate = apt_truncate,
#if (FUSE_USE_VERSION >= 30)
   .fs_utimens = apt_utimens,
#else
   .fs_utime = apt_utime,
#endif
   .fs_statfs = apt_statfs,
   // .fs_newfs = apt_newfs, // Not applicable
   .fs_fsinfo = apt_fsinfo,
};

off_t apt_offset; // Offset in bytes from the start of the overall image (i.e., start of APT partition in MBR)
int num_partitions;
int num_mappings;
struct apt_partition *partitions;
struct apt_partition mappings[15];

/*
 * Functions
 */

/*
 * find_apt_offset_in_mbr()
 *
 * Scan the MBT to find an APT partition and return the offset in bytes from the start.
 * If none is found, return 0.
 */
off_t find_apt_offset_in_mbr(struct atrfs *atrfs)
{
   struct mbr_partition_table *mbr = atrfs->atrmem;
   if ( mbr->mbr_boot_signature[0] != 0x55 || mbr->mbr_boot_signature[1] != 0xAA ) return 0;
   for (int i=0;i<4;++i)
   {
      if ( mbr->partition[i].type == 0 ) return 0; // End of partitions
      if ( mbr->partition[i].type == 0x7f )
      {
         return (off_t)le32toh(mbr->partition[i].lba_first) * 512;
      }
   }
   return 0;
}

/*
 * scan_apt_partitions()
 */
int scan_apt_partitions(struct atrfs *atrfs,void *mem,int header_offset,int first)
{
   struct apt_partition_table_header *head = mem;
   head += header_offset; // Not sure why it would ever be non-zero
   if ( options.debug ) printf("Scanning %s APT partition sector with %d entries (header offset %d)\n",first?"first":"additional",head->current_table_sector_entries,header_offset);

   if ( head->signature[0] != 'A' || head->signature[1] != 'P' || head->signature[2] != 'T' )
   {
      if ( options.debug ) printf("APT signature mismatch\n");
      return 1;
   }

   // Reserve room in local copy for all entries on this sector (plus 1, but who cares)
   partitions = realloc(partitions,sizeof(partitions[0])*(num_partitions+head->current_table_sector_entries));
   if ( !partitions )
   {
      fprintf(stderr,"Out of memory\n");
      exit(1);
   }

   for ( int i=0;i<head->current_table_sector_entries;++i )
   {
      struct partition_table_entry *entry = mem;
      struct apt_partition *this;
      int map=0;

      entry += i;
      if ( !entry->starting_sector || !entry->sector_count ) continue; // Blanks are normal for mappings

      if ( i==header_offset ) continue;
      // Add this entry to the table
      if ( first && i > 0 && i < 16 )
      {
         this = &mappings[i-1];
         map=i; // Flag for name
      }
      else
      {
         this=&partitions[num_partitions++];
      }
      // Fill in 'this' from the entry
      this->entry = entry;

      this->start = le32toh(this->entry->starting_sector);
      this->sectors = le32toh(this->entry->sector_count);
      if ( entry->partition_type == 0x00 )
      {
         // Usually zero, but the spec says there can be reserved sectors at the start
         this->start += BYTES2(&entry->partition_type_details[2]);
         this->sectors -= BYTES2(&entry->partition_type_details[2]);
      }
      this->mem = SECTORMEM(this->start);
      if ( entry->access_flags & 0x80 ) // Deleted or otherwise reserved
      {
         this->start = this->sectors = 0;
         this->mem = NULL;
         this->name = "";
         continue;
      }
      this->working = this->mem;
      if ( (this->start+1) * 512 > atrfs->atrstat.st_size )
      {
         fprintf(stderr,"ATP partition says it starts at sector %u which is past the end of the file\n",this->start);
         exit (1);
      }
      if ( (this->start+this->sectors) * 512 > atrfs->atrstat.st_size )
      {
         fprintf(stderr,"ATP partition says it runs through sector %u which is past the end of the file\n",this->start + this->sectors);
         this->sectors = atrfs->atrstat.st_size/512 - this->start;
         fprintf(stderr,"ATP partition adjusted to %u sectors\n",this->sectors);
      }
      this->bytes_per_sector = 64 << (entry->access_flags & 0x03);
      this->bytes_access = (entry->access_flags >> 2) & 0x03;
      this->sectors_per_sector = 1;
      if ( this->bytes_per_sector < 512 && ( this->bytes_access & 1 ) ) this->sectors_per_sector = 2;
      this->byte_interleave = 0; // Default to sector interleave
      if ( this->bytes_per_sector < 512 && ( this->bytes_access & 2 ) == 0 ) this->byte_interleave = 1;

      this->meta = (void *)((char *)(atrfs->atrmem) + le32toh(this->entry->starting_sector) * 512 - 512);
      this->chunk_size = this->sectors; // Most types have one big chunk
      this->chunks = 1;
      this->size_divisor = 1; // FIXME: Adjust to 2 or 4 if only 1 256-byte or 128-byte sector stored per sector.
      if ( entry->partition_type == 0x02 ) // floppy drawer; note chunk size
      {
         this->chunk_size = BYTES2(entry->partition_type_details);
         this->chunks = this->sectors / this->chunk_size;
      }
      if ( entry->partition_type == 0x03 ) // meta location is different
      {
         int lba = BYTES3(&(entry->partition_type_details[1]));
         if ( lba && lba < atrfs->atrstat.st_size/512 )
         {
            this->meta = (void *)((char *)(atrfs->atrmem) + lba * 512);
         }
      }
      if ( this->meta->apt_signature[0] != 'A' ||
           this->meta->apt_signature[1] != 'P' ||
           this->meta->apt_signature[2] != 'T' ||
           this->meta->metadata_signature[0] != 'M' ||
           this->meta->metadata_signature[1] != 'E' ||
           this->meta->metadata_signature[2] != 'T' ||
           this->meta->metadata_signature[3] != 'A' )
      {
         this->meta = NULL;
      }
      char name[256];

      if ( map )
      {
         sprintf(name,"D%c%s%.40s",map<10?'0'+map:'J'-10+map,this->meta?" ":"",this->meta?this->meta->partition_name:"");
      }
      else
      {
         if ( this->meta && this->meta->partition_name[0] )
            sprintf(name,"partition %d %.40s",num_partitions,this->meta->partition_name);
         else
            sprintf(name,"partition %d",num_partitions);
      }
      this->name=strdup(name);
   }
   if ( head->next_sector_in_table_chain && VALID_SECTOR( le32toh(head->next_sector_in_table_chain) ) )
   {
      return scan_apt_partitions(atrfs,(char *)(atrfs->atrmem) + le32toh(head->next_sector_in_table_chain) * 512,head->header_entry_offset,0);
   }
   return 0;
}

/*
 * apt_prepare_submounts()
 *
 * For each partition, if an Atari file system is detected, create the atrfs struct for it
 *
 * Note: This only works if the sectors are sequential in memory.
 */
void apt_prepare_submounts(struct atrfs *atrfs)
{
   for (int i=0;i<num_partitions;++i)
   {
      partitions[i].atrfs.fd = -1;
      partitions[i].atrfs.readonly = atrfs->readonly;
      partitions[i].atrfs.atrstat = atrfs->atrstat;
      partitions[i].atrfs.atrmem = partitions[i].working;
      partitions[i].atrfs.mem = partitions[i].working;
      partitions[i].atrfs.shortsectors = 0; // Not supported
      partitions[i].atrfs.sectorsize = partitions[i].bytes_per_sector;
      partitions[i].atrfs.sectors = partitions[i].sectors;
      partitions[i].atrfs.fstype = ATR_UNKNOWN; // Scan and check

      for (int j=ATR_SPECIAL;j<ATR_APT;++j)
      {
         if ( fs_ops[j] && fs_ops[j]->fs_sanity )
         {
            if ( (fs_ops[j]->fs_sanity)(&partitions[i].atrfs) == 0 )
            {
               partitions[i].atrfs.fstype = j;
               if ( options.debug ) fprintf(stderr,"DEBUG: %s detected %s image\n",__FUNCTION__,fs_ops[j]->name);
               partitions[i].submount = 1;
               break; // Next partition
            }
         }
      }
   }
}

/*
 * apt_path_to_partition()
 *
 * Return the index to the partition indicated by the path.
 * Return is negative if no partition is indicated.
 */
int apt_path_to_partition(const char *path)
{
   for (int i=0;i<num_partitions;++i)
   {
      if ( !partitions[i].start ) continue;
      int n = strlen(partitions[i].name);
      if ( strncasecmp(path+1,partitions[i].name,n) == 0 &&
           ( path[1+n] == 0 || path[1+n] == '/' ) )
         return i;
   }
   return -1;
}

/*
 * apt_subpath()
 *
 * Given a partition, return the partition-relative path.
 * i.e., move past /partition_name
 * If the path is too short, return NULL.
 * it's just "/partition" then return "/" instead of "".
 */
const char *apt_subpath(const char *path,int p)
{
   int n = strlen(partitions[p].name);
   if ( strlen(path) < (size_t)(n+1) ) return NULL; // Impossible if 'p' is from apt_path_to_partition
   if ( !(path[n+1]) ) return "/";
   return path+n+1;
}

/*
 * apt_copypartitions()
 *
 * Create a working and reference copy of each partition if it's not a straight linear mapping.
 */
void apt_copypartitions(void)
{
   for (int p=0;p<num_partitions;++p)
   {
      if ( partitions[p].bytes_per_sector == 512 ) continue;
      if ( partitions[p].bytes_per_sector == 256 && !partitions[p].byte_interleave && partitions[p].sectors_per_sector == 2 ) continue; // FIXME: Find an example and verify
      int sectors = partitions[p].sectors;
      if ( sectors > 65535 ) sectors = 65535; // No file system uses more
      partitions[p].working = malloc(partitions[p].bytes_per_sector * sectors);
      partitions[p].reference = malloc(partitions[p].bytes_per_sector * sectors);
      if ( !partitions[p].working || !partitions[p].reference )
      {
         fprintf(stderr,"Unable to allocate memory for partition reference copies\n");
         exit(1);
      }
      char path[128];
      sprintf(path,"/%s/.raw",partitions[p].name);
      int r = apt_read(NULL,path,partitions[p].working,partitions[p].bytes_per_sector * sectors,0);
      if ( r != partitions[p].bytes_per_sector * sectors )
      {
         free(partitions[p].working);
         free(partitions[p].reference);
         partitions[p].working = partitions[p].mem;
         partitions[p].reference = NULL;
         fprintf(stderr,"Unable to make partition reference copy; file system access not supported on partition %d\n",p+1);
         continue;
      }
      memcpy(partitions[p].reference,partitions[p].working,partitions[p].bytes_per_sector * sectors);
   }
}

/*
 * apt_copyback()
 *
 * After a write operation on a partition file system, copy back to the real base file.
 */
void apt_copyback(int p)
{
   if ( !partitions[p].reference ) return; // Not using this feature

   int sectors = partitions[p].sectors;
   if ( sectors > 65535 ) sectors = 65535; // No file system uses more
   char path[128];
   sprintf(path,"/%s/.raw",partitions[p].name);
   for (int s=0;s<sectors;++s)
   {
      if ( memcmp(&((char *)partitions[p].reference)[s*partitions[p].bytes_per_sector],
                  &((char *)partitions[p].working)[s*partitions[p].bytes_per_sector],
                  partitions[p].bytes_per_sector) != 0 )
      {
         // write back sector 's'
         apt_write(NULL,path,
                   &((char *)partitions[p].working)[s*partitions[p].bytes_per_sector],
                   partitions[p].bytes_per_sector,
                   s*partitions[p].bytes_per_sector);
         // save changes in reference copy
         memcpy(&((char *)partitions[p].reference)[s*partitions[p].bytes_per_sector],
                &((char *)partitions[p].working)[s*partitions[p].bytes_per_sector],
                partitions[p].bytes_per_sector);
      }
   }
}

/*
 * apt_info()
 *
 * Debugging for now, eventually it will be the .fsinfo output
 */
int apt_info(void)
{
   printf("Drive mappings:\n");
   for (int i=0;i<15;++i)
   {
      if ( mappings[i].start )
      {
         printf(" D%d: partition %d   start at sector %u, total sectors %u\n",i+1,le16toh(mappings[i].entry->partition_id),mappings[i].start,mappings[i].sectors);
      }
   }
   printf("Partitions: (%d)\n",num_partitions);
   for (int i=0;i<num_partitions;++i)
   {
      printf("Partition %d:\n",i+1);
      printf("  start %d sectors %d\n",partitions[i].start,partitions[i].sectors);
      if ( partitions[i].meta )
         printf("  label: %.40s\n",partitions[i].meta->partition_name);
   }
   return 0;
}

/*
 * apt_sanity()
 *
 * Return 0 if this is a valid Atari DOS 4 file system
 */
int apt_sanity(struct atrfs *atrfs)
{
   struct apt_partition_table_header *apt_table;

   apt_offset = find_apt_offset_in_mbr(atrfs);
   if ( options.debug && apt_offset ) printf("APT partition found in MBR at offset %lu\n",apt_offset);
   apt_table = (void *)&((char *)atrfs->atrmem)[apt_offset];
   if ( apt_table->signature[0] != 'A' || apt_table->signature[1] != 'P' || apt_table->signature[2] != 'T' ) return 1;
   if ( apt_table->current_table_sector_entries < 1 || apt_table->current_table_sector_entries > 32 ) return 1;
   if ( apt_table->boot_drive > 15 ) return 1;
   if ( apt_table->header_entry_offset > 31 ) return 1;
   if ( apt_table->header_entry_prev_offset ) return 1; // must be zero for first entry
   if ( apt_table->prev_sector_in_table_chain ) return 1; // prev must be zero for the first sector
   scan_apt_partitions(atrfs,apt_table,0,1);
   apt_copypartitions();
   apt_prepare_submounts(atrfs);
   if ( options.debug ) apt_info(); // DEBUG
   return 0;
}

/*
 * apt_getattr()
 */
int apt_getattr(struct atrfs *atrfs,const char *path, struct stat *stbuf)
{
   (void)atrfs;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   if ( strcmp(path,"/") == 0 )
   {
      stbuf->st_ino = 0x10000;
      stbuf->st_size = 0;
      stbuf->st_mode = MODE_DIR(stbuf->st_mode);
      stbuf->st_mode = MODE_RO(stbuf->st_mode);
      return 0;
   }
   for (int i=0;i<15;++i)
   {
      if ( !mappings[i].start ) continue;
      if ( strcasecmp(path+1,mappings[i].name) == 0 )
      {
         stbuf->st_ino = 0x20000 + i;
         stbuf->st_size = 0;
         stbuf->st_mode = 0120777; // symlink
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: found as mapping %d\n",__FUNCTION__,i);
         return 0;
      }
   }
   for (int i=0;i<num_partitions;++i)
   {
      if ( !partitions[i].start ) continue; // It might be a reserved or deleted partition
      if ( strcasecmp(path+1,partitions[i].name) == 0 )
      {
         stbuf->st_ino = 0x30000 + i;
         stbuf->st_size = 0;
         stbuf->st_mode = MODE_DIR(stbuf->st_mode);
         // Some partitions have a read-only option:
         if ( ( partitions[i].entry->partition_type == 0x00 || partitions[i].entry->partition_type == 0x03 ) &&
              partitions[i].entry->partition_type_details[0] & 0x80 )
         {
            stbuf->st_mode = MODE_RO(stbuf->st_mode);
         }
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: found as table entry %d\n",__FUNCTION__,i);
         return 0;
      }
      char match[128];
      if ( partitions[i].chunks > 1 )
      {
         for (int j=0;j<partitions[i].chunks;++j)
         {
            sprintf(match,"/%s/.raw%d",partitions[i].name,j);
            if ( strcasecmp(path,match) != 0 ) continue;
            stbuf->st_ino = 0x1000000+(i<<16)+j;
            stbuf->st_size = partitions[i].chunk_size*512/partitions[i].size_divisor;
         return 0;
         }
      }
      else
      {
         sprintf(match,"/%s/.raw",partitions[i].name);
         if ( strcasecmp(path,match) != 0 ) continue;
         stbuf->st_ino = 0x40000+i;
         stbuf->st_size = partitions[i].sectors*512/partitions[i].size_divisor;
         return 0;
      }
   }

   int p = apt_path_to_partition(path);
   if ( p >= 0 )
   {
      return (generic_ops.fs_getattr)(&partitions[p].atrfs,apt_subpath(path,p),stbuf);
   }
   return -ENOENT;
}

/*
 * apt_readdir()
 */
int apt_readdir(struct atrfs *atrfs,const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)atrfs; // Not needed
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   if ( strcmp(path,"/") == 0 )
   {
      for (int i=0;i<15;++i)
      {
         if ( mappings[i].start )
         {
            filler(buf,mappings[i].name,FILLER_NULL);
         }
      }
      for (int i=0;i<num_partitions;++i)
      {
         if ( !partitions[i].start ) continue;
         filler(buf,partitions[i].name,FILLER_NULL);
      }
      return 0;
   }

   int p = apt_path_to_partition(path);
   if ( p >= 0 )
   {
      path = apt_subpath(path,p);
      // Special files in the root directory
      if ( strcmp(path,"/") == 0 )
      {
         if ( partitions[p].chunks > 1 )
         {
            for (int j=0;j<partitions[p].chunks;++j)
            {
               char name[16];
               sprintf(name,".raw%d",j);
               filler(buf,name,FILLER_NULL);
            }
         }
         else
         {
            filler(buf,".raw",FILLER_NULL);
         }
      }
      // Files specific to the file system
      if ( !partitions[p].submount ) return 0;
      return (generic_ops.fs_readdir)(&partitions[p].atrfs,path,buf,filler,offset);
   }

   return -ENOENT;
}

/*
 * apt_read()
 */
int apt_read(struct atrfs *atrfs,const char *path, char *buf, size_t size, off_t offset)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return -ENOENT;
   path = apt_subpath(path,p);

   // Handle raw files
   {
      int chunk = 0;
      char match[128];
      int match_found = 0;
      if ( partitions[p].entry->partition_type == 0x02 ) // floppy drawer; use chunks
      {
         for ( chunk=0;chunk < partitions[p].chunks;++chunk )
         {
            sprintf(match,"/.raw%d",chunk);
            if ( strcasecmp(path,match) != 0 ) continue;
            match_found = 1;
            break;
         }
      }
      else
      {
         if ( strcasecmp(path,"/.raw") == 0 ) match_found = 1;
      }
      // Read from this partition
      if ( match_found )
      {
         // Adjust size/offset to be legal for the partition size
         if ( (size_t)offset >= partitions[p].chunk_size*512/partitions[p].size_divisor )
         {
            return 0;
         }
         if ( offset + size > partitions[p].chunk_size * 512 / partitions[p].size_divisor )
         {
            size = partitions[p].chunk_size*512 / partitions[p].size_divisor - offset;
         }

         // Do 512-byte logical sectors (easy case)
         if ( partitions[p].bytes_per_sector == 512 )
         {
            memcpy(buf,partitions[p].mem+offset+chunk*partitions[p].chunk_size,size);
            return size;
         }
         // Do sector-interleaved copy for 256-byte sectors, two sectors per sector
         // Note: I don't have an example of this to verify it, but it looks trivial
         if ( partitions[p].bytes_per_sector == 256 && !partitions[p].byte_interleave && partitions[p].sectors_per_sector == 2 )
         {
            memcpy(buf,partitions[p].mem+offset+chunk*partitions[p].chunk_size,size);
            return size;
         }
         // Do byte-interleaved copy for 256-byte sectors, one sector per sector
         // Note: Spec says use low-order byte of each word; example has bytes doubled instead of zero-padding
         if ( partitions[p].bytes_per_sector == 256 && partitions[p].byte_interleave && partitions[p].sectors_per_sector == 1 )
         {
            char *src = partitions[p].mem+offset*2+chunk*partitions[p].chunk_size;
            for ( size_t i=0;i<size;++i)
            {
               *buf++=*src;
               src+=2; // Skip the second byte
            }
            return size;
         }

         // Do byte-interleaved copy for 128-byte sectors, one sector per sector
         // Note: Spec says use low-order byte of each quadword; example has bytes duplicated instead of zero-padding
         if ( partitions[p].bytes_per_sector == 128 && partitions[p].byte_interleave && partitions[p].sectors_per_sector == 1 )
         {
            char *src = partitions[p].mem+offset*2+chunk*partitions[p].chunk_size;
            for ( size_t i=0;i<size;++i)
            {
               *buf++=*src;
               src+=4; // Skip the extra bytes
            }
            return size;
         }

         // Note: I don't have examples of any of the following:
         // FIXME: Do byte-interleaved copy for 128-byte sectors, two sectors per sector
         // FIXME: Do sector-interleaved copy for 128-byte sectors, two sectors per sector
         // FIXME: Do sector-interleaved copy for 128-byte sectors, one sector per sector
         // FIXME: Do sector-interleaved copy for 128-byte sectors, one sector per sector
         // FIXME: Do sector-interleaved copy for 256-byte sectors, one sector per sector
         // FIXME: Verify that 128-byte sectors pack in two per sector, not four; the spec is unclear

         // Fallback: read the raw data for debugging
         // FIXME:
         //   There are partition types that pack smaller sectors in non-linear ways.
         //   This includes some that interleave bytes and some that have unused bytes.
         //   The code here needs to be updated.
         memcpy(buf,partitions[p].mem+offset+chunk*partitions[p].chunk_size,size);
         return size;
      }
   }
   return (generic_ops.fs_read)(&partitions[p].atrfs,path,buf,size,offset);
}

/*
 * apt_write()
 */
int apt_write(struct atrfs *atrfs,const char *path, const char *buf, size_t size, off_t offset)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return -ENOENT;
   path = apt_subpath(path,p);

   // Handle raw files
   {
      int chunk = 0;
      char match[128];
      int match_found = 0;
      if ( partitions[p].entry->partition_type == 0x02 ) // floppy drawer; use chunks
      {
         for ( chunk=0;chunk < partitions[p].chunks;++chunk )
         {
            sprintf(match,"/.raw%d",chunk);
            if ( strcasecmp(path,match) != 0 ) continue;
            match_found = 1;
            break;
         }
      }
      else
      {
         if ( strcasecmp(path,"/.raw") == 0 ) match_found = 1;
      }

      // Write to this partition
      if ( match_found )
      {
         if ( (size_t)offset >= partitions[p].chunk_size*512/partitions[p].size_divisor )
         {
            return 0;
         }
         if ( offset + size > (size_t)partitions[p].chunk_size * 512 / partitions[p].size_divisor )
         {
            size = partitions[p].chunk_size*512 / partitions[p].size_divisor - offset;
         }
         // Do 512-byte logical sectors (easy case)
         if ( partitions[p].bytes_per_sector == 512 )
         {
            memcpy(partitions[p].mem+offset+chunk*partitions[p].chunk_size,buf,size);
            return size;
         }
         // Do sector-interleaved copy for 256-byte sectors, two sectors per sector
         // Note: I don't have an example of this to verify it, but it looks trivial
         if ( partitions[p].bytes_per_sector == 256 && !partitions[p].byte_interleave )
         {
            memcpy(partitions[p].mem+offset+chunk*partitions[p].chunk_size,buf,size);
            return size;
         }
         // Do byte-interleaved copy for 256-byte sectors, one sector per sector
         // Note: Spec says use low-order byte of each word; example has bytes doubled instead of zero-padding
         if ( partitions[p].bytes_per_sector == 256 && partitions[p].byte_interleave && partitions[p].sectors_per_sector == 1 )
         {
            char *dst = partitions[p].mem+offset*2+chunk*partitions[p].chunk_size;
            for ( size_t i=0;i<size;++i)
            {
               *dst++ = *buf;
               *dst++ = *buf++; // write twice instead of skipping as per example
            }
            return size;
         }
         // Do byte-interleaved copy for 128-byte sectors, one sector per sector
         // Note: Spec says use low-order byte of each quadword; example has bytes duplicated instead of zero-padding
         if ( partitions[p].bytes_per_sector == 128 && partitions[p].byte_interleave && partitions[p].sectors_per_sector == 1 )
         {
            char *dst = partitions[p].mem+offset*2+chunk*partitions[p].chunk_size;
            for ( size_t i=0;i<size;++i)
            {
               *dst++ = *buf;
               *dst++ = *buf;
               *dst++ = *buf;
               *dst++ = *buf++; // write all four bytes instead of skipping as per example
            }
            return size;
         }

         // FIXME: Add support for the other sector packing methods
         // Code should be nearly identical to the read case
         return -EIO; // Not supported, so don't try it
      }
   }
   int r = (generic_ops.fs_write)(&partitions[p].atrfs,path,buf,size,offset);
   apt_copyback(p);
   return r;
}

/*
 * apt_mkdir()
 */
int apt_mkdir(struct atrfs *atrfs,const char *path,mode_t mode)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return -EPERM;
   path = apt_subpath(path,p);
   int r = (generic_ops.fs_mkdir)(&partitions[p].atrfs,path,mode);
   apt_copyback(p);
   return r;
}

/*
 * apt_rmdir()
 */
int apt_rmdir(struct atrfs *atrfs,const char *path)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return -EIO;
   path = apt_subpath(path,p);
   int r = (generic_ops.fs_rmdir)(&partitions[p].atrfs,path);
   apt_copyback(p);
   return r;
}

/*
 * apt_unlink()
 */
int apt_unlink(struct atrfs *atrfs,const char *path)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return -EIO;
   // FIXME: Should we allow removing a mapping symlink?
   path = apt_subpath(path,p);
   int r = (generic_ops.fs_unlink)(&partitions[p].atrfs,path);
   apt_copyback(p);
   return r;
}

/*
 * apt_rename()
 */
int apt_rename(struct atrfs *atrfs,const char *path1, const char *path2, unsigned int flags)
{
   (void)atrfs;
   int p = apt_path_to_partition(path1);
   int p2 = apt_path_to_partition(path2);
   // Trying to rename a partition or mapping
   if ( p < 0 && p2 < 0 )
   {
      // FIXME: Allow renaming of a partition if there is a META label
      return -EIO;
   }
   // Trying to move a file from a partition to the main directory: Nope
   if ( p < 0 || p2 < 0 )
   {
      return -EIO;
   }
   // Moving a file between file systems
   if ( p != p2 )
   {
      // FIXME: It would be possible to do a create call on the target, copy it over, and then do an unlink
      return -EIO; // Can't move between file systems; copy and unlink instead
   }
   path1 = apt_subpath(path1,p);
   path2 = apt_subpath(path2,p);
   int r = (generic_ops.fs_rename)(&partitions[p].atrfs,path1,path2,flags);
   apt_copyback(p);
   return r;
}

/*
 * apt_chmod()
 *
 * For Atari DOS partitions partition_type_details[0] bit 7 is write-protect.
 * Same for MBR FAT partitions.  (Type 00 and 03 respectively)
 */
int apt_chmod(struct atrfs *atrfs,const char *path, mode_t mode)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return 0;
   path = apt_subpath(path,p);
   // Change permissions on a partition
   if ( strcmp(path,"/") == 0 )
   {
      if ( partitions[p].entry->partition_type != 0x00 && partitions[p].entry->partition_type != 0x03 ) return 0; // not supported

      // Use mode & 0200 to clear or set the write protect
      if ( mode & 0200 )
      {
         partitions[p].entry->partition_type_details[0] &= ~0x80; // Not write protected
      }
      else
      {
         partitions[p].entry->partition_type_details[0] |= 0x80;
      }
   }
   int r = (generic_ops.fs_chmod)(&partitions[p].atrfs,path,mode);
   apt_copyback(p);
   return r;
}

int apt_readlink(struct atrfs *atrfs,const char *path, char *buf, size_t size)
{
   (void)atrfs;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   for (int i=0;i<15;++i)
   {
      if ( !mappings[i].start ) continue;
      if ( strcasecmp(path+1,mappings[i].name) == 0 )
      {
         (void)size; // assume it's enough for our symlink, which could be 56 characters if I'm counting right
         strcpy_case(buf,partitions[le16toh(mappings[i].entry->partition_id)-1].name);
         return 0;
      }
   }
   // No other file systems have symbolic links, so nothing more needed
   return -ENOENT; // should not be reached
}

int apt_create(struct atrfs *atrfs,const char *path, mode_t mode)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return -EIO;
   path = apt_subpath(path,p);
   int r = (generic_ops.fs_create)(&partitions[p].atrfs,path,mode);
   apt_copyback(p);
   return r;
}

int apt_truncate(struct atrfs *atrfs,const char *path, off_t size)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return -EIO;
   path = apt_subpath(path,p);
   int r = (generic_ops.fs_truncate)(&partitions[p].atrfs,path,size);
   apt_copyback(p);
   return r;
}

#if (FUSE_USE_VERSION >= 30)
int apt_utimens(struct atrfs *atrfs,const char *path, const struct timespec tv[2])
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return 0;
   path = apt_subpath(path,p);
   int r = (generic_ops.fs_utimens)(&partitions[p].atrfs,path,tv);
   apt_copyback(p);
   return r;
}
#else
int apt_utime(struct atrfs *atrfs,const char *path, struct utimbuf *utimbuf)
{
   (void)atrfs;
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return 0;
   path = apt_subpath(path,p);
   int r = (generic_ops.fs_utime)(&partitions[p].atrfs,path,utimbuf);
   apt_copyback(p);
   return r;
}
#endif

/*
 * apt_statfs()
 *
 * This isn't a regular file system, so statfs doesn't make a lot of sense,
 * but things break without it.
 */
int apt_statfs(struct atrfs *atrfs,const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = 512;
   stfsbuf->f_frsize = 512;
   stfsbuf->f_blocks = atrfs->atrstat.st_size/512;
   stfsbuf->f_bfree = 0; // In theory, we could determine the amount of space free in the file or MBR partition that isn't in any partition
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = num_partitions;
   stfsbuf->f_ffree = 0;
   stfsbuf->f_namemax = 40;
   // This is going to be weird having different values for different paths in the same file system.
   // Fuse does pass in the full path, so it works.
   // Code to try to be useful if possible.
   int p = apt_path_to_partition(path);
   if ( p < 0 ) return 0; // Use above
   path = apt_subpath(path,p);
   return (generic_ops.fs_statfs)(&partitions[p].atrfs,path,stfsbuf);
}

/*
 * apt_fsinfo()
 */
char *apt_fsinfo(struct atrfs *atrfs)
{
   (void)atrfs;
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;
   b+=sprintf(b,"APT disk image\n");
   int mappings_count = 0;
   for (int i=0;i<15;++i)
   {
      if ( mappings[i].start )
      {
         if ( !mappings_count ) b+=sprintf(b,"Drive mappings:\n");
         ++mappings_count;
         b+=sprintf(b," D%d: partition %d   start at sector %u, total sectors %u\n",i+1,le16toh(mappings[i].entry->partition_id),mappings[i].start,mappings[i].sectors);
      }
   }

   b+=sprintf(b,"Partitions: (%d)\n",num_partitions);
   for (int i=0;i<num_partitions;++i)
   {
      if ( !partitions[i].start ) continue; // Deleted
      b+=sprintf(b,"Partition %d:\n",i+1);
      b+=sprintf(b,"  start %d sectors %d\n",partitions[i].start,partitions[i].sectors);
      if ( partitions[i].meta )
         b+=sprintf(b,"  label: %.40s\n",partitions[i].meta->partition_name);
      b+=sprintf(b,"  type: %02x %s\n",partitions[i].entry->partition_type,
                 partitions[i].entry->partition_type == 0x00 ? "Atari DOS" :
                 partitions[i].entry->partition_type == 0x01 ? "Firmware Config" :
                 partitions[i].entry->partition_type == 0x02 ? "Floppy Drawer" :
                 partitions[i].entry->partition_type == 0x03 ? "External FAT" :
                 "Unknown");
      b+=sprintf(b,"  bytes per sector: %d\n",partitions[i].bytes_per_sector);
      if ( partitions[i].bytes_per_sector < 512 )
      {
         b+=sprintf(b,"  %s per 512-byte sector\n",(partitions[i].sectors_per_sector > 1)?"two sectors":"one sector");
         b+=sprintf(b,"  sector interleave: %s\n",(partitions[i].byte_interleave)?"byte":"sector");
      }
      if ( partitions[i].entry->partition_type == 0x02 )
      {
         b+=sprintf(b,"  chunk size (per floppy): %lu sectors\n",partitions[i].chunk_size);
      }
   }

   return buf;
}
