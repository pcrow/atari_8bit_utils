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
#define CLUSTER_TO_SEC(n) (atrfs.sectorsize==128?(n)*2:(n))
#define CLUSTER(n) SECTOR(atrfs.sectorsize==128?(n)*2:(n)) // 256-byte logical sector
// Note: It may format a disk to not use all available clusters at the end
// It seems to ignore several clusters at the end of AT810 images, so MAX_PHYSCIAL_CLUSTERS > MAX_CLUSTERS
#define MAX_PHYSICAL_CLUSTER (atrfs.sectorsize==128?(atrfs.sectors-1)/2:atrfs.sectors)
#define MAX_CLUSTER (BYTES2(((struct dosxe_vtoc_cluster *)CLUSTER(4))->total_clusters)-1)
#define VTOC_CLUSTER 4
#define VTOC_CLUSTERS        (((MAX_PHYSICAL_CLUSTER+7) / 8 + 10 + 255)/256)
#define DATE_TO_YEAR(n)      (((n)[0])>>1)
#define DATE_TO_4YEAR(n)     (DATE_TO_YEAR(n)+(DATE_TO_YEAR(n)<87?2000:1900))
#define DATE_TO_MONTH(n)     ( ((((n)[0])&0x01)<<3) | (((n)[1])>>5) )
#define DATE_TO_DAY(n)       (((n)[1])&0x1f)
#define DATE_PRINT_FORMAT    "%d/%d/%d"
#define DATE_PRINT_FIELDS(n) DATE_TO_MONTH(n),DATE_TO_DAY(n),DATE_TO_4YEAR(n)
#define ROOT_DIR_CLUSTER     (((struct sector1_dosxe *)SECTOR(1))->main_directory)
#define ROOT_DIR             CLUSTER(ROOT_DIR_CLUSTER)

/*
 * File System Structures
 */
struct sector1_dosxe {
   // Initial fields used by the OS to boot
   unsigned char pad_zero; // 'X' for DOS XE
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
   unsigned char first_vtoc_byte; // 0x07 if one VTOC cluster >>1 for each additional
   unsigned char main_directory; // 0x05 if only one VTOC cluster
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
   char file_name[8];
   char file_ext[3];
   unsigned char file_clusters[2]; // Does not include last cluster
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
   struct dosxe_dir_entry entries[5];
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

enum drivetype {
   DRIVE_AT810,
   DRIVE_AT1050,
   DRIVE_XF551,
   DRIVE_SSDD,
   DRIVE_MAX_VAL // for array sizes
   // FIXME: Do we need anything for hacked large images?
};

/*
 * Function prototypes
 */
int dosxe_sanity(void);
int dosxe_getattr(const char *path, struct stat *stbuf);
int dosxe_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int dosxe_read(const char *path, char *buf, size_t size, off_t offset);
int dosxe_write(const char *path, const char *buf, size_t size, off_t offset);
int dosxe_mkdir(const char *path,mode_t mode);
int dosxe_rmdir(const char *path);
int dosxe_unlink(const char *path);
int dosxe_rename(const char *path1, const char *path2, unsigned int flags);
int dosxe_chmod(const char *path, mode_t mode);
int dosxe_create(const char *path, mode_t mode);
int dosxe_truncate(const char *path, off_t size);
int dosxe_utimens(const char *path, const struct timespec tv[2]);
int dosxe_statfs(const char *path, struct statvfs *stfsbuf);
int dosxe_newfs(void);
char *dosxe_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops dosxe_ops = {
   .name = "Atari DOS XE",
   .fstype = "dosxe",
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
   .fs_statfs = dosxe_statfs,
   .fs_newfs = dosxe_newfs,
   .fs_fsinfo = dosxe_fsinfo,
};

/*
 * Boot Sectors
 */
static const char bootsectors[DRIVE_MAX_VAL][3*128] = {
   [DRIVE_AT810] = {
      0x58, 0x03, 0x00, 0x07, 0xca, 0x0c, 0x4c, 0x30, 0x07, 0x03, 0x03, 0x00, 0x84, 0x14, 0xb1, 0x34, 
      0x41, 0x54, 0x38, 0x31, 0x30, 0x00, 0x01, 0x01, 0x61, 0x01, 0x5b, 0x01, 0x07, 0x05, 0x32, 0xdf, // AT810...a.[...2.
      0x5d, 0x0d, 0x52, 0x57, 0x00, 0x00, 0x21, 0x28, 0x28, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x80, 
      0xad, 0x1d, 0x07, 0xa0, 0x00, 0x8c, 0x04, 0x03, 0xa2, 0x09, 0x8e, 0x05, 0x03, 0x20, 0x30, 0x08, 
      0xa2, 0x00, 0x86, 0x44, 0xbd, 0x00, 0x09, 0x30, 0x52, 0xf0, 0x65, 0xa0, 0x0b, 0xbd, 0x01, 0x09, 
      0xd9, 0xb1, 0x07, 0xd0, 0x46, 0xe8, 0x88, 0xd0, 0xf4, 0xbd, 0x08, 0x09, 0xbc, 0x09, 0x09, 0x20, 
      0x30, 0x08, 0xa2, 0x16, 0x8e, 0x05, 0x03, 0xa2, 0x00, 0xad, 0x17, 0x07, 0x85, 0x45, 0xbd, 0x01, 
      0x0a, 0xa8, 0x1d, 0x00, 0x0a, 0xf0, 0x46, 0xbd, 0x00, 0x0a, 0xe8, 0xe8, 0x86, 0x44, 0x20, 0x30, 
      0x08, 0x38, 0xad, 0x04, 0x03, 0xe9, 0x06, 0x8d, 0x04, 0x03, 0xb0, 0x03, 0xce, 0x05, 0x03, 0xa6, 
      0x44, 0xc6, 0x45, 0xf0, 0xd4, 0x20, 0x42, 0x08, 0x4c, 0x81, 0x07, 0xa5, 0x44, 0x18, 0x69, 0x31, 
      0xaa, 0x69, 0x31, 0x90, 0x9d, 0xad, 0xf8, 0x09, 0xac, 0xf9, 0x09, 0xd0, 0x8b, 0xaa, 0xd0, 0x88, 
      0x38, 0x60, 0x53, 0x59, 0x53, 0x20, 0x20, 0x20, 0x45, 0x58, 0x53, 0x4f, 0x44, 0xa8, 0x8c, 0x2f, 
      0x02, 0xa5, 0x14, 0xc5, 0x14, 0xf0, 0xfc, 0x8c, 0x0e, 0xd4, 0x78, 0xad, 0x01, 0xd3, 0x48, 0x29, 
      0xfe, 0x8d, 0x01, 0xd3, 0x84, 0x44, 0xa9, 0x16, 0x85, 0x45, 0xb1, 0x44, 0x85, 0x46, 0xc8, 0xb1, 
      0x44, 0xf0, 0x3b, 0x85, 0x47, 0xc8, 0xc9, 0xff, 0xd0, 0x04, 0xc5, 0x46, 0xf0, 0xec, 0xb1, 0x44, 
      0xaa, 0xc8, 0xb1, 0x44, 0x85, 0x49, 0x38, 0x98, 0x65, 0x44, 0x85, 0x44, 0x90, 0x02, 0xe6, 0x45, 
      0xa0, 0x00, 0xb1, 0x44, 0x91, 0x46, 0xe6, 0x44, 0xd0, 0x02, 0xe6, 0x45, 0xe4, 0x46, 0xd0, 0x06, 
      0xa5, 0x47, 0xc5, 0x49, 0xf0, 0xc4, 0xe6, 0x46, 0xd0, 0xe8, 0xe6, 0x47, 0xd0, 0xe4, 0x68, 0x8d, 
      0x01, 0xd3, 0xa9, 0xc0, 0x8d, 0x0e, 0xd4, 0x58, 0xa9, 0x22, 0x8d, 0x2f, 0x02, 0x6c, 0xe0, 0x02, 
      0x8c, 0x0b, 0x03, 0xac, 0x2e, 0x07, 0x8c, 0x09, 0x03, 0xd0, 0x04, 0x0a, 0x2e, 0x0b, 0x03, 0x8d, 
      0x0a, 0x03, 0xad, 0x2f, 0x07, 0x8d, 0x08, 0x03, 0x8d, 0x0b, 0x07, 0xa9, 0x40, 0x8d, 0x03, 0x03, 
      0x20, 0x59, 0xe4, 0x30, 0x1a, 0xee, 0x0a, 0x03, 0xad, 0x2f, 0x07, 0xf0, 0x09, 0x18, 0x6d, 0x04, 
      0x03, 0x8d, 0x04, 0x03, 0x90, 0x03, 0xee, 0x05, 0x03, 0x0e, 0x0b, 0x07, 0xb0, 0xdd, 0x60, 0x68, 
      0x68, 0x38, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   },
   [DRIVE_AT1050] = {
      0x58, 0x03, 0x00, 0x07, 0xca, 0x0c, 0x4c, 0x30, 0x07, 0x03, 0x03, 0x00, 0x84, 0x14, 0xb1, 0x34, 
      0x41, 0x54, 0x31, 0x30, 0x35, 0x30, 0x01, 0x01, 0x01, 0x02, 0xfb, 0x01, 0x07, 0x05, 0x32, 0xdf, // AT1050........2.
      0x5d, 0x0d, 0x52, 0x57, 0x00, 0x00, 0x22, 0x32, 0x28, 0x00, 0x00, 0x1a, 0x00, 0x04, 0x00, 0x80, 
      0xad, 0x1d, 0x07, 0xa0, 0x00, 0x8c, 0x04, 0x03, 0xa2, 0x09, 0x8e, 0x05, 0x03, 0x20, 0x30, 0x08, 
      0xa2, 0x00, 0x86, 0x44, 0xbd, 0x00, 0x09, 0x30, 0x52, 0xf0, 0x65, 0xa0, 0x0b, 0xbd, 0x01, 0x09, 
      0xd9, 0xb1, 0x07, 0xd0, 0x46, 0xe8, 0x88, 0xd0, 0xf4, 0xbd, 0x08, 0x09, 0xbc, 0x09, 0x09, 0x20, 
      0x30, 0x08, 0xa2, 0x16, 0x8e, 0x05, 0x03, 0xa2, 0x00, 0xad, 0x17, 0x07, 0x85, 0x45, 0xbd, 0x01, 
      0x0a, 0xa8, 0x1d, 0x00, 0x0a, 0xf0, 0x46, 0xbd, 0x00, 0x0a, 0xe8, 0xe8, 0x86, 0x44, 0x20, 0x30, 
      0x08, 0x38, 0xad, 0x04, 0x03, 0xe9, 0x06, 0x8d, 0x04, 0x03, 0xb0, 0x03, 0xce, 0x05, 0x03, 0xa6, 
      0x44, 0xc6, 0x45, 0xf0, 0xd4, 0x20, 0x42, 0x08, 0x4c, 0x81, 0x07, 0xa5, 0x44, 0x18, 0x69, 0x31, 
      0xaa, 0x69, 0x31, 0x90, 0x9d, 0xad, 0xf8, 0x09, 0xac, 0xf9, 0x09, 0xd0, 0x8b, 0xaa, 0xd0, 0x88, 
      0x38, 0x60, 0x53, 0x59, 0x53, 0x20, 0x20, 0x20, 0x45, 0x58, 0x53, 0x4f, 0x44, 0xa8, 0x8c, 0x2f, 
      0x02, 0xa5, 0x14, 0xc5, 0x14, 0xf0, 0xfc, 0x8c, 0x0e, 0xd4, 0x78, 0xad, 0x01, 0xd3, 0x48, 0x29, 
      0xfe, 0x8d, 0x01, 0xd3, 0x84, 0x44, 0xa9, 0x16, 0x85, 0x45, 0xb1, 0x44, 0x85, 0x46, 0xc8, 0xb1, 
      0x44, 0xf0, 0x3b, 0x85, 0x47, 0xc8, 0xc9, 0xff, 0xd0, 0x04, 0xc5, 0x46, 0xf0, 0xec, 0xb1, 0x44, 
      0xaa, 0xc8, 0xb1, 0x44, 0x85, 0x49, 0x38, 0x98, 0x65, 0x44, 0x85, 0x44, 0x90, 0x02, 0xe6, 0x45, 
      0xa0, 0x00, 0xb1, 0x44, 0x91, 0x46, 0xe6, 0x44, 0xd0, 0x02, 0xe6, 0x45, 0xe4, 0x46, 0xd0, 0x06, 
      0xa5, 0x47, 0xc5, 0x49, 0xf0, 0xc4, 0xe6, 0x46, 0xd0, 0xe8, 0xe6, 0x47, 0xd0, 0xe4, 0x68, 0x8d, 
      0x01, 0xd3, 0xa9, 0xc0, 0x8d, 0x0e, 0xd4, 0x58, 0xa9, 0x22, 0x8d, 0x2f, 0x02, 0x6c, 0xe0, 0x02, 
      0x8c, 0x0b, 0x03, 0xac, 0x2e, 0x07, 0x8c, 0x09, 0x03, 0xd0, 0x04, 0x0a, 0x2e, 0x0b, 0x03, 0x8d, 
      0x0a, 0x03, 0xad, 0x2f, 0x07, 0x8d, 0x08, 0x03, 0x8d, 0x0b, 0x07, 0xa9, 0x40, 0x8d, 0x03, 0x03, 
      0x20, 0x59, 0xe4, 0x30, 0x1a, 0xee, 0x0a, 0x03, 0xad, 0x2f, 0x07, 0xf0, 0x09, 0x18, 0x6d, 0x04, 
      0x03, 0x8d, 0x04, 0x03, 0x90, 0x03, 0xee, 0x05, 0x03, 0x0e, 0x0b, 0x07, 0xb0, 0xdd, 0x60, 0x68, 
      0x68, 0x38, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
   },
   [DRIVE_SSDD] = {
      0x58, 0x03, 0x00, 0x07, 0xca, 0x0c, 0x4c, 0x30, 0x07, 0x03, 0x03, 0x00, 0x84, 0x14, 0xb1, 0x34, 
      0x53, 0x53, 0x44, 0x44, 0x00, 0x00, 0x01, 0x01, 0xd1, 0x02, 0xcb, 0x02, 0x07, 0x05, 0x5d, 0x0d, // SSDD..........].
      0x5d, 0x0d, 0x52, 0x57, 0x00, 0x00, 0x21, 0x28, 0x28, 0x00, 0x00, 0x12, 0x00, 0x04, 0x01, 0x00, 
      0xad, 0x1d, 0x07, 0xa0, 0x00, 0x8c, 0x04, 0x03, 0xa2, 0x09, 0x8e, 0x05, 0x03, 0x20, 0x30, 0x08, 
      0xa2, 0x00, 0x86, 0x44, 0xbd, 0x00, 0x09, 0x30, 0x52, 0xf0, 0x65, 0xa0, 0x0b, 0xbd, 0x01, 0x09, 
      0xd9, 0xb1, 0x07, 0xd0, 0x46, 0xe8, 0x88, 0xd0, 0xf4, 0xbd, 0x08, 0x09, 0xbc, 0x09, 0x09, 0x20, 
      0x30, 0x08, 0xa2, 0x16, 0x8e, 0x05, 0x03, 0xa2, 0x00, 0xad, 0x17, 0x07, 0x85, 0x45, 0xbd, 0x01, 
      0x0a, 0xa8, 0x1d, 0x00, 0x0a, 0xf0, 0x46, 0xbd, 0x00, 0x0a, 0xe8, 0xe8, 0x86, 0x44, 0x20, 0x30, 
      0x08, 0x38, 0xad, 0x04, 0x03, 0xe9, 0x06, 0x8d, 0x04, 0x03, 0xb0, 0x03, 0xce, 0x05, 0x03, 0xa6, 
      0x44, 0xc6, 0x45, 0xf0, 0xd4, 0x20, 0x42, 0x08, 0x4c, 0x81, 0x07, 0xa5, 0x44, 0x18, 0x69, 0x31, 
      0xaa, 0x69, 0x31, 0x90, 0x9d, 0xad, 0xf8, 0x09, 0xac, 0xf9, 0x09, 0xd0, 0x8b, 0xaa, 0xd0, 0x88, 
      0x38, 0x60, 0x53, 0x59, 0x53, 0x20, 0x20, 0x20, 0x45, 0x58, 0x53, 0x4f, 0x44, 0xa8, 0x8c, 0x2f, 
      0x02, 0xa5, 0x14, 0xc5, 0x14, 0xf0, 0xfc, 0x8c, 0x0e, 0xd4, 0x78, 0xad, 0x01, 0xd3, 0x48, 0x29, 
      0xfe, 0x8d, 0x01, 0xd3, 0x84, 0x44, 0xa9, 0x16, 0x85, 0x45, 0xb1, 0x44, 0x85, 0x46, 0xc8, 0xb1, 
      0x44, 0xf0, 0x3b, 0x85, 0x47, 0xc8, 0xc9, 0xff, 0xd0, 0x04, 0xc5, 0x46, 0xf0, 0xec, 0xb1, 0x44, 
      0xaa, 0xc8, 0xb1, 0x44, 0x85, 0x49, 0x38, 0x98, 0x65, 0x44, 0x85, 0x44, 0x90, 0x02, 0xe6, 0x45, 
      0xa0, 0x00, 0xb1, 0x44, 0x91, 0x46, 0xe6, 0x44, 0xd0, 0x02, 0xe6, 0x45, 0xe4, 0x46, 0xd0, 0x06, 
      0xa5, 0x47, 0xc5, 0x49, 0xf0, 0xc4, 0xe6, 0x46, 0xd0, 0xe8, 0xe6, 0x47, 0xd0, 0xe4, 0x68, 0x8d, 
      0x01, 0xd3, 0xa9, 0xc0, 0x8d, 0x0e, 0xd4, 0x58, 0xa9, 0x22, 0x8d, 0x2f, 0x02, 0x6c, 0xe0, 0x02, 
      0x8c, 0x0b, 0x03, 0xac, 0x2e, 0x07, 0x8c, 0x09, 0x03, 0xd0, 0x04, 0x0a, 0x2e, 0x0b, 0x03, 0x8d, 
      0x0a, 0x03, 0xad, 0x2f, 0x07, 0x8d, 0x08, 0x03, 0x8d, 0x0b, 0x07, 0xa9, 0x40, 0x8d, 0x03, 0x03, 
      0x20, 0x59, 0xe4, 0x30, 0x1a, 0xee, 0x0a, 0x03, 0xad, 0x2f, 0x07, 0xf0, 0x09, 0x18, 0x6d, 0x04, 
      0x03, 0x8d, 0x04, 0x03, 0x90, 0x03, 0xee, 0x05, 0x03, 0x0e, 0x0b, 0x07, 0xb0, 0xdd, 0x60, 0x68, 
      0x68, 0x38, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
   },
   [DRIVE_XF551] = {
      0x58, 0x03, 0x00, 0x07, 0xca, 0x0c, 0x4c, 0x30, 0x07, 0x03, 0x03, 0x00, 0x84, 0x14, 0xb1, 0x34, 
      0x58, 0x46, 0x35, 0x35, 0x31, 0x00, 0x01, 0x01, 0xa1, 0x05, 0x9b, 0x05, 0x07, 0x05, 0xe5, 0x0f, // XF551...........
      0x5d, 0x0d, 0xd2, 0xd7, 0x28, 0x10, 0xa1, 0x64, 0x28, 0x00, 0x00, 0x12, 0x01, 0x04, 0x01, 0x00, 
      0xad, 0x1d, 0x07, 0xa0, 0x00, 0x8c, 0x04, 0x03, 0xa2, 0x09, 0x8e, 0x05, 0x03, 0x20, 0x30, 0x08, 
      0xa2, 0x00, 0x86, 0x44, 0xbd, 0x00, 0x09, 0x30, 0x52, 0xf0, 0x65, 0xa0, 0x0b, 0xbd, 0x01, 0x09, 
      0xd9, 0xb1, 0x07, 0xd0, 0x46, 0xe8, 0x88, 0xd0, 0xf4, 0xbd, 0x08, 0x09, 0xbc, 0x09, 0x09, 0x20, 
      0x30, 0x08, 0xa2, 0x16, 0x8e, 0x05, 0x03, 0xa2, 0x00, 0xad, 0x17, 0x07, 0x85, 0x45, 0xbd, 0x01, 
      0x0a, 0xa8, 0x1d, 0x00, 0x0a, 0xf0, 0x46, 0xbd, 0x00, 0x0a, 0xe8, 0xe8, 0x86, 0x44, 0x20, 0x30, 
      0x08, 0x38, 0xad, 0x04, 0x03, 0xe9, 0x06, 0x8d, 0x04, 0x03, 0xb0, 0x03, 0xce, 0x05, 0x03, 0xa6, 
      0x44, 0xc6, 0x45, 0xf0, 0xd4, 0x20, 0x42, 0x08, 0x4c, 0x81, 0x07, 0xa5, 0x44, 0x18, 0x69, 0x31, 
      0xaa, 0x69, 0x31, 0x90, 0x9d, 0xad, 0xf8, 0x09, 0xac, 0xf9, 0x09, 0xd0, 0x8b, 0xaa, 0xd0, 0x88, 
      0x38, 0x60, 0x53, 0x59, 0x53, 0x20, 0x20, 0x20, 0x45, 0x58, 0x53, 0x4f, 0x44, 0xa8, 0x8c, 0x2f, 
      0x02, 0xa5, 0x14, 0xc5, 0x14, 0xf0, 0xfc, 0x8c, 0x0e, 0xd4, 0x78, 0xad, 0x01, 0xd3, 0x48, 0x29, 
      0xfe, 0x8d, 0x01, 0xd3, 0x84, 0x44, 0xa9, 0x16, 0x85, 0x45, 0xb1, 0x44, 0x85, 0x46, 0xc8, 0xb1, 
      0x44, 0xf0, 0x3b, 0x85, 0x47, 0xc8, 0xc9, 0xff, 0xd0, 0x04, 0xc5, 0x46, 0xf0, 0xec, 0xb1, 0x44, 
      0xaa, 0xc8, 0xb1, 0x44, 0x85, 0x49, 0x38, 0x98, 0x65, 0x44, 0x85, 0x44, 0x90, 0x02, 0xe6, 0x45, 
      0xa0, 0x00, 0xb1, 0x44, 0x91, 0x46, 0xe6, 0x44, 0xd0, 0x02, 0xe6, 0x45, 0xe4, 0x46, 0xd0, 0x06, 
      0xa5, 0x47, 0xc5, 0x49, 0xf0, 0xc4, 0xe6, 0x46, 0xd0, 0xe8, 0xe6, 0x47, 0xd0, 0xe4, 0x68, 0x8d, 
      0x01, 0xd3, 0xa9, 0xc0, 0x8d, 0x0e, 0xd4, 0x58, 0xa9, 0x22, 0x8d, 0x2f, 0x02, 0x6c, 0xe0, 0x02, 
      0x8c, 0x0b, 0x03, 0xac, 0x2e, 0x07, 0x8c, 0x09, 0x03, 0xd0, 0x04, 0x0a, 0x2e, 0x0b, 0x03, 0x8d, 
      0x0a, 0x03, 0xad, 0x2f, 0x07, 0x8d, 0x08, 0x03, 0x8d, 0x0b, 0x07, 0xa9, 0x40, 0x8d, 0x03, 0x03, 
      0x20, 0x59, 0xe4, 0x30, 0x1a, 0xee, 0x0a, 0x03, 0xad, 0x2f, 0x07, 0xf0, 0x09, 0x18, 0x6d, 0x04, 
      0x03, 0x8d, 0x04, 0x03, 0x90, 0x03, 0xee, 0x05, 0x03, 0x0e, 0x0b, 0x07, 0xb0, 0xdd, 0x60, 0x68, 
      0x68, 0x38, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
   },
};

/*
 * Functions
 */

/*
 * dosxe_datecode()
 *
 * Given a time in seconds, return the date code
 */
int dosxe_datecode(time_t secs)
{
   int datecode = 0;
   struct tm *tm;
   tm = localtime(&secs);

   datecode |= (tm->tm_year%100)<<1; // Two-digit year
   datecode |= (tm->tm_mday) << 8; // day of month
   datecode |= ((tm->tm_mon+1) >> 3) & 0x01; // Linux: 0-11; Atari: 1-12
   datecode |= ((tm->tm_mon & 0x07) << 5) & 0x01;
   return datecode;
}

/*
 * dosxe_bitmap_status()
 *
 * Return the bit from the bitmap for the cluster (0 if allocated)
 */
int dosxe_bitmap_status(int cluster)
{
   // DOS XE doesn't have bits for clusters 0-3
   if ( cluster < 4 ) return 0;
   if ( cluster > MAX_CLUSTER ) return 0;
   cluster -= 1; // Make it 0-based

   struct dosxe_vtoc_cluster *vtoc = CLUSTER(VTOC_CLUSTER);
   unsigned char mask = 1<<(7-(cluster & 0x07));
   return (vtoc->bitmap[cluster/8] & mask) != 0; // 0 or 1
}

/*
 * dosxe_free_cluster()
 */
int dosxe_free_cluster(int cluster)
{
   if ( cluster < 4 ) return -EIO;
   if ( cluster > MAX_CLUSTER ) return -EIO;
   cluster -= 1; // Make it 0-based
   struct dosxe_vtoc_cluster *vtoc = CLUSTER(VTOC_CLUSTER);
   unsigned char mask = 1<<(7-(cluster & 0x07));
   unsigned int byte = cluster / 8;
   if ((vtoc->bitmap[byte] & mask) != 0) return 1; // Already free
   vtoc->bitmap[byte] |= mask;

   int free_count = BYTES2(vtoc->free_clusters);
   ++free_count;
   STOREBYTES2(vtoc->free_clusters,free_count);
   return 0;
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
   if ( BYTES2(sec1->exec_addr) != 0x0730 )
   {
      if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s: NOT DOS XE exec_addr %04x\n",__FUNCTION__,BYTES2(sec1->exec_addr));
      return 1;
   }
   if ( (sec1->sio_read_cmd & 0x7f) != 'R' )
   {
      if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s: NOT DOS XE sio_read_cmd: %02x -> %c\n",__FUNCTION__,sec1->sio_read_cmd,sec1->sio_read_cmd);
      return 1;
   }
   // if ( (sec1->sio_write_cmd & 0x7f) != 'W' ) return 1; // Could also be 'P'
   if ( (sec1->sio_format_cmd & 0x7f) != '!' ) {;} // FIXME: is it different for 1050 ED?
   if ( BYTES2R(sec1->sector_size) != atrfs.sectorsize )
   {
      if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s: NOT DOS XE inconsistent sector size: %d != %d\n",__FUNCTION__,BYTES2R(sec1->sector_size), atrfs.sectorsize);
      return 1;
   }

   struct dosxe_vtoc_cluster *vtoc = CLUSTER(VTOC_CLUSTER);
   if ( BYTES2(vtoc->total_clusters) != BYTES2(sec1->total_clusters) )
   {
      if ( options.debug > 1 ) fprintf(stderr,"DEBUG: %s: NOT DOS XE inconsistent total clusters: %d != %d\n",__FUNCTION__,BYTES2(vtoc->total_clusters),BYTES2(sec1->total_clusters));
      return 1;
   }


   atrfs.readonly = 1; // FIXME: No write support yet
   return 0;
}

/*
 * dosxe_info()
 */
char *dosxe_info(const char *path,int isdir,int parent_dir_first_cluster,struct dosxe_dir_entry *entry)
{
   char *buf,*b;

   buf = malloc(64*1024);
   b = buf;
   *b = 0;
   if ( isdir )
   {
      b+=sprintf(b,"Directory information and analysis\n\n  %.*s\n\n",(int)(strrchr(path,'.')-path),path);
      b+=sprintf(b,"Parent directory starts on cluster %d\n",parent_dir_first_cluster);
      if ( entry )
      {
         b+=sprintf(b,
                    "Directory entry internals:\n"
                    "  Flags: $%02x\n"
                    "  Clusters: %d\n",
                    entry->status,BYTES2(entry->file_clusters));
      }
      else
      {
         b+=sprintf(b,"There is no directory entry for the main directory itself\n");
      }
      buf = realloc(buf,strlen(buf)+1);
      return buf;
   }

   int filesize = BYTES2(entry->file_clusters) * 250 + entry->bytes_in_last_cluster;
   b+=sprintf(b,"File information and analysis\n\n  %.*s\n  %d bytes\n\n",(int)(strrchr(path,'.')-path),path,filesize);
   b+=sprintf(b,"Parent directory starts on cluster %d\n",parent_dir_first_cluster);
   b+=sprintf(b,
              "Directory entry internals:\n"
              "  Flags: $%02x\n"
              "  Clusters: %d\n",
              entry->status,BYTES2(entry->file_clusters));
   // FIXME: Show file maps

   // Generic info for the file type
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
 * dosxe_path()
 *
 * Given a text path, get the pointer to the directory entry.
 * If it's the root directory, the 'entry' will be NULL.
 * If none, then 'entry' might point to an available entry, or it
 * might be NULL if the directory would have to be expanded to add a file.
 */
int dosxe_path(const char *path,struct dosxe_dir_entry **entry,struct dosxe_dir_entry **parent_entry,int *parent_dir_first_cluster,int *isdir,int *isinfo)
{
   char name[8+3+1]; // 8+3+NULL
   int junk1=0,junk2,junk3;
   struct dosxe_dir_entry *junk4;
   if ( !parent_dir_first_cluster ) parent_dir_first_cluster = &junk1;
   if ( !isdir ) isdir = &junk2;
   if ( !isinfo ) isinfo = &junk3;
   if ( !parent_entry ) parent_entry = &junk4;

   // If not recursing, initialize
   if ( !*parent_dir_first_cluster )
   {
      *parent_dir_first_cluster = ROOT_DIR_CLUSTER;
      *entry = NULL;
      *parent_entry = NULL;
      *isdir = 0;
      *isinfo = 0;
   }

   while ( *path )
   {
      while ( *path == '/' ) ++path;
      if ( ! *path )
      {
         *isdir = 1;
         *entry = *parent_entry; // No entry for the root directory itself
         return 0;
      }

      // If it's just ".info" then it's for the directory
      if ( strcmp(path,".info") == 0 )
      {
         *isdir = 1;
         *isinfo = 1;
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: Found info file for directory entry\n",__FUNCTION__);
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
         if ( *path && *path != '.' && *path != '/' )
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
      if ( *path && *path != '/' )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: Extracted name %s but have path left %s\n",__FUNCTION__,name,path);
         return -ENOENT; // Name too long
      }
      if ( *path == '/' ) ++path;

      if ( options.debug ) fprintf(stderr,"DEBUG: %s: Look for %11.11s in dir at cluster %d\n",__FUNCTION__,name,*parent_dir_first_cluster);
      // Scan directory starting at *parent_dir_first_cluster for file name
      struct dosxe_dir_entry *firstfree = NULL;
      int dir_cluster_num=*parent_dir_first_cluster;
      while ( dir_cluster_num && dir_cluster_num < MAX_CLUSTER )
      {
         struct dosxe_dir_cluster *dir_cluster = CLUSTER(dir_cluster_num);
         for (int i=0;i<5;++i)
         {
            struct dosxe_dir_entry *e = &dir_cluster->entries[i];
            if ( e->status == 0 || e->status == FLAGS_DELETED )
            {
               if ( !firstfree ) firstfree = e;
            }
            if ( e->status == 0 ) break;
            if ( e->status == FLAGS_DELETED ) continue;
            if ( strncmp(e->file_name,name,8+3) != 0 ) continue;
            // Found a match!
            if ( *path && (e->status & FLAGS_DIR) && !*isinfo )
            {
               // Recurse on a subdirectory
               if ( options.debug ) fprintf(stderr,"DEBUG: %s: Recurrse on subdir %s\n",__FUNCTION__,path);
               *parent_dir_first_cluster = BYTES2(e->file_map_blocks[0]);
               *parent_entry = *entry;
               *entry = e;
               return dosxe_path(path,entry,parent_entry,parent_dir_first_cluster,isdir,isinfo);
            }
            if ( *path ) return -ENOTDIR; // Should have been a directory
            if ( (e->status & FLAGS_DIR) ) *isdir = 1;
            *parent_entry = *entry;
            *entry = e;
            return 0;
         }
         dir_cluster_num = BYTES2(dir_cluster->next);
      }
   }
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: Unexpectedly reached end of function\n",__FUNCTION__);
   return -ENOENT; // Shouldn't be reached
}

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
   if ( VTOC_CLUSTERS == 1 )
      b+=sprintf(b,"VTOC cluster:        %d\n",VTOC_CLUSTER);
   else
      b+=sprintf(b,"VTOC clusters:       %d--%d\n",VTOC_CLUSTER,VTOC_CLUSTER+VTOC_CLUSTERS-1);
   b+=sprintf(b,"Main directory:      %d\n",sec1->main_directory);
   b+=sprintf(b,"Tracks:              %d\n",sec1->tracks);
   b+=sprintf(b,"Sectors per track:   %d\n",BYTES2R(sec1->sectors_per_track));
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
   b+=sprintf(b,"VTOC Total Clusters: %d (i.e., %d--%d)\n",BYTES2(vtoc->total_clusters),4,MAX_CLUSTER);
   if ( MAX_PHYSICAL_CLUSTER > MAX_CLUSTER )
   {
      b+=sprintf(b,"Clusters not used:   %d--%d (wasted space)\n",MAX_CLUSTER+1,MAX_PHYSICAL_CLUSTER);
   }
   else if ( MAX_PHYSICAL_CLUSTER > MAX_CLUSTER )
   {
      b+=sprintf(b,"Clusters past end:   %d--%d (short ATR image)\n",MAX_PHYSICAL_CLUSTER+1,MAX_CLUSTER);
   }
   b+=sprintf(b,"VTOC Free Clusters:  %d\n",BYTES2(vtoc->free_clusters));
   b+=sprintf(b,"File/Dir count:      %d\n",BYTES2(vtoc->file_sequence_number));
   b+=sprintf(b,"Volume rnd number:   %04x\n",BYTES2(vtoc->volume_number));

   // Free bit map; generally not useful and may be too much data
#if 1
   b+=sprintf(b,"\nCluster Allocation Map:\n");
   int prev=2; // Never matches 0 or 1
   int start= -1;
   int new=0; // Flag to finish last line after loop
   for ( int i=4;i <= MAX_CLUSTER;++i )
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
      b+=sprintf(b," -- %d",MAX_CLUSTER);
   b+=sprintf(b,"\n");
#endif
   return buf;
}

/*
 * dosxe_getattr()
 */
int dosxe_getattr(const char *path, struct stat *stbuf)
{
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   if ( strcmp(path,"/") == 0 )
   {
      stbuf->st_ino = CLUSTER_TO_SEC(ROOT_DIR_CLUSTER);
      stbuf->st_size = 0;
      stbuf->st_mode = MODE_DIR(stbuf->st_mode);
      stbuf->st_mode = MODE_RO(stbuf->st_mode);
      return 0;
   }

   // Magic cluster files always work; expose them with high debugging options
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec > 3 && sec <= MAX_PHYSICAL_CLUSTER )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = 256;
         stbuf->st_ino = sec;
         if ( atrfs.sectorsize == 128 ) stbuf->st_ino = sec*2;
         return 0; // Good, can read this sector
      }
   }

   // Get time stamp
   int r,isdir,isinfo;
   int parent_dir_first_cluster=0;
   struct dosxe_dir_entry *entry=NULL;
   r = dosxe_path(path,&entry,NULL,&parent_dir_first_cluster,&isdir,&isinfo);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s path search returned %d\n",__FUNCTION__,path,r);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;

   if ( entry )
   {
      struct tm tm;
      struct timespec ts,cts;
      memset(&tm,0,sizeof(tm));
      tm.tm_sec=0;
      tm.tm_min=0;
      tm.tm_hour=12;
      tm.tm_mday=DATE_TO_DAY(entry->modification_date);
      tm.tm_mon=DATE_TO_MONTH(entry->modification_date)-1; // Want 0-11, not 1-12
      tm.tm_year=DATE_TO_4YEAR(entry->modification_date)-1900;
      tm.tm_isdst = -1; // Have the system determine if DST is in effect
      ts.tv_nsec = 0;
      ts.tv_sec = mktime(&tm);
      memset(&tm,0,sizeof(tm));
      tm.tm_sec=0;
      tm.tm_min=0;
      tm.tm_hour=12;
      tm.tm_mday=DATE_TO_DAY(entry->creation_date);
      tm.tm_mon=DATE_TO_MONTH(entry->creation_date)-1; // Want 0-11, not 1-12
      tm.tm_year=DATE_TO_4YEAR(entry->creation_date)-1900;
      tm.tm_isdst = -1; // Have the system determine if DST is in effect
      cts.tv_nsec = 0;
      cts.tv_sec = mktime(&tm);
      if ( ts.tv_sec != -1 )
      {
         stbuf->st_mtim = ts;
         stbuf->st_atim = ts;
      }
      if ( cts.tv_sec != -1 )
      {
         stbuf->st_ctim = cts;
      }

      if ( isdir && !isinfo )
      {
         ++stbuf->st_nlink;
         stbuf->st_mode = MODE_DIR(atrfs.atrstat.st_mode & 0777);
      }
      if ( (entry->status & FLAGS_LOCKED) ) stbuf->st_mode = MODE_RO(stbuf->st_mode);
      stbuf->st_size = BYTES2(entry->file_clusters) * 250 + entry->bytes_in_last_cluster;
      stbuf->st_ino = CLUSTER_TO_SEC(BYTES2(entry->file_map_blocks[0])); // Unique, but not that useful
   }
   else
   {
      // Info on root dir; there is no entry to get info from
      stbuf->st_ino = CLUSTER_TO_SEC(ROOT_DIR_CLUSTER);
   }

   if ( isinfo )
   {
      stbuf->st_mode = MODE_RO(stbuf->st_mode); // These files are never writable
      stbuf->st_ino += 0x10000;

      char *info = dosxe_info(path,isdir,parent_dir_first_cluster,entry);
      stbuf->st_size = strlen(info);
      free(info);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s info\n",__FUNCTION__,path);
      return 0;
   }

   return 0; // Whatever, don't really care
}

/*
 * dosxe_readdir()
 */
int dosxe_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)offset;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   int r,isdir,isinfo;
   struct dosxe_dir_entry *entry=NULL;
   r = dosxe_path(path,&entry,NULL,NULL,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( !isdir ) return -ENOTDIR;

   int dir_cluster_num=entry ? BYTES2(entry->file_map_blocks[0]) : ROOT_DIR_CLUSTER;
   while ( dir_cluster_num && dir_cluster_num < MAX_CLUSTER )
   {
      struct dosxe_dir_cluster *dir_cluster = CLUSTER(dir_cluster_num);
      for (int i=0;i<5;++i)
      {
         struct dosxe_dir_entry *e = &dir_cluster->entries[i];
         if ( e->status == 0 ) break;
         if ( e->status == FLAGS_DELETED ) continue;
         char name[8+1+3+1];
         char *n = name;
         for (int j=0;j<8;++j)
         {
            if ( e->file_name[j] == ' ' ) break;
            *n = e->file_name[j];
            ++n;
         }
         if ( e->file_ext[0] != ' ' )
         {
            *n = '.';
            ++n;
            for (int j=0;j<3;++j)
            {
               if ( e->file_ext[j] == ' ' ) break;
               *n=e->file_ext[j];
               ++n;
            }
         }
         *n=0;
         filler(buf, name, FILLER_NULL);
      }
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s dir cluster %d -> %d\n",__FUNCTION__,path, dir_cluster_num, BYTES2(dir_cluster->next));
      dir_cluster_num = BYTES2(dir_cluster->next);
   }

   if ( options.debug > 2 ) // Expose non-zero clusters for high debugging levels
   {
      if ( strcmp(path,"/") == 0 )
      {
         // Create .cluster4 ... .cluster359 as appropriate
         unsigned char zero[256];
         memset(zero,0,256);
         char name[32];
         int digits = sprintf(name,"%d",MAX_PHYSICAL_CLUSTER);
         for (int sec=4; sec<=MAX_PHYSICAL_CLUSTER; ++sec)
         {
            unsigned char *s = CLUSTER(sec);
            if ( memcmp(s,zero,256) == 0 ) continue; // Skip empty sectors
            sprintf(name,".cluster%0*d",digits,sec);
            filler(buf,name,FILLER_NULL);
         }
      }
   }
   return 0;
}

/*
 * dosxe_read()
 */
int dosxe_read(const char *path, char *buf, size_t size, off_t offset)
{

   // Magic /.cluster### files
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec <= 3 || sec > MAX_PHYSICAL_CLUSTER ) return -ENOENT;

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
   int r,isdir,isinfo;
   struct dosxe_dir_entry *entry=NULL;
   int parent_dir_first_cluster=0;
   r = dosxe_path(path,&entry,NULL,&parent_dir_first_cluster,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isdir && !isinfo ) return -EISDIR;

   // Info files
   if ( isinfo )
   {
      char *info = dosxe_info(path,isdir,parent_dir_first_cluster,entry);
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
   if ( options.debug ) fprintf(stderr, "DEBUG: %s: %s File has %d clusters and %d more bytes\n",__FUNCTION__,path,BYTES2(entry->file_clusters),entry->bytes_in_last_cluster);
   int bytes_read = 0;
   int full_blocks = BYTES2(entry->file_clusters);
   for ( int map=0;map<12;++map )
   {
      if ( !BYTES2(entry->file_map_blocks[map]) ) break; // EOF
      struct dosxe_file_map_cluster *mapcluster = CLUSTER(BYTES2(entry->file_map_blocks[map]));
      for ( int map_entry=0;map_entry<125;++map_entry )
      {
         int data_cluster_num = BYTES2(mapcluster->data_block[map_entry]);
         if ( !data_cluster_num ) break; // EOF
         struct dosxe_data_cluster *data = CLUSTER(data_cluster_num);
         unsigned char *s = data->data;
         if ( !data ) return -EIO;

         int bytes=250;
         if ( !full_blocks ) bytes = entry->bytes_in_last_cluster;
         else --full_blocks;
         if ( offset < bytes )
         {
            s+=offset;
            bytes -= offset;
            offset = 0;
         }
         if ( offset > bytes )
         {
            offset -= bytes;
            bytes = 0;
         }
         if ( (size_t)bytes > size ) bytes = size;
         // FIXME: Validate setor label
         if ( bytes )
         {
            memcpy(buf,s,bytes);
            buf += bytes;
            bytes_read += bytes;
            size -= bytes;
         }
      }
   }
   if ( bytes_read ) return bytes_read;
   return -EOF;
}
// Implement these
int dosxe_write(const char *path, const char *buf, size_t size, off_t offset);
int dosxe_mkdir(const char *path,mode_t mode);
int dosxe_rmdir(const char *path);
int dosxe_unlink(const char *path);
int dosxe_rename(const char *path1, const char *path2, unsigned int flags);
int dosxe_chmod(const char *path, mode_t mode);
int dosxe_create(const char *path, mode_t mode);
int dosxe_truncate(const char *path, off_t size);
int dosxe_utimens(const char *path, const struct timespec tv[2]);

/*
 * dosxe_statfs()
 */
int dosxe_statfs(const char *path, struct statvfs *stfsbuf)
{
   struct dosxe_vtoc_cluster *vtoc = CLUSTER(VTOC_CLUSTER);

   (void)path; // meaningless
   stfsbuf->f_bsize = 256;
   stfsbuf->f_frsize = 256;
   stfsbuf->f_blocks = BYTES2(vtoc->total_clusters);
   stfsbuf->f_bfree = BYTES2(vtoc->free_clusters);
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = 0;
   stfsbuf->f_ffree = 0;
   stfsbuf->f_namemax = 12; // 8.3 including '.'
   return 0;
}

/*
 * dosxe_newfs()
 */
int dosxe_newfs(void)
{
   // Simple sanity: Must have VTOC, DIR, and one data cluster to be of any use
   if ( MAX_PHYSICAL_CLUSTER < 6 )
   {
      fprintf(stderr,"Error: DOS XE must have 6 clusters (6 DD sectors or 12 SD sectors\n");
      return -EINVAL;
   }

   // Non-standard image size warning
   if ( !( atrfs.sectors == 720 ||
           (atrfs.sectors == 1040 && atrfs.sectorsize == 128) ||
           (atrfs.sectors == 1440 && atrfs.sectorsize == 256) ) )
   {
      fprintf(stderr,
              "WARNING: DOS XE does not always work with non-standard image sizes\n"
              "         You may need to hack DOS to recognize these images\n"
              "         Recommended values:\n"
              "           720  x 128 (SS/SD) AT810\n"
              "           1040 x 128 (SS/ED) AT1050\n"
              "           720  x 256 (SS/DD) SSDD\n"
              "           1440 x 256 (DS/DD) XF551\n"
              "         Images with more than 3960 clusters (DD sectors) will fail\n"
              "         a directory listing in unmodified DOS XE with an error 144\n"
         );
      if ( MAX_PHYSICAL_CLUSTER > 3960 )
      {
         fprintf(stderr,
                 "WARNING: THIS IMAGE WILL NOT WORK!!!\n"
                 "   See above unless you're modifying the DOS XE drive table\n"
            );
      }
   }
   // Copy in saved boot sectors
   for (int i=0;i<3;++i)
   {
      int imagetype;
      unsigned char *s = SECTOR(i+1);
      if ( atrfs.sectorsize == 128 )
      {
         if ( atrfs.sectors > 720 ) imagetype = DRIVE_AT1050;
         else imagetype = DRIVE_AT810;
      }
      if ( atrfs.sectorsize == 256 )
      {
         if ( atrfs.sectors <= 720 ) imagetype = DRIVE_SSDD;
         else imagetype = DRIVE_XF551;
      }
      // FIXME: Does it need hacking for non-standard images ?
      memcpy(s,bootsectors[imagetype]+128*i,128);
   }
   // Important, but already copied in:
   //sec1->pad_zero = 'X';
   //sec1->boot_sectors = 3;

   // Stuff that is image-specific:
   struct sector1_dosxe *sec1 = SECTOR(1);
   STOREBYTES2(sec1->total_clusters,MAX_PHYSICAL_CLUSTER + 1); // Add one
   if ( MAX_PHYSICAL_CLUSTER == 0xffff ) STOREBYTES2(sec1->total_clusters,MAX_PHYSICAL_CLUSTER); // Don't add one
   STOREBYTES2(sec1->max_free_clusters,MAX_PHYSICAL_CLUSTER - 3 - VTOC_CLUSTERS - 1); // Allocate: boot, VTOC, main directory
   sec1->main_directory = 4 + VTOC_CLUSTERS; // 5 on standard images
   STOREBYTES2(sec1->date_code,dosxe_datecode(time(NULL)));
   sec1->first_vtoc_byte = 0x0f >> VTOC_CLUSTERS; // 0x07 if one VTOC cluster >>1 for each additional

   // Initialize VTOC
   struct dosxe_vtoc_cluster *vtoc = CLUSTER(VTOC_CLUSTER);
   STOREBYTES2(vtoc->total_clusters,BYTES2(sec1->total_clusters));
   // STOREBYTES2(vtoc->free_clusters,BYTES2(sec1->max_free_clusters));
   srandom(time(NULL));
   vtoc->volume_number[0] = random() & 0xff;
   do
   {
      vtoc->volume_number[1] = random() & 0xff;
   } while ( vtoc->volume_number[1] == vtoc->volume_number[0]);

   for ( int cluster = VTOC_CLUSTER + VTOC_CLUSTERS + 1; cluster <= MAX_PHYSICAL_CLUSTER; ++cluster )
   {
      int r;
      r = dosxe_free_cluster(cluster);
      if ( r )
      {
         fprintf(stderr,"Error: Inital set of free clusters says %d is already free\n",cluster);
      }
   }

   // Set sector signature on main directory
   struct dosxe_dir_cluster *dir = ROOT_DIR;
   STOREBYTES2(dir->label.volume_number,BYTES2(vtoc->volume_number));
   dir->label.sequence[1] = 0xff; // Flag for directory
   return 0;
}
