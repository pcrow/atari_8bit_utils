/*
 * special.c
 *
 * Functions for accessing special files independent of the Atari file system
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
#include "atrfs.h"

/*
 * Macros and defines
 */

/*
 * Data types
 */
struct special_files {
   char *name;
   int (*getattr)(const char *,struct stat *);
   int (*read)(const char *,char *,size_t,off_t);
   void *write; // If we allow writing to boot sectors
   char *(*textdata)(void);
};

/*
 * Function prototypes
 */
int special_getattr(const char *path, struct stat *stbuf);
int special_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset);
int special_read(const char *path, char *buf, size_t size, off_t offset);
char *fsinfo_textdata(void);
char *bootinfo_textdata(void);

/*
 * Global variables
 */
const struct fs_ops special_ops = {
   .fs_getattr = special_getattr,
   .fs_readdir = special_readdir,
   .fs_read = special_read,
};

const struct special_files files[] = {
   {
      .name = ".bootinfo",
   },
   {
      .name = ".bootsectors"
   },
   {
      .name = ".fsinfo"
   },
};

/*
 * Functions
 */
int special_getattr(const char *path, struct stat *stbuf)
{
   if ( *path == '/' )
   {
      for (int i=0;(long unsigned)i<sizeof(files)/sizeof(files[0]);++i)
      {
         if ( strcmp(files[i].name,path+1)==0 )
         {
            stbuf->st_mode = MODE_RO(stbuf->st_mode); // These files are never writable
            stbuf->st_ino = 0x100000000 + i;
            // file-specific getattr
            // If this gets any more complicated, use a table of functions
            if ( i == 1 )
            {
               struct sector1 *sec1=SECTOR(1);
               if ( sec1->boot_sectors < 3 && atrfs.ssbytes )
               {
                  stbuf->st_size = sec1->boot_sectors * 128;
               }
               else
               {
                  stbuf->st_size = sec1->boot_sectors * atrfs.sectorsize - atrfs.ssbytes;
               }
            }
            else if ( i == 0 )
            {
               stbuf->st_size = strlen(bootinfo_textdata());
            }
            else if ( i == 2 )
            {
               stbuf->st_size = strlen(fsinfo_textdata());
            }
            return 0;
         }
      }
   }
   return -ENOENT; // Continue with regular file system
}

int special_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset)
{
   (void)offset; // Ignored in our usage mode
   if ( !options.nodotfiles && strcmp(path,"/") == 0 )
   {
      for (int i=0;(long unsigned)i<sizeof(files)/sizeof(files[0]);++i)
      {
         filler(buf, files[i].name, FILLER_NULL);
      }
   }
   return 0;
}

int special_read(const char *path, char *buf, size_t size, off_t offset)
{
   if ( *path == '/' )
   {
      for (int i=0;(long unsigned)i<sizeof(files)/sizeof(files[0]);++i)
      {
         if ( strcmp(files[i].name,path+1)==0 )
         {
            // If this gets any more complicated, use a switch with function tables
            if ( i==1 )
            {
               if ( options.debug ) fprintf(stderr,"DEBUG: %s %s Special file 1\n",__FUNCTION__,path);
               unsigned char *s=SECTOR(1);
               struct sector1 *sec1=SECTOR(1);
               int bytes = sec1->boot_sectors * atrfs.sectorsize - atrfs.ssbytes;
               if ( offset >= bytes ) return -EOF;
               s += offset;
               bytes -= offset;
               if ( (size_t)bytes > size ) bytes = size;
               memcpy(buf,s,bytes);
               return bytes;
            }
            if ( i==2 || i==0 )
            {
               if ( options.debug ) fprintf(stderr,"DEBUG: %s %s Special file %d\n",__FUNCTION__,path,i);
               char *b;
               if ( i==2 ) b = fsinfo_textdata();
               else b = bootinfo_textdata();
               int bytes = strlen(b);
               if (offset >= bytes) return -EOF;
               b += offset;
               bytes -= offset;
               if ( (size_t)bytes > size ) bytes = size;
               memcpy(buf,b,bytes);
               return bytes;
            }
            else
            {
               return -EOF; // Should be unreachable
            }
         }
      }
   }
   return 0; // Indicates non-special file
}

char *fsinfo_textdata(void)
{
   static char *buf;
   char *b;

   if ( buf ) return buf;
   buf=malloc(32*1024);
   b=buf;
   b+=sprintf(b,
           "File system information\n"
           "Sectors: %d\n"
           "Sector size: %d",
           atrfs.sectors,atrfs.sectorsize);
   if ( atrfs.ssbytes )
   {
      b+=sprintf(b," (except first three sectors are 128 bytes)");
   }
   b+=sprintf(b,
              "\n"
              "File system type: ");
   if ( fs_ops[atrfs.fstype]->name )
   {
      b+=sprintf(b,"%s",fs_ops[atrfs.fstype]->name);
   }
   else
   {
      b+=sprintf(b,"unknown");
   }
   b+=sprintf(b,"\n");
   // Allow each file system to have a function to call to display more information
   if ( fs_ops[atrfs.fstype] && fs_ops[atrfs.fstype]->fs_fsinfo )
   {
      char *t;
      t=(fs_ops[atrfs.fstype]->fs_fsinfo)();
      if ( t )
      {
         b+=sprintf(b,"%s",t);
         free(t);
      }
   }
   buf=realloc(buf,strlen(buf)+1);
   return buf;
}

char *bootinfo_textdata(void)
{
   static char *buf;
   char *b;
   int code_start;
   int code_offset;

   if ( buf ) return buf;
   buf=malloc(32*1024);
   b=buf;
   b+=sprintf(b,"Boot information\n");
   struct sector1 *sec1 = SECTOR(1);
   b+=sprintf(b,"Boot sectors: %d\n",sec1->boot_sectors);
   int boot = BYTES2(sec1->boot_addr);
   b+=sprintf(b,"Load boot sectors at $%04x-$%04x\n",boot,boot+128*sec1->boot_sectors-1);
   if ( sec1->jmp == 0x4c )
   {
      code_start = BYTES2(sec1->exec_addr);
      b+=sprintf(b,"Init with JMP $%04x\n",code_start);
   }
   else
   {
      code_start = boot + offsetof(struct sector1,jmp);
      b+=sprintf(b,"Init with JSR to $%04x\n",code_start);
   }
   code_offset = code_start - boot;
   if ( !code_start ) code_offset = 256;
   b+=sprintf(b,"Run after load at: $%02x%02x\n",sec1->dos_ini[1],sec1->dos_ini[0]);
   if ( sec1->max_open_files < 16 ) // Assume not DOS 2 if larger
   {
      b+=sprintf(b,"Max open files: %d ",sec1->max_open_files);
      if ( sec1->max_open_files == 3 ) b+=sprintf(b,"[default]");
      else b+=sprintf(b,"[default:3]");
      b+=sprintf(b," (DOS 2 and compatible only)\n");
   }
   if ( code_offset > (int)offsetof(struct sector1,drive_bits) )
   {
      int drives = 0;
      int bits=sec1->drive_bits;
      while ( bits & 0x01 )
      {
         ++drives;
         bits = bits >> 1;
      }
      if ( ! bits )
      {
         b+=sprintf(b,"Number of drives supported: %d ",drives);
         // I think DOS 1 and DOS 2.5 default to 4, but my samples may be hacked
         if ( drives == 2 ) b+=sprintf(b,"[DOS 2 default]");
         else if ( drives == 8 ) b+=sprintf(b,"[MyDOS default]");
         else b+=sprintf(b,"[DOS 2 default: 2]");
         b+=sprintf(b," (DOS 2 and compatible only)\n");
      }
      else
      {
         b+=sprintf(b,"DOS 2 Drive support bits: %02x (likely interpreted differently on this image)\n",sec1->drive_bits);
      }
   }
   if ( code_offset > (int)offsetof(struct sector1,dos_flag) )
   {
      b+=sprintf(b,"DOS 2 flag for DOS.SYS: $%02x\n",sec1->dos_flag);
   }
   if ( code_offset > (int)offsetof(struct sector1,dos_sector) )
   {
      b+=sprintf(b,"DOS 2 DOS.SYS starting sector: %d\n",BYTES2(sec1->dos_sector));
   }
   buf=realloc(buf,strlen(buf)+1);
   return buf;
}
