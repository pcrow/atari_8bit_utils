/*
 * dosxe.c
 *
 * Functions for accessing Atari DOS XE file systems.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 *
 * Notes:
 *
 * References for the DOS XE structure:
 *
 * Page 67:
 *  https://atarionline.pl/biblioteka/materialy_ksiazkowe/Atari%20DOS%20XE%20Owner's%20manual.pdf
 *
 * https://forums.atariage.com/topic/354568-dos-xe-technical-details-sector-1-vtoc-etc/
 *
 * Percom block: ("Option Table Table 2"):
 * https://www.atarimax.com/freenet/freenet_material/5.8-BitComputersSupportArea/7.TechnicalResourceCenter/showarticle.php?75
 *
 * DOS XE treats all disks as having 256-byte sectors, using pairs on
 * 128-byte disks.  With everything memory mapped here, that's easy.  While
 * DOS XE often refers to these as "sectors," I use the term "cluster" in
 * the code for clarity.
 *
 * With single-density, clusters start on even numbers, so sector 720 can't
 * be used.  My sample image seems to only use through cluster 352, which
 * would be through sector 705.
 *
 * DOS XE has a signature at the end of each cluster which is an
 * extended version of the check with file numbers at the end of DOS 2
 * data sectors, but it applies to directories and other cluster types, too.
 *
 * On a single-density disk:
 *   physical sectors 1-3: boot sectors
 *   physical sectors 4-7: blank and unused
 *   physical sectors 8-9: cluster 4, VTOC
 *   physical sectors 10-11: cluster 5, main directory
 *
 * On other file systems, it's easy to identify a file or directory with a
 * single unique number.  For DOS 2/MyDOS, the starting sector number works
 * great.  For Sparta DOS, there's the sector number of the first map
 * sector.  But here, you need the directory entry that points to them, as
 * that's where you find the list of sector maps.  So you need the cluster
 * number of the start of the directory and the entry number within that
 * directory.  So for inodes, use cccceeee for the cluster number and the
 * entry number within that directory.  We could use the cluster number
 * where the entry is found, so the entry number would be 0-4, but that's
 * probably less useful.  In the code, we'll probably just be passing
 * pointers to the entry around.
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
#define CLUSTER(n) (atrfs.sectorsize==128?SECTOR((n)*2):SECTOR(n)) // 256-byte logical sector
#define DATE_TO_YEAR(n)      (((n)[0])>>1)
#define DATE_TO_4YEAR(n)     (DATE_TO_YEAR(n)+(DATE_TO_YEAR(n)<87?2000:1900))
#define DATE_TO_MONTH(n)     ( ((((n)[0])&0x01)<<3) | (((n)[1])>>5) )
#define DATE_TO_DAY(n)       (((n)[1])&0x1f)
#define DATE_PRINT_FORMAT    "%d/%d/%d"
#define DATE_PRINT_FIELDS(n) DATE_TO_MONTH(n),DATE_TO_DAY(n),DATE_TO_4YEAR(n)

/*
 * File System Structures
 */
struct sector1_dosxe {
   // Initial fields used by the OS to boot
   unsigned char pad_zero; // Usually 00, could be 0x53 for 'S' for DosxeDOS
   unsigned char boot_sectors; // Usually 3, 1 if 512-byte sectors
   unsigned char boot_addr[2]; // Where to load the boot sectors in memory
   unsigned char dos_ini[2]; // JSR here at the end
   unsigned char jmp; // Normally 0x4C, but could be start of code (MyDOS/DOS 2)
   unsigned char exec_addr[2]; // execute address after boot sectors loaded ([0] is 8)
   unsigned char open_files; // default 3
   unsigned char active_drives; // bitmap like DOS 2; default 3
   unsigned char pad_zero_0b; // 00 at offset 0x0b
   unsigned char buffer_base_addr[2]; // 0x1484 or something like that
   unsigned char date_code[2]; // When disk was formatted or DOS written
   // Next 0x20 describe the disk/drive
   unsigned char disk_type[6]; // ASCII padded with zeros; e.g. AT810
   unsigned char unknown_16; // 0x01 ?
   unsigned char unknown_17; // 0x01 ?
   unsigned char total_clusters[2]; // plus 1
   unsigned char max_free_clusters[2];
   unsigned char first_vtoc_byte; // 0x07 ?
   unsigned char main_directory; // 0x05 if only one VTOC sector
   unsigned char sio_routine[2]; // df32 ?
   unsigned char unknwon_20[2]; // 5d 0d
   unsigned char sio_read_cmd; // 'R' or 'R'|0x80 for fast mode
   unsigned char sio_write_cmd; // 'W' or 'W'|0x80 for fast mode, maybe 'P'
   unsigned char pokey_baud[2]; // AUDF3/4 for baud rate, 00 00 standard, 28 10 fast
   unsigned char sio_format_cmd; // '!' possibly +128 for turbo interleave or 00 for ramdisk
   unsigned char unknwon_27; // 0x28 ? Format timeout value?
   // First 8 bytes of PERCOM disk parameters
   unsigned char tracks;    // 0x28 for 40 tracks (810 value)
   unsigned char step_rate; // 0x00 for 30 msecs (810 value)
   unsigned char sectors_per_track[2]; // 00 12 for 18 (reverse byte order)
   unsigned char sides;     // 0x00 for 1, 0x01 for 2
   unsigned char density;   // 0x00 for single, 0x04 for double
   unsigned char sector_size[2]; // 00 80 for 128 (reverse byte order)
   // Code follows at 0x30
};

struct cluster_label {
   unsigned char file_id_number[2]; // 0000 for root directory
   unsigned char volume_number[2];
   unsigned char sequence[2];
   // if ( sequence[1] == 0xff ) it's a directory and only use sequence[0]
   // if ( sequence[1] & 0x80 ) it's a file map cluster and only use sequence[0]
   // else sequence is BYTES2(sequence)
};

/*
 * Directory entries are 49 bytes!  That's four per cluster, plus a label
 *
 * Date: 7 bits for year (00-99), 4 bits for month (1-12), 5 bits day (1-31)
 * Time: Not saved
 */
struct dosxe_dir_entry {
   unsigned char status;
   unsigned char file_name[8];
   unsigned char file_ext[3];
   unsigned char file_clusters[2];
   unsigned char bytes_in_last_cluster; // 00 and ignored for a directory
   unsigned char file_sequence_number[2];
   unsigned char volume_number[2];
   unsigned char file_map_blocks[12][2]; // 0000 if unassigned, of course
   unsigned char creation_date[2];
   unsigned char modification_date[2];
   unsigned char reserved[2];
};

enum dosxe_dir_status {
   FLAGS_LOCKED  = 0x02,
   FLAGS_IN_USE  = 0x40,
   FLAGS_DELETED = 0x80,
   FLAGS_DIR     = 0x01,
   FLAGS_OPEN    = 0x80|0x40|0x02,
};

struct dosxe_dir_cluster {
   struct dosxe_dir_entry entries[4];
   unsigned char          unused[3];
   unsigned char          next[2]; // Next cluster in directory or zero
   struct cluster_label    label;
};
struct dosxe_vtoc_cluster {
   // First 10 bytes of cluster 4 are disk status
   unsigned char unknown_0[2]; // Always: 01 01 (possibly version or magic)
   unsigned char total_clusters[2];
   unsigned char free_clusters[2];
   unsigned char file_sequence_number[2]; // Number of files and directories written
   unsigned char volume_number[2]; // random; bytes not equal
   // bitmaps continue for remaining bytes and clusters if needed
   // No label on VTOC clusters, so just treat it as one big array
   unsigned char bitmap[0x10000 / 8]; // Probably less for most images
};
struct dosxe_file_map_cluster {
   unsigned char data_block[125][2]; // cluster numbers of data clusters
   struct cluster_label label;
};
struct dosxe_data_cluster {
   unsigned char       data[250];
   struct cluster_label label;
};

/*
 * Function prototypes
 */
int dosxe_sanity(void);
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
 * Global variables
 */
const struct fs_ops dosxe_ops = {
   .name = "Atari DOS XE",
   .fs_sanity = dosxe_sanity,
   .fs_getattr = dosxe_getattr,
   .fs_readdir = dosxe_readdir,
   .fs_read = dosxe_read,
   // .fs_write = dosxe_write,
   // .fs_mkdir = dosxe_mkdir,
   // .fs_rmdir = dosxe_rmdir,
   // .fs_unlink = dosxe_unlink,
   // .fs_rename = dosxe_rename,
   // .fs_chmod = dosxe_chmod,
   // .fs_create = dosxe_create,
   // .fs_truncate = dosxe_truncate,
   // .fs_utimens = dosxe_utimens,
   // .fs_statfs = dosxe_statfs,
   // .fs_newfs = dosxe_newfs,
   .fs_fsinfo = dosxe_fsinfo,
};

/*
 * Functions
 */

/*
 * dosxe_bitmap_status()
 *
 * Return the bit from the bitmap for the cluster (0 if allocated)
 */
int dosxe_bitmap_status(int cluster)
{
   struct dosxe_vtoc_cluster *vtoc = CLUSTER(4);
   cluster-=4; // DOS XE doesn't have bits for clusters 0-3
   unsigned char mask = 1<<(7-(cluster & 0x07));
   return (vtoc->bitmap[cluster/8] & mask) != 0; // 0 or 1
}

/*
 * dosxe_sanity()
 *
 * Return 0 if this is a valid DOS XE file system
 */
int dosxe_sanity(void)
{
   if ( atrfs.sectorsize != 128 && atrfs.sectorsize != 256 ) return 1; // Must be SD or DD

   struct sector1_dosxe *sec1 = SECTOR(1);
   if ( sec1->pad_zero != 'X' ) return 1; // Flag for DOS XE
   if ( sec1->boot_sectors != 3 ) return 1; // Must have 3 boot sectors, how original
   if ( BYTES2(sec1->exec_addr) != 0x0730 ) return 1;
   if ( (sec1->sio_read_cmd & 0x7f) != 'R' ) return 1;
   // if ( (sec1->sio_write_cmd & 0x7f) != 'W' ) return 1; // Could also be 'P'
   if ( (sec1->sio_format_cmd & 0x7f) != '!' ) {;} // FIXME: is it different for 1050 ED?
   if ( BYTES2R(sec1->sector_size) != atrfs.sectorsize ) return 1;

   struct dosxe_vtoc_cluster *vtoc = CLUSTER(4);
   if ( BYTES2(vtoc->total_clusters) != BYTES2(sec1->total_clusters) ) return 1;

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

/*
 * dosxe_fsinfo()
 */
char *dosxe_fsinfo(void)
{
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;

   struct sector1_dosxe *sec1 = SECTOR(1);

   b+=sprintf(b,"Disk format date:    "DATE_PRINT_FORMAT"\n",DATE_PRINT_FIELDS(sec1->date_code));
   b+=sprintf(b,"Disk drive type:     %.6s\n",sec1->disk_type);
   b+=sprintf(b,"Total blocks:        %d\n",BYTES2(sec1->total_clusters));
   b+=sprintf(b,"Initial free blocks: %d\n",BYTES2(sec1->max_free_clusters));
   b+=sprintf(b,"Main directory:      %d\n",sec1->main_directory);
   b+=sprintf(b,"Tracks:              %d\n",sec1->tracks);
   b+=sprintf(b,"Clusters per track:  %d\n",BYTES2R(sec1->sectors_per_track));
   b+=sprintf(b,"Sides                %d\n",sec1->sides+1);
   b+=sprintf(b,"Sector size:         %d\n",BYTES2R(sec1->sector_size));
   b+=sprintf(b,"Density:             ");
   switch(sec1->density)
   {
      case 0x00: b+=sprintf(b,"Single\n"); break;
      case 0x04: b+=sprintf(b,"Double\n"); break;
      default:
         b+=sprintf(b,"unknown (%02x)\n",sec1->density);
         break;
   }
   b+=sprintf(b,"Step Rate:           ");
   switch(sec1->step_rate)
   {
      case 0x00: b+=sprintf(b,"30ms\n"); break;
      case 0x01: b+=sprintf(b,"20ms\n"); break;
      case 0x02: b+=sprintf(b,"12ms\n"); break;
      case 0x03: b+=sprintf(b,"6ms\n"); break;
      default:
         b+=sprintf(b,"unknown (%02x)\n",sec1->step_rate);
         break;
   }
   
   struct dosxe_vtoc_cluster *vtoc = CLUSTER(4);

   b+=sprintf(b,"\n");
   b+=sprintf(b,"VTOC Total Clusters: %d\n",BYTES2(vtoc->total_clusters));
   b+=sprintf(b,"VTOC Free Clusters:  %d\n",BYTES2(vtoc->free_clusters));
   b+=sprintf(b,"File/Dir count:      %d\n",BYTES2(vtoc->file_sequence_number));
   b+=sprintf(b,"Volume rnd number:   %04x\n",BYTES2(vtoc->volume_number));

   // Free bit map; generally not useful and may be too much data
#if 1
   b+=sprintf(b,"\nCluster Allocation Map:\n");
   int prev=2; // Never matches 0 or 1
   int start= -1;
   int new=0; // Flag to finish last line after loop
   int maxcluster = BYTES2(vtoc->total_clusters)+2;
   for ( int i=4;i<= maxcluster;++i )
   {
      int r = dosxe_bitmap_status(i);
      new=0;

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
      new=1;
      prev=r;
   }
   if ( !new )
      b+=sprintf(b," -- %d",maxcluster);
   b+=sprintf(b,"\n");
#endif
   return buf;
}

/*
 * Temporary
 */
int dosxe_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
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
      int maxcluster = atrfs.sectors;
      if ( atrfs.sectorsize == 128 )
      {
         maxcluster--;
         maxcluster /= 2;
      }
      if ( sec > 3 && sec <= maxcluster )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = 256;
         stbuf->st_ino = sec;
         if ( atrfs.sectorsize == 128 ) stbuf->st_ino = sec*2;
         return 0; // Good, can read this sector
      }
   }
   stbuf->st_ino = 0x10001;
   stbuf->st_size = 0;
   return 0; // Whatever, don't really care
}

int dosxe_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
   (void)path; // Always "/"
   (void)offset;
   (void)fi;
   (void)flags;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   filler(buf, "DOS_XE_DISK_IMAGE", NULL, 0, 0);
   filler(buf, "NOT_YET_SUPPORTED", NULL, 0, 0);
   filler(buf, "USE_.cluster#_for_raw_nonzero_clusters", NULL, 0, 0);

#if 1 // This will work even if we skip the entries
   // Create .cluster4 ... .cluster359 as appropriate
   unsigned char zero[256];
   memset(zero,0,256);
   char name[32];
   int digits = sprintf(name,"%d",atrfs.sectors);
   int maxcluster = atrfs.sectors;
   if ( atrfs.sectorsize == 128 )
   {
      maxcluster--;
      maxcluster /= 2;
   }
   for (int sec=4; sec<=maxcluster; ++sec)
   {
      unsigned char *s = CLUSTER(sec);
      if ( memcmp(s,zero,256) == 0 ) continue; // Skip empty sectors
      sprintf(name,".cluster%0*d",digits,sec);
      filler(buf,name,NULL,0,0);
   }
#endif
   return 0;
}

int dosxe_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;

   // Magic /.cluster### files
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = atoi(path+sizeof("/.cluster")-1);
      int maxcluster = atrfs.sectors;
      if ( atrfs.sectorsize == 128 )
      {
         maxcluster--;
         maxcluster /= 2;
      }
      if ( sec <= 3 || sec > maxcluster ) return -ENOENT;

      int bytes = 256;
      if (offset >= bytes ) return -EOF;
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
