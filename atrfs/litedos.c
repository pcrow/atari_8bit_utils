/*
 * litedos.c
 *
 * Functions for accessing LiteDOS file systems.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 *
 * LiteDOS is a very minimal memory DOS 2 -ish file system.
 * It can read DOS 2 files, so that is preserved here.
 * The VTOC uses clusters, not sectors, and the cluster size
 * determines the directory size and max file system size.
 * For a minimal memory footprint, there are no subdirectories.
 *
 * However, the file chain is based on sectors, not clusters.  So MyDOS
 * should be able to read files, but the VTOC is incompatible, so it can't
 * write them.
 *
 * This is just different enough from DOS 2 that it's its own module.
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
#define BITMAPBYTE(n)   (n/8)
#define BITMAPMASK(n)   (1<<(7-(n%8)))
#define CLUSTER_SIZE    ((((unsigned char *)SECTOR(360))[0]&0x3f)+1)
#define CLUSTER_BYTES   (CLUSTER_SIZE * atrfs.sectorsize)
#define VTOC_FIRST_SECTOR (CLUSTER_SIZE <= 4 ? 360 : CLUSTER_SIZE <= 16 ? 352 : CLUSTER_SIZE <= 32 ? 320 : 256) // Is there a formula?
#define SECTOR_TO_CLUSTER_NUM(n)  ((n)/CLUSTER_SIZE)
#define VTOC_LAST_SECTOR (VTOC_FIRST_SECTOR - 1 + 2 * CLUSTER_SIZE)
#define DIRENT_SECTOR(n) ((((n)/8)+361) <= VTOC_LAST_SECTOR ? (((n)/8)+361) : (n)/8 - (VTOC_LAST_SECTOR-360) + VTOC_FIRST_SECTOR)
#define DIRENT_COUNT     ((VTOC_LAST_SECTOR - VTOC_FIRST_SECTOR) * 8) // Add 1 sector, but subtract the bitmap sector
#define DIRENT_ENTRY(n)  (((struct litedos_dirent *)SECTOR(DIRENT_SECTOR(n)))+(n)%8)
#define LITEDOS_VTOC  ((struct litedos_vtoc *)SECTOR(360))
#define MAP_VALUE(n)  (LITEDOS_VTOC->bitmap[BITMAPBYTE(n)] & BITMAPMASK(n))
#define MAX_CLUSTER   (atrfs.sectors / CLUSTER_SIZE)
#define CLUSTER(n)    (SECTOR((n)/CLUSTER_SIZE))

/*
 * File System Structures
 *
 * https://atari.fox-1.nl/disk-formats-explained/
 */
struct litedos_vtoc {
   unsigned char litedos_id; // (cluster_size - 1) | 0x40
   unsigned char total_sectors[2];
   unsigned char free_sectors[2];
   unsigned char unused[5];
   unsigned char bitmap[128]; // 118 for SD, 128 for DD
};

struct litedos_dirent {
   unsigned char flags;
   unsigned char sectors[2];
   unsigned char start[2];
   unsigned char name[8];
   unsigned char ext[3];
};

enum dirent_flags {
   FLAGS_OPEN     = 0x01, // Open for write, not visible
   FLAGS_DOS2     = 0x02, // Not set by DOS 1
   FLAGS_NOFILENO = 0x04, // Full 2-byte sector numbers
   FLAGS_LOCKED   = 0x20, // May not be implemented in LiteDOS, but support here
   FLAGS_INUSE    = 0x40, // Visible to DOS 1 and DOS 2
   FLAGS_DELETED  = 0x80,
};

/*
 * Function prototypes
 */
int litedos_sanity(void);
int litedos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int litedos_getattr(const char *path, struct stat *stbuf);
int litedos_read(const char *path, char *buf, size_t size, off_t offset);
int litedos_write(const char *path, const char *buf, size_t size, off_t offset);
int litedos_unlink(const char *path);
int litedos_rename(const char *path1, const char *path2, unsigned int flags);
int litedos_chmod(const char *path, mode_t mode);
int litedos_create(const char *path, mode_t mode);
int litedos_truncate(const char *path, off_t size);
int litedos_statfs(const char *path, struct statvfs *stfsbuf);
int litedos_newfs(void);
char *litedos_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops litedos_ops = {
   .name = "LiteDOS",
   .fstype = "litedos",
   .fs_sanity = litedos_sanity,
   .fs_getattr = litedos_getattr,
   .fs_readdir = litedos_readdir,
   .fs_read = litedos_read,
   .fs_write = litedos_write,
   .fs_unlink = litedos_unlink,
   .fs_rename = litedos_rename,
   .fs_chmod = litedos_chmod,
   .fs_create = litedos_create,
   .fs_truncate = litedos_truncate,
   .fs_statfs = litedos_statfs,
   .fs_newfs = litedos_newfs,
   .fs_fsinfo = litedos_fsinfo,
};

/*
 * Boot Sectors
 *
 * These are copied from real images.
 *
 * The code will detect creating DOS.SYS and will update sector 1 as appropriate.
 */
static const char bootsector[128] =
{
 0x07, 0x01, 0x28, 0x01, 0x75, 0x0b, 0xad, 0x0f, 0x02, 0x10, 0x3f, 0x8d, 0x93, 0x01, 0xad, 0x0e, 
 0x02, 0x8d, 0x92, 0x01, 0xa9, 0xa2, 0xcd, 0x99, 0xec, 0xf0, 0x1d, 0xa0, 0x9d, 0xcd, 0x9d, 0xec, 
 0xf0, 0x13, 0xa0, 0xbe, 0xcd, 0xbe, 0xec, 0xf0, 0x0c, 0xa0, 0xdb, 0xcd, 0xdb, 0xea, 0xd0, 0x12, 
 0xa2, 0xea, 0x8e, 0x98, 0x01, 0x8c, 0x97, 0x01, 0xad, 0x0f, 0xd2, 0x29, 0x08, 0xd0, 0x03, 0x20, 
 0x99, 0x01, 0xee, 0x0a, 0x03, 0x20, 0x53, 0xe4, 0x10, 0x03, 0x4c, 0x77, 0xe4, 0x4c, 0x00, 0x04, 
 0x98, 0x48, 0xac, 0x18, 0x03, 0xb9, 0xfe, 0x00, 0xcd, 0x98, 0x01, 0xd0, 0x0a, 0xa9, 0x01, 0x99, 
 0xfe, 0x00, 0xa9, 0x93, 0x99, 0xfd, 0x00, 0x68, 0xa8, 0x4c, 0x00, 0x00, 0xa0, 0x0d, 0x4c, 0x99, 
 0xec, 0xa9, 0x78, 0x8d, 0x0e, 0x02, 0xa9, 0x01, 0x8d, 0x0f, 0x02, 0x60, 0x00, 0x00, 0x00, 0x00, 
};
/*
 * Functions
 */

/*
 * litedos_sanity()
 *
 * Return 0 if this is a valid Litedos file system
 */
int litedos_sanity(void)
{
   if ( atrfs.sectors < 361 ) return 1; // Must have the root directory
   if ( atrfs.sectorsize > 256 ) return 1; // No support for 512-byte sectors
   struct sector1 *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 1 ) return 1; // Must have 1 boot sector
   if ( sec1->pad_zero == 'X' || sec1->pad_zero == 'S' ) return 1; // Flagged as DOS XE or SpartaDOS
   if ( sec1->boot_addr[1] != 1 ) return 1; // Loads boot sector in the stack area!
   if ( sec1->jmp == 0x4c ) return 1; // Code starts there, and not with a JMP
   switch (CLUSTER_SIZE) {
      case 1:
      case 2:
      case 4:
      case 8:
      case 16:
      case 32:
      case 64:
         break;
      default: return 1; // Invalid cluster size
   }
   if ( (((unsigned char *)SECTOR(360))[0]>>6) != 1 ) return 1; // LiteDOS always sets 01 as the top two bits there.
   if ( BYTES2(LITEDOS_VTOC->total_sectors) != atrfs.sectors ) return 1;
   if ( BYTES2(LITEDOS_VTOC->total_sectors) < BYTES2(LITEDOS_VTOC->free_sectors) ) return 1;

   // Cluster 0 is always used, as is SECTOR_TO_CLUSTER_NUM(VTOC_FIRST_SECTOR) and that +1
   struct litedos_vtoc *vtoc = SECTOR(360);
   if ( vtoc->bitmap[0]&0x80 ) return 1; // Cluster 0 (sector 0) is always used
   if ( CLUSTER_SIZE == 1 && ( vtoc->bitmap[0]&0x40 ) ) return 1; // Boot sector if cluster size is 1
   if ( MAP_VALUE(SECTOR_TO_CLUSTER_NUM(VTOC_FIRST_SECTOR)) ) return 1; // First VTOC cluster
   if ( MAP_VALUE(1+SECTOR_TO_CLUSTER_NUM(VTOC_FIRST_SECTOR)) ) return 1; // Second VTOC cluster
   for ( int i=0;i<DIRENT_COUNT;++i )
   {
      // Validate DIRENT_ENTRY(i) is valid
      struct litedos_dirent *dirent = DIRENT_ENTRY(i);
      if ( (dirent->flags & 0x80) && (dirent->flags & 0x7f) ) return 1; // If deleted, not anything else
      if ( BYTES2(dirent->start) > atrfs.sectors ) return 1;
   }
   return 0;
}

/*
 * litedos_trace_file()
 *
 * Trace through a file.
 * If *sectors is non-NULL, malloc an array of ints for the chain of sector numbers, ending with a zero
 * If fileno is 0-63, expect the fileno to be at the end of the sectors (DOS 2 mode).
 * Return 0 on success, non-zero if chain fails, but array is still valid as far as the file is readable.
 */
int litedos_trace_file(int sector,int fileno,int *size,int **sectors)
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
         if ( options.debug ) fprintf(stderr,"DEBUG: %s: block %d sector %d max %d (fileno %d)\n",__FUNCTION__,block,sector,MAX_CLUSTER,fileno);
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
      *size += s[atrfs.sectorsize-1];
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
 * litedos_remove_fileno()
 *
 * Remove the file number from the ends of the sectors (if there)
 */
int litedos_remove_fileno(struct litedos_dirent *dirent,int fileno)
{
   int *sectors;
   int r,size;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s %8.8s.%3.3s fileno %d sector %d count %d flags %02x\n",__FUNCTION__,dirent->name,dirent->ext,fileno,BYTES2(dirent->start),BYTES2(dirent->sectors),dirent->flags);
   if ( (dirent->flags & FLAGS_NOFILENO) ) return 0; // Already removed
   r = litedos_trace_file(BYTES2(dirent->start),fileno,&size,&sectors);
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
 * litedos_add_fileno()
 *
 * Add the file number from the ends of the sectors (if not already there)
 */
int litedos_add_fileno(struct litedos_dirent *dirent,int fileno)
{
   int *sectors,*s;
   int r,size;

   if ( !( dirent->flags & FLAGS_NOFILENO ) ) return 0; // Already there
   r = litedos_trace_file(BYTES2(dirent->start),-1,&size,&sectors);
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
 * litedos_path()
 *
 * Given a path, find the file.
 * 
 * Return value:
 *   0 File found
 *   1 File not found, but could be created
 *   -x return this error (usually -ENOENT)
 */
int litedos_path(const char *path,int *sector,int *count,int *locked,int *fileno,int *entry,int *isdir,int *isinfo)
{
   unsigned char name[8+3+1]; // 8+3+NULL

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   *sector = 0;
   *count = 0;
   *isdir=0;
   *isinfo=0;
   *fileno = -1;
   *locked=0;
   *entry=0;

   while ( *path == '/' ) ++path;
   if ( ! *path )
   {
      *isdir = 1; // Root directory; fix calling code to never hit this case
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
      
   int firstfree = -1;
   for ( i=0;i<DIRENT_COUNT;++i )
   {
      struct litedos_dirent *dirent = DIRENT_ENTRY(i);

      if ( dirent->flags == 0 )
      {
         if ( firstfree < 0 ) firstfree = i;
         break;
      }
      if ( dirent->flags & FLAGS_DELETED )
      {
         if ( firstfree < 0 ) firstfree = i;
         continue;
      }
      if ( atrfs_strncmp((char *)dirent->name,(char *)name,8+3) != 0 ) continue;
      *entry = i;
      *sector = BYTES2(dirent->start);
      *count = BYTES2(dirent->sectors);
      if ( dirent->flags & FLAGS_LOCKED ) *locked=1;
      if ( (dirent->flags & FLAGS_NOFILENO) == 0 ) *fileno=i;
      return 0;
   }
   if ( firstfree >= 0 )
   {
      *entry = firstfree;
      return 1;
   }
   return -ENOENT; // Shouldn't be reached
}

/*
 * litedos_bitmap_status()
 *
 * Return the bit from the bitmap for the sector (0 if allocated)
 */
int litedos_bitmap_status(int sector)
{
   return MAP_VALUE(sector);
}

/*
 * litedos_bitmap()
 *
 * Set or clear bitmap for a given cluster.
 * If 'allocate' is non-zero, change from free to used; otherwise reverse
 * Return 0 if changed, 1 if already at target value.
 */
int litedos_bitmap(int sector,int allocate)
{
   int value = MAP_VALUE(sector);
   if ( value && !allocate ) return 1;
   if ( !value && allocate ) return 1;

   // Flip the bit in the map
   if ( allocate ) LITEDOS_VTOC->bitmap[BITMAPBYTE(sector)] &= ~BITMAPMASK(sector);
   else LITEDOS_VTOC->bitmap[BITMAPBYTE(sector)] |= BITMAPMASK(sector);

   // Caller must update the free count
   return 0;
}

/*
 * litedos_free_cluster()
 */
int litedos_free_cluster(int sector)
{
   int r;
   r = litedos_bitmap(sector,0);

   // Only update the free sector count if the bitmap was modified
   if ( r == 0 )
   {
      struct litedos_vtoc *vtoc = LITEDOS_VTOC;
      int free_count = BYTES2(vtoc->free_sectors);
      free_count += CLUSTER_SIZE;
      STOREBYTES2(vtoc->free_sectors,free_count);
   }
   return r;
}

/*
 * litedos_free_sector()
 */
int litedos_free_sector(int sector)
{
   if ( sector % CLUSTER_SIZE ) return 0; // Success
   return litedos_free_cluster(sector/CLUSTER_SIZE);
}

/*
 * litedos_alloc_cluster()
 */
int litedos_alloc_cluster(int sector)
{
   int r;
   r = litedos_bitmap(sector,1);

   // Only update the free sector count if the bitmap was modified
   if ( r == 0 )
   {
      struct litedos_vtoc *vtoc = LITEDOS_VTOC;
      int free_count = BYTES2(vtoc->free_sectors);
      free_count -= CLUSTER_SIZE;
      STOREBYTES2(vtoc->free_sectors,free_count);
   }
   return r;
}

int litedos_alloc_any_cluster(void)
{
   int r;
   for ( int i=2;i<=MAX_CLUSTER; ++i )
   {
      r=litedos_alloc_cluster(i);
      if ( r==0 ) return i;
   }
   return -1;
}

/*
 * litedos_alloc_sector()
 */
int litedos_alloc_sector(int prev_sector)
{
   if ( prev_sector > 0 &&  prev_sector/CLUSTER_SIZE == (prev_sector+1)/CLUSTER_SIZE ) return prev_sector+1;
   int cluster = litedos_alloc_any_cluster();
   if ( cluster < 0 ) return cluster;
   return cluster * CLUSTER_SIZE;
}

/*
 * litedos_info()
 *
 * Return the buffer containing information specific to this file.
 *
 * This could be extended to do a full analysis of binary load files,
 * BASIC files, or any other recognizable file type.
 *
 * The pointer returned should be 'free()'d after use.
 */
char *litedos_info(const char *path,struct litedos_dirent *dirent,int entry,int sector,int *sectors,int filesize)
{
   char *buf,*b;

   (void)sector; // FIXME: Use or remove parameter
   buf = malloc(64*1024);
   b = buf;
   *b = 0;
   if ( dirent )
   {
      b+=sprintf(b,"File information and analysis\n\n  %.*s\n  %d bytes\n\n",(int)(strrchr(path,'.')-path),path,filesize);
      b+=sprintf(b,
                 "Directory entry internals:\n"
                 "  Entry %d in directory\n"
                 "  Flags: $%02x\n"
                 "  Sectors: %d\n"
                 "  Starting Sector: %d\n\n",
                 entry,
                 dirent->flags,BYTES2(dirent->sectors),BYTES2(dirent->start));
   }
   if ( !sectors )
   {
      b+=sprintf(b,"This is the root directory\n");
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
 * litedos_readdir()
 *
 * Also use for dos2 and dos25
 */
int litedos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   if ( strcmp(path,"/") != 0 )
   {
      // Just check to see which errno to return
      (void)offset; // FUSE will always read directories from the start in our use
      int r;
      int sector=0,count,locked,fileno,entry,isdir,isinfo;

      r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
      if ( r<0 ) return r;
      return -ENOTDIR;
   }

   // Read directory at: sector
   char name[8+1+3+1];
   for ( int i=0;i<DIRENT_COUNT;++i )
   {
      struct litedos_dirent *dirent = DIRENT_ENTRY(i);

      if ( dirent->flags == 0 ) break;
      if ( dirent->flags & FLAGS_DELETED ) continue; // Deleted

      memcpy(name,dirent->name,8);
      int k;
      for (k=0;k<8;++k)
      {
         if ( dirent->name[k] == ' ' ) break;
      }
      name[k]=0;
      if ( dirent->ext[0] != ' ' )
      {
         name[k]='.';
         ++k;
         for (int l=0;l<3;++l)
         {
            if ( dirent->ext[l] == ' ' ) break;
            name[k+l]=dirent->ext[l];
            name[k+l+1]=0;
         }
      }
      filler(buf, name, FILLER_NULL);
   }
   return 0;
}

/*
 * litedos_getattr()
 */

int litedos_getattr(const char *path, struct stat *stbuf)
{
   // stbuf initialized from atr image stats

   int r;
   int sector=0,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s lookup %d\n",__FUNCTION__,path,r);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;

   struct litedos_dirent *dirent = NULL;
   if ( sector ) dirent = DIRENT_ENTRY(entry);

   if ( isinfo )
   {
      stbuf->st_mode = MODE_RO(stbuf->st_mode); // These files are never writable
      stbuf->st_ino = sector + 0x10000;

      int filesize = atrfs.sectorsize*8,*sectors = NULL; // defaults for directories
      if ( entry )
      {
         r = litedos_trace_file(sector,fileno,&filesize,&sectors);
         if ( r<0 ) return r;
      }

      char *info = litedos_info(path,dirent,entry,sector,sectors,filesize);     
      stbuf->st_size = info?strlen(info):0;
      free(info);
      free(sectors);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s info\n",__FUNCTION__,path);
      return 0;
   }
   if ( isdir )
   {
      ++stbuf->st_nlink;
      stbuf->st_mode = MODE_DIR(atrfs.atrstat.st_mode & 0777);
      stbuf->st_size = (CLUSTER_SIZE-1)*atrfs.sectorsize;
   }
   else
   {
      int s;
      r = litedos_trace_file(sector,fileno,&s,NULL);
      stbuf->st_size = s;
   }
   if ( locked ) stbuf->st_mode = MODE_RO(stbuf->st_mode);
   stbuf->st_ino = sector; // We don't use this, but it's fun info
   stbuf->st_blocks = count;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s mode 0%o\n",__FUNCTION__,path,stbuf->st_mode);
   return 0;
}

int litedos_read(const char *path, char *buf, size_t size, off_t offset)
{
   int r,sector=0,count,locked,fileno,entry,filesize,*sectors;
   int isdir,isinfo;
   unsigned char *s;
   r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;

   struct litedos_dirent *dirent = NULL;
   if ( sector )
   {
      dirent = DIRENT_ENTRY(entry);
      r = litedos_trace_file(sector,fileno,&filesize,&sectors);

      if ( r<0 ) return r;
      int bad=0;
      if ( r>0 ) bad=1;
      if ( bad && options.debug ) fprintf(stderr,"DEBUG: %s %s File is bad (%d)\n",__FUNCTION__,path,r);
      r=0;
   }

   if ( isinfo )
   {
      char *info = litedos_info(path,dirent,entry,sector,sectors,filesize);
      char *i = info;
      int bytes = info?strlen(info):0;
      free(sectors);
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

int litedos_write(const char *path, const char *buf, size_t size, off_t offset)
{
   int r,sector=0,count,locked,fileno,entry,filesize,*sectors;
   int isdir,isinfo;
   unsigned char *s = NULL;
   r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -ENOENT; // These aren't writable

   struct litedos_dirent *dirent;
   dirent = DIRENT_ENTRY(entry);

   r = litedos_trace_file(sector,fileno,&filesize,&sectors);

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
      if ( 0 ) // FIXME
      {
         if ( options.debug ) fprintf(stderr,"DEBUG: %s Updating boot sectors to match DOS.SYS\n",__FUNCTION__);
         unsigned char *s = SECTOR(1);
         memcpy(s,bootsector,128);

         struct sector1 *sec1 = SECTOR(1);
         // FIXME: This is wrong:
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
      sector = sectors[i];
      s=SECTOR(sectors[i]);

      int bytes_this_sector = s[atrfs.sectorsize-1];
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
   litedos_remove_fileno(dirent,fileno);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s extend file %lu bytes\n",__FUNCTION__,path,size+offset);
   while ( size+offset )
   {
      // Fill last sector
      int bytes_this_sector = s[atrfs.sectorsize-1];
      bytes = CLUSTER_BYTES-3 - bytes_this_sector;
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
      if ( s[atrfs.sectorsize-1] == CLUSTER_BYTES-3 )
      {
         // Allocate a new sector and set it to zero bytes
         sector = litedos_alloc_sector(sector);
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
            litedos_add_fileno(dirent,entry);
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
         memset(s,0,CLUSTER_BYTES); // No garbage, unlike real DOS
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
   litedos_add_fileno(dirent,entry);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s success: %d\n",__FUNCTION__,path,r);
   return r;
}

int litedos_unlink(const char *path)
{
   int r;
   int sector=0,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);
   r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s path %s %s at sector %d\n",__FUNCTION__,path,r?"did not find":"found",sector);
   if ( r < 0 ) return r;
   if ( r > 0 ) return -ENOENT;
   if ( isdir ) return -EISDIR;
   if ( isinfo ) return -ENOENT;
   if ( locked ) return -EACCES;

   struct litedos_dirent *dirent = DIRENT_ENTRY(entry);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: Dirent %02x %d %d %8.8s%3.3s\n",__FUNCTION__,dirent->flags,BYTES2(dirent->sectors),BYTES2(dirent->start),dirent->name,dirent->ext);

   int size,*sectors,*s;
   r = litedos_trace_file(sector,fileno,&size,&sectors);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s.%d r is %d\n",__FUNCTION__,__LINE__,r);
   if ( r < 0 ) return r;
   dirent->flags = FLAGS_DELETED;
   s = sectors;
   while ( *s )
   {
      r = litedos_free_sector(*s);
      if ( r && (*s%CLUSTER_SIZE)==0 )
      {
         fprintf(stderr,"Warning: Freeing free sector %d when deleting %s\n",*s,path);
      }
      ++s;
   }
   free(sectors);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s Success!\n",__FUNCTION__,path);
   return 0;
}

int litedos_rename(const char *old, const char *new, unsigned int flags)
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
   int sector=0,count,locked,fileno,entry,isdir,isinfo;
   int sector2=0,fileno2,entry2,isdir2;
   struct litedos_dirent *dirent1, *dirent2 = NULL;
   
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s %s\n",__FUNCTION__,old,new);
   r = litedos_path(old,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -ENOENT;
   if ( isdir ) return -EINVAL; // no subdir support

   dirent1 = DIRENT_ENTRY(entry);

   r = litedos_path(new,&sector2,&count,&locked,&fileno2,&entry2,&isdir2,&isinfo);
   if ( r<0 ) return r;
   if ( isinfo ) return -ENAMETOOLONG;
   if ( r==0 && (flags & RENAME_NOREPLACE) ) return -EEXIST;
   if ( r!=0 && (flags & RENAME_EXCHANGE) ) return -ENOENT;
   if ( isdir2 ) return -EINVAL;
   if ( r==0 && locked ) return -EACCES;
   if ( r==0 && sector==sector2 ) return -EINVAL; // Rename to self

   if ( r == 0 )
   {
      dirent2 = DIRENT_ENTRY(entry);
   }

   if ( options.debug ) fprintf(stderr,"DEBUG: %s Lookup results: %s %d %s %d\n",__FUNCTION__,old,sector,new,sector2);
   
   // Handle weird exchange case
   if ( flags & RENAME_EXCHANGE )
   {
      char copy[5];

      if ( options.debug ) fprintf(stderr,"DEBUG: %s Exchange %s and %s\n",__FUNCTION__,new,old);
      // Names stay the same, but everything else swaps
      // If they have file numbers, we need to update the sector ends
      litedos_remove_fileno(dirent1,entry);
      litedos_remove_fileno(dirent2,entry2);
      memcpy(copy,dirent1,5);
      memcpy(dirent1,dirent2,5);
      memcpy(dirent2,copy,5);
      litedos_add_fileno(dirent1,entry);
      litedos_add_fileno(dirent2,entry2);
      return 0;
   }

   // Handle the move/replace case
   if ( dirent2 )
   {
      // Swap them, and then unlink the original
      char copy[5];

      if ( options.debug ) fprintf(stderr,"DEBUG: %s replace %s with %s\n",__FUNCTION__,old,new);
      litedos_remove_fileno(dirent1,entry);
      litedos_remove_fileno(dirent2,entry2);
      memcpy(&copy,dirent1,5);
      memcpy(dirent1,dirent2,5);
      memcpy(dirent2,&copy,5);
      litedos_add_fileno(dirent1,entry);
      litedos_add_fileno(dirent2,entry2);
      r = litedos_unlink(old);
      if ( options.debug ) fprintf(stderr,"DEBUG: %s unlink removed file returned %d\n",__FUNCTION__,r);
      return r;
   }

   // Rename in place
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s Rename in-place (same dir): %s -> %s\n",__FUNCTION__,old,new);
      struct litedos_dirent *dirent = DIRENT_ENTRY(entry);
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
}

int litedos_chmod(const char *path, mode_t mode)
{
   int r;
   int sector=0,count,locked,fileno,entry,isdir,isinfo;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s.%d\n",__FUNCTION__,__LINE__);
   r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r < 0 ) return r;
   if ( r > 0 ) return -ENOENT;
   if ( isinfo ) return -EACCES;

   if ( locked && !(mode & 0200) ) return 0; // Already read-only
   if ( !locked && (mode & 0200) ) return 0; // Already has write permissions

   struct litedos_dirent *dirent = DIRENT_ENTRY(entry);
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

int litedos_create(const char *path, mode_t mode)
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
   int sector=0,count,locked,fileno,entry,isdir,isinfo;

   r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( r<0 ) return r;
   if ( r==0 ) return -EEXIST;
   if ( entry<0 ) return -ENOSPC; // directory is full

   struct litedos_dirent *dirent;
   dirent = DIRENT_ENTRY(entry);
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s entry %d\n",__FUNCTION__,path,entry);
   memcpy(dirent->name,name,11);
   STOREBYTES2(dirent->sectors,1);
   int start = litedos_alloc_sector(0);
   if ( start < 0 ) return -ENOSPC;
   STOREBYTES2(dirent->start,start);
   dirent->flags = FLAGS_INUSE|FLAGS_DOS2; // In-use, made by DOS 2
   if ( start >= 1024 )
   {
      dirent->flags |= FLAGS_NOFILENO;
   }
   char *buf = CLUSTER(start);
   memset(buf,0,CLUSTER_BYTES);
   if ( !(dirent->flags & FLAGS_NOFILENO) )
   {
      buf[atrfs.sectorsize-2] = entry << 2;
   }
#if 0 // FIXME: Make this work; but I think it's covered in write()
   if ( atrfs_strcmp(path,"/DOS.SYS")==0 )
   {
      /*
       * When writing /DOS.SYS, check first sector for known patterns and adjust boot
       * sectors to match the DOS type.
       */
      struct sector1 *sec1 = SECTOR(1);
      sec1->dos_flag = (atrfs.sectorsize == 128) ? 1 : 2;
      sec1->dos_sector[0] = start & 0xff;
      sec1->dos_sector[1] = start >> 8;
   }
#endif
   return 0;
}

int litedos_truncate(const char *path, off_t size)
{
   int r,sector=0,count,locked,fileno,entry,filesize,*sectors;
   int isdir,isinfo;
   unsigned char *s;
   r = litedos_path(path,&sector,&count,&locked,&fileno,&entry,&isdir,&isinfo);
   if ( isdir ) return -EISDIR;
   if ( r<0 ) return r;
   if ( r>0 ) return -ENOENT;
   if ( isinfo ) return -ENOENT; // These aren't writable
   struct litedos_dirent *dirent;
   dirent = DIRENT_ENTRY(entry);

   r = litedos_trace_file(sector,fileno,&filesize,&sectors);

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
      r = litedos_write(path,buf,size-filesize,filesize);
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
         litedos_free_sector(sectors[i]);
         int count = BYTES2(dirent->sectors) - 1;
         dirent->sectors[0] = count & 0xff;
         dirent->sectors[1] = count >> 8;
         continue;
      }
      s=SECTOR(sectors[i]);

      int bytes_this_sector = s[atrfs.sectorsize-1];
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
      size = 0;
      if ( s[atrfs.sectorsize-1] < CLUSTER_BYTES - 3 )
      {
         shrink = 1;
      }
   }
   free(sectors);
   return 0;
}

int litedos_statfs(const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = atrfs.sectorsize;
   stfsbuf->f_frsize = atrfs.sectorsize;
   stfsbuf->f_blocks = MAX_CLUSTER * CLUSTER_SIZE; // May be less than atrfs.sectors
   stfsbuf->f_bfree = BYTES2(LITEDOS_VTOC->free_sectors);
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = DIRENT_COUNT;
   stfsbuf->f_ffree = 0; // Count them up below
   for (int i=0;i<DIRENT_COUNT;++i)
   {
      struct litedos_dirent *dirent = DIRENT_ENTRY(i);
      if ( (dirent->flags & FLAGS_DELETED) || (dirent->flags == 0x00) )
      {
         ++stfsbuf->f_ffree;
      }
   }
   stfsbuf->f_namemax = 12; // 8.3, plus ".info" if enabled
   // if ( !options.noinfofiles ) stfsbuf->f_namemax += 5; // Don't report this
   return 0;
}

int litedos_newfs(void)
{
   // Boot sector
   unsigned char *s = SECTOR(1);
   memcpy(s,bootsector,128);

   // Sector 1
#if 0 // Old code to use if we don't have the real boot sectors   
   struct sector1 *sec1 = SECTOR(1);
   sec1->boot_sectors = (atrfs.fstype == ATR_DOS1 ) ? 1 : 3;
   sec1->boot_addr[1] = 7; // $700
   sec1->jmp = 0x4C; // JMP instruction
   sec1->max_open_files = 3;
   sec1->drive_bits = (atrfs.fstype == ATR_DOS1 ) ? 0x0f : (atrfs.fstype == ATR_LITEDOS ) ? 0xff : 0x03;
#endif

   // Set VTOC sector count or DOS flag
   struct litedos_vtoc *vtoc = LITEDOS_VTOC;

   // Compute minimum cluster size as default
   int cluster_size = 1;
   int max_sectors = 1024;
   while ( atrfs.sectors >= max_sectors )
   {
      cluster_size *= 2;
      max_sectors *= 2;
   }
   // Increase cluster size if requested and legal
   while ( cluster_size < 64 && cluster_size < options.clustersize ) cluster_size *= 2;
   
   vtoc->litedos_id = (1 << 6) | (cluster_size-1);
   STOREBYTES2(vtoc->total_sectors,MAX_CLUSTER);

   for (int i=2;i<MAX_CLUSTER;++i)
   {
      if ( i==SECTOR_TO_CLUSTER_NUM(VTOC_FIRST_SECTOR) ) continue;
      if ( i==SECTOR_TO_CLUSTER_NUM(VTOC_FIRST_SECTOR)+1 ) continue;
      litedos_free_cluster(i);
   }

   return 0;
}

char *litedos_fsinfo(void)
{
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;
   struct litedos_vtoc *vtoc = LITEDOS_VTOC;
   b+=sprintf(b,"Total sectors:     %d\n",BYTES2(vtoc->total_sectors));
   b+=sprintf(b,"Free sectors:      %d\n",BYTES2(vtoc->free_sectors));
   b+=sprintf(b,"Cluster size:      %d sectors or %d bytes\n",CLUSTER_SIZE,CLUSTER_BYTES);
   b+=sprintf(b,"Total clusters:    %d\n",MAX_CLUSTER);
   b+=sprintf(b,"VTOC/Dir sectors:  %d--%d\n",VTOC_FIRST_SECTOR,VTOC_LAST_SECTOR);
   b+=sprintf(b,"Directory entries: %d\n",DIRENT_COUNT);
   
   
   
   return buf;
}
