/*
 * info.c
 *
 * Functions for analyzing file data
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
#include <sys/mman.h>
#include "atrfs.h"

struct basic_header {
   unsigned char lomem[2]; // always 0000
   unsigned char vnt[2];
   unsigned char vnte[2];
   unsigned char vvt[2];
   unsigned char stmtab[2];
   unsigned char stmcur[2];
   unsigned char starp[2];
};

char *atr_info(const char *path,int filesize)
{
   unsigned char *filebuf = malloc(filesize);
   unsigned char *f;
   int r;
   char *buf,*b;
   char *p,*dot;

   if ( !filebuf ) return NULL;
   p = strdup(path);
   if ( !p ) { free(filebuf);return NULL;}
   dot = strrchr(p,'.');
   if ( dot ) *dot=0;
   else { free(filebuf);free(p);return NULL;}
   r = atr_read(p,(void *)filebuf,filesize,0,NULL);
   free(p);
   if ( r != filesize )
   {
      if ( options.debug ) fprintf(stderr,"DEBUG: %s: %s: Expected %d bytes but read %d\n",__FUNCTION__,path,filesize,r);
      free(filebuf);
      return NULL;
   }
   buf = malloc(256*1024); // FIXME: monitor and expand as needed
   b = buf;
   if ( !buf )
   {
      free(filebuf);
      return NULL;
   }

   struct basic_header *basic=(void *)filebuf;
   // Binary load file
   if ( filesize > 6 && filebuf[0] == 0xff && filebuf[1] == 0xff )
   {
      b+=sprintf(b,"Binary load file\n");
      f=filebuf;
      f+=2;
      filesize -= 2;
      while ( filesize > 4 )
      {
         int start,end;

         while (1 && filesize > 4)
         {
            start = BYTES2(f);
            f+=2;
            filesize -= 2;
            if ( start != 0xffff ) break;
            b+=sprintf(b,"  Stray second $FFFF header\n");
         }
         end = BYTES2(f);
         f+=2;
         filesize -= 2;
         int len = end - start + 1;
         b+=sprintf(b,"  $%04x -- $%04x  length: $%04x or %d\n",start,end,len,len);
         if ( len < 0 )
         {
            b+=sprintf(b,"  Negative block; file may be corrupt\n");
            break;
         }
         if ( len > filesize )
         {
            b+=sprintf(b,"  Block larger than remaining file size ($%04x > $%04x or %d > %d)\n",len,filesize,len,filesize);
            break;
         }
         f+=len;
         filesize -= len;
      }
   }
   // BASIC save file format
   else if ( BYTES2(basic->lomem) == 0 &&
             BYTES2(basic->vnt) == 0x100 &&
             BYTES2(basic->vnte) > BYTES2(basic->vnt) &&
             BYTES2(basic->vvt) - 1 == BYTES2(basic->vnte) &&
             BYTES2(basic->stmtab) > BYTES2(basic->vvt) &&
             BYTES2(basic->stmcur) > BYTES2(basic->stmtab) &&
             BYTES2(basic->starp) >= BYTES2(basic->stmcur) )
   {
      b+=sprintf(b,"BASIC Save file\n");
   }
   else
   {
      b+=sprintf(b,"File type unknown\n");
   }

   free(filebuf);
   buf = realloc(buf,strlen(buf)+1);
   return buf;
}
