/*
 * common.c
 *
 * Functions that may be used by multiple file systems
 *
 * Copyright 2023
 * Preston Crow
 *
 * Released under the GPL version 2.0
 */

#include FUSE_INCLUDE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <ctype.h>
#include "atrfs.h"

/*
 * string_to_sector()
 *
 * Used by magic .sector# and .cluster# functions
 */
int string_to_sector(const char *path)
{
   const char *num = path;
   int sec;

   // Skip over leading file name
   if ( *num == '/' )
   {
      ++num;
      while ( (*num >='a' && *num <='z') || *num=='.' ) ++num;
   }

   // Skip junk
   while ( *num && ( *num=='-' || *num=='_' || *num=='+' || *num=='.' ) ) ++num;

   // Allow sector number to be hex with '$' or '0x'
   if ( num[0] == '$' )
   {
      sec = strtol(num+1,NULL,16);
   }
   else if (strncmp(num,"0x",2) == 0 || strncmp(num,"0X",2) == 0)
   {
      sec = strtol(num+2,NULL,16);
   }
   else
   {
      sec = atoi(num);
   }
   if ( options.debug ) fprintf(stderr,"DEBUG: %s %s -> %s -> %d\n",__FUNCTION__,path,num,sec);
   return sec;
}

/*
 * atrfs_strncmp()
 *
 * strncmp() or strncasecmp() if --upcase
 */
int atrfs_strncmp(const char *s1, const char *s2, size_t n)
{
   if ( options.upcase )
   {
      return strncasecmp(s1,s2,n);
   }
   else
   {
      return strncmp(s1,s2,n);
   }
}

/*
 * atrfs_strcmp()
 *
 * strcmp() or strcasecmp() if --upcase
 */
int atrfs_strcmp(const char *s1, const char *s2)
{
   if ( options.upcase )
   {
      return strcasecmp(s1,s2);
   }
   else
   {
      return strcmp(s1,s2);
   }
}

/*
 * atrfs_memcmp()
 *
 * memcmp() or memcasecmp() if --upcase
 */
int atrfs_memcmp(const void *s1, const void *s2, size_t n)
{
   if ( options.upcase )
   {
      const char *cs1 = s1;
      const char *cs2 = s2;
      for ( size_t i=0;i<n;++i )
      {
         if ( toupper(cs1[i])<toupper(cs2[i]) ) return -1;
         if ( toupper(cs1[i])>toupper(cs2[i]) ) return 1;
      }
      return 0; // Same
   }
   else
   {
      return memcmp(s1,s2,n);
   }
}

/*
 * strcpy_upcase()
 *
 * copy the string; upcase if --upcase
 */
char *strcpy_upcase(char *dst,const char *src)
{
   if ( options.upcase )
   {
      char *d = dst;
      while (*src) {
         *dst++ = toupper(*src++);
      }
      *dst = 0;
      
      return d;
   }
   return strcpy(dst,src);
}

/*
 * strcpy_lowcase()
 *
 * copy the string; lowcase if --lowcase
 */
char *strcpy_lowcase(char *dst,const char *src)
{
   if ( options.lowcase )
   {
      char *d = dst;
      while (*src) {
         *dst++ = tolower(*src++);
      }
      *dst = 0;
      
      return d;
   }
   return strcpy(dst,src);
}
