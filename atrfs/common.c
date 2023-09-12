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
