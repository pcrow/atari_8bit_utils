/*
 * mydos.c
 *
 * Functions for accessing DOS 2, DOS 2.5, and MyDOS file systems.
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
#ifdef __linux__
#include <linux/fs.h> // Linux-only options; fix if supported elsewhere
#else
#define RENAME_NOREPLACE  0
#define RENAME_EXCHANGE 0
#endif
#include "atrfs.h"

/*
 * Macros and defines
 */
#define BITMAPBYTE(n)   ((n)/8)
#define BITMAPMASK(n)   (1<<(7-((n)%8)))
#define BITMAP(map,sector)  (map[BITMAPBYTE(sector)]&BITMAPMASK(sector))
#define DIRENT_ENTRY(n) ((n) + ((atrfs.sectorsize == 256 ) ? (n)/8 * 8 : 0 ))

/*
 * File System Structures
 *
 * https://atari.fox-1.nl/disk-formats-explained/
 */
struct mydos_vtoc {
   unsigned char vtoc_sectors; // SD: X*2-3 ; DD: X-1 (extra SD sectors allocated in pairs)
   unsigned char total_sectors[2];
   unsigned char free_sectors[2];
   unsigned char unused[5];
   unsigned char bitmap[118]; // sectors 0-943; continues on sector 359, 358,... as needed
};
// mydos_vtoc2: Just a bitmap; no need for a struct

struct dos2_dirent {
   unsigned char flags;
   unsigned char sectors[2];
   unsigned char start[2];
   unsigned char name[8];
   unsigned char ext[3];
};

/*
 * Directory entry flags
 *
 * All known expected values for unlocked files:
 *
 * Regular DOS 1 file: $40 (last byte in sector has different meaning)
 * Regular DOS 2 file: $42
 * Regular MyDOS file: $46
 * MyDOS directory:    $10
 * DOS 2.5 extended:   $03
 * DOS 1 open file:    $41
 * DOS 2.0 open file:  $43
 * DOS 2.5 write open: $43 ?
 * MyDOS open file:    $47
 *
 * MyDOS files without the file numbers in the sector chain are
 * still visible to other versions of DOS and will generate errors.
 */
enum dirent_flags {
   FLAGS_OPEN     = 0x01, // Open for write, not visible
   FLAGS_DOS2     = 0x02, // Not set by DOS 1
   FLAGS_NOFILENO = 0x04, // MyDOS created; full 2-byte sector numbers
   FLAGS_UNDEF    = 0x08, // Not used by any known variant
   FLAGS_DIR      = 0x10, // MyDOS directory
   FLAGS_LOCKED   = 0x20,
   FLAGS_INUSE    = 0x40, // Visible to DOS 1 and DOS 2
   FLAGS_DELETED  = 0x80,
   FLAGS_DOS25_regular = 0x03, // open and created by DOS 2
   FLAGS_MYDOS_REGULAR = 0x46, // in-use, no fileno, dos 2
};

struct dos25_vtoc { // sector 360
   unsigned char dos_code; // Always 2
   unsigned char total_sectors[2]; // sectors below 720: 707
   unsigned char free_sectors[2]; // free sectors below 720: max of 707
   unsigned char unused[5];
   unsigned char bitmap[90]; // sectors 0-719
   unsigned char unused2[28];
};

struct dos25_vtoc2 { // sector 1024
   unsigned char bitmap_repeat[84]; // Sectors 48-719 bitmap duplicated
   unsigned char bitmap[38]; // sectors 720-1023
   unsigned char free_high_sectors[2]; // vtoc1 only lists free low sectors
   unsigned char unused[4]; // should be zero
};

/*
 * Function prototypes
 */
int mydos_sanity(void);
int dos1_sanity(void);
int dos2_sanity(void);
int dos25_sanity(void);
int mydos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int mydos_getattr(const char *path, struct stat *stbuf);
int mydos_read(const char *path, char *buf, size_t size, off_t offset);
int mydos_write(const char *path, const char *buf, size_t size, off_t offset);
int mydos_mkdir(const char *path,mode_t mode);
int mydos_rmdir(const char *path);
int mydos_unlink(const char *path);
int mydos_rename(const char *path1, const char *path2, unsigned int flags);
int mydos_chmod(const char *path, mode_t mode);
int mydos_create(const char *path, mode_t mode);
int mydos_truncate(const char *path, off_t size);
int mydos_statfs(const char *path, struct statvfs *stfsbuf);
int mydos_newfs(void);
char *mydos_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops dos1_ops = {
   .name = "Atari DOS 1",
   .fstype = "dos1",
   .fs_sanity = dos1_sanity,
   .fs_getattr = mydos_getattr,
   .fs_readdir = mydos_readdir,
   .fs_read = mydos_read,
   .fs_write = mydos_write,
   // .fs_mkdir = mydos_mkdir, // MyDOS can't read DOS 1 files; don't mix
   .fs_rmdir = mydos_rmdir,
   .fs_unlink = mydos_unlink,
   .fs_rename = mydos_rename,
   .fs_chmod = mydos_chmod,
   .fs_create = mydos_create,
   .fs_truncate = mydos_truncate,
   .fs_statfs = mydos_statfs,
   .fs_newfs = mydos_newfs,
   .fs_fsinfo = mydos_fsinfo,
};
const struct fs_ops dos2_ops = {
   .name = "Atari DOS 2.0s",
   .fstype = "dos2",
   .fs_sanity = dos2_sanity,
   .fs_getattr = mydos_getattr,
   .fs_readdir = mydos_readdir,
   .fs_read = mydos_read,
   .fs_write = mydos_write,
   .fs_mkdir = mydos_mkdir,
   .fs_rmdir = mydos_rmdir,
   .fs_unlink = mydos_unlink,
   .fs_rename = mydos_rename,
   .fs_chmod = mydos_chmod,
   .fs_create = mydos_create,
   .fs_truncate = mydos_truncate,
   .fs_statfs = mydos_statfs,
   .fs_newfs = mydos_newfs,
   .fs_fsinfo = mydos_fsinfo,
};
const struct fs_ops dos20d_ops = {
   .name = "Atari DOS 2.0d",
   //.fstype = "dos2d",
   //.fs_sanity = dos2_sanity, // Never detected
   .fs_getattr = mydos_getattr,
   .fs_readdir = mydos_readdir,
   .fs_read = mydos_read,
   .fs_write = mydos_write,
   .fs_mkdir = mydos_mkdir,
   .fs_rmdir = mydos_rmdir,
   .fs_unlink = mydos_unlink,
   .fs_rename = mydos_rename,
   .fs_chmod = mydos_chmod,
   .fs_create = mydos_create,
   .fs_truncate = mydos_truncate,
   .fs_statfs = mydos_statfs,
   .fs_newfs = mydos_newfs,
   .fs_fsinfo = mydos_fsinfo,
};
const struct fs_ops dos25_ops = {
   .name = "Atari DOS 2.5",
   .fstype = "dos25",
   .fs_sanity = dos25_sanity,
   .fs_getattr = mydos_getattr,
   .fs_readdir = mydos_readdir,
   .fs_read = mydos_read,
   .fs_write = mydos_write,
   // .fs_mkdir = mydos_mkdir, // MyDOS and DOS 2.5 are too incompatible to mix features
   .fs_rmdir = mydos_rmdir, // Useless, but harmless
   .fs_unlink = mydos_unlink,
   .fs_rename = mydos_rename,
   .fs_chmod = mydos_chmod,
   .fs_create = mydos_create,
   .fs_truncate = mydos_truncate,
   .fs_statfs = mydos_statfs,
   .fs_newfs = mydos_newfs,
   .fs_fsinfo = mydos_fsinfo,
};
const struct fs_ops mydos_ops = {
   .name = "MyDOS 4.53 or compatible",
   .fstype = "mydos",
   .fs_sanity = mydos_sanity,
   .fs_getattr = mydos_getattr,
   .fs_readdir = mydos_readdir,
   .fs_read = mydos_read,
   .fs_write = mydos_write,
   .fs_mkdir = mydos_mkdir,
   .fs_rmdir = mydos_rmdir,
   .fs_unlink = mydos_unlink,
   .fs_rename = mydos_rename,
   .fs_chmod = mydos_chmod,
   .fs_create = mydos_create,
   .fs_truncate = mydos_truncate,
   .fs_statfs = mydos_statfs,
   .fs_newfs = mydos_newfs,
   .fs_fsinfo = mydos_fsinfo,
};

/*
 * Boot Sectors
 *
 * These are copied from real images.
 *
 * The code will detect creating DOS.SYS and will update sector 1 as appropriate.
 */
static const char bootsectors[ATR_MAXFSTYPE][3*128] =
{
   [ATR_DOS1] = {
#if 1 // Without DOS.SYS
0x00, 0x01, 0x00, 0x07, 0x00, 0x13, 0x4c, 0x12, 0x07, 0x03, 0x0f, 0x00, 0x80, 0x26, 0x00, 0x00, 
0x02, 0x00, 0xad, 0x0e, 0x07, 0xf0, 0x2f, 0xa9, 0x07, 0x85, 0x44, 0xa9, 0x03, 0x85, 0x43, 0x20, 
#else // With DOS.SYS
0x00, 0x01, 0x00, 0x07, 0x00, 0x13, 0x4c, 0x12, 0x07, 0x03, 0x0f, 0x00, 0x80, 0x26, 0xff, 0x00, 
0x02, 0x00, 0xad, 0x0e, 0x07, 0xf0, 0x2f, 0xa9, 0x07, 0x85, 0x44, 0xa9, 0x03, 0x85, 0x43, 0x20, 
#endif
0x4f, 0x07, 0xad, 0x11, 0x07, 0xac, 0x10, 0x07, 0x18, 0x20, 0x63, 0x07, 0x30, 0x18, 0xa0, 0x7f, 
0xb1, 0x43, 0x30, 0x16, 0xa0, 0x7d, 0xb1, 0x43, 0x29, 0x03, 0x48, 0xc8, 0xb1, 0x43, 0xa8, 0x20, 
0x4f, 0x07, 0x68, 0x4c, 0x28, 0x07, 0xa9, 0xc0, 0xd0, 0x02, 0xa9, 0x00, 0x0a, 0xa8, 0x60, 0x18, 
0xa5, 0x43, 0x69, 0x7d, 0x8d, 0x04, 0x03, 0x85, 0x43, 0xa5, 0x44, 0x69, 0x00, 0x8d, 0x05, 0x03, 
0x85, 0x44, 0x60, 0x8d, 0x0b, 0x03, 0x8c, 0x0a, 0x03, 0xa9, 0x52, 0x90, 0x02, 0xa9, 0x57, 0x8d, 
0x02, 0x03, 0x20, 0x53, 0xe4, 0xae, 0x22, 0x11, 0xad, 0x03, 0x03, 0x60, 0xfc, 0x07, 0x8e, 0x09, 
   },
   [ATR_DOS2] = {
0x00, 0x03, 0x00, 0x07, 0x40, 0x15, 0x4c, 0x14, 0x07, 0x03, 0x03, 0x00, 0x7c, 0x1a, 0x00, 0x04, 
0x00, 0x7d, 0xcb, 0x07, 0xac, 0x0e, 0x07, 0xf0, 0x36, 0xad, 0x12, 0x07, 0x85, 0x43, 0x8d, 0x04, 
0x03, 0xad, 0x13, 0x07, 0x85, 0x44, 0x8d, 0x05, 0x03, 0xad, 0x10, 0x07, 0xac, 0x0f, 0x07, 0x18, 
0xae, 0x0e, 0x07, 0x20, 0x6c, 0x07, 0x30, 0x17, 0xac, 0x11, 0x07, 0xb1, 0x43, 0x29, 0x03, 0x48, 
0xc8, 0x11, 0x43, 0xf0, 0x0e, 0xb1, 0x43, 0xa8, 0x20, 0x57, 0x07, 0x68, 0x4c, 0x2f, 0x07, 0xa9, 
0xc0, 0xd0, 0x01, 0x68, 0x0a, 0xa8, 0x60, 0x18, 0xa5, 0x43, 0x6d, 0x11, 0x07, 0x8d, 0x04, 0x03, 
0x85, 0x43, 0xa5, 0x44, 0x69, 0x00, 0x8d, 0x05, 0x03, 0x85, 0x44, 0x60, 0x8d, 0x0b, 0x03, 0x8c, 
0x0a, 0x03, 0xa9, 0x52, 0xa0, 0x40, 0x90, 0x04, 0xa9, 0x57, 0xa0, 0x80, 0x8d, 0x02, 0x03, 0x8c, 
0x03, 0x03, 0xa9, 0x31, 0xa0, 0x0f, 0x8d, 0x00, 0x03, 0x8c, 0x06, 0x03, 0xa9, 0x03, 0x8d, 0xff, 
0x12, 0xa9, 0x00, 0xa0, 0x80, 0xca, 0xf0, 0x04, 0xa9, 0x01, 0xa0, 0x00, 0x8d, 0x09, 0x03, 0x8c, 
0x08, 0x03, 0x20, 0x59, 0xe4, 0x10, 0x1d, 0xce, 0xff, 0x12, 0x30, 0x18, 0xa2, 0x40, 0xa9, 0x52, 
0xcd, 0x02, 0x03, 0xf0, 0x09, 0xa9, 0x21, 0xcd, 0x02, 0x03, 0xf0, 0x02, 0xa2, 0x80, 0x8e, 0x03, 
0x03, 0x4c, 0xa2, 0x07, 0xae, 0x01, 0x13, 0xad, 0x03, 0x03, 0x60, 0xaa, 0x08, 0x14, 0x0b, 0xbe, 
0x0a, 0xcb, 0x09, 0x00, 0x0b, 0xa6, 0x0b, 0x07, 0x85, 0x44, 0xad, 0x0a, 0x07, 0x8d, 0xd6, 0x12, 
0xad, 0x0c, 0x07, 0x85, 0x43, 0xad, 0x0d, 0x07, 0x85, 0x44, 0xad, 0x0a, 0x07, 0x8d, 0x0c, 0x13, 
0xa2, 0x07, 0x8e, 0x0d, 0x13, 0x0e, 0x0c, 0x13, 0xb0, 0x0d, 0xa9, 0x00, 0x9d, 0x11, 0x13, 0x9d, 
0x29, 0x13, 0x9d, 0x31, 0x13, 0xf0, 0x36, 0xa0, 0x05, 0xa9, 0x00, 0x91, 0x43, 0xe8, 0x8e, 0x01, 
0x03, 0xa9, 0x53, 0x8d, 0x02, 0x03, 0x20, 0x53, 0xe4, 0xa0, 0x02, 0xad, 0xea, 0x02, 0x29, 0x20, 
0xd0, 0x01, 0x88, 0x98, 0xae, 0x0d, 0x13, 0x9d, 0x11, 0x13, 0xa5, 0x43, 0x9d, 0x29, 0x13, 0xa5, 
0x44, 0x9d, 0x31, 0x13, 0x20, 0x70, 0x08, 0x88, 0xf0, 0x03, 0x20, 0x70, 0x08, 0xca, 0x10, 0xb2, 
0xac, 0x09, 0x07, 0xa2, 0x00, 0xa9, 0x00, 0x88, 0x10, 0x01, 0x98, 0x9d, 0x19, 0x13, 0x98, 0x30, 
0x0d, 0xa5, 0x43, 0x9d, 0x39, 0x13, 0xa5, 0x44, 0x9d, 0x49, 0x13, 0x20, 0x70, 0x08, 0xe8, 0xe0, 
0x10, 0xd0, 0xe2, 0xa5, 0x43, 0x8d, 0xe7, 0x02, 0xa5, 0x44, 0x8d, 0xe8, 0x02, 0x4c, 0x7e, 0x08, 
0x18, 0xa5, 0x43, 0x69, 0x80, 0x85, 0x43, 0xa5, 0x44, 0x69, 0x00, 0x85, 0x44, 0x60, 0xa0, 0x7f, 
   },
   [ATR_DOS25] = {
0x00, 0x03, 0x00, 0x07, 0x40, 0x15, 0x4c, 0x14, 0x07, 0x03, 0x0f, 0x00, 0xcc, 0x19, 0x01, 0x04, 
0x00, 0x7d, 0xcb, 0x07, 0xac, 0x0e, 0x07, 0xf0, 0x35, 0x20, 0x5f, 0x07, 0xad, 0x10, 0x07, 0xac, 
0x0f, 0x07, 0xa6, 0x24, 0x8e, 0x04, 0x03, 0xa6, 0x25, 0x8e, 0x05, 0x03, 0x18, 0x20, 0x6c, 0x07, 
0x30, 0x1c, 0xac, 0x11, 0x07, 0xb1, 0x24, 0x29, 0x03, 0xaa, 0xc8, 0x11, 0x24, 0xf0, 0x11, 0xb1, 
0x24, 0x48, 0xc8, 0xb1, 0x24, 0x20, 0x55, 0x07, 0x68, 0xa8, 0x8a, 0x4c, 0x22, 0x07, 0xa9, 0xc0, 
0x0a, 0xa8, 0x60, 0xa9, 0x80, 0x18, 0x65, 0x24, 0x85, 0x24, 0x90, 0x02, 0xe6, 0x25, 0x60, 0xad, 
0x12, 0x07, 0x85, 0x24, 0xad, 0x13, 0x07, 0x85, 0x25, 0x60, 0x00, 0x00, 0x8d, 0x0b, 0x03, 0x8c, 
0x0a, 0x03, 0xa9, 0x52, 0xa0, 0x40, 0x90, 0x04, 0xa9, 0x57, 0xa0, 0x80, 0x08, 0xa6, 0x21, 0xe0, 
0x08, 0xd0, 0x07, 0x28, 0x20, 0x81, 0x14, 0x4c, 0xb9, 0x07, 0x28, 0x8d, 0x02, 0x03, 0xa9, 0x0f, 
0x8d, 0x06, 0x03, 0x8c, 0x17, 0x13, 0xa9, 0x31, 0x8d, 0x00, 0x03, 0xa9, 0x03, 0x8d, 0x09, 0x13, 
0xa9, 0x80, 0x8d, 0x08, 0x03, 0x0a, 0x8d, 0x09, 0x03, 0xad, 0x17, 0x13, 0x8d, 0x03, 0x03, 0x20, 
0x59, 0xe4, 0x10, 0x05, 0xce, 0x09, 0x13, 0x10, 0xf0, 0xa6, 0x49, 0x98, 0x60, 0x20, 0xad, 0x11, 
0x20, 0x64, 0x0f, 0x20, 0x04, 0x0d, 0x4c, 0xc7, 0x12, 0x00, 0x00, 0x64, 0x08, 0x8f, 0x0a, 0x4d, 
0x0a, 0x8f, 0x09, 0xbc, 0x07, 0x2a, 0x0b, 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0xff, 
0xad, 0x0c, 0x07, 0x85, 0x24, 0xad, 0x0d, 0x07, 0x85, 0x25, 0xad, 0x0a, 0x07, 0x85, 0x43, 0xa2, 
0x07, 0xa9, 0x00, 0x06, 0x43, 0x90, 0x15, 0xa0, 0x05, 0x91, 0x24, 0xa5, 0x24, 0x9d, 0x29, 0x13, 
0xa5, 0x25, 0x9d, 0x31, 0x13, 0xa9, 0x90, 0x20, 0x55, 0x07, 0xa9, 0x64, 0x9d, 0x19, 0x13, 0xca, 
0x10, 0xdf, 0xa5, 0x24, 0x8d, 0x39, 0x13, 0xa5, 0x25, 0x8d, 0x3a, 0x13, 0xac, 0x09, 0x07, 0xa2, 
0x00, 0x88, 0x98, 0x9d, 0x21, 0x13, 0x30, 0x03, 0x20, 0x53, 0x07, 0xe8, 0xe0, 0x08, 0xd0, 0xf1, 
0xa5, 0x24, 0x8d, 0xe7, 0x02, 0xa5, 0x25, 0x8d, 0xe8, 0x02, 0xa9, 0x00, 0xa8, 0x99, 0x81, 0x13, 
0xc8, 0x10, 0xfa, 0xa8, 0xb9, 0x1a, 0x03, 0xf0, 0x0c, 0xc9, 0x44, 0xf0, 0x08, 0xc8, 0xc8, 0xc8, 
0xc0, 0x1e, 0xd0, 0xf0, 0x00, 0xa9, 0x44, 0x99, 0x1a, 0x03, 0xa9, 0xcb, 0x99, 0x1b, 0x03, 0xa9, 
0x07, 0x99, 0x1c, 0x03, 0x60, 0x20, 0xad, 0x11, 0x20, 0x7d, 0x0e, 0xbd, 0x4a, 0x03, 0x9d, 0x82, 
0x13, 0x29, 0x02, 0xf0, 0x03, 0x4c, 0x72, 0x0d, 0x20, 0xec, 0x0e, 0x08, 0xbd, 0x82, 0x13, 0xc9, 
   },
   [ATR_DOS20D] = {
0x00, 0x03, 0x00, 0x07, 0x40, 0x15, 0x4c, 0x14, 0x07, 0x03, 0x03, 0x00, 0x7c, 0x1a, 0x02, 0x04, 
0x00, 0xfd, 0xcb, 0x07, 0xac, 0x0e, 0x07, 0xf0, 0x36, 0xad, 0x12, 0x07, 0x85, 0x43, 0x8d, 0x04, 
0x03, 0xad, 0x13, 0x07, 0x85, 0x44, 0x8d, 0x05, 0x03, 0xad, 0x10, 0x07, 0xac, 0x0f, 0x07, 0x18, 
0xae, 0x0e, 0x07, 0x20, 0x6c, 0x07, 0x30, 0x17, 0xac, 0x11, 0x07, 0xb1, 0x43, 0x29, 0x03, 0x48, 
0xc8, 0x11, 0x43, 0xf0, 0x0e, 0xb1, 0x43, 0xa8, 0x20, 0x57, 0x07, 0x68, 0x4c, 0x2f, 0x07, 0xa9, 
0xc0, 0xd0, 0x01, 0x68, 0x0a, 0xa8, 0x60, 0x18, 0xa5, 0x43, 0x6d, 0x11, 0x07, 0x8d, 0x04, 0x03, 
0x85, 0x43, 0xa5, 0x44, 0x69, 0x00, 0x8d, 0x05, 0x03, 0x85, 0x44, 0x60, 0x8d, 0x0b, 0x03, 0x8c, 
0x0a, 0x03, 0xa9, 0x52, 0xa0, 0x40, 0x90, 0x04, 0xa9, 0x57, 0xa0, 0x80, 0x8d, 0x02, 0x03, 0x8c, 
0x03, 0x03, 0xa9, 0x31, 0xa0, 0x0f, 0x8d, 0x00, 0x03, 0x8c, 0x06, 0x03, 0xa9, 0x03, 0x8d, 0xff, 
0x12, 0xa9, 0x00, 0xa0, 0x80, 0xca, 0xf0, 0x04, 0xa9, 0x01, 0xa0, 0x00, 0x8d, 0x09, 0x03, 0x8c, 
0x08, 0x03, 0x20, 0x59, 0xe4, 0x10, 0x1d, 0xce, 0xff, 0x12, 0x30, 0x18, 0xa2, 0x40, 0xa9, 0x52, 
0xcd, 0x02, 0x03, 0xf0, 0x09, 0xa9, 0x21, 0xcd, 0x02, 0x03, 0xf0, 0x02, 0xa2, 0x80, 0x8e, 0x03, 
0x03, 0x4c, 0xa2, 0x07, 0xae, 0x01, 0x13, 0xad, 0x03, 0x03, 0x60, 0xaa, 0x08, 0x14, 0x0b, 0xbe, 
0x0a, 0xcb, 0x09, 0x00, 0x0b, 0xa6, 0x0b, 0x07, 0x85, 0x44, 0xad, 0x0a, 0x07, 0x8d, 0xd6, 0x12, 
0xad, 0x0c, 0x07, 0x85, 0x43, 0xad, 0x0d, 0x07, 0x85, 0x44, 0xad, 0x0a, 0x07, 0x8d, 0x0c, 0x13, 
0xa2, 0x07, 0x8e, 0x0d, 0x13, 0x0e, 0x0c, 0x13, 0xb0, 0x0d, 0xa9, 0x00, 0x9d, 0x11, 0x13, 0x9d, 
0x29, 0x13, 0x9d, 0x31, 0x13, 0xf0, 0x36, 0xa0, 0x05, 0xa9, 0x00, 0x91, 0x43, 0xe8, 0x8e, 0x01, 
0x03, 0xa9, 0x53, 0x8d, 0x02, 0x03, 0x20, 0x53, 0xe4, 0xa0, 0x02, 0xad, 0xea, 0x02, 0x29, 0x20, 
0xd0, 0x01, 0x88, 0x98, 0xae, 0x0d, 0x13, 0x9d, 0x11, 0x13, 0xa5, 0x43, 0x9d, 0x29, 0x13, 0xa5, 
0x44, 0x9d, 0x31, 0x13, 0x20, 0x70, 0x08, 0x88, 0xf0, 0x03, 0x20, 0x70, 0x08, 0xca, 0x10, 0xb2, 
0xac, 0x09, 0x07, 0xa2, 0x00, 0xa9, 0x00, 0x88, 0x10, 0x01, 0x98, 0x9d, 0x19, 0x13, 0x98, 0x30, 
0x0d, 0xa5, 0x43, 0x9d, 0x39, 0x13, 0xa5, 0x44, 0x9d, 0x49, 0x13, 0x20, 0x70, 0x08, 0xe8, 0xe0, 
0x10, 0xd0, 0xe2, 0xa5, 0x43, 0x8d, 0xe7, 0x02, 0xa5, 0x44, 0x8d, 0xe8, 0x02, 0x4c, 0x7e, 0x08, 
0x18, 0xa5, 0x43, 0x69, 0x80, 0x85, 0x43, 0xa5, 0x44, 0x69, 0x00, 0x85, 0x44, 0x60, 0xa0, 0x7f, 
   },
   [ATR_MYDOS] = {
0x4d, 0x03, 0x00, 0x07, 0xe0, 0x07, 0x4c, 0x14, 0x07, 0x03, 0xff, 0x01, 0xe9, 0x1b, 0x02, 0x04, 
0x00, 0xfd, 0x15, 0x0b, 0xac, 0x12, 0x07, 0xad, 0x13, 0x07, 0x20, 0x58, 0x07, 0xad, 0x10, 0x07, 
0xac, 0x0f, 0x07, 0x18, 0xae, 0x0e, 0x07, 0xf0, 0x1d, 0x20, 0x63, 0x07, 0x30, 0x18, 0xac, 0x11, 
0x07, 0xb1, 0x43, 0x29, 0xff, 0x48, 0xc8, 0x11, 0x43, 0xf0, 0x0e, 0xb1, 0x43, 0x48, 0x20, 0x4d, 
0x07, 0x68, 0xa8, 0x68, 0x90, 0xdd, 0xa9, 0xc0, 0xa0, 0x68, 0x0a, 0xa8, 0x60, 0xad, 0x11, 0x07, 
0x18, 0x65, 0x43, 0xa8, 0xa5, 0x44, 0x69, 0x00, 0x84, 0x43, 0x85, 0x44, 0x8c, 0x04, 0x03, 0x8d, 
0x05, 0x03, 0x60, 0x8d, 0x0b, 0x03, 0x8c, 0x0a, 0x03, 0xa0, 0x03, 0xa9, 0x52, 0x90, 0x03, 0xad, 
0x79, 0x07, 0x84, 0x48, 0x8d, 0x02, 0x03, 0x18, 0xa9, 0x57, 0x8c, 0x06, 0x03, 0xa9, 0x80, 0xca, 
0xf0, 0x0d, 0xae, 0x0b, 0x03, 0xd0, 0x07, 0xae, 0x0a, 0x03, 0xe0, 0x04, 0x90, 0x01, 0x0a, 0x8d, 
0x08, 0x03, 0x2a, 0x8d, 0x09, 0x03, 0xa0, 0x31, 0x8c, 0x00, 0x03, 0xc6, 0x48, 0x30, 0x16, 0xae, 
0x02, 0x03, 0xe8, 0x8a, 0xa2, 0x40, 0x29, 0x06, 0xd0, 0x02, 0xa2, 0x80, 0x8e, 0x03, 0x03, 0x20, 
0x59, 0xe4, 0x88, 0x30, 0xe6, 0xa6, 0x2e, 0xc8, 0x98, 0x60, 0x10, 0x71, 0x01, 0x00, 0x80, 0xf6, 
0x23, 0x28, 0x50, 0x4d, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x12, 0xd2, 0xd2, 
0xd2, 0xd2, 0xd2, 0xd2, 0x5c, 0x0c, 0x5c, 0x0e, 0x62, 0x0d, 0xc6, 0x0d, 0x50, 0x0e, 0x67, 0x10, 
0xa9, 0x69, 0x8d, 0xbb, 0x07, 0xa9, 0x01, 0x8d, 0xbc, 0x07, 0xa2, 0x08, 0x8e, 0x01, 0x03, 0x20, 
0xb6, 0x0b, 0xbd, 0xcb, 0x07, 0x30, 0x1d, 0x20, 0x9a, 0x0b, 0xf0, 0x18, 0xa0, 0x09, 0xb9, 0x25, 
0x0b, 0x99, 0x02, 0x03, 0x88, 0x10, 0xf7, 0xbd, 0xcb, 0x07, 0xc9, 0x40, 0xb0, 0x06, 0xbc, 0xc3, 
0x07, 0x20, 0x2f, 0x0b, 0xca, 0xd0, 0xd5, 0xa0, 0xae, 0x8a, 0x99, 0x60, 0x08, 0x88, 0xd0, 0xfa, 
0xee, 0x64, 0x08, 0xad, 0x0c, 0x07, 0x8d, 0xe7, 0x02, 0xac, 0x0d, 0x07, 0xa2, 0x0f, 0xec, 0x09, 
0x07, 0x90, 0x05, 0xde, 0xe8, 0x08, 0x30, 0x05, 0x98, 0x9d, 0xf8, 0x08, 0xc8, 0xca, 0x10, 0xee, 
0x8c, 0xe8, 0x02, 0xe8, 0xe8, 0xe8, 0xbd, 0x18, 0x03, 0xf0, 0x04, 0xc9, 0x44, 0xd0, 0xf4, 0xa9, 
0x44, 0x9d, 0x18, 0x03, 0xa9, 0xd4, 0x9d, 0x19, 0x03, 0xa9, 0x07, 0x9d, 0x1a, 0x03, 0x4c, 0x8c, 
0x1a, 0x00, 0x00, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x80, 0xfd, 0x00, 0x03, 0x04, 0x00, 
   },
};
/*
 * Functions
 */

/*
 * dos_dirent_sanity()
 *
 * Return:
 *    0 - good
 *    1 - good and never allocated
 *   neg - bad
 */
int dos_dirent_sanity(const struct dos2_dirent *dirent)
{
   struct dos2_dirent zero;
   memset(&zero,0,sizeof(zero));
   if ( atrfs_memcmp(&zero,dirent,sizeof(zero)) == 0 ) return 1;
   if ( (dirent->flags & FLAGS_DELETED) && (dirent->flags & ~FLAGS_DELETED) ) return -1;
   if ( dirent->flags == 0 ) return -1; // Not unless full directory is empty
   if ( (dirent->flags & FLAGS_UNDEF) ) return -1; // Should never be set
   if ( BYTES2(dirent->sectors) > atrfs.sectors ) return -1; // File too large to exist
   if ( BYTES2(dirent->start) > atrfs.sectors ) return -1; // File starts past end of image
   return 0;
}

/*
 * dos_root_dir_sanity()
 */
int dos_root_dir_sanity(void)
{
   int at_end=0;
   int r;

   for ( int i=0;i<64;++i )
   {
      struct dos2_dirent *dirent;
      dirent = SECTOR(361);
      r = dos_dirent_sanity(&dirent[DIRENT_ENTRY(i)]);
      if ( r < 0 ) return r;
      if ( r == 0 && at_end ) return -1;
      if ( r == 1 ) at_end = 1;
   }
   return 0;
}

/*
 * mydos_sanity()
 *
 * Return 0 if this is a valid MyDOS file system
 */
int mydos_sanity(void)
{
   if ( atrfs.sectors < 368 ) return 1; // Must have the root directory
   if ( atrfs.sectorsize > 256 ) return 1; // No support for 512-byte sectors
   struct sector1 *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 3 ) return 1; // Must have 3 boot sectors
   if ( sec1->pad_zero == 'X' || sec1->pad_zero == 'S' ) return 1; // Flagged as DOS XE or SpartaDOS
   struct mydos_vtoc *vtoc = SECTOR(360);
   if ( BYTES2(vtoc->total_sectors) < BYTES2(vtoc->free_sectors) ) return 1; // free must not exceed total
   if ( dos_root_dir_sanity() ) return 1; // Root directory not sane

   if ( !BYTES2(vtoc->total_sectors) ) return 1; // Must have some sectors
   if ( vtoc->bitmap[0]&0xf0 ) return 1; // sectors 0-4 are always used
   for (int i=360;i<=368;++i) if ( BITMAP(vtoc->bitmap,i) ) return 1; // VTOC and directory used

   int vtoc_code = 2; // Small images: 2 matches DOS 2 for one sector
   int vtoc_sectors = 1;
   if ( atrfs.sectors > 943 )
   {
      // Add 1 for each 256-bytes needed (single-density sectors added in pairs)
      const int vtoc_bytes = (atrfs.sectors - 943 + 7)/8;
      vtoc_code += (vtoc_bytes + 255) / 256;
   }
   if ( atrfs.sectorsize == 128 ) vtoc_sectors = vtoc_code*2-3;
   else vtoc_sectors=vtoc_code-1;

   // Checks for which we will print warnings but not abort
   if ( vtoc->vtoc_sectors != vtoc_code ) // VTOC Code for number of sectors doesn't match
   {
      fprintf(stderr,"Warning: MyDOS VTOC sector code should be %d, observed %d (VTOC %d sectors of %d bytes)\n",vtoc_code,vtoc->vtoc_sectors,vtoc_sectors,atrfs.sectorsize);
      // return 1; // Don't abort; some disks are wrong.
   }
   int reserved_sectors = 3+vtoc_sectors+8; // 3 boot sectors, VTOC, main directory
   if ( atrfs.sectors >= 720 ) ++reserved_sectors; // sector 720 is skipped unless it's a short image
   if ( BYTES2(vtoc->total_sectors) != atrfs.sectors - reserved_sectors )
   {
      fprintf(stderr,"Warning: MyDOS total sectors reported %d; should be %d - %d = %d\n",BYTES2(vtoc->total_sectors), atrfs.sectors, reserved_sectors, atrfs.sectors - reserved_sectors);
      // return 1; // Don't abort; some disks are wrong.
   }
   // We could add up the free sectors in the bitmap and validate the free count; probably overkill as this isn't fsck.
   return 0;
}

/*
 * dos1_sanity()
 *
 * Return 0 if this is a valid DOS 1 file system
 */
int dos1_sanity(void)
{
   if ( atrfs.sectors < 368 ) return 1; // Must have the root directory
   if ( atrfs.sectors > 720 ) return 1;
   if ( atrfs.sectorsize > 128 ) return 1;
   struct sector1 *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 1 ) return 1; // Must have 1 boot sector!
   struct mydos_vtoc *vtoc = SECTOR(360);
   if ( BYTES2(vtoc->total_sectors) < BYTES2(vtoc->free_sectors) ) return 1; // free must not exceed total
   if ( dos_root_dir_sanity() ) return 1; // Root directory not sane

   if ( vtoc->vtoc_sectors != 1 ) return 1; // Code is 1 for DOS 1
   int reserved_sectors = 1+1+8+1;
   
   if ( !BYTES2(vtoc->total_sectors) ) return 1; // Must have some sectors
   if ( BYTES2(vtoc->total_sectors) != atrfs.sectors - reserved_sectors )
   {
      fprintf(stderr,"Warning: DOS total sectors reported %d; should be %d - %d = %d\n",BYTES2(vtoc->total_sectors), atrfs.sectors, reserved_sectors, atrfs.sectors - reserved_sectors);
      // return 1;
   }
   if ( vtoc->bitmap[0]&0xc0 ) return 1; // sectors 0-1 are always used
   for (int i=360;i<=368;++i) if ( BITMAP(vtoc->bitmap,i) ) return 1; // VTOC and directory used

   if ( sec1->drive_bits == 0xff ) return 1; // Support for all 8 drives suggests MyDOS

   // We could scan the directory for MyDOS subdirectories
   return 0;
}

/*
 * dos2_sanity()
 *
 * Return 0 if this is a valid DOS 2.0s file system
 */
int dos2_sanity(void)
{
   if ( atrfs.sectors < 368 ) return 1; // Must have the root directory
   if ( atrfs.sectors > 720 ) return 1;
   if ( atrfs.sectorsize > 128 ) return 1; // DOS 2.0d separate
   struct sector1 *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 3 ) return 1; // Must have 3 boot sectors
   if ( sec1->pad_zero == 'X' || sec1->pad_zero == 'S' ) return 1; // Flagged as DOS XE or SpartaDOS
   struct mydos_vtoc *vtoc = SECTOR(360);
   if ( BYTES2(vtoc->total_sectors) < BYTES2(vtoc->free_sectors) ) return 1; // free must not exceed total
   if ( dos_root_dir_sanity() ) return 1; // Root directory not sane

   if ( vtoc->vtoc_sectors != 2 ) return 1; // Code is 2 for DOS 2
   int reserved_sectors = 3+1+8+1; // boot; vtoc; dir(8); 720
   
   if ( !BYTES2(vtoc->total_sectors) ) return 1; // Must have some sectors
   if ( BYTES2(vtoc->total_sectors) != atrfs.sectors - reserved_sectors )
   {
      fprintf(stderr,"Warning: DOS total sectors reported %d; should be %d - %d = %d\n",BYTES2(vtoc->total_sectors), atrfs.sectors, reserved_sectors, atrfs.sectors - reserved_sectors);
      // return 1;
   }
   if ( vtoc->bitmap[0]&0xf0 ) return 1; // sectors 0-4 are always used
   for (int i=360;i<=368;++i) if ( BITMAP(vtoc->bitmap,i) ) return 1; // VTOC and directory used

   if ( sec1->drive_bits == 0xff ) return 1; // Support for all 8 drives suggests MyDOS

   // We could scan the directory for MyDOS subdirectories
   return 0;
}

/*
 * dos25_sanity()
 *
 * Return 0 if this is a valid DOS 2.5 file system
 */
int dos25_sanity(void)
{
   if ( atrfs.sectors < 1024 ) return 1; // Need sector 1024 for VTOC 2
   if ( atrfs.sectors > 1040 ) return 1; // No large drive support
   if ( atrfs.sectorsize > 128 ) return 1; // No DD support
   struct sector1 *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 3 ) return 1; // Must have 3 boot sectors
   if ( sec1->pad_zero == 'X' || sec1->pad_zero == 'S' ) return 1; // Flagged as DOS XE or SpartaDOS
   struct mydos_vtoc *vtoc = SECTOR(360);
   if ( BYTES2(vtoc->total_sectors) < BYTES2(vtoc->free_sectors) ) return 1; // free must not exceed total
   if ( dos_root_dir_sanity() ) return 1; // Root directory not sane

   if ( vtoc->vtoc_sectors != 2 ) return 1; // Code is 2 for DOS 2
   int reserved_sectors = 1+3+1+8+1; // Counts sector zero
   
   if ( !BYTES2(vtoc->total_sectors) ) return 1; // Must have some sectors
   int total_sec = atrfs.sectors;
   if ( total_sec > 1024 ) total_sec = 1024;
   if ( BYTES2(vtoc->total_sectors) != total_sec - reserved_sectors )
   {
      fprintf(stderr,"Warning: DOS 2.5 total sectors reported %d; should be %d - %d = %d\n",BYTES2(vtoc->total_sectors), total_sec, reserved_sectors, total_sec - reserved_sectors);
      // return 1;
   }
   if ( vtoc->bitmap[0]&0xf0 ) return 1; // sectors 0-4 are always used
   for (int i=360;i<=368;++i) if ( BITMAP(vtoc->bitmap,i) ) return 1; // VTOC and directory used

   // Check VTOC2
   unsigned char *vtoc2 = SECTOR(1024);
   int upper_free = BYTES2( (vtoc2+122) );
   if ( upper_free > 304 ) return 1;
   int free_count=0;
   for (int i=84;i<=121;++i)
   {
      unsigned char m = vtoc2[i];
      while (m)
      {
         free_count += (m&1);
         m>>=1;
      }
   }
   if ( free_count != upper_free ) return 1; // Bitmap doesn't match free count
   return 0;
}

/*
 * mydos_trace_file()
 *
 * Trace through a file.
 * If *sectors is non-NULL, malloc an array of ints for the chain of sector numbers, ending with a zero
 * If fileno is 0-63, expect the fileno to be at the end of the sectors (DOS 2 mode).
 * Return 0 on success, non-zero if chain fails, but array is still valid as far as the file is readable.
 */
int mydos_trace_file(int sector,int fileno,int dos1,int *size,int **sectors)
{
   unsigned char *s;
   int r=0;
   int block=0;

   *size = 0;
   if ( sectors ) *sectors = NULL;

   while (1)
   {
      if ( sector > atrfs.sectors )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: block %d sector %d max %d (fileno %d)\n",__FUNCTION__,block,sector,atrfs.sectors,fileno);
         r = 1;
         break;
      }
      s=SECTOR(sector);
      int next = s[atrfs.sectorsize-3] * 256 + s[atrfs.sectorsize-2]; // Reverse order from normal
      if ( fileno >=0 && fileno < 64 )
      {
         next &= 0x3ff; // Mask off file number
         if ( fileno != s[atrfs.sectorsize-3]>>2 ) // File number mismatch; don't use the block
         {
            r=164;
            if ( options.debug ) fprintf(stderr,"DEBUG: %s: block %d sector %d fileno mismatch %d should be %d\n",__FUNCTION__,block,sector,s[atrfs.sectorsize-3]>>2,fileno);
            break;
         }
      }
      // If it's DOS 1, the last byte is the sector sequence of the file starting with zero
      // Verified with testing: Large files mask off the high bit, so it can wrap
      if ( dos1 )
      {
         if ( next )
         {
            *size += 125;
            if ( s[atrfs.sectorsize-1] != (block & 0x7f) )
            {
               if ( options.debug ) fprintf(stderr,"DEBUG: %s: DOS 1 sector chain error: sector %d sequence number %d but expected %d\n",__FUNCTION__,sector,s[atrfs.sectorsize-1],block);
            }
         }
         else
         {
            *size += (s[atrfs.sectorsize-1] & 0x7f);
            if ( !(s[atrfs.sectorsize-1] & 0x80) )
            {
               if ( options.debug ) fprintf(stderr,"DEBUG: %s: DOS 1 file error: Last sector should have high bit set on last byte: $%02x\n",__FUNCTION__,s[atrfs.sectorsize-1]);
            }
         }
      }
      else
      {
         *size += s[atrfs.sectorsize-1];
      }
      ++block;
      if ( sectors )
      {
         *sectors = realloc(*sectors,sizeof(int)*(block+1));
         (*sectors)[block-1] = sector;
         (*sectors)[block] = 0;
      }
      if ( next==0 ) break;
      sector = next;
      if ( block > atrfs.sectors )
      {
         fprintf(stderr,"Corrupted file number %d has a circular list of sectors\n",fileno);
         if ( sectors )
         {
            free(*sectors);
            *sectors=NULL;
         }
         return -EIO;
      }
   }
   return r;
}

/*
 * mydos_remove_fileno()
 *
 * Remove the file number from the ends of the sectors (if there)
 */
int mydos_remove_fileno(struct dos2_dirent *dirent,int fileno)
{
   int *sectors;
   int r,size;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s %8.8s.%3.3s fileno %d sector %d count %d flags %02x\n",__FUNCTION__,dirent->name,dirent->ext,fileno,BYTES2(dirent->start),BYTES2(dirent->sectors),dirent->flags);
   if ( (dirent->flags & FLAGS_NOFILENO) ) return 0; // Already removed
   r = mydos_trace_file(BYTES2(dirent->start),fileno,0 /* don't care */,&size,&sectors);
   if ( r )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s %8.8s.%3.3s fileno %d trace error return %d\n",__FUNCTION__,dirent->name,dirent->ext,fileno,r);
      return r;
   }
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %8.8s.%3.3s fileno %d\n",__FUNCTION__,dirent->name,dirent->ext,fileno);
   
   dirent->flags |= FLAGS_NOFILENO; // Flag no file numbers
   int *s = sectors;
   while ( *s )
   {
      char *buf = SECTOR(*s);
      buf[atrfs.sectorsize-3] &= 0x03; // Mask off the file number
      ++s;
   }
   free(sectors);
   return 0;
}

/*
 * mydos_add_fileno()
 *
 * Add the file number from the ends of the sectors (if not already there)
 */
int mydos_add_fileno(struct dos2_dirent *dirent,int fileno)
{
   int *sectors,*s;
   int r,size;

   if ( !( dirent->flags & FLAGS_NOFILENO ) ) return 0; // Already there
   if ( dirent->flags & FLAGS_DIR ) return 1; // Directory; can't add fileno
   r = mydos_trace_file(BYTES2(dirent->start),-1,0 /* don't care */,&size,&sectors);
   if ( r ) return r;
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %8.8s.%3.3s fileno %d\n",__FUNCTION__,dirent->name,dirent->ext,fileno);

   // Make sure adding file numbers is possible
   s = sectors;
   while ( *s )
   {
      char *buf = SECTOR(*s);
      if ( buf[atrfs.sectorsize-3]>>2 )
      {
         free(sectors);
         if ( options.debug ) fprintf(stderr,"DEBUG: %s %8.8s.%3.3s can't add fileno due to sector %d\n",__FUNCTION__,dirent->name,dirent->ext,((buf[atrfs.sectorsize-3]>>2)<<8)|buf[atrfs.sectorsize-2]);
         return 1; // Nope!
      }
      ++s;
   }

   // Add file numbers
   dirent->flags &= ~FLAGS_NOFILENO; // Flag file numbers
   s=sectors;
   while ( *s )
   {
      char *buf = SECTOR(*s);
      buf[atrfs.sectorsize-3] &= 0x03; // Mask off the file number (should be a no-op)
      buf[atrfs.sectorsize-3] |= fileno << 2; // Add file number
      ++s;
   }
   free(sectors);
   return 0;
}

/*
 * mydos_path()
 *
 * Given a path, return the starting sector number.
 * Also valid for DOS 2.0 and 2.5
 * 
 * Return value:
 *   0 File found
 *   1 File not found, but could be created in parent_dir
 *   -x return this error (usually -ENOENT)
 */
int mydos_path(const char *path,int *sector,int *parent_dir_sector,int *count,int *locked,int *fileno,int *entry,int *isdir,int *isinfo)
{
   unsigned char name[8+3+1]; // 8+3+NULL
   struct dos2_dirent *dirent;

   if ( !*sector )
   {
      *parent_dir_sector = 361;
      *sector = 361;
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s default dir %d\n",__FUNCTION__,path,*parent_dir_sector);
   }
   else
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s recursing on dir %d\n",__FUNCTION__,path,*parent_dir_sector);
   }
   *entry = -1;
   *count = 0;
   *isdir=0;
   *isinfo=0;
   *fileno = -1;
   *locked=0;
   while ( *path )
   {
      while ( *path == '/' ) ++path;
      if ( ! *path )
      {
         *isdir = 1;
         return 0;
      }
      if ( atrfs_strcmp(path,".info")==0 )
      {
         *isinfo = 1;
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
      if ( atrfs_strncmp(path,".info",5)==0 )
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
      if ( atrfs_strncmp(path,".info",5)==0 )
      {
         *isinfo=1;
         path += 5;
      }
      if ( *path && *path != '/' )
      {
         return -ENOENT; // Name too long
      }
      if ( *path == '/' ) ++path;
      
      dirent=SECTOR(*sector);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: Look for %11.11s in dir at sector %d\n",__FUNCTION__,name,*sector);
      for ( i=0;i<64;++i )
      {
         const int j=DIRENT_ENTRY(i);

         // Scan parent_dir_sector for file name
         //if ( options.debug ) fprintf(stderr,"DEBUG: %s: entry %d:%d %11.11s flags %02x\n",__FUNCTION__,i,j,dirent[j].name,dirent[j].flags);
         if ( dirent[j].flags == 0 ) break;
         if ( dirent[j].flags & FLAGS_DELETED ) continue;
         if ( atrfs_strncmp((char *)dirent[j].name,(char *)name,8+3) != 0 ) continue;
         *entry = i;
         // subdirectories: MyDOS only, but MyDOS could add them on any compatible disk except DOS 2.5
         //if ( atrfs.fstype == ATR_MYDOS )
         if ( ! *isinfo && atrfs.fstype != ATR_DOS25 )
         {
            if ( dirent[j].flags & FLAGS_DIR )
            {
               if ( *path )
               {
                  *parent_dir_sector = *sector;
                  *sector = BYTES2(dirent[j].start);
                  if ( options.debug ) fprintf(stderr,"DEBUG: %s: recurse in with dir %d path %s\n",__FUNCTION__,*parent_dir_sector,path);
                  return mydos_path(path,sector,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo);
               }
               *isdir = 1;
               *sector = BYTES2(dirent[j].start);
               *count = BYTES2(dirent[j].sectors);
               return 0;
            }
         }
         if ( *path ) return -ENOTDIR; // Should have been a directory
         *parent_dir_sector = *sector;
         *sector = BYTES2(dirent[j].start);
         *count = BYTES2(dirent[j].sectors);
         if ( dirent[j].flags & FLAGS_LOCKED ) *locked=1;
         if ( (dirent[j].flags & FLAGS_NOFILENO) == 0 ) *fileno=i;
         return 0;
      }
      if ( *path ) return -ENOENT; // Not found in directory scan
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: Did not find %11.11s in dir at sector %d; could create\n",__FUNCTION__,name,*sector);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: set dir sector %d\n",__FUNCTION__,*parent_dir_sector);
      *parent_dir_sector = *sector;
      *sector = 0;
      // In theory could be created here if there's an empty spot
      for ( i=0;i<64;++i )
      {
         const int j=DIRENT_ENTRY(i);
         if ( dirent[j].flags == 0 || (dirent[j].flags & FLAGS_DELETED) )
         {
            *entry = i;
            return 1;
         }
      }
      *entry = -1; // Invalid
      return 1;
   }
   return -ENOENT; // Shouldn't be reached
}

/*
 * mydos_bitmap_status()
 *
 * Return the bit from the bitmap for the sector (0 if allocated)
 */
int mydos_bitmap_status(int sector)
{
   int vtoc=360;
   int bitmap_start=0;
   int bitmap_offset=10;

   switch (atrfs.fstype)
   {
      case ATR_DOS1:
      case ATR_DOS2:
         break;
      case ATR_DOS25:
         if ( sector >= 720 )
         {
            vtoc=1024;
            bitmap_start = 48;
         }
         break;
      case ATR_MYDOS:
         if ( sector > 943 )
         {
            vtoc = 359 - ((sector-944)/8/atrfs.sectorsize);
            bitmap_start = ((sector-944)/8/atrfs.sectorsize) * 8;
            bitmap_offset = 0;
         }
         break;
      default: return -1; // Impossible
   }

   int sec_bitmap = sector - bitmap_start + 8 * bitmap_offset; // Sector relative to bitmap at byte zero of 'bitmap_start'
   unsigned char mask = 1<<(7-(sector & 0x07));
   unsigned char *map = SECTOR(vtoc);
   return map[sec_bitmap/8] & mask;
}

/*
 * mydos_bitmap()
 *
 * Set or clear bitmap for a given sector.
 * If 'allocate' is non-zero, change from free to used; otherwise reverse
 * Return 0 if changed, 1 if already at target value.
 */
int mydos_bitmap(int sector,int allocate)
{
   int vtoc,vtoc2=0;
   int bitmap_start=0,bitmap2_start;
   int bitmap_offset=10;

   switch (atrfs.fstype)
   {
      case ATR_DOS1:
      case ATR_DOS2:
         vtoc=360;
         break;
      case ATR_DOS25:
         if ( sector < 720 )
         {
            vtoc=360;
            if ( sector >= 48 )
            {
               vtoc2=1024;
               bitmap2_start = 48;
            }
         }
         else
         {
            vtoc=1024;
            bitmap_offset = 0;
            bitmap_start = 48;
         }
         break;
      case ATR_MYDOS:
         if ( sector <= 943 )
         {
            vtoc=360;
         }
         else
         {
            vtoc = 359 - ((sector-944)/8/atrfs.sectorsize);
            bitmap_start = 944 + ((sector-944)/8/atrfs.sectorsize) * 8*atrfs.sectorsize;
            bitmap_offset = 0;
         }
         break;
      default: return -1; // Impossible
   }

   int sec_bitmap = sector - bitmap_start + 8 * bitmap_offset; // Sector relative to bitmap at byte zero of 'bitmap_start'
   unsigned char mask = 1<<(7-(sector & 0x07));
   unsigned char *map = SECTOR(vtoc);
   unsigned char value = map[sec_bitmap/8] & mask;
   if ( allocate )
   {
      map[sec_bitmap/8] &= ~mask; // Set to zero (used)
   }
   else
   {
      map[sec_bitmap/8] |= mask; // Set to one (free)
   }
   // Also change in DOS 2.5 second bitmap
   // Don't worry if it's not the same; legal if DOS 2 modified the files.
   // Really, this duplicate copy is stupid and should never have been
   // there; I think it's write only, but might as well follow the spec.
   if ( vtoc2 )
   {
      map = SECTOR(vtoc2);
      sec_bitmap = sector - bitmap2_start;
      if ( allocate )
      {
         map[sec_bitmap/8] &= ~mask; // Set to zero (used)
      }
      else
      {
         map[sec_bitmap/8] |= mask; // Set to one (free)
      }
   }

   if ( allocate && value ) return 0;
   if ( !allocate && !value ) return 0;
   return 1;
}

/*
 * mydos_free_sector()
 */
int mydos_free_sector(int sector)
{
   int r;
   r = mydos_bitmap(sector,0);

   // Only update the free sector count if the bitmap was modified
   int vtoc = 360;
   int offset = 3;
   if ( r == 0 )
   {
      if ( atrfs.fstype == ATR_DOS25 && sector >= 720 )
      {
         vtoc=1024;
         offset = 122;
      }

      unsigned char *free_count = SECTOR(vtoc)+offset;
      if ( free_count[0] < 0xff ) ++free_count[0];
      else
      {
         ++free_count[1];
         free_count[0] = 0;
      }
   }
   return r;
}

/*
 * mydos_alloc_sector()
 */
int mydos_alloc_sector(int sector)
{
   int r;
   r = mydos_bitmap(sector,1);

   // Only update the free sector count if the bitmap was modified
   int vtoc = 360;
   int offset = 3;
   if ( r == 0 )
   {
      if ( atrfs.fstype == ATR_DOS25 && sector >= 720 )
      {
         vtoc=1024;
         offset = 122;
      }

      unsigned char *free_count = SECTOR(vtoc)+offset;
      if ( free_count[0] ) --free_count[0];
      else if ( free_count[1] )
      {
         --free_count[1];
         free_count[0] = 0xff;
      }
   }
   return r;
}

int mydos_alloc_any_sector(void)
{
   int r;
   for ( int i=2;i<=atrfs.sectors; ++i )
   {
      r=mydos_alloc_sector(i);
      if ( r==0 ) return i;
   }
   return -1;
}

/*
 * mydos_info()
 *
 * Return the buffer containing information specific to this file.
 *
 * This could be extended to do a full analysis of binary load files,
 * BASIC files, or any other recognizable file type.
 *
 * The pointer returned should be 'free()'d after use.
 */
char *mydos_info(const char *path,struct dos2_dirent *dirent,int parent_dir_sector,int entry,int sector,int *sectors,int filesize)
{
   char *buf,*b;

   buf = malloc(64*1024);
   b = buf;
   *b = 0;
   b+=sprintf(b,"File information and analysis\n\n  %.*s\n  %d bytes\n\n",(int)(strrchr(path,'.')-path),path,filesize);
   if ( dirent )
   {
      b+=sprintf(b,
                 "Directory entry internals:\n"
                 "  Entry %d in directory at sector %d\n"
                 "  Flags: $%02x\n"
                 "  Sectors: %d\n"
                 "  Starting Sector: %d\n\n",
                 entry,parent_dir_sector,
                 dirent->flags,BYTES2(dirent->sectors),BYTES2(dirent->start));
   }
   if ( !sectors )
   {
      b+=sprintf(b,"This is a directory\n");
      b+=sprintf(b,"Sectors: %d -- %d\n",sector,sector+7);
   }
   else
   {
      b+=sprintf(b,"Sector chain:\n");
      int *s = sectors;
      int prev = -1,pprint = -1;
      while ( *s )
      {
         if ( *s == prev+1 )
         {
            ++prev;
            ++s;
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
         b+=sprintf(b,"  %d",*s);
         prev = *s;
         pprint = *s;
         ++s;
      }
      if ( prev > 0 && pprint != prev )
      {
         b+=sprintf(b," -- %d",prev);
      }
      b+=sprintf(b,"\n\n");
   }

   // Generic info for the file type, but only if it's not a directory
   if ( sectors )
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
 * mydos_readdir()
 *
 * Also use for dos2 and dos25
 */
int mydos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)offset; // FUSE will always read directories from the start in our use
   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;

   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( !isdir ) return -ENOTDIR;

   // Read directory at: sector
   struct dos2_dirent *dirent=SECTOR(sector);
   char name[8+1+3+1];
   for ( int i=0;i<64;++i )
   {
      int j=i + ((atrfs.sectorsize == 256 ) ? i/8 * 8 : 0 ); // Use 8 128-byte sectors, even on DD

      if ( dirent[j].flags == 0 ) break;
      if ( dirent[j].flags & FLAGS_DELETED ) continue; // Deleted

      memcpy(name,dirent[j].name,8);
      int k;
      for (k=0;k<8;++k)
      {
         if ( dirent[j].name[k] == ' ' ) break;
      }
      name[k]=0;
      if ( dirent[j].ext[0] != ' ' )
      {
         name[k]='.';
         ++k;
         for (int l=0;l<3;++l)
         {
            if ( dirent[j].ext[l] == ' ' ) break;
            name[k+l]=dirent[j].ext[l];
            name[k+l+1]=0;
         }
      }
      filler(buf, name, FILLER_NULL);
   }
   return 0;
}

/*
 * mydos_getattr()
 */

int mydos_getattr(const char *path, struct stat *stbuf)
{
   // stbuf initialized from atr image stats

   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s lookup %d\n",__FUNCTION__,path,r);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;

   struct dos2_dirent *dirent = NULL;
   if ( entry >= 0 )
   {
      dirent = SECTOR(parent_dir_sector);
      dirent += DIRENT_ENTRY(entry);
   }

   if ( isinfo )
   {
      stbuf->st_mode = MODE_RO(stbuf->st_mode); // These files are never writable
      stbuf->st_ino = sector + 0x10000;

      int filesize = atrfs.sectorsize*8,*sectors = NULL; // defaults for directories
      if ( dirent && !(dirent->flags & FLAGS_DIR) )
      {
         r = mydos_trace_file(sector,fileno,(dirent->flags & FLAGS_DOS2) == 0,&filesize,&sectors);
         if ( r<0 ) return r;
      }

      char *info = mydos_info(path,dirent,parent_dir_sector,entry,sector,sectors,filesize);     
      stbuf->st_size = strlen(info);
      free(info);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s info\n",__FUNCTION__,path);
      return 0;
   }
   if ( isdir )
   {
      ++stbuf->st_nlink;
      stbuf->st_mode = MODE_DIR(atrfs.atrstat.st_mode & 0777);
      stbuf->st_size = 8*atrfs.sectorsize;
   }
   else
   {
      int s;
      r = mydos_trace_file(sector,fileno,(dirent->flags & FLAGS_DOS2) == 0,&s,NULL);
      stbuf->st_size = s;
   }
   if ( locked ) stbuf->st_mode = MODE_RO(stbuf->st_mode);
   stbuf->st_ino = sector; // We don't use this, but it's fun info
   stbuf->st_blocks = count;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s mode 0%o\n",__FUNCTION__,path,stbuf->st_mode);
   return 0;
}

int mydos_read(const char *path, char *buf, size_t size, off_t offset)
{
   int r,sector=0,parent_dir_sector,count,locked,fileno,entry,filesize,*sectors;
   int isdir,isinfo;
   unsigned char *s;
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;

   struct dos2_dirent *dirent=NULL;
   if ( entry >= 0 )
   {
      dirent = SECTOR(parent_dir_sector);
      dirent += DIRENT_ENTRY(entry);
   }

   if ( isinfo && dirent && (dirent->flags & FLAGS_DIR) )
   {
      sectors = NULL;
      filesize = 8 * atrfs.sectorsize;
   }
   else if ( !isinfo || dirent )
   {
      r = mydos_trace_file(sector,fileno,(dirent->flags & FLAGS_DOS2) == 0,&filesize,&sectors);

      if ( r<0 ) return r;
      int bad=0;
      if ( r>0 ) bad=1;
      if ( bad && options.debug ) fprintf(stderr,"DEBUG: %s %s File is bad (%d)\n",__FUNCTION__,path,r);
      r=0;
   }

   if ( isinfo )
   {
      if ( !dirent || dirent->flags & FLAGS_DIR )
      {
         sectors = NULL;
         filesize = 8 * atrfs.sectorsize;
      }
      char *info = mydos_info(path,dirent,parent_dir_sector,entry,sector,sectors,filesize);
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

   for ( int i=0;size && sectors[i]; ++i )
   {
      s=SECTOR(sectors[i]);
      int bytes_this_sector = s[atrfs.sectorsize-1];
      // If it's DOS 1, the last byte is the sector sequence of the file starting with zero except for the last sector
      if ( (dirent->flags & FLAGS_DOS2) == 0 )
      {
         if ( sectors[i+1] ) bytes_this_sector = 125;
         else bytes_this_sector = s[atrfs.sectorsize-1] & 0x7f;
      }
      if ( offset >= bytes_this_sector )
      {
         offset -= bytes_this_sector;
         continue;
      }
      // Want to read some data from this sector
      int bytes = bytes_this_sector;
      bytes -= offset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(buf,s+offset,bytes);
      buf+=bytes;
      offset=0;
      size-=bytes;
      r+=bytes;
   }
   free(sectors);
   if ( r == 0 && size ) r=-EOF;
   return r;
}

int mydos_write(const char *path, const char *buf, size_t size, off_t offset)
{
   int r,sector=0,parent_dir_sector,count,locked,fileno,entry,filesize,*sectors;
   int isdir,isinfo;
   unsigned char *s = NULL;
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -ENOENT; // These aren't writable

   struct dos2_dirent *dirent;
   dirent = SECTOR(parent_dir_sector);
   dirent += DIRENT_ENTRY(entry);

   r = mydos_trace_file(sector,fileno,(dirent->flags & FLAGS_DOS2) == 0,&filesize,&sectors);

   if ( r<0 ) return r;
   int bad=0;
   if ( r>0 ) bad=1;
   if ( bad )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s %s File is bad (%d)\n",__FUNCTION__,path,r);
      free(sectors);
      return -EIO;
   }

   // If start of DOS.SYS, check to see if the boot sectors match the version of DOS
   if ( offset == 0 && size > 16 && atrfs_strcmp(path,"/DOS.SYS") == 0 )
   {
      enum atrfstype dostype;
      switch ((unsigned char)(buf[0]))
      {
         case 0x3b:
            dostype = ATR_DOS1;
            break;
         case 0xaa:
            dostype = ATR_DOS2;
            // DOS.SYS for 2.0d is the same until byte 1408
            if ( atrfs.sectorsize == 256 ) dostype = ATR_DOS20D;
            break;
         case 0x64:
            dostype = ATR_DOS25;
            break;
         case 0x00:
            dostype = ATR_MYDOS;
            break;
         default:
            dostype = ATR_SPECIAL; // flag unknown
            break;
      }
      if ( dostype == ATR_SPECIAL )
      {
         // nothing to do; the code doesn't recognize this DOS.SYS; hope it works!
         if ( options.debug ) fprintf(stderr,"DEBUG: %s Writing unrecognized DOS.SYS file; hope it boots!\n",__FUNCTION__);
      }
      else if ( dostype == ATR_DOS1 && atrfs.fstype == ATR_DOS1 )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s Writing DOS 1 DOS.SYS; should boot correctly\n",__FUNCTION__);
      }
      if ( dostype == ATR_DOS1 )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s Writing DOS 1 DOS.SYS to non-DOS 1 image; not likely to boot\n",__FUNCTION__);
      }
      if ( atrfs.fstype == ATR_DOS1 )
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s Writing newer DOS.SYS to DOS 1 image; not likely to boot\n",__FUNCTION__);
      }
      else
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s Updating boot sectors to match DOS.SYS\n",__FUNCTION__);
         for (int i=1; i<=3; ++i)
         {
            unsigned char *s = SECTOR(i);
            memcpy(s,bootsectors[atrfs.fstype]+128*(i-1),128);
         }
         struct sector1 *sec1 = SECTOR(1);
         sec1->dos_flag = (atrfs.sectorsize == 128) ? 1 : 2;
         sec1->dos_sector[0] = sector & 0xff;
         sec1->dos_sector[1] = sector >> 8;
      }
   }

   // Handle overwriting existing file:
   r=0; // Count of bytes written
   int bytes;
   for ( int i=0;size && sectors[i]; ++i )
   {
      s=SECTOR(sectors[i]);
      // If it's DOS 1, the last byte is the sector sequence of the file starting with zero except for the last sector
      int bytes_this_sector = s[atrfs.sectorsize-1];
      if ( (dirent->flags & FLAGS_DOS2) == 0 )
      {
         if ( sectors[i+1] ) bytes_this_sector = 125;
         else bytes_this_sector = s[atrfs.sectorsize-1] & 0x7f;
      }
      if ( offset >= bytes_this_sector )
      {
         offset -= bytes_this_sector;
         continue;
      }
      // Want to write some data to this sector
      bytes = bytes_this_sector;
      bytes -= offset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(s+offset,buf,bytes);
      buf+=bytes;
      offset=0;
      size-=bytes;
      r+=bytes;
   }
   if ( !size ) return 0;

   // Handle writing at end of file
   // 'offset' is offset from end of file (from above)
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s Remove file numbers from file for extension\n",__FUNCTION__,path);
   mydos_remove_fileno(dirent,fileno);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s extend file %lu bytes\n",__FUNCTION__,path,size+offset);
   while ( size+offset )
   {
      // Fill last sector
      int bytes_this_sector = s[atrfs.sectorsize-1];
      if ( (dirent->flags & FLAGS_DOS2) == 0 ) bytes_this_sector &= 0x7f; // last sector; strip EOF bit
      bytes = atrfs.sectorsize-3 - bytes_this_sector;
      if ( (size_t)bytes > size+offset ) bytes = size+offset;
      if ( bytes )
      {
         if ( offset ) // Zero-fill to write past the end of the file
         {
            memset(s+bytes_this_sector,0,bytes);
            if ( bytes > offset ) // write 'offset' zeros
            {
               bytes -= offset;
               s[atrfs.sectorsize-1] += offset;
               bytes_this_sector += offset;
               offset = 0;
            }
            else // fill remaining bytes in sector with zeros
            {
               offset -= bytes;
               s[atrfs.sectorsize-1] += bytes;
               bytes_this_sector += bytes;
               bytes = 0;
            }
         }
         if ( !offset && bytes )
         {
            memcpy(s+bytes_this_sector,buf,bytes);
            buf += bytes;
            s[atrfs.sectorsize-1] += bytes;
            size -= bytes;
            r += bytes;
         }
      }
      if ( size || ((dirent->flags & FLAGS_DOS2) && s[atrfs.sectorsize-1] == atrfs.sectorsize-3) )
      {
         // Allocate a new sector and set it to zero bytes
         sector = mydos_alloc_any_sector();
         if ( sector < 0 )
         {
            s[atrfs.sectorsize-2]=0; // next sector should already be zero
            s[atrfs.sectorsize-3]=0;
            if (dirent->flags & FLAGS_DOS2)
            {
               --s[atrfs.sectorsize-1]; // Can't leave the last sector full
               if ( r>0 ) --r; // Undid one last byte
            }
            else
            {
               s[atrfs.sectorsize-1] |= 0x80; // Flag EOF
            }
            mydos_add_fileno(dirent,entry);
            if ( r>0 ) return r; // Wrote some
            return -ENOSPC;
         }
         if ( options.debug ) fprintf(stderr,"DEBUG: %s %s add sector to file: %d\n",__FUNCTION__,path,sector);
         s[atrfs.sectorsize-2]=sector&0xff;
         s[atrfs.sectorsize-3]=sector>>8;
         int len = BYTES2(dirent->sectors);
         if ( !(dirent->flags & FLAGS_DOS2) )
         {
            s[atrfs.sectorsize-1]=(BYTES2(dirent->sectors)-1)&0x7f; // DOS 1; byte holds sector sequence number in low 7 bits
         }
         // Will fix up sector chain later
         s=SECTOR(sector);
         memset(s,0,atrfs.sectorsize); // No garbage, unlike real DOS
         ++len;
         dirent->sectors[0]=len&0xff;
         dirent->sectors[1]=len>>8;
         if ( !(dirent->flags & FLAGS_DOS2) )
         {
            s[atrfs.sectorsize-1]=0x80; // DOS 1: Flag EOF
         }
      }
   }
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s Restore file numbers from file after extension\n",__FUNCTION__,path);
   mydos_add_fileno(dirent,entry);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s success: %d\n",__FUNCTION__,path,r);
   return r;
}
int mydos_mkdir(const char *path,mode_t mode)
{
   (void)mode; // Always create read-write, but allow chmod to lock
   // Find a place to create the file:
   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s.%d\n",__FUNCTION__,__LINE__);
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r < 0 ) return r;
   if ( r == 0 ) return -EEXIST;
   if ( entry < 0 ) return -ENOSPC; // Directory is full

   struct dos2_dirent *dirent = SECTOR(parent_dir_sector);
   dirent += DIRENT_ENTRY(entry);

   // Use 'dirent' for new entry

   // Need to find 8 free sectors in a row:
   // Question: Does MyDOS just look for a FF byte, forcing directory alignment?
   // Answer: Tests show no, it doesn't
   int alloc;
   for ( alloc=1; alloc < atrfs.sectors-7; ++alloc)
   {
      int range=0;
      if ( mydos_bitmap_status(alloc) != 0 )
      {
         for ( int i=1;i<8;++i )
         {
            if ( mydos_bitmap_status(alloc+i) == 0 ) break;
            ++range;
         }
      }
      if ( range == 7 ) break;
   }
   if ( alloc >= atrfs.sectors-7 ) return -ENOSPC; // No free block to store the directory

   for (int i=0;i<8;++i ) mydos_bitmap(alloc+i,1);

   // Create directory entry:
   dirent->flags = FLAGS_DIR;
   dirent->sectors[0]=8;
   dirent->sectors[1]=0;
   dirent->start[0] = alloc & 0xff;
   dirent->start[1] = alloc >> 8;
   const char *n = strrchr(path,'/');
   if (n) ++n;
   else n=path; // Shouldn't happen
   for (int i=0;i<8;++i)
   {
      dirent->name[i]=' ';
      if ( *n && *n != '.' )
      {
         dirent->name[i]=*n;
         ++n;
      }
   }
   if ( *n == '.' ) ++n;
   for (int i=0;i<3;++i)
   {
      dirent->ext[i]=' ';
      if ( *n )
      {
         dirent->ext[i]=*n;
         ++n;
      }
   }

   // Memset the directory to zeros
   void *dirmem = SECTOR(alloc);
   memset(dirmem,0,atrfs.sectorsize * 8 );
   return 0;
}
int mydos_rmdir(const char *path)
{
   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s.%d\n",__FUNCTION__,__LINE__);
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r < 0 ) return r;
   if ( r > 0 ) return -ENOENT;
   if ( !isdir ) return -ENOTDIR;
   if ( isinfo ) return -ENOENT;
   if ( locked ) return -EACCES;
   
   struct dos2_dirent *dirent;

   // Make sure directory is empty
   dirent = SECTOR(sector);
   for (int i=0;i<64;++i)
   {
      int e = i + ((atrfs.sectorsize == 256 ) ? entry/8 * 8 : 0 );
      if ( dirent[e].flags == 0 ) break;
      if ( dirent[e].flags & FLAGS_DELETED ) continue;
      return -ENOTEMPTY;
   }

   // Delete directory
   dirent = SECTOR(parent_dir_sector);
   dirent += DIRENT_ENTRY(entry);

   int sectors[9],*s;
   for ( int i=0;i<8;++i ) sectors[i]=i+sector;
   sectors[8]=0;
   dirent->flags = FLAGS_DELETED;
   s = sectors;
   while ( *s )
   {
      r = mydos_free_sector(*s);
      if ( r )
      {
         fprintf(stderr,"Warning: Freeing free sector %d when deleting %s\n",*s,path);
      }
      ++s;
   }
   return 0;
}

int mydos_unlink(const char *path)
{
   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s path %s %s at sector %d in dir %d\n",__FUNCTION__,path,r?"did not find":"found",sector,parent_dir_sector);
   if ( r < 0 ) return r;
   if ( r > 0 ) return -ENOENT;
   if ( isdir ) return -EISDIR;
   if ( isinfo ) return -ENOENT;
   if ( locked ) return -EACCES;

   struct dos2_dirent *dirent = SECTOR(parent_dir_sector);
   dirent += DIRENT_ENTRY(entry);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: dir sector %d Dirent %02x %d %d %8.8s%3.3s\n",__FUNCTION__,parent_dir_sector,dirent->flags,BYTES2(dirent->sectors),BYTES2(dirent->start),dirent->name,dirent->ext);

   int size,*sectors,*s;
   r = mydos_trace_file(sector,fileno,(dirent->flags & FLAGS_DOS2) == 0,&size,&sectors);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s.%d r is %d\n",__FUNCTION__,__LINE__,r);
   if ( r < 0 ) return r;
   dirent->flags = FLAGS_DELETED;
   s = sectors;
   while ( *s )
   {
      r = mydos_free_sector(*s);
      if ( r )
      {
         fprintf(stderr,"Warning: Freeing free sector %d when deleting %s\n",*s,path);
      }
      ++s;
   }
   free(sectors);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s Success!\n",__FUNCTION__,path);
   return 0;
}

int mydos_rename(const char *old, const char *new, unsigned int flags)
{
   /*
    * flags:
    *    RENAME_EXCHANGE: Both files must exist; swap names on entries
    *    RENAME_NOREPLACE: New name must not exist
    *    default: New name is deleted if it exists
    *
    * Note: If we rename a file into a directory, FUSE will generate the
    * 'new' path to have the target file name.
    * For example: "mv FOO BAR/" translates to "/FOO" "/BAR/FOO" here.
    * Even "mv FOO BAR" does the translation if BAR is a directory.
    */

   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;
   int sector2=0,parent_dir_sector2,fileno2,entry2,isdir2;
   struct dos2_dirent *dirent1, *dirent2 = NULL;
   
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s %s\n",__FUNCTION__,old,new);
   r = mydos_path(old,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -ENOENT;
   if ( isdir ) if ( atrfs_strncmp(old,new,strlen(old)) == 0 ) return -EINVAL; // attempt to make a directory a subdirectory of itself

   dirent1 = SECTOR(parent_dir_sector);
   dirent1 += entry + ((atrfs.sectorsize == 256 ) ? entry/8 * 8 : 0 ); // Point to this directory entry

   r = mydos_path(new,&sector2,&parent_dir_sector2,&count,&locked,&fileno2,&entry2,&isdir2,&isinfo);
   if ( r<0 ) return r;
   if ( isinfo ) return -ENAMETOOLONG;
   if ( r==0 && (flags & RENAME_NOREPLACE) ) return -EEXIST;
   if ( r!=0 && (flags & RENAME_EXCHANGE) ) return -ENOENT;
   if ( isdir2 && !(flags & RENAME_EXCHANGE) ) return -EISDIR;
   if ( r==0 && locked ) return -EACCES;
   if ( r==0 && sector==sector2 ) return -EINVAL; // Rename to self
   if ( (flags & RENAME_EXCHANGE) && isdir2 && atrfs_strncmp(old,new,strlen(new)) == 0 ) return -EINVAL;

   if ( r == 0 )
   {
      dirent2 = SECTOR(parent_dir_sector);
      dirent2 += entry + ((atrfs.sectorsize == 256 ) ? entry/8 * 8 : 0 ); // Point to this directory entry
   }

   if ( options.debug ) fprintf(stderr,"DEBUG: %s Lookup results: %s %d/%d %s %d/%d\n",__FUNCTION__,old,parent_dir_sector,sector,new,parent_dir_sector2,sector2);
   
   // Handle weird exchange case
   if ( flags & RENAME_EXCHANGE )
   {
      char copy[5];

      if ( options.debug ) fprintf(stderr,"DEBUG: %s Exchange %s and %s\n",__FUNCTION__,new,old);
      // Names stay the same, but everything else swaps
      // If they have file numbers, we need to update the sector ends
      mydos_remove_fileno(dirent1,entry);
      mydos_remove_fileno(dirent2,entry2);
      memcpy(copy,dirent1,5);
      memcpy(dirent1,dirent2,5);
      memcpy(dirent2,copy,5);
      mydos_add_fileno(dirent1,entry);
      mydos_add_fileno(dirent2,entry2);
      return 0;
   }

   // Handle the move/replace case
   if ( dirent2 )
   {
      // Swap them, and then unlink the original
      char copy[5];

      if ( options.debug ) fprintf(stderr,"DEBUG: %s replace %s with %s\n",__FUNCTION__,old,new);
      mydos_remove_fileno(dirent1,entry);
      mydos_remove_fileno(dirent2,entry2);
      memcpy(&copy,dirent1,5);
      memcpy(dirent1,dirent2,5);
      memcpy(dirent2,&copy,5);
      mydos_add_fileno(dirent1,entry);
      mydos_add_fileno(dirent2,entry2);
      r = mydos_unlink(old);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s unlink removed file returned %d\n",__FUNCTION__,r);
      return r;
   }

   // Rename in place?
   if ( !dirent2 && parent_dir_sector2 == parent_dir_sector )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s Rename in-place (same dir): %s -> %s\n",__FUNCTION__,old,new);
      struct dos2_dirent *dirent = SECTOR(parent_dir_sector);
      dirent += DIRENT_ENTRY(entry);
      const char *n = strrchr(new,'/');
      if (n) ++n;
      else n=new; // Shouldn't happen
      for (int i=0;i<8;++i)
      {
         dirent->name[i]=' ';
         if ( *n && *n != '.' )
         {
            dirent->name[i]=*n;
            ++n;
         }
      }
      if ( *n == '.' ) ++n;
      for (int i=0;i<3;++i)
      {
         dirent->ext[i]=' ';
         if ( *n )
         {
            dirent->ext[i]=*n;
            ++n;
         }
      }
      return 0;
   }

   // Move file (or directory) to new directory
   if ( 1 )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s Rename to new dir: %s -> %s\n",__FUNCTION__,old,new);
      if ( parent_dir_sector == sector2 ) return -EINVAL; // e.g., mv dir/file dir/
      const char *n = strrchr(old,'/');
      if ( !n ) return -EIO; // impossible
      ++n; // 'n' is now the name of the source file; what we're trying to put in the new directory.
      mydos_remove_fileno(dirent1,entry);

      // Find available entry in dir2
      struct dos2_dirent *dirent = SECTOR(parent_dir_sector2);
      int i;
      for (i=0;i<64;++i)
      {
         if ( dirent[DIRENT_ENTRY(i)].flags == 0 || (dirent[DIRENT_ENTRY(i)].flags & FLAGS_DELETED) ) break;
      }
      if ( i >= 64 ) return -ENOSPC;
      dirent += DIRENT_ENTRY(i); // *dirent is now the target

      // Copy entry
      struct dos2_dirent *old_dirent = SECTOR(parent_dir_sector);
      old_dirent += DIRENT_ENTRY(entry);
      *dirent = *old_dirent;
      mydos_add_fileno(dirent,i);

      // Flag old entry as deleted
      old_dirent->flags = FLAGS_DELETED;
      return 0;
   }

   // Should have covered all cases by this point
   return -EIO;
}
int mydos_chmod(const char *path, mode_t mode)
{
   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s.%d\n",__FUNCTION__,__LINE__);
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r < 0 ) return r;
   if ( r > 0 ) return -ENOENT;
   if ( isinfo ) return -EACCES;

   if ( locked && !(mode & 0200) ) return 0; // Already read-only
   if ( !locked && (mode & 0200) ) return 0; // Already has write permissions

   struct dos2_dirent *dirent = SECTOR(parent_dir_sector);
   dirent += DIRENT_ENTRY(entry);
   if ( mode & 0200 ) // make writable; unlock
   {
      dirent->flags &= ~FLAGS_LOCKED;
   }
   else
   {
      dirent->flags |= FLAGS_LOCKED;
   }
   return 0;
}
int mydos_create(const char *path, mode_t mode)
{
   (void)mode; // Always create read-write, but allow chmod to lock it
   // Create a file
   // Note: An empty file has one sector with zero bytes in it, not zero sectors
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
   
   int r;
   int sector=0,parent_dir_sector,count,locked,fileno,entry,isdir,isinfo;

   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r==0 ) return -EEXIST;
   if ( entry<0 ) return -ENOSPC; // directory is full

   struct dos2_dirent *dirent;
   dirent = SECTOR(parent_dir_sector);
   dirent += DIRENT_ENTRY(entry);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s entry %d in dir at %d\n",__FUNCTION__,path,entry,parent_dir_sector);
   memcpy(dirent->name,name,11);
   STOREBYTES2(dirent->sectors,1);
   int start = mydos_alloc_any_sector();
   if ( start < 0 ) return -ENOSPC;
   STOREBYTES2(dirent->start,start);
   if ( start < 720 )
   {
      dirent->flags = FLAGS_INUSE|FLAGS_DOS2; // In-use, made by DOS 2
      if ( atrfs.fstype == ATR_DOS1 ) dirent->flags = FLAGS_INUSE;
   }
   else if ( atrfs.fstype == ATR_DOS25 )
      dirent->flags = FLAGS_DOS25_regular; // Uses high sectors
   else
      dirent->flags = FLAGS_MYDOS_REGULAR; // Regular file without file numbers
   char *buf = SECTOR(start);
   memset(buf,0,atrfs.sectorsize);
   if ( start < 720 || atrfs.fstype == ATR_DOS25 )
   {
      buf[atrfs.sectorsize-3] = entry << 2;
   }
   if ( atrfs_strcmp(path,"/DOS.SYS")==0 )
   {
      /*
       * Except for DOS 1:
       *
       *  First sector of DOS.SYS at offset 0x0f (2 bytes).
       *  Flag for DOS.SYS at offset 0x0e, 1 for SD, 2 for DD
       *
       * DOS 1:
       *
       *  First sector of DOS.SYS at offset 0x10 (2 bytes).
       *  Flag for DOS.SYS at offset 0x0e, 0xff if DOS.SYS present, 0 if not
       *
       * To-do: Format disks with each DOS, copy boot sectors, and compare before and
       * after writing DOS files.
       */
      /*
       * When writing /DOS.SYS, check first sector for known patterns and adjust boot
       * sectors to match the DOS type.
       */
      struct sector1 *sec1 = SECTOR(1);
      if ( atrfs.fstype != ATR_DOS1 )
      {
         sec1->dos_flag = (atrfs.sectorsize == 128) ? 1 : 2;
         sec1->dos_sector[0] = start & 0xff;
         sec1->dos_sector[1] = start >> 8;
      }
      else
      {
         sec1->dos_flag = 0xff;
         sec1->dos_sector[1] = start & 0xff;
         //sec1->dos_sector[2] = start >> 8; // compiler warning, but this is intentional
         sec1->displacement  = start >> 8; // ugly code, but writing the same byte
      }
   }
   return 0;
}

int mydos_truncate(const char *path, off_t size)
{
   int r,sector=0,parent_dir_sector,count,locked,fileno,entry,filesize,*sectors;
   int isdir,isinfo;
   unsigned char *s;
   r = mydos_path(path,&sector,&parent_dir_sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -ENOENT; // These aren't writable
   struct dos2_dirent *dirent;
   dirent = SECTOR(parent_dir_sector);
   dirent += DIRENT_ENTRY(entry);

   r = mydos_trace_file(sector,fileno,(dirent->flags & FLAGS_DOS2) == 0,&filesize,&sectors);

   if ( r<0 ) return r;
   int bad=0;
   if ( r>0 ) bad=1;
   if ( bad )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s %s File is bad (%d)\n",__FUNCTION__,path,r);
      free(sectors);
      return -EIO;
   }

   // Trivial no-change case
   if ( size == filesize )
   {
      free(sectors);
      return 0;
   }

   // Grow the file
   if ( size > filesize )
   {
      char *buf = malloc ( size - filesize );
      if ( !buf ) return -ENOMEM;
      memset(buf,0,size-filesize);
      r = mydos_write(path,buf,size-filesize,filesize);
      free(buf);
      free(sectors);
      if ( r == size-filesize ) return 0;
      if ( r < 0 ) return r;
      return -ENOSPC; // Seems like a good guess
   }

   // Shrink the file
   int shrink = 0;
   for ( int i=0;sectors[i];++i )
   {
      if ( shrink )
      {
         mydos_free_sector(sectors[i]);
         int count = BYTES2(dirent->sectors) - 1;
         dirent->sectors[0] = count & 0xff;
         dirent->sectors[1] = count >> 8;
         continue;
      }
      s=SECTOR(sectors[i]);

      // If it's DOS 1, the last byte is the sector sequence of the file starting with zero except for the last sector
      int bytes_this_sector = s[atrfs.sectorsize-1];
      if ( (dirent->flags & FLAGS_DOS2) == 0 )
      {
         if ( sectors[i+1] ) bytes_this_sector = 125;
         else bytes_this_sector = s[atrfs.sectorsize-1] & 0x7f;
      }
      if ( size > bytes_this_sector  )
      {
         size -= bytes_this_sector;
         continue;
      }
      s[atrfs.sectorsize-1] = size;
      s[atrfs.sectorsize-2] = 0;
      if ( (dirent->flags & FLAGS_NOFILENO) == 0 )
      {
         s[atrfs.sectorsize-3] &= ~0x03; // Leave file number
      }
      if ( (dirent->flags & FLAGS_DOS2) == 0 )
      {
         s[atrfs.sectorsize-1] |= 0x80; // Flag EOF
      }
      size = 0;
      if ( s[atrfs.sectorsize-1] < atrfs.sectorsize - 3 )
      {
         shrink = 1;
      }
   }
   free(sectors);
   return 0;
}

int mydos_statfs(const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = atrfs.sectorsize;
   stfsbuf->f_frsize = atrfs.sectorsize;
   stfsbuf->f_blocks = atrfs.sectors;
   unsigned char *s = SECTOR(360);
   stfsbuf->f_bfree = BYTES2((s+3));
   if ( atrfs.fstype == ATR_DOS25 )
   {
      s = SECTOR(1024);
      stfsbuf->f_bfree += BYTES2((s+122));
   }
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = 0; // Meaningless for MyDOS
   stfsbuf->f_ffree = 0;
   if ( atrfs.fstype != ATR_MYDOS )
   {
      stfsbuf->f_files = 64;
      struct dos2_dirent *dirent = SECTOR(361);
      for (int i=0;i<64;++i)
      {
         if ( (dirent[i].flags & FLAGS_DELETED) || (dirent[i].flags == 0x00) )
         {
            ++stfsbuf->f_ffree;
         }
      }
   }
   stfsbuf->f_namemax = 12; // 8.3, plus ".info" if enabled
   // if ( !options.noinfofiles ) stfsbuf->f_namemax += 5; // Don't report this
   return 0;
}

int mydos_newfs(void)
{
   // Boot sectors: from table
   for (int i=1; i<=((atrfs.fstype == ATR_DOS1 ) ? 1 : 3); ++i)
   {
      unsigned char *s = SECTOR(i);
      memcpy(s,bootsectors[atrfs.fstype]+128*(i-1),128);
   }

   // Sector 1
   struct sector1 *sec1 = SECTOR(1);
#if 0 // Old code to use if we don't have the real boot sectors   
   sec1->boot_sectors = (atrfs.fstype == ATR_DOS1 ) ? 1 : 3;
   sec1->boot_addr[1] = 7; // $700
   sec1->jmp = 0x4C; // JMP instruction
   sec1->max_open_files = 3;
   sec1->drive_bits = (atrfs.fstype == ATR_DOS1 ) ? 0x0f : (atrfs.fstype == ATR_MYDOS ) ? 0xff : 0x03;
#endif

   // Set VTOC sector count or DOS flag
   struct mydos_vtoc *vtoc = SECTOR(360);
   int first_vtoc = 360;
   switch (atrfs.fstype)
   {
      case ATR_DOS1:
         vtoc->vtoc_sectors = 1; // Flag DOS 1
         break;
      default:
         vtoc->vtoc_sectors = 2; // Flag DOS 2
         break;
      case ATR_MYDOS: // indicates number of VTOC sectors
         vtoc->vtoc_sectors = 2; // 1 VTOC sector for smaller images
         if ( atrfs.sectors > 943 )
         {
            int vtoc_count;
            int sectors_left = atrfs.sectors - 943;
            // Always allocated as if double-density
            // SD allocated in pairs after the first
            vtoc_count = 1 + (sectors_left / 8 + 255) / 256;
            vtoc->vtoc_sectors = vtoc_count + 1;
            if ( atrfs.sectorsize == 128 )
            {
               // number of vtoc sectors is (code * 2)-3 (always odd)
               // VTOC Sectors  Code
               //     1          2
               //     3          3
               //     5          4
               first_vtoc = 361 - vtoc_count * 2;
            }
            else
            {
               first_vtoc = 361 - vtoc_count;
            }
         }
   }

   // Free free sectors
   int f1=0,f2=0;
   if ( options.debug ) fprintf(stderr,"%s: Free sectors %d--%d\n",__FUNCTION__,sec1->boot_sectors+1,first_vtoc-1);
   for ( int i=sec1->boot_sectors+1; i<first_vtoc; ++i )
   {
      mydos_free_sector(i);
      ++f1;
   }
   for ( int i=369; i<=atrfs.sectors; ++i )
   {
      if ( i == 720 ) continue;
      if ( i == 1024 && atrfs.fstype == ATR_DOS25 ) break;
      mydos_free_sector(i);
      ++f2;
   }
   if ( options.debug ) fprintf(stderr,"%s: Free sectors below+above VTOC: %d+%d=%d; recorded %d\n",__FUNCTION__,f1,f2,f1+f2,BYTES2(vtoc->free_sectors));

   vtoc->total_sectors[0] = vtoc->free_sectors[0];
   vtoc->total_sectors[1] = vtoc->free_sectors[1];

   return -EIO;
}

char *mydos_fsinfo(void)
{
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;
   unsigned char *vtoc = SECTOR(360);
   unsigned char *vtoc2 = SECTOR(1024);
   switch ( atrfs.fstype )
   {
      case ATR_DOS1:
      case ATR_DOS2:
      case ATR_MYDOS:
         b+=sprintf(b,"Total data sectors: %d\n",BYTES2((vtoc+1)));
         b+=sprintf(b,"Free sectors:       %d\n",BYTES2((vtoc+3)));
         break;
      case ATR_DOS25:
         b+=sprintf(b,"DOS 2 compatible data sectors: %d\n",BYTES2((vtoc+1)));
         b+=sprintf(b,"Extended range data sectors:   %d\n",1023-720);
         b+=sprintf(b,"Total data sectors:            %d\n",BYTES2((vtoc+1))+1023-720);
         b+=sprintf(b,"DOS 2 compatible free sectors: %d\n",BYTES2((vtoc+1)));
         b+=sprintf(b,"Extended range free sectors:   %d\n",BYTES2((vtoc2+122)));
         b+=sprintf(b,"Total free sectors:            %d\n",BYTES2((vtoc+1))+BYTES2((vtoc2+122)));
         break;
      default: break; // Not reached
   }
   return buf;
}
