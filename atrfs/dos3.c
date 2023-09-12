/*
 * dos3.c
 *
 * Functions for accessing Atari DOS 3 file systems.
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 *
 * Reference:
 *  https://forums.atariage.com/topic/102915-dos-3-history/page/3/#comment-4998383
 *
 *  Boot sectors: 1-15 (only uses 9)
 *  Directory: 16-23
 *  VTOC: 24
 *  Data blocks: 25-32,33-40,...,713-720,...,1033-1040
 *
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
#define CLUSTER_TO_SECTOR(n)  ((n)*8+25) // 1K blocks starting with '0' at sector 25
#define CLUSTER(n)            SECTOR(CLUSTER_TO_SECTOR(n))


/*
 * File System Structures
 */
struct dos3_dir_head {
   unsigned char zeros[14];
   unsigned char max_free_blocks; // ((720-24)/8)==87 for SD, 127 for ED
   unsigned char dos_id; // 0xA5
};

struct dos3_dir_entry {
   unsigned char status; // Bit 7 valid, bit 6 exists, bit 1 locked, bit 0 open
   unsigned char file_name[8];
   unsigned char file_ext[3];
   unsigned char blocks;
   unsigned char start;
   unsigned char file_size[2]; // Low bytes of size for computing EOF in final block
};

enum dos3_dir_status {
   FLAGS_OPEN    = 0x01, // Open for write
   FLAGS_LOCKED  = 0x02,
   FLAGS_IN_USE  = 0x40, // This is a file
   FLAGS_ACTIVE  = 0x80, // Entry has been used
   // Deleted is ACTIVE but not IN_USE
};

// struct dos3_vtoc; // This is just 128 bytes for each cluster
enum dos3_vtoc_entry {
   // 00--7F: Link to next block in file
   // 80--FC: Reserved value
   VTOC_EOF   = 0xFD,
   VTOC_FREE  = 0xFE,
   VTOC_RESERVED = 0xFF, // Block is excluded from use, e.g., beyond end of disk for SD
};

/*
 * Function prototypes
 */
int dos3_sanity(void);
int dos3_getattr(const char *path, struct stat *stbuf);
int dos3_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int dos3_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dos3_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dos3_unlink(const char *path);
int dos3_rename(const char *path1, const char *path2, unsigned int flags);
int dos3_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos3_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos3_truncate(const char *path, off_t size);
int dos3_statfs(const char *path, struct statvfs *stfsbuf);
int dos3_newfs(void);
char *dos3_fsinfo(void);

/*
 * Global variables
 */
const struct fs_ops dos3_ops = {
   .name = "Atari DOS 3",
   .fs_sanity = dos3_sanity,
   .fs_getattr = dos3_getattr,
   .fs_readdir = dos3_readdir,
   .fs_read = dos3_read,
   // .fs_write = dos3_write,
   // .fs_mkdir // not relevant
   // .fs_rmdir // not relevant
   // .fs_unlink = dos3_unlink,
   // .fs_rename = dos3_rename,
   // .fs_chmod = dos3_chmod,
   // .fs_create = dos3_create,
   // .fs_truncate = dos3_truncate,
   // .fs_utimens // not relevant
   .fs_statfs = dos3_statfs,
   // .fs_newfs = dos3_newfs,
   .fs_fsinfo = dos3_fsinfo,
};

/*
 * Functions
 */

/*
 * dos3_get_dir_entry()
 */
int dos3_get_dir_entry(const char *path,struct dos3_dir_entry **dirent_found,int *isinfo)
{
   struct dos3_dir_entry *junk;
   if ( !dirent_found ) dirent_found = &junk; // Avoid NULL check later
   int junkinfo;
   if ( !isinfo ) isinfo=&junkinfo;

   *dirent_found = NULL;
   *isinfo = 0;
   while (*path == '/') ++path;
   if ( strchr(path,'/') ) return -ENOENT; // No subdirectories alowed

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
      if ( *path && *path != '.' )
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
   if ( *path )
   {
      return -ENOENT; // Name too long
   }
   if ( *path == '/' ) ++path;

   struct dos3_dir_entry *dirent = SECTOR(16);
   int firstfree = 0;
   for ( int i=1; i<64; ++i ) // Entry 0 is info about the file system
   {
      if (!dirent[i].status)
      {
         if ( !firstfree ) firstfree = i;
         break;
      }
      if ( dirent[i].status == FLAGS_ACTIVE )
      {
         if ( !firstfree ) firstfree = i;
         continue; // Not in use
      }
      if ( (dirent[i].status & FLAGS_OPEN) ) continue; // Hidden; incomplete writes
      if ( memcmp(name,dirent[i].file_name,8+3)!=0 ) continue;

      *dirent_found = &dirent[i];
      return 0;
   }
   if ( firstfree ) *dirent_found = &dirent[firstfree];
   return -ENOENT;
}

/*
 * dos3_info()
 *
 * Return the buffer containing information specific to this file.
 *
 * This could be extended to do a full analysis of binary load files,
 * BASIC files, or any other recognizable file type.
 *
 * The pointer returned should be 'free()'d after use.
 */
char *dos3_info(const char *path,struct dos3_dir_entry *dirent)
{
   char *buf,*b;

   int filesize = (dirent->blocks-1)*1024 + BYTES2(dirent->file_size)%1024;

   buf = malloc(64*1024);
   b = buf;
   *b = 0;
   b+=sprintf(b,"File information and analysis\n\n  %.*s\n  %d bytes\n\n",(int)(strrchr(path,'.')-path),path,filesize);
   b+=sprintf(b,
              "Directory entry internals:\n"
              "  Entry %ld\n"
              "  Flags: $%02x\n"
              "  1K Clusters: %d\n"
              "  Starting cluster: %d\n\n",
              dirent - (struct dos3_dir_entry *)SECTOR(16),
              dirent->status,dirent->blocks,dirent->start);

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
 * dos3_sanity()
 *
 * Return 0 if this is a valid Atari DOS 3 file system
 */
int dos3_sanity(void)
{
   if ( atrfs.sectorsize != 128 ) return 1; // Must be SD
   struct sector1 *sec1 = SECTOR(1);
   if ( sec1->boot_sectors != 9 ) return 1; // Nice to have a different number
   // FIXME: Would be nice to have a little more structure verification on sector 1
   struct dos3_dir_head *head = SECTOR(16);
   unsigned char zeros[14];
   memset(zeros,0,sizeof(zeros));
   if ( memcmp(zeros,head->zeros,sizeof(zeros)) != 0 ) return 1;
   if ( head->dos_id != 0xA5 ) return 1;

   if ( !atrfs.readonly ) // FIXME: No write support yet
   {
      fprintf(stderr,"DOS 3 write support not implemented; read-only mode forced\n");
      atrfs.readonly = 1;
   }
   return 0;
}

/*
 * dos3_getattr()
 */
int dos3_getattr(const char *path, struct stat *stbuf)
{
   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   // The only directory option
   if ( strcmp(path,"/") == 0 )
   {
      stbuf->st_ino = 16; // Root dir starts on sector 16
      stbuf->st_size = 0;
      stbuf->st_mode = MODE_DIR(stbuf->st_mode);
      stbuf->st_mode = MODE_RO(stbuf->st_mode);
      return 0;
   }

   // Special files for 1K blocks or clusters
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec >= 0 && sec*8+25+7<=atrfs.sectors )
      {
         stbuf->st_mode = MODE_RO(stbuf->st_mode);
         stbuf->st_size = 1024;
         stbuf->st_ino = sec*8+25;
         return 0; // Good, can read this sector
      }
   }

   // Real or info files
   struct dos3_dir_entry *dirent;
   int isinfo;
   int r;
   r = dos3_get_dir_entry(path,&dirent,&isinfo);
   if ( r ) return r;
   stbuf->st_ino = CLUSTER_TO_SECTOR(dirent->start);
   if ( isinfo ) stbuf->st_ino += 0x10000;
   stbuf->st_size = (dirent->blocks-1)*1024 + BYTES2(dirent->file_size)%1024;
   if ( isinfo )
   {
      char *info = dos3_info(path,dirent);
      stbuf->st_size = strlen(info);
      free(info);
   }

   return 0; // Whatever, don't really care
}

/*
 * dos3_readdir()
 */
int dos3_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
   (void)path; // Always "/"
   (void)offset;
   (void)fi;

   if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s\n",__FUNCTION__,path);

   struct dos3_dir_entry *dirent = SECTOR(16);
   for ( int i=1; i<64; ++i ) // Entry 0 is info about the file system
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: entry %d, status %02x: %.8s.%.3s\n",__FUNCTION__,i,dirent[i].status,dirent[i].file_name,dirent[i].file_ext);
      if (!dirent[i].status) break;
      if ( dirent[i].status == FLAGS_ACTIVE ) continue; // Not in use
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
#if 0 // Reserved, DIR, and VTOC sectors
   // Create .sector4 ... .sector24 as appropriate
   {
      struct sector1 *s1 = SECTOR(1);
      unsigned char *zero = calloc(1,atrfs.sectorsize);
      if ( !zero ) return -ENOMEM; // Weird
      char name[32];
      int digits = 2;
      for (int sec=s1->boot_sectors + 1; sec<=atrfs.sectors && sec< 25; ++sec)
      {
         unsigned char *s = SECTOR(sec);
         if ( memcmp(s,zero,atrfs.sectorsize) == 0 ) continue; // Skip empty sectors
         sprintf(name,".sector%0*d",digits,sec);
         filler(buf,name,FILLER_NULL);
      }
      free(zero);
   }
#endif
#if 0 // Data clusters
   // Create .cluster000 ... .cluster126 as appropriate
   {
      unsigned char *zero = calloc(1,1024);
      if ( !zero ) return -ENOMEM; // Weird
      char name[32];
      int digits = 3;
      for (int sec=0; sec*8+25+7<=atrfs.sectors; ++sec)
      {
         unsigned char *s = CLUSTER(sec);
         if ( memcmp(s,zero,1024) == 0 ) continue; // Skip empty sectors
         sprintf(name,".cluster%0*d",digits,sec);
         filler(buf,name,FILLER_NULL);
      }
      free(zero);
   }
#endif
   return 0;
}

/*
 * dos3_read()
 */
int dos3_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
   (void)fi;

   // Magic /.cluster### files
   if ( strncmp(path,"/.cluster",sizeof("/.cluster")-1) == 0 )
   {
      int sec = string_to_sector(path);
      if ( sec < 0 || sec*8+25+7>atrfs.sectors ) return -ENOENT;

      int bytes = 1024;
      if (offset >= bytes ) return -EOF;
      unsigned char *s = CLUSTER(sec);
      bytes -= offset;
      s += offset;
      if ( (size_t)bytes > size ) bytes = size;
      memcpy(buf,s,bytes);
      return bytes;
   }

   // Look up file
   struct dos3_dir_entry *dirent;
   int isinfo;
   int r;
   r = dos3_get_dir_entry(path,&dirent,&isinfo);
   if ( r ) return r;

   // Info files
   if ( isinfo )
   {
      char *info = dos3_info(path,dirent);
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
   unsigned char *vtoc = SECTOR(24);
   int cluster = dirent->start;
   int bytes_read = 0;
   while ( size )
   {
      int bytes = 1024;
      if ( vtoc[cluster] == 0xff ) return -EIO; // Reserved entry
      if ( vtoc[cluster] == 0xfe ) return -EIO; // Free entry
      if ( vtoc[cluster] == 0xfd )
      {
         bytes = BYTES2(dirent->file_size)%1024;
      }
      unsigned char *s=CLUSTER(cluster);
      if ( !s ) return -EIO; // Invalid CLUSTER
      if ( offset < bytes )
      {
         bytes -= offset;
         s+=offset;
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
      if ( vtoc[cluster] == 0xFD ) break; // EOF
      cluster=vtoc[cluster];
   }
   if ( bytes_read ) return bytes_read;
   return -EOF;
}

// Implement these for read-write support
int dos3_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int dos3_unlink(const char *path);
int dos3_rename(const char *path1, const char *path2, unsigned int flags);
int dos3_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos3_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int dos3_truncate(const char *path, off_t size);
int dos3_newfs(void);

/*
 * dos3_statfs()
 */
int dos3_statfs(const char *path, struct statvfs *stfsbuf)
{
   (void)path; // meaningless
   stfsbuf->f_bsize = 1024;
   stfsbuf->f_frsize = 1024;
   stfsbuf->f_blocks = (atrfs.sectors - 24) / 8;
   unsigned char *vtoc = SECTOR(24);
   stfsbuf->f_bfree = 0;
   for ( int i=0;i<0x80;++i )
   {
      if ( vtoc[i]==0xfe ) ++stfsbuf->f_bfree;
   }
   stfsbuf->f_bavail = stfsbuf->f_bfree;
   stfsbuf->f_files = 63;
   stfsbuf->f_ffree = 0;
   struct dos3_dir_entry *dirent = SECTOR(16);
   for (int i=1;i<64;++i)
   {
      if ( (dirent[i].status == FLAGS_ACTIVE) || (dirent[i].status == 0x00) )
      {
         ++stfsbuf->f_ffree;
      }
   }
   stfsbuf->f_namemax = 12; // 8.3 including '.'
   // stfsbuf->f_namemax += 5; // Don't report this for ".info" files; not needed by FUSE
   return 0;
}

/*
 * dos3_fsinfo()
 */
char *dos3_fsinfo(void)
{
   char *buf=malloc(16*1024);
   if ( !buf ) return NULL;
   char *b = buf;
   
   struct dos3_dir_head *head = SECTOR(16);
   b+=sprintf(b,"Total data clusters: %d\n",head->max_free_blocks);
   int free_count=0;
   unsigned char *vtoc = SECTOR(24);
   for ( int i=0;i<0x80;++i )
   {
      if ( vtoc[i]==0xfe ) ++free_count;
   }
   b+=sprintf(b,"Free clusters:       %d\n",free_count);
   return buf;
}
