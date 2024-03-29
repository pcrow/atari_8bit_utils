/*
 * atrfs.h
 *
 * Data types and variables used for code for specific file systems to
 * interact with the general ATR file system code.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 */

/*
 * Macros and defines
 */
// Atari memory access
#define BYTES2(n)	((n)[0]+256*(n)[1]) // Read two-byte array as lo,hi
#define BYTES2R(n)	((n)[1]+256*(n)[0]) // Read two-byte array as lo,hi
#define BYTES3(n)	((n)[0]+256*(n)[1]+256*256*(n)[2])
#define STOREBYTES2(n,v) ((n)[0]=(v)&0xff,(n)[1]=(v)>>8)
// SECTOR(n): return a pointer to sector 'n' or NULL if out of range
#define SECTOR(n)       ((void *)(((n)>0 && (n)<=atrfs->sectors)?( ( ((n)<4)&&atrfs->shortsectors ) ? atrfs->mem+((n)-1)*128: atrfs->mem + ((n)-1)*atrfs->sectorsize-atrfs->ssbytes ):NULL))
#define MSECTOR(n)       ((void *)(((n)>0 && (n)<=master_atrfs.sectors)?( ( ((n)<4)&&master_atrfs.shortsectors ) ? master_atrfs.mem+((n)-1)*128: master_atrfs.mem + ((n)-1)*master_atrfs.sectorsize-master_atrfs.ssbytes ):NULL))
// Adjust mode to match needs
#define MODE_DIR(m)     (((m)&0777) | S_IFDIR | 0111) // Add dir bits to mode
#define MODE_FILE(m)    (((m) & ~S_IFDIR & ~0111) | S_IFREG) // Regular file, not dir
#define MODE_RO(m)      ((m) & ~000222) // Remove write bits
#if (FUSE_USE_VERSION >= 30)
#define FILLER_NULL     NULL,0,0
#else
#define FILLER_NULL     NULL,0
#endif

/*
 * MacOS compatibility
 */
// Source: https://github.com/hboetes/mg/issues/7
#if defined(__APPLE__) || defined(__NetBSD__) 
#define st_atim st_atimespec
#define st_ctim st_ctimespec
#define st_mtim st_mtimespec
#endif


/*
 * Data types
 */
struct fuse_file_info;

enum atrfstype {
   ATR_SPECIAL, // files that aren't part of the Atari file system
   ATR_DOS1, // Mostly DOS 2
   ATR_DOS2, // subset of MyDOS
   ATR_DOS25, // enhanced density: bitmaps different from MyDOS
   ATR_DOS20D, // detected as MyDOS; behaviour is identical
   ATR_MYDOS, // Also Atari DOS 2.0d
   ATR_SPARTA,
   ATR_DOS3,
   ATR_DOS4, // 1450 XLD DOS: Not planning to implement
   ATR_DOSXE,
   ATR_LITEDOS, // http://www.mr-atari.com/Mr.Atari/LiteDOS/
   ATR_APT, // Atari Partition Table; present subdirectories for each partition
   ATR_UNKNOWN, // Just do the special files
   ATR_MAXFSTYPE
};

// One struct for all global information about the file system
struct atrfs {
   int fd;
   int readonly; // non-zero if file and memmap are read-only
   struct stat atrstat; // stat of the atr file at mount time
   void *atrmem; // memmapped file
   char *mem; // mem mapped image (without ATR header)
   size_t atrsize; // size of ATR file
   int shortsectors; // If the first 3 sectors are smaller in the image, treat first three sectors as 128-bytes
   int ssbytes; // bytes missing due to short sectors
   int sectorsize; // 128, 256, or possibly even 512
   int sectors;
   enum atrfstype fstype;
};

struct options {
   const char *filename;
   int nodotfiles; // True if special dotfiles are to be created
   int help;
   int debug;
   int info;
   int create;
   int upcase;
   int lowcase;
   // Following only matter if create is specified:
   unsigned int sectors;
   unsigned int secsize;
   const char *fstype;
   int clustersize;
   const char *volname;
};

struct sector1 {
   unsigned char pad_zero; // Usually 00, could be 0x53 for 'S' for SD, 'X' for DOS XE
   unsigned char boot_sectors; // Usually 3
   unsigned char boot_addr[2]; // Where to load the boot sectors in memory
   unsigned char dos_ini[2]; // I think this is a JSR address after loading boot sectors
   unsigned char jmp; // Normally 0x4C, but could be start of code (MyDOS/DOS 2)
   unsigned char exec_addr[2]; // execute address after boot loaded
   unsigned char max_open_files; // default 3; impacts number of buffers (MyDOS/DOS 2 only)
   unsigned char drive_bits; // DOS 2: one bit for each drive supported; default 3
   unsigned char unused_070b;
   unsigned char buffer_addr[2];
   unsigned char dos_flag;
   unsigned char dos_sector[2];
   unsigned char displacement;
   unsigned char dos_addr[2];
   unsigned char code_0714;
};

// FIXME: Get correct types for these functions
struct fs_ops {
   const char *name;
   const char *fstype; // For command-line create image option
   int (*fs_sanity)(struct atrfs *atrfs);
   int (*fs_getattr)(struct atrfs *atrfs,const char *,struct stat *);
   int (*fs_readdir)(struct atrfs *atrfs,const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
   int (*fs_read)(struct atrfs *atrfs,const char *,char *,size_t,off_t);
   int (*fs_write)(struct atrfs *atrfs,const char *,const char *,size_t,off_t);
   int (*fs_mkdir)(struct atrfs *atrfs,const char *, mode_t);
   int (*fs_rmdir)(struct atrfs *atrfs,const char *);
   int (*fs_unlink)(struct atrfs *atrfs,const char *);
   int (*fs_rename)(struct atrfs *atrfs,const char *, const char *, unsigned int);
   int (*fs_chmod)(struct atrfs *atrfs,const char *, mode_t);
   int (*fs_readlink)(struct atrfs *atrfs,const char *, char *,size_t);
   int (*fs_create )(struct atrfs *atrfs,const char *, mode_t);
   int (*fs_statfs)(struct atrfs *atrfs,const char *, struct statvfs *);
   int (*fs_truncate) (struct atrfs *atrfs,const char *, off_t);
#if (FUSE_USE_VERSION >= 30)
   int (*fs_utimens)(struct atrfs *atrfs,const char *, const struct timespec tv[2]);
#else
   int (*fs_utime)(struct atrfs *atrfs,const char *, struct utimbuf *utimbuf);
#endif
   int (*fs_newfs)(struct atrfs *atrfs); // Used from command-line optoin
   char *(*fs_fsinfo)(struct atrfs *atrfs); // Added text for .fsinfo file
};

/*
 * Function prototypes
 */
// atrfs.c functions used elsewhere
int atr_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
char *atr_info(const char *path,int filesize);
// special.c functions
char *fsinfo_textdata(struct atrfs *atrfs);
// common.c functions
int string_to_sector(const char *path);
int atrfs_strncmp(const char *s1, const char *s2, size_t n);
int atrfs_strcmp(const char *s1, const char *s2);
int atrfs_memcmp(const void *s1, const void *s2, size_t n);
char *strcpy_upcase(char *dst,const char *src);
char *strcpy_lowcase(char *dst,const char *src);
char *strcpy_case(char *dst,const char *src);

/*
 * Global variables
 */

extern struct atrfs master_atrfs;
extern struct options options;
extern const struct fs_ops *fs_ops[ATR_MAXFSTYPE];
extern const struct fs_ops generic_ops;
extern const struct fs_ops special_ops;
extern const struct fs_ops dos1_ops;
extern const struct fs_ops dos2_ops;
extern const struct fs_ops dos20d_ops;
extern const struct fs_ops dos25_ops;
extern const struct fs_ops mydos_ops;
extern const struct fs_ops sparta_ops;
extern const struct fs_ops dos3_ops;
extern const struct fs_ops dos4_ops;
extern const struct fs_ops dosxe_ops;
extern const struct fs_ops litedos_ops;
extern const struct fs_ops apt_ops;
extern const struct fs_ops unknown_ops;
