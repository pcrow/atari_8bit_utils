/*
 * Atari BASIC Program Analyzer
 *
 * Copyright 2023 Preston Crow
 * Released under the GPL version 2.0
 *
 * Based mostly on documentation at:
 *   https://www.atarimax.com/jindroush.atari.org/afmtbas.html
 *
 * One usage would be to test if a file is an Atari BASIC program.
 * If the return code is success (0), then it should load.
 *
 * There are a number of to-do comments where this could be expanded,
 * but this did all I needed it to do.
 *
 * Questions this raised while I was writing this:
 *
 * Normally the VNT offset is 0100 from LOMEM.  Sometimes it's higher.
 * It can't be lower, as this memory is used by BASIC as a stack.
 * Is this the rev-B bug that adds 16 bytes?  Or is this a hack to reserve
 * memory for assembly routines?  Or maybe both?
 *
 * Some programs have a bit of data at the start of the STARP area.  This
 * is just garbage as far as I can tell.  Where does that come from?
 * Since this memory will be used for dimed variables, I suppose you could
 * append anything you like to the end of the file within reason and it would
 * be harmless.  Like some adding metadata tags.
 *
 * The STMCUR area doesn't look to be useful, but it does save the command
 * used to save the program, which may be intersting.  Would there be any harm
 * in setting STARP to STMCUR and cutting those bytes from the end of the file?
 * Is saving this area in the first place arguably a bug?
 *
 * I believe the VVT (variable value table) is wiped on load.  Can you put
 * just anything in that memory?  Are the variable type and index number needed?
 * This is another place where you could probably stash some hidden data if you
 * wanted to.
 *
 * I'm just curious as to what was possible, not that I would expect anyone to
 * do anything with this now other than run this program to check the integrity
 * of files they have laying around.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>

struct basic_header {
   unsigned char lomem[2];
   unsigned char vnt[2];
   unsigned char vnte[2];
   unsigned char vvt[2];
   unsigned char stmtab[2];
   unsigned char stmcur[2];
   unsigned char starp[2];
};
struct basic_header_parsed {
   unsigned short lomem;
   unsigned short vnt;
   unsigned short vnte;
   unsigned short vvt;
   unsigned short stmtab;
   unsigned short stmcur;
   unsigned short starp;
};

int basic_parse(char *filename)
{
   int fd;
   struct basic_header head;
   struct basic_header_parsed h;
   unsigned char buf[64*1024];
   char vnt[256][256]; // variable names
   int vnc; // variable name count
   int vvc; // variable value count
   struct stat statbuf;
   int b;
   int s;

   fd=open(filename,O_RDONLY);
   if ( fd < 0 )
   {
      printf("%s: Unable to open file\n",filename);
      return -1;
   }
   fstat(fd,&statbuf);
   b=read(fd,&head,sizeof(head));
   if ( b != sizeof(head) )
   {
      printf("%s: Too short for header: %u bytes\n",filename,(unsigned int)statbuf.st_size);
      return -1;
   }
   // Parse the header values; don't assume we have 16-bit shorts and don't assume little-endian.
   for ( unsigned int i=0; i<sizeof(head); i+=2 )
   {
      ((unsigned short *)&h)[i/2] = ((unsigned char *)&head)[i] + 256 * ((unsigned char *)&head)[i+1];
      // printf("%04x\n",((unsigned short *)&h)[i/2]);
   }

   for ( unsigned int i=1; i<sizeof(head)/2; ++i )
   {
      if ( ((unsigned short *)&h)[i] < ((unsigned short *)&h)[i-1] )
      {
         // It's possible for a zero-length VNT or VVT if there are no variables.
         // It's possible for the STMTAB to be empty if there are no lines of code
         // It's possible for STMCUR to be empty if it was manually removed.
         printf("%s: Header offset fields decreasing\n",filename);
      }
   }
   if ( h.lomem )
   {
      printf("%s: Does not start with LOMEM of 0000, found %04x\n",filename,h.lomem);
      return -1;
   }
   if ( h.vnt < 0x0100 )
   {
      printf("%s: VNT starts at at least 0100 to reserve argument stack space, found %04x\n",filename,h.vnt);
      return -1;
   }
   if ( h.vnt > 0x0100 )
   {
      printf("%s: VNT should start at 0100, found %04x (wasted memory)\n",filename,h.vnt);
   }
   if ( h.vvt != h.vnte + 1 )
   {
      printf("%s: VVT does not start immediately after VNTE\n",filename);
      return -1;
   }
   if ( (h.stmtab - h.vvt) % 8 )
   {
      printf("%s: VVT is not a multiple of 8 bytes: %u bytes\n",filename,h.stmtab - h.vvt);
   }
   vvc = (h.stmtab - h.vvt)/8;

   /*
    * Variable Name Table
    */
   printf("%s: Variable Name Table: %04x-%04x\n",filename,h.vnt,h.vnte);
   s=h.vnte-h.vnt+1;
   b=read(fd,buf,s);
   if ( b != s )
   {
      printf("%s: Too short for VNT: %u bytes total, VNT is %d bytes\n",filename,(unsigned int)statbuf.st_size,s);
      return -1;
   }
   vnc=0; // Variable name count
   int namelen=0;
   memset(vnt,0,sizeof(vnt));
   int diff=0;
   for ( int i=0;i+1<s;++i )
   {
      if ( buf[i] != buf[0] )
      {
         diff=1;
      }
   }
   if ( !diff && vvc )
   {
      printf("%s: VNT is wiped with all values set to $%02x\n",filename,buf[0]);
   }
   else
   {
      for ( int i=0;i<s;++i )
      {
         if ( i+1 == s )
         {
            if ( buf[i] == 0 ) break; // There's supposed to be a NULL to also mark the end
            printf("%s: VNT doesn't end with a null: %02x\n",filename,buf[i]);
         }
         vnt[vnc][namelen++]=buf[i]&0x7f;
         if ( buf[i]&0x80 )
         {
            ++vnc;
            namelen=0;
         }
      }
   }
   if ( vnc != vvc )
   {
      if ( diff ) printf("%s: Variable name count (%d) doesn't match variable value entry count (%d)\n",filename,vnc,vvc);
      for (int i=vnc;i<vvc;++i)
      {
         sprintf(vnt[i],"_VAR_%d",i);
      }
   }

   // To-Do: detect and report non-printable VNT
   // To-Do: update VNT if non-printable and told to fix 

   /*
    * Variable Value Table
    */
   s=h.stmtab-h.vvt;
   b=read(fd,buf,s);
   if ( b != s )
   {
      printf("%s: Too short for VNT: %u bytes total, VVT is %d bytes\n",filename,(unsigned int)statbuf.st_size,s);
      return -1;
   }

   // Spec I found says byte 1 of each entry is: btNumber - offset to variable name table
   // I'm observing 0 or 128; nothing useful
   for ( int i=0;i<s/8;++i )
   {
      if ( buf[i*8+1] != i )
      {
         printf("%s: VVT entry %d is variable %d\n",filename,i,buf[i*8+1]);
      }
   }
   
   for ( int i=0;i<s/8;++i )
   {
      switch ( buf[i*8+0] )
      {
         case 0:
            printf("%s: Var %3d is scalar:             %02x%02x%02x%02x%02x%02x: %s\n",filename,i,buf[i*8+2],buf[i*8+3],buf[i*8+4],buf[i*8+5],buf[i*8+6],buf[i*8+7],vnt[i]);
            break;
         case 0x40:
            printf("%s: Var %3d is array (undimed):    %02x%02x%02x%02x%02x%02x: %s\n",filename,i,buf[i*8+2],buf[i*8+3],buf[i*8+4],buf[i*8+5],buf[i*8+6],buf[i*8+7],vnt[i]);
            break;
         case 0x41:
            printf("%s: Var %3d is array (dimmed):     %02x%02x%02x%02x%02x%02x: %s\n",filename,i,buf[i*8+2],buf[i*8+3],buf[i*8+4],buf[i*8+5],buf[i*8+6],buf[i*8+7],vnt[i]);
            break;
         case 0x80:
            printf("%s: Var %3d is string (undimed):   %02x%02x%02x%02x%02x%02x: %s\n",filename,i,buf[i*8+2],buf[i*8+3],buf[i*8+4],buf[i*8+5],buf[i*8+6],buf[i*8+7],vnt[i]);
            break;
         case 0x81:
            printf("%s: Var %3d is string (dimed):     %02x%02x%02x%02x%02x%02x: %s\n",filename,i,buf[i*8+2],buf[i*8+3],buf[i*8+4],buf[i*8+5],buf[i*8+6],buf[i*8+7],vnt[i]);
            break;
         default:
            printf("%s: Var %3d is unknown type %02x:  %02x%02x%02x%02x%02x%02x: %s\n",filename,i,buf[i*8+0],buf[i*8+2],buf[i*8+3],buf[i*8+4],buf[i*8+5],buf[i*8+6],buf[i*8+7],vnt[i]);
            break;
      }
   }

   /*
    * Token area (lines of the program)
    */
   s=h.stmcur-h.stmtab;
   b=read(fd,buf,s);
   if ( b != s )
   {
      printf("%s: File too short for token area: %d bytes expected, %d bytes read\n",filename,s,b);
      return -1;
   }
   {
      int i=0;
      int linecount=0;
      unsigned int prev=0;
      while ( i+2 < s ) // Must have 3 bytes left for a line
      {
         unsigned int linenum = buf[i] + 256 * buf[i+1];
         unsigned int len = buf[i+2];
         if ( prev && linenum <= prev )
         {
            printf("%s: Lines out of order; %u follows %u\n",filename,linenum,prev);
            break;
         }
         if ( linenum > 32767 )
         {
            printf("%s: Linenumber too large: %u\n",filename,linenum);
            break;
         }
         if ( len < 4 )
         {
            printf("%s: Line %u is too short: %u bytes\n",filename,linenum,len);
            break;
         }
         if ( i + (signed int)len > s )
         {
            printf("%s: Line %u is %u bytes but only %u bytes left\n",filename,linenum,len,s-i);
            break;
         }
         // To-do: Parse tokens (len, byte, ...) to verify line structure
         ++linecount;
         i += len;
         printf("%s: Line %u is %u bytes\n",filename,linenum,len);
      }
      if ( i<s )
      {
         printf("%s: Stray bytes at end of token area: %d\n",filename,s-i);
      }
      printf("%s: %d lines of code read\n",filename,linecount);
   }

   /*
    * STMCUR -- should be immiate save command that saved the program
    */
   s=h.starp-h.stmcur;
   b=read(fd,buf,s);
   if ( b != s )
   {
      printf("%s: Too short for STMCUR: %d bytes expected, %d bytes read\n",filename,s,b);
      return -1;
   }
   if ( s <= 3 )
   {
      printf("%s: Immediate command area too small: %d bytes\n",filename,s);
   }
   else
   {
      // To-do: Use a function for line parsing here and above
      unsigned int linenum = buf[0] + 256 * buf[0+1];
      unsigned int len = buf[0+2];
      printf("%s: Line %u (immediate) is %u bytes\n",filename,linenum,len);
      if ( linenum != 32768 )
      {
         printf("%s: Immeidate line number is not 32768 as expected\n",filename);
      }
      if ( len != (unsigned)s )
      {
         printf("%s: Immediate area (%d) is not the same size as the command (%d)\n",filename,s,len);
      }
   }

   /*
    * Should be EOF
    */
   b=read(fd,buf,sizeof(buf));
   if ( b )
   {
      printf("%s: %d bytes past end of immediate area\n",filename,b);
      FILE *hd = popen("hexdump -C","w");
      fwrite(buf,b,1,hd);
      pclose(hd);
   }
   close(fd);

   return 0;
}

int main(int argc,char *argv[])
{
   /*
    * Parse command line options
    */
   // To-do

   /*
    * Parse files
    */
   int r=0;
   while ( argc > 1 )
   {
      r+=basic_parse(argv[1]);
      ++argv;
      --argc;
   }
   return r;
}
