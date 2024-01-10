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
#define VALID_SECTOR(_s) ((_s) < atrfs.atrstat.st_size/512 )
#define SECTORMEM(_s) ((void *)((char *)(atrfs.atrmem)+512*(_s)))

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
   unsigned int start;
   unsigned int sectors;
   int bytes_per_sector;
   int bytes_access;
   struct partition_table_entry *entry;
   struct metadata_leader *meta;
   char *name;
};

/*
 * Function prototypes
 */
int apt_sanity(void);
int apt_getattr(const char *path, struct stat *stbuf);
int apt_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int apt_read(const char *path, char *buf, size_t size, off_t offset);
int apt_write(const char *path, const char *buf, size_t size, off_t offset);
int apt_unlink(const char *path);
int apt_rename(const char *path1, const char *path2, unsigned int flags);
int apt_chmod(const char *path, mode_t mode);
int apt_readlink(const char *path, char *buf, size_t size);
int apt_create(const char *path, mode_t mode);
int apt_truncate(const char *path, off_t size);
int apt_statfs(const char *path, struct statvfs *stfsbuf);
int apt_newfs(void);
char *apt_fsinfo(void);

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
   // .fs_unlink = apt_unlink,
   // .fs_rename = apt_rename,
   .fs_chmod = apt_chmod,
   .fs_readlink = apt_readlink,
   // .fs_create = apt_create,
   // .fs_truncate = apt_truncate,
   .fs_statfs = apt_statfs,
   // .fs_newfs = apt_newfs,
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
off_t find_apt_offset_in_mbr(void)
{
   struct mbr_partition_table *mbr = atrfs.atrmem;
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
int scan_apt_partitions(void *mem,int header_offset,int first)
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
      this->mem = SECTORMEM(this->start);
      this->sectors = le32toh(this->entry->sector_count);
      if ( entry->access_flags & 0x80 ) // Deleted or otherwise reserved
      {
         this->start = this->sectors = 0;
         this->mem = NULL;
         this->name = "";
         continue;
      }
      if ( (this->start+1) * 512 > atrfs.atrstat.st_size )
      {
         fprintf(stderr,"ATP partition says it starts at sector %u which is past the end of the file\n",this->start);
         exit (1);
      }
      if ( (this->start+this->sectors) * 512 > atrfs.atrstat.st_size )
      {
         fprintf(stderr,"ATP partition says it runs through sector %u which is past the end of the file\n",this->start + this->sectors);
         this->sectors = atrfs.atrstat.st_size/512 - this->start;
         fprintf(stderr,"ATP partition adjusted to %u sectors\n",this->sectors);
      }
      this->bytes_per_sector = 64 << (entry->access_flags & 0x03);
      this->bytes_access = (entry->access_flags >> 2) & 0x03;
      this->meta = (void *)((char *)(atrfs.atrmem) + this->start * 512 - 512);
      if ( entry->partition_type == 0x03 ) // meta location is different
      {
         int lba = BYTES3(&(entry->partition_type_details[1]));
         if ( lba && lba < atrfs.atrstat.st_size/512 )
         {
            this->meta = (void *)((char *)(atrfs.atrmem) + lba * 512);
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
         sprintf(name,"D%d%s%.40s",map,this->meta?" ":"",this->meta?this->meta->partition_name:"");
      }
      else
      {
         sprintf(name,"partition %d%s%.40s",num_partitions,this->meta?" ":"",this->meta?this->meta->partition_name:"");
      }
      this->name=strdup(name);
   }
   if ( head->next_sector_in_table_chain && VALID_SECTOR( le32toh(head->next_sector_in_table_chain) ) )
   {
      return scan_apt_partitions((char *)(atrfs.atrmem) + le32toh(head->next_sector_in_table_chain) * 512,head->header_entry_offset,0);
   }
   return 0;
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
int apt_sanity(void)
{
   struct apt_partition_table_header *apt_table;

   apt_offset = find_apt_offset_in_mbr();
   if ( options.debug && apt_offset ) printf("APT partition found in MBR at offset %lu\n",apt_offset);
   apt_table = (void *)&((char *)atrfs.atrmem)[apt_offset];
   if ( apt_table->signature[0] != 'A' || apt_table->signature[1] != 'P' || apt_table->signature[2] != 'T' ) return 1;
   if ( apt_table->current_table_sector_entries < 1 || apt_table->current_table_sector_entries > 32 ) return 1;
   if ( apt_table->boot_drive > 15 ) return 1;
   if ( apt_table->header_entry_offset > 31 ) return 1;
   if ( apt_table->header_entry_prev_offset ) return 1; // must be zero for first entry
   if ( apt_table->prev_sector_in_table_chain ) return 1; // prev must be zero for the first sector
   scan_apt_partitions(apt_table,0,1);
   if ( options.debug ) apt_info(); // DEBUG
   return 0;
}

/*
 * apt_getattr()
 */
int apt_getattr(const char *path, struct stat *stbuf)
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
      sprintf(match,"/%s/.raw",partitions[i].name);
      if ( strcasecmp(path,match) != 0 ) continue;
      stbuf->st_ino = 0x40000+i;
      stbuf->st_size = partitions[i].sectors*512;
      return 0;
   }
   return -ENOENT;
}

/*
 * apt_readdir()
 */
int apt_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)path; // Always "/"
   (void)offset;

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

   for (int i=0;i<num_partitions;++i)
   {
      if ( strcasecmp(path+1,partitions[i].name) != 0 ) continue;
      filler(buf,".raw",FILLER_NULL);
      // FIXME: Create a subdirectory for the contents
      return 0;
   }

   return -ENOENT;
}

/*
 * apt_read()
 */
int apt_read(const char *path, char *buf, size_t size, off_t offset)
{
   for (int i=0;i<num_partitions;++i)
   {
      char match[128];
      sprintf(match,"/%s/.raw",partitions[i].name);
      if ( strcasecmp(path,match) != 0 ) continue;
      // Read from this partition
      // FIXME:
      //   There are partition types that pack smaller sectors in non-linear ways.
      //   This includes some that interleave bytes and some that have unused bytes.
      //   The code here needs to be updated to skip the unused bytes.
      if ( offset >= partitions[i].sectors*512 )
      {
         return 0;
      }
      if ( offset + size > (size_t)partitions[i].sectors * 512 )
      {
         size = partitions[i].sectors*512 - offset;
      }
      memcpy(buf,partitions[i].mem+offset,size);
      return size;
   }
   return -ENOENT; // Should not be reached
}

/*
 * apt_write()
 */
int apt_write(const char *path, const char *buf, size_t size, off_t offset)
{
   for (int i=0;i<num_partitions;++i)
   {
      char match[128];
      sprintf(match,"/%s/.raw",partitions[i].name);
      if ( strcasecmp(path,match) != 0 ) continue;

      if ( offset >= partitions[i].sectors*512 )
      {
         return 0;
      }
      if ( offset + size > (size_t)partitions[i].sectors * 512 )
      {
         size = partitions[i].sectors*512 - offset;
      }
      memcpy(partitions[i].mem+offset,buf,size);
      return size;
   }
   return -ENOENT; // Should not be reached
}

/*
 * apt_unlink()
 *
 * Only allow the special case of unlinking a mapping symlink
 */
int apt_unlink(const char *path);

/*
 * apt_rename()
 *
 * Only allow renaming a partition where the source and dest are the same
 * directory, so the meta label is changed.
 *
 * Only works if there is a meta label.
 */
int apt_rename(const char *path1, const char *path2, unsigned int flags);

/*
 * apt_chmod()
 *
 * For Atari DOS partitions partition_type_details[0] bit 7 is write-protect.
 * Same for MBR FAT partitions.  (Type 00 and 03 respectively)
 */
int apt_chmod(const char *path, mode_t mode)
{
   for (int i=0;i<num_partitions;++i)
   {
      if ( !partitions[i].start ) continue; // It might be a reserved or deleted partition
      if ( partitions[i].entry->partition_type != 0x00 && partitions[i].entry->partition_type != 0x03 ) continue;
      if ( strcasecmp(path+1,partitions[i].name) == 0 )
      {
         // Use mode & 0200 to clear or set the write protect
         if ( mode & 0200 )
         {
            partitions[i].entry->partition_type_details[0] &= ~0x80; // Not write protected
         }
         else
         {
            partitions[i].entry->partition_type_details[0] |= 0x80;
         }
      }
   }
   return 0; // Fake success for anything else
}

/*
 * I don't expect these to have meaning
 */
int apt_create(const char *path, mode_t mode);
int apt_truncate(const char *path, off_t size);
int apt_newfs(void);

/*
 * apt_statfs()
 *
 * This isn't a regular file system, so statfs doesn't make a lot of sense,
 * but things break without it.
 */
int apt_statfs(const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = 512;
   stfsbuf->f_frsize = 512;
   stfsbuf->f_blocks = atrfs.atrstat.st_size/512;
   stfsbuf->f_bfree = 0; // In theory, we could determine the amount of space free in the file or MBR partition that isn't in any partition
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = num_partitions;
   stfsbuf->f_ffree = 0;
   stfsbuf->f_namemax = 40;
   return 0; // FIXME
}

int apt_readlink(const char *path, char *buf, size_t size)
{
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
   return -ENOENT; // should not be reached
}

/*
 * apt_fsinfo()
 */
char *apt_fsinfo(void)
{
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
         b+=sprintf(b,"  %s per 512-byte sector\n",(partitions[i].bytes_access & 0x01)?"two sectors":"one sector");
         b+=sprintf(b,"  sector interleave: %s\n",(partitions[i].bytes_access & 0x01)?"sector":"byte");
      }
      if ( partitions[i].entry->partition_type == 0x02 )
      {
         b+=sprintf(b,"  chunk size (per floppy): %d sectors\n",BYTES2(partitions[i].entry->partition_type_details));
      }
   }

   return buf;
}
