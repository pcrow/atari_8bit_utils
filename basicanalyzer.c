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
 * Detects extended BASIC programs
 *        * BASIC XL
 *        * BASIC XE
 *        * Turbo BASIC XL
 *        * Altirra BASIC
 * I do not have sufficient documenation to determine which BASIC a
 * program is using, but it detects extended commands and operands.
 *
 * Questions this raised while I was writing this:
 *
 * Normally the VNT offset is 0100 from LOMEM.  Sometimes it's higher.
 * It can't be lower, as this memory is used by BASIC as a stack.
 * This is almost always the rev-B bug that adds 16 bytes every time the
 * program is saved, but do any programs do this on purpose to save space
 * for assembly routines?  That would be clever, except that it's not saved.
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
 *
 * Hack for getting opcodes and operands:
 *
 * H1.SAV: change LET which is 06
 * O1.SAV: change PEEK which is 46
 */
#if 0 // can't use a comment block due to sed expressions
for ((X=0;X<112;++X)); do echo $X ; done | LC_ALL=C awk '{printf("%d LET OP%03dH%02X=OP%03dH%02X%c",$1+1024+128,$1,$1,$1,$1,155)}' > H1.LST
for ((X=0;X<100;++X)); do echo $X ; done | LC_ALL=C awk '{printf("%d LET OP%03dH%02X=PEEK(OP%03dH%02X)%c",$1+1024+128,$1,$1,$1,$1,155)}' > O1.LST

# Use Atari BASIC to ENTER files above and save them

X=H1.SAV ; V=06
X=O1.SAV ; V=46
STMTAB=$(basicanalyzer $X | grep STMTAB | sed -e 's/.* /0x/' | awk --non-decimal-data '{printf("%d\n",$1 - 1)}')
dd if=$X of=${X/1/2}a bs=1 count=$((STMTAB - 257))
dd if=$X of=${X/1/2}b bs=1 skip=$((STMTAB - 257))
sed -i -e "s/\x$V/\xff/g" ${X/1/2}b
hexdump -C ${X/1/2}b | sed -e 's/^[^ ]* *//' -e 's/ *|.*//' -e 's/ /\n/g' | grep . | awk '{if ( $1 == "ff" ) {printf("%02x\n",X);X=X+1} else print }' | sed -e 's/^/0x/' | LC_ALL=C awk --non-decimal-data '{printf("%c",$1)}' > ${X/1/2}c
cat ${X/1/2}a ${X/1/2}c > ${X/1/2}
#endif
/*
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

/*
 * Data types
 */

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

struct vnt {
   int vnt_size; // (vnte-vnt+1) bytes
   unsigned char *vnt_raw;
   int vnt_entry_count;
   unsigned char vname[130][256]; // BASIC will create 130 entries, though only 128 can be used.  Max name size is unclear if table is hacked.
};
struct bcd_float {
   unsigned char atarifloat[6];
};
struct arraydim {
   unsigned short offset;
   unsigned short dim1;
   unsigned short dim2;
};
struct stringdim {
   unsigned short offset;
   unsigned short length;
   unsigned short dim;
};
struct var {
   unsigned char var_type;
   unsigned char var_number;
   union {
      struct bcd_float scalar;
      struct arraydim array;
      struct stringdim string;
   } var_data;
};
struct vvt {
   int vvt_size; // Size is (stmtab-vvt) bytes
   unsigned char *vvt_raw;
   int vvt_entry_count; // (stmtab-vvt)/8
   struct var var[130];
};
struct token {
   unsigned char *raw;
   unsigned char token;
   unsigned char tokenlen;
   unsigned char *operands;
};
struct codeline {
   unsigned char *raw;
   unsigned short linenum;
   unsigned char linebytes;
   int token_count;
   struct token *tokens;
};

struct token_table {
   unsigned char *raw;
   int raw_size;
   int linecount;
   struct codeline *lines;
};

enum basic_mode {
   auto_detect,
   atari_basic,
   turbo_basic_xl,
   altirra_basic,
   basic_xl,
   basic_xe,
   basic_ap, // BASIC A+ (opcodes/operands in different order)
   unknown, // opcodes or operands outside of any known implementation
};

struct basic_program {
   char *filename;
   int fd;
   char *outfilename;
   int ofd;
   struct basic_header_parsed head;
   struct vnt vnt;
   struct vvt vvt;
   struct token_table code;
   struct token_table immediate;
   int junk_size;
   unsigned char *post_code_junk;
   int token_use_count[256];
   int operand_use_count[128];
   int var_use_count[128];
   int merge_minus_count;
   enum basic_mode compatibility;
   int turbo_basic_compatibility; // True if 'compatibility' could also be Turbo
   int hex_constant_out_of_range;
   int highest_token;
   int highest_operand;
   int basic_a_plus_save;
   int normal_save;
};


/*
 * Global variables
 *
 * These are set by command-line options
 *
 * Not all options are implemented
 */
// Options for display
int display_header=1;
int display_variables=1;
int display_lines=1;
int display_full_lines=0; // Full listing, not just per-line summary
int display_full_lines_with_nonascii=0; // If a string has embedded assembly, display it as-is (otherwise use hex)
int display_immediate_command=1;
int display_post_junk_hexdump=1;
enum basic_mode display_mode=auto_detect;

// Options for modifying the file
int fix_pointer_rev_b_bug=0; // If non-zero, adjust pointers for the VNT to start at 0100 from LOMEM
int strip_immediate=0; // If non-zero, remove the immediate command
int strip_end_data=0; // If non-zero, remove any data past the end
int recreate_vnt=0; // If non-zero, recreate the VNT if it has been intentionally mangled
int wipe_vvt=0; // If non-zero, clear all VVT values
int merge_minus=0; // If non-zero, merge unary minus with subsequent scalar
int remove_unreferenced_variables=0; // If non-zero, any variable that isn't used is removed; adjusts all references
// To-do: variable rename options
// To-do: variable re-order options

/*
 * parse_file()
 *
 * Parse a BASIC file into the provided struct.
 *
 * In: struct basic_program with the fd field set.
 */
int parse_file(struct basic_program *prog);
int read_and_parse_head(struct basic_program *prog);
int read_program_raw(struct basic_program *prog);
int parse_vnt(struct basic_program *prog);
int parse_vvt(struct basic_program *prog);
int parse_lines(struct basic_program *prog);
int parse_immediate(struct basic_program *prog);
int parse_line(struct basic_program *prog,struct token_table *table,int immediate);
int parse_line_into_tokens(struct basic_program *prog,struct codeline *line);
void detect_compatibility(struct basic_program *prog);
void display_program(struct basic_program *prog);
int modify_program(struct basic_program *prog);
int save_program(struct basic_program *prog,int modified);

int parse_file(struct basic_program *prog)
{
   int r;

   if ( !prog || prog->fd < 0 )
   {
      return -1; // Sanity check failed
   }

   r=read_and_parse_head(prog);
   if ( r ) return r;

   r=read_program_raw(prog);
   if ( r ) return r;

   r=parse_vnt(prog);
   if ( r ) return r;

   r=parse_vvt(prog);
   if ( r ) return r;

   r=parse_lines(prog);
   if ( r ) return r;

   r=parse_immediate(prog);
   if ( r ) return r;

   detect_compatibility(prog);

   return 0;
}

int read_and_parse_head(struct basic_program *prog)
{
   struct basic_header head;

   int b=read(prog->fd,&head,sizeof(head));
   if ( b != sizeof(head) )
   {
      printf("%s: Too short for header: %u bytes\n",prog->filename,b);
      return -1;
   }
   // Parse the header values; don't assume we have 16-bit shorts and don't assume little-endian.
   for ( unsigned int i=0; i<sizeof(head); i+=2 )
   {
      ((unsigned short *)&(prog->head))[i/2] = ((unsigned char *)&head)[i] + 256 * ((unsigned char *)&head)[i+1];
   }

   for ( unsigned int i=1; i<sizeof(head)/2; ++i )
   {
      if ( ((unsigned short *)&prog->head)[i] < ((unsigned short *)&prog->head)[i-1] )
      {
         // It's possible for a zero-length VNT or VVT if there are no variables.
         // It's possible for the STMTAB to be empty if there are no lines of code
         // It's possible for STMCUR to be empty if it was manually removed.
         printf("%s: Header offset fields decreasing\n",prog->filename);
         return -1;
      }
   }
   if ( prog->head.lomem )
   {
      printf("%s: Does not start with LOMEM of 0000, found %04x\n",prog->filename,prog->head.lomem);
      return -1;
   }
   if ( prog->head.vnt < 0x0100 )
   {
      printf("%s: VNT starts at at least 0100 to reserve argument stack space, found %04x\n",prog->filename,prog->head.vnt);
      return -1;
   }
   if ( prog->head.vnt & 0x0f )
   {
      printf("%s: VNT should start at 0100; Rev.B bug adds multiples of 0010; unexplained odd value: %04x\n",prog->filename,prog->head.vnt);
   }
   if ( prog->head.vnt > 0x0100 && !display_header ) // Alert in the header display if not skipped
   {
      printf("%s: VNT should start at 0100, found %04x (wasted memory)\n",prog->filename,prog->head.vnt);
   }
   if ( prog->head.vvt != prog->head.vnte + 1 )
   {
      printf("%s: VVT does not start immediately after VNTE\n",prog->filename);
      return -1;
   }
   if ( (prog->head.stmtab - prog->head.vvt) % 8 )
   {
      printf("%s: VVT is not a multiple of 8 bytes: %u bytes\n",prog->filename,prog->head.stmtab - prog->head.vvt);
      return -1;
   }
   if ( (prog->head.starp - prog->head.stmcur) > 256 )
   {
      printf("%s: Immediate command too large: %u bytes\n",prog->filename,prog->head.starp - prog->head.stmcur);
      return -1;
   }

   struct stat statbuf;
   fstat(prog->fd,&statbuf);
   prog->junk_size = statbuf.st_size - sizeof(struct basic_header) - prog->head.starp + prog->head.vnt;

   if ( prog->junk_size < 0 )
   {
      printf("%s: File is too small; expected %u bytes, observed %lu\n",prog->filename,prog->head.starp + prog->head.vnt + 1,statbuf.st_size);
      return -1;
   }
   return 0;
}

int read_program_raw(struct basic_program *prog)
{
   int b;

   // Read VNT
   prog->vnt.vnt_size = prog->head.vnte - prog->head.vnt + 1;
   prog->vnt.vnt_raw = malloc(prog->vnt.vnt_size);
   if ( !prog->vnt.vnt_raw )
   {
      printf("Internal error: Unable to allocate memory\n");
      return -1;
   }
   b=read(prog->fd,prog->vnt.vnt_raw,prog->vnt.vnt_size);
   if ( b != prog->vnt.vnt_size )
   {
      printf("%s: Too short for VNT: VNT is %d bytes, only read %d bytes\n",prog->filename,prog->vnt.vnt_size,b);
      return -1;
   }

   // Read VVT
   prog->vvt.vvt_size = prog->head.stmtab - prog->head.vvt;
   prog->vvt.vvt_raw = malloc(prog->vvt.vvt_size);
   if ( !prog->vnt.vnt_raw )
   {
      printf("Internal error: Unable to allocate memory\n");
      return -1;
   }
   b=read(prog->fd,prog->vvt.vvt_raw,prog->vvt.vvt_size);
   if ( b != prog->vvt.vvt_size )
   {
      printf("%s: Too short for VVT: VVT is %d bytes, only read %d bytes\n",prog->filename,prog->vvt.vvt_size,b);
      return -1;
   }

   // Read program code
   prog->code.raw_size = prog->head.stmcur - prog->head.stmtab;
   prog->code.raw = malloc(prog->code.raw_size);
   if ( !prog->code.raw )
   {
      printf("Internal error: Unable to allocate memory\n");
      return -1;
   }
   b=read(prog->fd,prog->code.raw,prog->code.raw_size);
   if ( b != prog->code.raw_size )
   {
      printf("%s: Too short for code: Code is %d bytes, only read %d bytes\n",prog->filename,prog->code.raw_size,b);
      return -1;
   }

   // Read immediate area
   prog->immediate.raw_size = prog->head.starp - prog->head.stmcur;
   prog->immediate.raw = malloc(prog->immediate.raw_size);
   if ( !prog->immediate.raw )
   {
      printf("Internal error: Unable to allocate memory\n");
      return -1;
   }
   b=read(prog->fd,prog->immediate.raw,prog->immediate.raw_size);
   if ( b != prog->immediate.raw_size )
   {
      printf("%s: Too short for immediate: Immediate is %d bytes, only read %d bytes\n",prog->filename,prog->immediate.raw_size,b);
      return -1;
   }

   // Read post-file junk
   if (prog->junk_size)
   {
      prog->post_code_junk = malloc(prog->junk_size);
      if ( !prog->post_code_junk )
      {
         printf("Internal error: Unable to allocate memory\n");
         return -1;
      }
      b=read(prog->fd,prog->post_code_junk,prog->junk_size);
      if ( b != prog->junk_size )
      {
         printf("%s: Failed to read junk at the end: expected %d bytes, only read %d bytes\n",prog->filename,prog->junk_size,b);
         return -1;
      }
   }

   // Done reading the file
   close(prog->fd);
   prog->fd = -1;
   return 0;
}

int parse_vnt(struct basic_program *prog)
{

   if ( prog->vnt.vnt_raw[prog->vnt.vnt_size-1] != 0 )
   {
      printf("%s: Last byte of VNT is not zero (likely mangled table)\n",prog->filename);
   }

   // Check for legal variable names and count
   int count=0;
   int illegal=0;
   int eov=1;
   int namlen=0;
   int toolong=0;
   int fixname=0;
   memset(&(prog->vnt.vname),0,sizeof(prog->vnt.vname));
   for ( int i=0;i<prog->vnt.vnt_size; ++i)
   {
      if ( i+1 == prog->vnt.vnt_size && prog->vnt.vnt_raw[i]==0 ) continue; // Normal to have the last byte be zero
      if ( count <= 129 && namlen+1 < 256 ) prog->vnt.vname[count][namlen]=prog->vnt.vnt_raw[i]&0x7f;
      else ++toolong;
      if ( !isalnum(prog->vnt.vnt_raw[i] & 0x7f) && prog->vnt.vnt_raw[i] != ('$'|0x80) && prog->vnt.vnt_raw[i] != ('('|0x80) )
      {
         ++fixname;
         ++illegal; // Note: Allows lower-case letters, but why be picky?
      }
      if ( prog->vnt.vnt_raw[i] & 0x80 ) // end of variable name
      {
         eov=1;
         if ( fixname && count < 130 )
         {
            sprintf((void *)prog->vnt.vname[count],"_var_%d",i);
         }
         fixname=0;
         ++count;
         namlen=0;
      }
      else
      {
         ++namlen;
         eov=0;
      }
      if ( eov && (prog->vnt.vnt_raw[i] == '$' || prog->vnt.vnt_raw[i] == '(' ) ) continue; // Legal: Implies string or array
   }
   // If illegal is non-zero, then the table is corrupt, possibly intentionally
   // If eov is non-zero, then it doesn't end with a variable name as expected
   // If toolong is non-zero, then a variable name was too long to fit in the table.

   prog->vnt.vnt_entry_count = count;
   // Just in case, make sure all entries have something printable
   for ( int i=count;i<130; ++i)
   {
      sprintf((void *)prog->vnt.vname[count],"_var_%d",i);
   }

   return 0;
}

int parse_vvt(struct basic_program *prog)
{
   prog->vvt.vvt_entry_count = (prog->head.stmtab-prog->head.vvt)/8;
   for ( int i=0; i<prog->vvt.vvt_entry_count && i<129; ++i )
   {
      prog->vvt.var[i].var_type = prog->vvt.vvt_raw[i*8+0];
      prog->vvt.var[i].var_number = prog->vvt.vvt_raw[i*8+1];
      switch(prog->vvt.var[i].var_type & 0xfe)
      {
         default: // Just copy bytes like a scalar
         case 0x00:
            for ( int j=0;j<6;++j )
               prog->vvt.var[i].var_data.scalar.atarifloat[j]= prog->vvt.vvt_raw[i*8+2+j];
            break;
         case 0x40: // array
         case 0x80: // string (both are three shorts
            prog->vvt.var[i].var_data.array.offset = prog->vvt.vvt_raw[i*8+2] + 256 * prog->vvt.vvt_raw[i*8+3];
            prog->vvt.var[i].var_data.array.dim1 = prog->vvt.vvt_raw[i*8+4] + 256 * prog->vvt.vvt_raw[i*8+5];
            prog->vvt.var[i].var_data.array.dim2 = prog->vvt.vvt_raw[i*8+6] + 256 * prog->vvt.vvt_raw[i*8+7];
            break;
      }
   }

   if ( prog->vvt.vvt_entry_count > 128 )
   {
      printf("%s: Too many variable value entries: %d\n",prog->filename,prog->vvt.vvt_entry_count);
      if ( prog->vvt.vvt_entry_count > 130 )
      {
         printf("%s: BASIC may enter two extra, but this is impossible\n",prog->filename);
         return -1;
      }
   }
   for ( int i=0;i<prog->vvt.vvt_entry_count; ++i )
   {
      if ( prog->vvt.var[i].var_number != i && i<129 )
      {
         printf("%s: Variable value table entry %d claims to be variable %d\n",prog->filename,i,prog->vvt.var[i].var_number);
         return -1;
      }
   }
   for ( int i=0;i<prog->vvt.vvt_entry_count; ++i )
   {
      unsigned char vtype = prog->vvt.var[i].var_type;
      if ( vtype != 0x00 && vtype != 0x40 && vtype != 0x41 && vtype != 0x80 && vtype != 0x81 )
      {
         printf("%s: Variable value table entry %d has illegal type: %0x2\n",prog->filename,i,prog->vvt.var[i].var_type);
         return -1;
      }
   }
   // Note: We could validate scalars have valid BCD and array dimensions are legal, but it doesn't matter
   return 0;
}

int parse_line(struct basic_program *prog,struct token_table *table,int immediate)
{
   // Pass 1: Count the lines, then allocate the array of lines
   int bytes = table->raw_size;
   unsigned char *next=table->raw;
   int count=0;
   int linenum,prev;
   if ( immediate ) prev=32767;
   else prev = -1;
   while ( bytes > 0 )
   {
      if ( bytes < 5 )
      {
         printf("%s: Last line doesn't fit: %d bytes left\n",prog->filename,bytes);
         return -1;
      }
      linenum = next[0]+256*next[1];
      if ( next[2] < 4 )
      {
         printf("%s: Illegal line of code less than 4 bytes: line %d is %d bytes; %d bytes of code remaining\n",prog->filename,linenum,next[2],bytes);
         if ( immediate )
         {
            table->linecount = 0;
            return 0;
         }
         else return -1;
      }
      if ( next[2] > bytes )
      {
         printf("%s: Illegal line of code longer than remaining space\n",prog->filename);
         return -1;
      }
      if ( linenum > 32767 + immediate )
      {
         printf("%s: Illegal line number %d\n",prog->filename,linenum);
         return -1;
      }
      if ( linenum <= prev )
      {
         printf("%s: line %d follows line %d\n",prog->filename,linenum,prev);
         return -1;
      }
      bytes -= next[2];
      next += next[2];
      ++count;
   }
   table->lines = malloc(sizeof(table->lines[0]) * count );
   if ( !table->lines )
   {
      printf("%s: Memory allocation error\n",prog->filename);
      return -1;
   }
   table->linecount = count;

   // Pass 2: Fill out the line structs
   bytes = table->raw_size;
   next=table->raw;
   for (int i=0; i<table->linecount; ++i)
   {
      table->lines[i].raw=next;
      table->lines[i].linenum = next[0]+256*next[1];
      table->lines[i].linebytes = next[2];
      next+=next[2];
   }
   // Pass 3: Parse lines into tokens
   int save_19=prog->token_use_count[0x19],save_1d=prog->token_use_count[0x1d];
   for (int i=0; i<table->linecount; ++i)
   {
      int r;
      r = parse_line_into_tokens(prog,&table->lines[i]);
      if ( r )
      {
         if ( immediate )
         {
            printf("%s: Immediate area corrupte\n",prog->filename);
         }
         else return r;
      }
   }
   if ( immediate )
   {
      if ( save_19 != prog->token_use_count[0x19] && save_1d == prog->token_use_count[0x1d] )
      {
         // Immdiate SAVE command detected with regular opcode, not BASIC A+
         prog->normal_save=1;
      }
      else if ( save_19 == prog->token_use_count[0x19] && save_1d != prog->token_use_count[0x1d] )
      {
         // Immediate SAVE command detect with BASIC A+ opcode.
         prog->basic_a_plus_save=1;
      }
   }

   return 0;
}

int scan_and_validate_token(struct basic_program *prog,struct token *token)
{
   // Scan the command to look for non-standard opcodes, operands, and count variable references
   ++prog->token_use_count[token->token];
   if ( token->token == 0x00 || token->token == 0x01 || token->token == 0x37 ) // rem, data, error
   {
      // 0x37 is wrong for BASIC A+ where it's the 'CP' opcode and 0x53 is 'ERROR-  '
      return 0;
   }
   if ( token->token > 0x37 ) // nonstandard command
   {
      ; // All other BASICs seem to use the same parsing for these
   }

   unsigned char *next=token->operands;
   int len=token->tokenlen-2;
   int minus=0;

   while ( len )
   {
      if ( *next & 0x80 )
      {
         ++prog->var_use_count[*next & 0x7f];
         ++next;
         --len;
         minus=0;
         continue;
      }
      ++prog->operand_use_count[*next];
      if ( *next == 0x0e && len >= 7 ) // scalar
      {
         if ( minus ) ++prog->merge_minus_count;
         next += 7;
         len -= 7;
         minus=0;
         continue;
      }
      if ( *next == 0x0f && len >= 2 && len >= 2 + next[1] ) // string
      {
         len -= 2+next[1];
         next+= 2+next[1];
         minus=0;
         continue;
      }
      minus=0;
      if ( *next == 0x36 ) minus=1; // unary minus, 39 in BASIC A+, but 36 there is a string comparison, so we're safe
      ++next;
      --len;
   }
   return 0;
}

int parse_line_into_tokens(struct basic_program *prog,struct codeline *line)
{
   line->token_count = 0;
   line->tokens=NULL;

   // Pass 1: Count the tokens, then allocate the array of tokens
   int bytes = line->linebytes-3;
   unsigned char *next=line->raw+3;
   int count=0;
   int past=3;

   while ( bytes )
   {
      // next[0] is the start of the next token from the beginning of the line
      if ( next[0] > line->linebytes)
      {
         printf("%s: line %d: Token %d longer than line\n",prog->filename,line->linenum,count);
         return -1;
      }
      if ( next[0] < past+2 )
      {
         printf("%s: line %d: Token %d too short: %u\n",prog->filename,line->linenum,count,next[0]);
         return -1;
      }
      ++count;
      past = next[0];
      bytes = line->linebytes - next[0];
      next = line->raw + next[0];
   }
   line->token_count = count;
   line->tokens = malloc(sizeof(struct token)*count);
   if ( !line->tokens )
   {
      printf("%s: Memory allocation error\n",prog->filename);
      return -1;
   }

   // Pass 2: Fill out the token structs
   next=line->raw+3;
   for (int i=0;i<line->token_count;++i)
   {
      line->tokens[i].raw=next;
      line->tokens[i].token=next[1];
      line->tokens[i].tokenlen=next[0] - (next - line->raw);
      line->tokens[i].operands=&next[2];
      next = line->raw + next[0];
      scan_and_validate_token(prog,&line->tokens[i]);
   }

   return 0;
}
int parse_lines(struct basic_program *prog)
{
   int r;
   r = parse_line(prog,&prog->code,0);
   return r;
}
int parse_immediate(struct basic_program *prog)
{
   int r;
   r = parse_line(prog,&prog->immediate,1);
   return r;
}

void detect_compatibility(struct basic_program *prog)
{
   int also_turbo = 1; // Clear if something found not compatible (also implied by 'unknown')
   prog->compatibility = atari_basic; // This is the starting point
   prog->highest_token=0;
   prog->highest_operand=0;
   for ( int i=0x0;i<256;++i) if ( prog->token_use_count[i] ) prog->highest_token = i;
   for ( int i=0x0;i<128;++i) if ( prog->operand_use_count[i] ) prog->highest_operand = i;

   for ( int i=0;i<0x11;++i)
   {
      if ( !prog->operand_use_count[i] ) continue;
      if ( i==0xd || i==0xe || i==0xf ) continue; // legal
      prog->compatibility = unknown; // Illegal operand
      prog->turbo_basic_compatibility = 0;
      return;
   }
   if ( prog->basic_a_plus_save && prog->highest_token <= 53 && prog->highest_operand <= 61 && !prog->operand_use_count[0x0d] )
   {
      prog->compatibility = basic_ap;
      prog->turbo_basic_compatibility = 0;
      return;
   }

   for ( int i=0x38;i<256;++i)
   {
      if ( prog->token_use_count[i] )
      {
         if ( i >= 0x66 )
         {
            prog->compatibility = unknown;  // No known BASIC uses opcodes 0x66 and above
         }
         else if ( i >= 0x64 ) { also_turbo = 0; prog->compatibility = basic_xe; } // 'END' token in BASIC XE
         else if ( i >= 0x59 ) { prog->compatibility = basic_xe; } // both XE and turbo
         else if ( i >= 0x52 || i == 0x45 || i == 0x42 || i == 0x41 || i == 0x40 || i <= 0x3b )
         {
            prog->compatibility = basic_xl;
         }
         else
         {
            prog->compatibility = altirra_basic;
         }
      }
   }
   for ( int i=0x55;i<127;++i)
   {
      if ( prog->operand_use_count[i] )
      {
         if ( i >= 0x6E )
         {
            prog->compatibility = unknown;  // No known BASIC uses operands 0x6E and above
         }
         else if ( i >= 0x69 ) prog->compatibility = turbo_basic_xl; // Only Turbo BASIC XL uses these
         else switch (i) {
               case 0x55:
               case 0x56:
               case 0x59:
               case 0x5B:
               case 0x5D:
               case 0x5F:
               case 0x64:
               case 0x65:
               case 0x66:
               case 0x67:
               case 0x68: // These are not in Altirra
                  if ( prog->compatibility == atari_basic || prog->compatibility == altirra_basic )
                  {
                     prog->compatibility = basic_xl;
                  }
                  break;
               default: // These are in all extended BASICS
                  if ( prog->compatibility == atari_basic )
                  {
                     prog->compatibility = altirra_basic;
                  }
                  break;
            }
      }
   }
   if ( prog->operand_use_count[0x0d] ) // hex constant
   {
      also_turbo = 0;
      if ( prog->compatibility == atari_basic ) prog->compatibility = altirra_basic;
   }
   if ( prog->compatibility == unknown || prog->compatibility == atari_basic ) also_turbo = 0;
   prog->turbo_basic_compatibility = also_turbo;
}

/*
 * display_program()
 *
 * Display the program based on the options set.
 */
void print_atari_string(unsigned char *str,int count)
{
   if ( display_full_lines_with_nonascii ) printf("%*s",count,str);
   else
   {
      for (;count;--count,++str)
      {
         if ( *str < 0x7f && isprint(*str) ) printf("%c",*str);
         else if ( *str >= 0x80 && isprint(*str&0x7f) ) printf("[7m" "%c" "[0m",*str&0x7f);
         else if ( *str < 0x7f ) printf(".");
         else printf("[7m" "." "[0m");
      }
   }
}

char *print_atari_float(struct basic_program *prog,struct bcd_float *f,int hex)
{
   static char buf[32];
   char *b = buf;
   int exp;
   unsigned long man;

   memset(buf,0,sizeof(buf));
   //b+=sprintf(buf,"%02x%02x%02x%02x%02x%02x:",f->atarifloat[0],f->atarifloat[1],f->atarifloat[2],f->atarifloat[3],f->atarifloat[4],f->atarifloat[5]); // debug

   if ( f->atarifloat[0]&0x80 )
   {
      *b='-';
      ++b;
   }

   exp = (f->atarifloat[0]&0x7f);
   exp -= 64; // exponent in excess 64 notation
   man = 0;
   for ( int i=1;i<6;++i )
   {
      man *= 100;
      man += (f->atarifloat[i] / 16) * 10 + (f->atarifloat[i] & 0xf);
   }
   if ( !man )
   {
      *b='0';
      return buf;
   }

   exp -= 4; // 100^4 is implied to put the decimal point after the first byte; normalize man.
   exp *= 2; // powers of 10 now, not 100
   while ( man%10 == 0 )
   {
      man /= 10;
      ++exp;
   }

   /*
    * Number is now 'man' * 10^'exp'
    */
   // Conver to integer if appropriate
   if ( hex && exp < 4 && !(f->atarifloat[0]&0x80) )
   {
      unsigned long m=man; // Don't modify 'man' in case we don't print
      for (int i=0;i<exp;++i) m *=10;
      if ( m <= 0xffff )
      {
         b+=sprintf(b,"$%04lX",m);
         return buf;
      }
   }

   if ( hex ) ++prog->hex_constant_out_of_range; // Hex constant isn't $0000 through $ffff

   // Add a small number (possibly zero) of zeros
   if ( exp >= 0 && exp < 7 )
   {
      b+=sprintf(b,"%lu",man);
      b+=sprintf(b,"000000"+(6-exp));
      return buf;
   }
   int digits=sprintf(b,"%lu",man); // Just to count the digits; don't adjust 'b'
   // Print as a regular decimal value
   if ( exp < 0 && digits+exp > -4 )
   {
      if ( digits+exp > 0 ) // digits before the decimal point
      {
         // characters are already printed, just move past them
         b+=digits+exp;
         *b='.';
         ++b;
         // Now remove the leading digits from m
         int mod=1;
         for (int i=0;i<-exp;++i ) mod*=10;
         man=man%mod;
         b+=sprintf(b,"%lu",man);
         return buf;
      }
      b+=sprintf(b,"0.");
      b+=sprintf(b,"000000"+(6+(digits+exp)));
      b+=sprintf(b,"%lu",man);
      return buf;
   }

   // Print it as x.xxxE+xx
   ++b;
   if ( digits > 1 )
   {
      sprintf(b,"%lu",man);
      *b='.'; // clobber duplicated first digit
      b+=digits;
   }
   *b='E';
   ++b;
   exp+=digits-1;
   b+=sprintf(b,exp>0?"+":"-");
   b+=sprintf(b,"%02d",abs(exp));
   return buf;
}
void print_token(struct basic_program *prog,struct token *token)
{
   enum basic_mode mode = display_mode;
   if ( mode == auto_detect )
   {
      mode = prog->compatibility;
      if ( mode == unknown ) mode = basic_xe; // Seems like the best guess
   }

   char *command_name[] = {
      "REM",
      "DATA",
      "INPUT",
      "COLOR",
      "LIST",
      "ENTER",
      "LET",
      "IF",
      "FOR",
      "NEXT",
      "GOTO",
      "GO TO",
      "GOSUB",
      "TRAP",
      "BYE",
      "CONT",
      "COM",
      "CLOSE",
      "CLR",
      "DEG",
      "DIM",
      "END",
      "NEW",
      "OPEN",
      "LOAD",
      "SAVE",
      "STATUS",
      "NOTE",
      "POINT",
      "XIO",
      "ON",
      "POKE",
      "PRINT",
      "RAD",
      "READ",
      "RESTORE",
      "RETURN",
      "RUN",
      "STOP",
      "POP",
      "?",
      "GET",
      "PUT",
      "GRAPHICS",
      "PLOT",
      "POSITION",
      "DOS",
      "DRAWTO",
      "SETCOLOR",
      "LOCATE",
      "SOUND",
      "LPRINT",
      "CSAVE",
      "CLOAD",
      "", // implied LET
      "ERROR-  ", // 37
   };
   char *command_name_turbo_basic_xl[] = {
      // https://github.com/rossumur/esp_8_bit/blob/master/atr_image_explorer.htm
      // Verified with direct testing
      "DPOKE", // 38
      "MOVE","-MOVE","*F",
      "REPEAT", // 3C
      "UNTIL","WHILE","WEND",
      "ELSE","ENDIF","BPUT","BGET","FILLTO","DO","LOOP","EXIT",
      "DIR","LOCK","UNLOCK","RENAME","DELETE","PAUSE","TIME$=","PROC",
      "  EXEC", // 50 (yes, it has two leading spaces)
      "ENDPROC","FCOLOR",
      "*L", // 53
      "------------------------------", // 54 (30 dashes, ignores operands; enter two or more dashes and this is produced)
      "RENUM","DEL","DUMP",
      "TRACE","TEXT","BLOAD","BRUN","GO#","#", "*B","PAINT",
      "CLS","DSOUND","CIRCLE",
      "%PUT", // 63
      // 64 on up are garbage
   };
   // BASIC XL 1.03 is mostly a subset of BASIC XE except for NUM and END
   char *command_name_basic_xe[] = {
      // https://www.virtualdub.org/downloads/Altirra%20BASIC%20Reference%20Manual.pdf
      // I created a file a sequence of extended opcodes, then did a list to see what they were for BASIC XE and BASIC XL 1.03
      "WHILE",    // 38, not in Altirra (with two spaces at the start in XL but not XE)
      "ENDWHILE", // 39, not in Altirra (with two spaces at the start in XL but not XE)
      "TRACEOFF", // 3A, not in Altirra
      "TRACE", // 3B, not in Altirra
      "ELSE", // 3C
      "ENDIF",
      "DPOKE",
      "LOMEM", // 3F
      "DEL",  // 40, not in Altirra
      "RPUT", // 41, not in Altirra
      "RGET", // 42, not in Altirra
      "BPUT",
      "BGET",
      "TAB",  // 45, not in Altirra
      "CP",
      "ERASE",
      "PROTECT",
      "UNPROTECT",
      "DIR",
      "RENAME",
      "MOVE",
      "MISSILE",
      "PMCLR",
      "PMCOLOR",
      "PMGRAPHICS",
      "PMMOVE", // 51, last command also in Altirra
      "PMWIDTH",
      "SET",
      "LVAR",
      "RENUM", // 55
      "FAST",  // 56 last one that is the same on BASIC XL (1.03)
      "LOCAL", // 57 BASIC XL has this as "NUM"
      "EXTEND", // 58 BASIC XL has this as "END"
      "PROCEDURE", // 59 BASIC XL displays garbage ("-?")
      " ", // 5A Listing displays as two spaces (intentional indentation?) // BASIC XL generates error 100 when listing
      "", // 5B Listing displays as one space
      "", // 5C Listing displays as one space
      "", // 5D Listing displays as one space
      "EXIT",
      "NUM",
      "HITCLR",
      "INVERSE",
      "NORMAL",
      "BLOAD",
      "END", // Are these real?  Two new contexts for END?
      "END", // yes, same as previous, not sure if this is real
      // 0x66 displays garbage
      // 0x67 generates error 100 when listing

   };
   char *command_name_basic_a_plus[] = {
      "REM",
      "DATA",
      "INPUT",
      "LIST",
      "ENTER",
      "LET",
      "IF",
      "FOR",
      "  NEXT",
      "GOTO",
      "RENUM",
      "GOSUB",
      "TRAP",
      "BYE",
      "CONT",
      "CLOSE",
      "CLR", // 10
      "DEG",
      "DIM",
      "WHILE",
      "  ENDWHILE",
      "TRACEOFF",
      "TRACE",
      "ELSE",
      "ENDIF",
      "END", // 19
      "NEW",
      "OPEN",
      "LOAD",
      "SAVE", // 1D
      "STATUS",
      "NOTE",
      "POINT", //20
      "XIO",
      "ON",
      "POKE",
      "DPOKE",
      "PRINT",
      "RAD",
      "READ",
      "RESTORE",
      "RETURN",
      "RUN",
      "STOP",
      "POP",
      "?",
      "GET",
      "PUT",
      "LOMEM", // 30
      "DEL",
      "RPUT", // 32
      "RGET",
      "BPUT",
      "BGET",
      "TAB",
      "CP", // 37
      "DOS",
      "ERASE",
      "PROTECT",
      "UNPROTECT",
      "DIR",
      "RENAME",
      "MOVE",
      "COLOR",
      "GRAPHICS",
      "PLOT",
      "POSITION",
      "DRAWTO",
      "SETCOLOR",
      "LOCATE",
      "SOUND",
      "LPRINT",
      "CSAVE", // 48
      "CLOAD",
      "MISSILE",
      "PMCLR",
      "PMCOLOR",
      "PMGRAPHICS",
      "PMMOVE",
      "PMWIDTH", // 4F
      "SET",
      "LVAR",
      "", // 52 (implied let?)
      "ERROR-  ", // 53 last opcode
   };

   char *operand_name[] = {
      ",", // starting at 0x12
      "$",
      ":", // statement end
      ";",
      "", // line end
      " GOTO ",
      " GOSUB ",
      " TO ",
      " STEP ",
      " THEN ",
      "#",
      "<=",
      "<>",
      ">=",
      "<",
      ">",
      "=",
      " ", // What is this?
      "*",
      "+",
      "-",
      "/",
      " NOT ",
      " OR ",
      " AND ",
      "(",
      ")",
      "=", // numeric assign
      "=", // string assign
      "<=", // string
      "<>",
      ">=",
      "<",
      ">",
      "=",
      "+", // unary
      "-", // 36
      "(", // string
      "(", // array
      "(", // dim array
      "(", // function
      "(", // dim string
      ",", // array
      "STR$",
      "CHR$",
      "USR",
      "ASC",
      "VAL",
      "LEN",
      "ADR",
      "ATN",
      "COS",
      "PEEK",
      "SIN",
      "RND",
      "FRE",
      "EXP",
      "LOG",
      "CLOG",
      "SQR",
      "SGN",
      "ABS",
      "INT",
      "PADDLE",
      "STICK",
      "PTRIG",
      "STRIG", // 54
   };
   char *operand_name_turbo_basic_xl[] = {
      // https://github.com/rossumur/esp_8_bit/blob/master/atr_image_explorer.htm
      // verified and corrected spaces with manual testing
      "DPEEK", // 55
      "&","!","INSTR","INKEY$"," EXOR ","HEX$","DEC",
      " DIV ","FRAC","TIME$","TIME"," MOD "," EXEC ","RND","RAND",
      "TRUNC","%0","%1","%2","%3"," GO# ","UINSTR","ERR",
      "ERL", // 6D
   };
   char *operand_name_basic_xe[] = { // A subset of BASIC XL/XE
      // https://www.virtualdub.org/downloads/Altirra%20BASIC%20Reference%20Manual.pdf
      // manual testing in BASIC XL/XE; both are the same for opcodes
      " USING", // 55, not in Altirra
      "%", // 56
      "!",
      "&",
      ";", // 59, not in Altirra
      "BUMP(",
      "FIND(", // 5B, not in Altirra
      "HEX$",
      "RANDOM(", // 5D, not in Altirra
      "DPEEK",
      "SYS", // 5F, not in Altirra
      "VSTICK",
      "HSTICK",
      "PMADR",
      "ERR", // 63, last one in Altirra
      "TAB", // 64
      "PEN",
      "LEFT$(",
      "RIGHT$(",
      "MID$(", // 68
      // opcode 69 causes BASIC XE to hang when listing
   };
   char *operand_name_basic_a_plus[] = {
      ",", // starting at 0x12
      "$",
      ":", // statement end
      ";",
      "", // line end
      " GOTO ",
      " GOSUB ",
      " TO ",
      " STEP ",
      " THEN ",
      " USING ", // 1C
      "#",
      "<=",
      "<>",
      ">=",
      "<",
      ">",
      "=",
      "^", // 24
      "*",
      "+",
      "-",
      "/",
      " NOT ",
      " OR ",
      " AND ",
      "!", // 2C
      "&", // 2D
      "(",
      ")",
      "=", // numeric assign
      "=", // string assign
      "<=", // string
      "<>", // 33
      ">=",
      "<",
      ">",
      "=",
      "+", // unary
      "-", // 39
      "(", // string
      "", // array
      "", // dim array
      "(", // function
      "(", // dim string
      ",", // array
      "STR$",
      "CHR$",
      "USR",
      "ASC",
      "VAL",
      "LEN",
      "ADR",
      "BUMP", // 47
      "FIND",
      "DPEEK", //49
      "ATN",
      "COS",
      "PEEK",
      "SIN",
      "RND",
      "FRE",
      "EXP(", // 50
      "LOG(",
      "CLOG(",
      "SQR(",
      "SGN(",
      "ABS(",
      "INT(",
      "SYS(",
      "PADDLE(",
      "STICK(",
      "PTRIG(",
      "STRIG(",
      "VSTICK(",
      "HSTICK(",
      "PMADR(",
      "ERR(", // 5F
      "TAB(", // 60
      "PEN(", // 61 last opcode
   };

   if ( mode == basic_ap && token->token < sizeof(command_name_basic_a_plus)/sizeof(command_name_basic_a_plus[0])  )
   {
      printf("%s",command_name_basic_a_plus[token->token]);
      if ( *command_name_basic_a_plus[token->token] ) printf(" "); // no space after implied let
   }
   else if ( token->token < sizeof(command_name)/sizeof(command_name[0]) )
   {
      printf("%s",command_name[token->token]);
      if ( *command_name[token->token] ) printf(" "); // no space after implied let
   }
   else if ( mode == basic_xl && token->token == 0x57 )
   {
      printf("NUM"); // "LOCAL" in BASIC XE
   }
   else if ( mode == basic_xl && token->token == 0x58 )
   {
      printf("END"); // "EXTEND" in BASIC XE
   }
   else if ( ( mode == altirra_basic || mode == basic_xl || mode == basic_xe ) && token->token - sizeof(command_name)/sizeof(command_name[0]) < sizeof(command_name_basic_xe)/sizeof(command_name_basic_xe[0]) )
   {
      printf("%s",command_name_basic_xe[token->token - sizeof(command_name)/sizeof(command_name[0])]);
   }
   else if ( ( mode == turbo_basic_xl ) && token->token - sizeof(command_name)/sizeof(command_name[0]) < sizeof(command_name_turbo_basic_xl)/sizeof(command_name_turbo_basic_xl[0]) )
   {
      printf("%s",command_name_turbo_basic_xl[token->token - sizeof(command_name)/sizeof(command_name[0])]);
   }
   else
   {
      printf("(command %02x) ",token->token);
   }

   // Print operands
   // REM, DATA, and ERROR are special
   if ( token->token == 0x00 || token->token == 0x01 || (token->token == 0x37 && mode != basic_ap) || (token->token == 0x53 && mode == basic_ap) ) // rem, data, error
   {
      print_atari_string(token->operands,token->tokenlen-3);
      return;
   }

   unsigned char *next=token->operands;
   int len=token->tokenlen-2;

   while ( len )
   {
      if ( *next & 0x80 )
      {
         if ( (*next & 0x7f) < prog->vnt.vnt_entry_count )
         {
            printf("%s",prog->vnt.vname[*next & 0x7f]);
         }
         else
         {
            printf("_var_%d",*next & 0x7f);
         }
         ++next;
         --len;
      }
      else if ( (*next == 0x0e || *next == 0x0d) && len >= 7 ) // 0D is a float displayed as hex in Altirra, XL, and XE BASICS
      {
         printf("%s",print_atari_float(prog,(void *)next+1,(*next == 0x0d)) );
         next += 7;
         len -= 7;
      }
      else if ( *next == 0x0f && len >= 2 && len >= 2 + next[1] )
      {
         printf("%c",'"');
         print_atari_string(&next[2],next[1]);
         printf("%c",'"');
         len -= 2+next[1];
         next+= 2+next[1];
      }
      else if ( mode == basic_ap &&  *next - (unsigned)0x12 < sizeof(operand_name_basic_a_plus)/sizeof(operand_name_basic_a_plus[0]) )
      {
         printf("%s",operand_name_basic_a_plus[*next-0x12]);
         ++next;
         --len;
      }
      else if ( *next - (unsigned)0x12 < sizeof(operand_name)/sizeof(operand_name[0]) )
      {
         printf("%s",operand_name[*next-0x12]);
         ++next;
         --len;
      }
      else if ( ( mode == altirra_basic || mode == basic_xl || mode == basic_xe ) && *next - sizeof(operand_name)/sizeof(operand_name[0]) < sizeof(operand_name_basic_xe)/sizeof(operand_name_basic_xe[0]) )
      {
         printf("%s",operand_name_basic_xe[*next - 0x12 - sizeof(operand_name)/sizeof(operand_name[0])]);
         ++next;
         --len;
      }
      else if ( ( mode == turbo_basic_xl ) && *next - sizeof(operand_name)/sizeof(operand_name[0]) < sizeof(operand_name_turbo_basic_xl)/sizeof(operand_name_turbo_basic_xl[0]) )
      {
         printf("%s",operand_name_turbo_basic_xl[*next - 0x12 - sizeof(operand_name)/sizeof(operand_name[0])]);
         ++next;
         --len;
      }
      else
      {
         printf("(operand %02x)",*next);
         break;
      }
   }
}

void print_line(struct basic_program *prog,struct codeline *line)
{
   if ( line->linenum < 32768 ) printf("%u ",line->linenum);
   for ( int i=0;i<line->token_count;++i ) print_token(prog,&line->tokens[i]);
   printf("\n");
}

void print_listing(struct basic_program *prog,struct token_table *table)
{
   for ( int i=0; i<table->linecount; ++i ) print_line(prog,&table->lines[i]);
}

void display_program(struct basic_program *prog)
{
   if ( display_header )
   {
      printf("%s: BASIC header\n",prog->filename);
      printf("%s: LOMEM  %04x\n",prog->filename,prog->head.lomem);
      if ( prog->head.vnt == 0x100 )
         printf("%s: VNT    %04x\n",prog->filename,prog->head.vnt);
      else
         printf("%s: VNT    %04x (Rev.B bug added %04x bytes)\n",prog->filename,prog->head.vnt,prog->head.vnt-0x100);
      printf("%s: VNTE   %04x\n",prog->filename,prog->head.vnte);
      printf("%s: VVT    %04x\n",prog->filename,prog->head.vvt);
      printf("%s: STMTAB %04x\n",prog->filename,prog->head.stmtab);
      printf("%s: STMCUR %04x\n",prog->filename,prog->head.stmcur);
      printf("%s: STARP  %04x\n",prog->filename,prog->head.starp);
      if ( prog->junk_size )
         printf("%s: junk   %04x (extra bytes at end, not in header)\n",prog->filename,prog->junk_size );
   }

   if ( display_variables )
   {
      printf("%s: %d variables\n",prog->filename,prog->vvt.vvt_entry_count);
      for (int i=0;i<prog->vvt.vvt_entry_count; ++i)
      {
         unsigned char *varname = prog->vnt.vname[i];
         char buf[64];
         if ( i > prog->vnt.vnt_entry_count )
         {
            sprintf(buf,"_invalid_var_name_%d",i);
            varname=(void *)buf;
         }
         switch(prog->vvt.var[i].var_type)
         {
         case 0:
            printf("%s: Var %3d is scalar:             %s: %s",prog->filename,i,
                   print_atari_float(prog,&prog->vvt.var[i].var_data.scalar,0),
                   varname);
            break;
         case 0x40:
            printf("%s: Var %3d is array (undimed):    offset %04x  dim1 %04x  dim2 %04x: %s",prog->filename,i,
                   prog->vvt.var[i].var_data.array.offset,
                   prog->vvt.var[i].var_data.array.dim1,
                   prog->vvt.var[i].var_data.array.dim2,
                   varname);
            break;
         case 0x41:
            printf("%s: Var %3d is array (undimed):    offset %04x  dim1 %04x  dim2 %04x: %s",prog->filename,i,
                   prog->vvt.var[i].var_data.array.offset,
                   prog->vvt.var[i].var_data.array.dim1,
                   prog->vvt.var[i].var_data.array.dim2,
                   varname);
            break;
         case 0x80:
            printf("%s: Var %3d is string (undimed):   offset %04x  length %04x  dim %04x: %s",prog->filename,i,
                   prog->vvt.var[i].var_data.string.offset,
                   prog->vvt.var[i].var_data.string.length,
                   prog->vvt.var[i].var_data.string.dim,
                   varname);
            break;
         case 0x81:
            printf("%s: Var %3d is string (dimed):     offset %04x  length %04x  dim %04x: %s",prog->filename,i,
                   prog->vvt.var[i].var_data.string.offset,
                   prog->vvt.var[i].var_data.string.length,
                   prog->vvt.var[i].var_data.string.dim,
                   varname);
            break;
         default:
            printf("%s: Var %3d is unknown type %02x:  %02x%02x%02x%02x%02x%02x: %s",prog->filename,i,prog->vvt.var[i].var_type,
                   prog->vvt.var[i].var_data.scalar.atarifloat[0],
                   prog->vvt.var[i].var_data.scalar.atarifloat[1],
                   prog->vvt.var[i].var_data.scalar.atarifloat[2],
                   prog->vvt.var[i].var_data.scalar.atarifloat[3],
                   prog->vvt.var[i].var_data.scalar.atarifloat[4],
                   prog->vvt.var[i].var_data.scalar.atarifloat[5],
                   varname);
            break;
         }
         printf(" (%d references)\n",i<128?prog->var_use_count[i]:0);
      }
   }

   if ( display_lines && display_full_lines ) print_listing(prog,&prog->code);
   else if ( display_lines )
   {
      printf("%s: Lines of code: %d\n",prog->filename,prog->code.linecount);
      for (int i=0;i<prog->code.linecount;++i)
      {
         printf("  %d line is %d bytes with %d statements\n",prog->code.lines[i].linenum,prog->code.lines[i].linebytes,prog->code.lines[i].token_count);
      }
   }
   if ( display_immediate_command )
   {
      if ( prog->immediate.linecount == 0 )
         printf("%s: No immediate command\n",prog->filename);
      else
      {
         //printf("%s: Immediate command:  %d bytes with %d statements\n",prog->filename,prog->immediate.lines[0].linebytes,prog->immediate.lines[0].token_count);
         print_listing(prog,&prog->immediate);
      }
   }

   // Report on possible optimizations
   if ( prog->merge_minus_count )
   {
      // This is due to the parser being very unoptimized
      printf("%s: unary minus before a scaler %d instances could be merged\n",prog->filename,prog->merge_minus_count);
   }

   // Report non-standard opcodes/operands
   if ( prog->compatibility != atari_basic )
   {
      int match_display=0;
      switch ( prog->compatibility )
      {
         case basic_ap:
            if( display_mode == basic_ap ) match_display=1;
            break;
         case altirra_basic:
            if ( display_mode == altirra_basic || display_mode == basic_xl || display_mode == basic_xe ) match_display=1;
            break;
         case basic_xl:
            if ( display_mode == basic_xl || display_mode == basic_xe ) match_display=1;
            break;
         case basic_xe:
            if ( display_mode == basic_xl || display_mode == basic_xe ) match_display=1;
            break;
         default: break;
      }
      if ( display_mode == turbo_basic_xl && prog->turbo_basic_compatibility ) match_display=1;

      if ( !match_display )
      {
         printf("%s: WARNING: File opcodes/operands do not match display setting\n",prog->filename);
         printf("%s: Detected compatibility: ",prog->filename);
         switch(prog->compatibility)
         {
            case basic_ap:
               printf("BASIC A+");
               break;
            case altirra_basic:
               printf("Altirra BASIC, "); // fall through
            case basic_xl:
               printf("BASIC XL, "); // fall through
            case basic_xe:
               printf("BASIC XE");
               break;
            case turbo_basic_xl:
               printf("Turbo BASIC XL");
               break;
            default: // Shouldn't hit this
            case unknown:
               printf("unknown");
         }
         if ( prog->turbo_basic_compatibility ) printf(", Turbo BASIC XL");
         printf("\n");

         int weird_opcode=0;
         int weird_operand=0;
         for ( int i=0x38;i<256;++i)
         {
            if ( display_mode == turbo_basic_xl && i <= 0x63 ) continue;
            if ( display_mode == basic_xl && i <= 0x58 ) continue;
            if ( display_mode == basic_xe && i <= 0x65 ) continue;
            if ( display_mode == altirra_basic && i <= 0x51 &&
                 i!=0x38 && i!=0x39 && i!=0x3A && i!=0x3B && i!=0x41 && i!=0x42 && i!=0x45
               ) continue;
            if ( prog->token_use_count[i] )
            {
               ++weird_opcode;
               printf("%s: WARNING: non-standard opcode %02x used %d times; alternate BASIC implementation suspected\n",prog->filename,i,prog->token_use_count[i]);
            }
         }
         for ( int i=0;i<128;++i)
         {
            if ( i==0xd && ( display_mode == altirra_basic || display_mode == basic_xl || display_mode == basic_xe ) ) continue; // hex constant
            if ( i==0xe || i==0xf ) continue; // constant float or string
            if ( i >=0x12 && i <= 0x54 ) continue; // standard operands
            if ( display_mode == turbo_basic_xl && i >= 0x55 && i <= 0x6D ) continue;
            if ( (display_mode == basic_xl || display_mode == basic_xe) && i >= 0x55 && i <= 0x68 ) continue;
            if ( display_mode == altirra_basic && i >= 0x55 && i <= 0x63 &&
                 i!=0x55 && i!=0x59 && i!=0x5B && i!=0x5D && i!=0x5F
               ) continue;
            if ( prog->operand_use_count[i] )
            {
               ++weird_operand;
               printf("%s: WARNING: non-standard operand %02x used %d times; alternate BASIC implementation suspected\n",prog->filename,i,prog->operand_use_count[i]);
            }
         }
      }
   }
   if ( prog->hex_constant_out_of_range )
   {
      printf("%s: WARNING: hex constants outside of $0000-$FFFF used %d times\n",prog->filename,prog->hex_constant_out_of_range);
   }

   if ( display_post_junk_hexdump && prog->junk_size )
   {
      printf("%s: %d bytes past end of immediate area\n",prog->filename,prog->junk_size);
      fflush(stdout);
      FILE *hd = popen("hexdump -C","w");
      fwrite(prog->post_code_junk,prog->junk_size,1,hd);
      pclose(hd);
   }
}

/*
 * modify_program()
 */
int modify_program(struct basic_program *prog)
{
   int modified = 0;

   if ( fix_pointer_rev_b_bug && prog->head.vnt != 0x100 )
   {
      int adjust = prog->head.vnt - 0x100;
      printf("%s: Adjusting VNT to 0100, saving %d bytes of memory\n",prog->filename,adjust);
      prog->head.vnt -= adjust;
      prog->head.vnte -= adjust;
      prog->head.vvt -= adjust;
      prog->head.stmtab -= adjust;
      prog->head.stmcur -= adjust;
      prog->head.starp -= adjust;
      modified=1;
   }

   if ( strip_immediate && prog->immediate.raw_size )
   {
      printf("%s: Eliminate immediate command: %d bytes\n",prog->filename,prog->immediate.raw_size);
      prog->immediate.linecount=0;
      prog->immediate.raw_size=0;
      prog->head.starp = prog->head.stmcur;
   }

   if ( strip_end_data && prog->junk_size )
   {
      printf("%s: Eliminate junk at end of file: %d bytes\n",prog->filename,prog->junk_size);
      prog->junk_size=0;
      free(prog->post_code_junk);
      prog->post_code_junk=0;
      modified=1;
   }

   int regen_vnt=0;
   if ( recreate_vnt )
   {
      for (int i=0;i<prog->vvt.vvt_entry_count;++i)
      {
         if ( i > prog->vnt.vnt_entry_count || !isalpha(prog->vnt.vname[i][0]&0x7f) )
         {
            char l='V';
            char *suffix="";
            if ( prog->vvt.var[i].var_type & 0x40 )
            {
               l='A';
               suffix="(";
            }
            if ( prog->vvt.var[i].var_type & 0x80 )
            {
               l='S';
               suffix="$";
            }
            sprintf((void *)prog->vnt.vname[i],"%c%d%s",l,i,suffix);
            regen_vnt=1;
         }
      }
   }

   if ( wipe_vvt )
   {
      int wipe=0;
      for (int i=0;i<prog->vvt.vvt_entry_count;++i)
      {
         if ( prog->vvt.var[i].var_type == 0x41 ) { prog->vvt.var[i].var_type = 0x40; wipe=1; }
         if ( prog->vvt.var[i].var_type == 0x81 ) { prog->vvt.var[i].var_type = 0x80; wipe=1; }
         struct var compare;
         memset(&compare,0,sizeof(compare));
         if ( memcmp(&prog->vvt.var[i].var_data,&compare.var_data,sizeof(compare.var_data)) != 0 ) wipe=1;
         memset( &prog->vvt.var[i].var_data,0,sizeof(prog->vvt.var[i].var_data) );
      }
      printf("%s: Wipe variable value table%s\n",prog->filename,wipe?"":" (already wiped)");
      modified+=wipe;
   }

   if ( merge_minus )
   {
      if ( prog->merge_minus_count )
      {
         printf("%s: unary minus merging not yet implemented\n",prog->filename); // FIXME
      }
   }

   if ( remove_unreferenced_variables )
   {
      // First remove any entries beyond the legal 128 variables
      // BASIC will add 129 and 130 in the process of generating an error, but nothing beyond that
      if ( prog->vvt.vvt_entry_count > 128 )
      {
         prog->vvt.vvt_entry_count = 128;
         int minus = prog->vvt.vvt_size - 8 * prog->vvt.vvt_entry_count;
         prog->vvt.vvt_size -= minus;
         prog->head.stmtab -= minus;
         prog->head.stmcur -= minus;
         prog->head.starp -= minus;
         modified=1;
      }
      if ( prog->vnt.vnt_entry_count > 128 )
      {
         prog->vnt.vnt_entry_count = 128;
         regen_vnt=1; // Need to update vnt_raw for writing out.
      }
      int referenced=0;
      for ( int i=prog->vvt.vvt_entry_count - 1;i>=0;--i )
      {
         if ( i>=128 ) i=127;
         if ( prog->var_use_count[i] ) ++referenced;
         if ( prog->var_use_count[i] == 0 && !referenced ) // easy to remove at the end
         {
            printf("%s: Variable %s is unreferenced; removed\n",prog->filename,prog->vnt.vname[i]);
            prog->vvt.vvt_entry_count = i;
            int minus = prog->vvt.vvt_size - 8 * prog->vvt.vvt_entry_count;
            prog->vvt.vvt_size -= minus;
            prog->head.stmtab -= minus;
            prog->head.stmcur -= minus;
            prog->head.starp -= minus;
            modified=1;
            regen_vnt=1; // Need to update vnt_raw for writing out.
         }
         else if ( prog->var_use_count[i] == 0 )
         {
            printf("%s: Variable %s is unreferenced; removal not implemented yet\n",prog->filename,prog->vnt.vname[i]); // FIXME
         }
      }
   }

   if ( regen_vnt )
   {
      int increase;
      modified = 1;
      int s=0;
      if ( prog->vnt.vnt_entry_count > prog->vvt.vvt_entry_count ) prog->vnt.vnt_entry_count = prog->vvt.vvt_entry_count;
      for ( int i=0;i<prog->vnt.vnt_entry_count; ++i ) s+=strlen((void *)prog->vnt.vname[i]);
      ++s;
      prog->vnt.vnt_raw = malloc(s);
      increase = s - prog->vnt.vnt_size;
      prog->vnt.vnt_size = s;
      unsigned char *b = prog->vnt.vnt_raw;
      if ( !b )
      {
         printf("%s: Memory allocation error\n",prog->filename);
         exit(-1); // Return value is 'modified'
      }
      for ( int i=0;i<prog->vnt.vnt_entry_count; ++i )
      {
         b+=sprintf((char *)b,"%s",prog->vnt.vname[i]);
         b[-1] |= 0x80;
      }
      prog->head.vnte += increase;
      prog->head.vvt += increase;
      prog->head.stmtab += increase;
      prog->head.stmcur += increase;
      prog->head.starp += increase;
   }

   return modified;
}

/*
 * save_program()
 *
 * Write out the BASIC file if an output file was given.
 * Skip if no modifications and the output and input are the same.
 */
int save_program(struct basic_program *prog,int modified)
{
   if ( !prog->outfilename ) return 0;
   if ( !modified && strcmp(prog->filename,prog->outfilename)== 0 ) return 0;

   prog->ofd = open(prog->outfilename,O_WRONLY|O_CREAT|O_TRUNC,0644);
   if ( prog->ofd < 0 )
   {
      printf("%s: Failed to open output file %s\n",prog->filename,prog->outfilename);
      return -1;
   }

   int r=0;
   int b;

   // Write header
   struct basic_header h;
   h.lomem[0]=prog->head.lomem&0xff;
   h.lomem[1]=prog->head.lomem/256;
   h.vnt[0]=prog->head.vnt&0xff;
   h.vnt[1]=prog->head.vnt/256;
   h.vnte[0]=prog->head.vnte&0xff;
   h.vnte[1]=prog->head.vnte/256;
   h.vvt[0]=prog->head.vvt&0xff;
   h.vvt[1]=prog->head.vvt/256;
   h.stmtab[0]=prog->head.stmtab&0xff;
   h.stmtab[1]=prog->head.stmtab/256;
   h.stmcur[0]=prog->head.stmcur&0xff;
   h.stmcur[1]=prog->head.stmcur/256;
   h.starp[0]=prog->head.starp&0xff;
   h.starp[1]=prog->head.starp/256;
   b=write(prog->ofd,&h,sizeof(h));
   if ( b!=sizeof(h) )
   {
      printf("%s: Failed writing BASIC header\n",prog->outfilename);
      r=-1;
      goto out;
   }
   // Write VNT
   b=write(prog->ofd,prog->vnt.vnt_raw,prog->vnt.vnt_size);
   if ( b != prog->vnt.vnt_size )
   {
      printf("%s: Failed writing VNT\n",prog->outfilename);
      r=-1;
      goto out;
   }
   // Write VVT
   for (int i=0;i<prog->vvt.vvt_entry_count;++i)
   {
      b=write(prog->ofd,&prog->vvt.var[i].var_type,1);
      b+=write(prog->ofd,&prog->vvt.var[i].var_number,1);
      unsigned char *v;
      unsigned char buf[6];
      switch ( prog->vvt.var[i].var_type )
      {
         case 0x40:
         case 0x41:
         case 0x80:
         case 0x81: // Both types are compatible
            buf[0]=prog->vvt.var[i].var_data.array.offset & 0xff;
            buf[1]=prog->vvt.var[i].var_data.array.offset / 256;
            buf[2]=prog->vvt.var[i].var_data.array.dim1   & 0xff;
            buf[3]=prog->vvt.var[i].var_data.array.dim1   / 256;
            buf[4]=prog->vvt.var[i].var_data.array.dim2   & 0xff;
            buf[5]=prog->vvt.var[i].var_data.array.dim2   / 256;
            v=buf;
            break;
         default:
         case 0x00:
            v=prog->vvt.var[i].var_data.scalar.atarifloat;
            break;
      }
      b+=write(prog->ofd,v,6);
      if ( b!=8 )
      {
         printf("%s: Failed writing VVT entry %d\n",prog->outfilename,i);
         r=-1;
         goto out;
      }
   }
   // Write Code
   b=write(prog->ofd,prog->code.raw,prog->code.raw_size);
   if ( b != prog->code.raw_size )
   {
      printf("%s: Failed writing code\n",prog->outfilename);
      r=-1;
      goto out;
   }
   // Write Immediate
   b=write(prog->ofd,prog->immediate.raw,prog->immediate.raw_size);
   if ( b != prog->immediate.raw_size )
   {
      printf("%s: Failed writing immediate statement\n",prog->outfilename);
      r=-1;
      goto out;
   }
   // Write Junk
   b=write(prog->ofd,prog->post_code_junk,prog->junk_size);
   if ( b != prog->junk_size )
   {
      printf("%s: Failed writing junk at end\n",prog->outfilename);
      r=-1;
      goto out;
   }

out:
   close(prog->ofd);
   prog->ofd = -1;
   return r;
}

/*
 * process_one_file()
 */
int process_one_file(struct basic_program *prog)
{
   if ( prog->fd < 0 )
   {
      prog->fd = open(prog->filename,O_RDONLY);
      if ( prog->fd < 0 )
      {
         printf("%s: Failed to open file\n",prog->filename);
         return -1;
      }
   }
   int r;
   r=parse_file(prog);
   if ( r ) return r;
   display_program(prog);
   r=modify_program(prog);
   r=save_program(prog,r);
   return r;
}

/*
 * main()
 */
#define USAGE                                                           \
   "Atari BASIC Analyzer\n"                                             \
   "\n"                                                                 \
   "Display options:"                                                   \
   " --display-header=[0|1]  Display file header (default: 1)\n"        \
   " --display-variables=[0|1] Display the variable name and value tables (default: 1)\n" \
   " --display-lines=[0|1]   Print a summary of each line of code (default: 1)\n" \
   " --display-full-lines=[0|1]  Print the full program listing (default: 0)\n" \
   " --display-nonascii=[0|1]    Print non-ASCII as-is (default: 0)\n"  \
   " --display-immediate=[0|1]   Print the command used to save the program (default: 1)\n" \
   " --display-junk=[0|1]        Hex dump any extra data at the end (default: 1)\n" \
   " --parse=[auto,atari,ap,turbo,altirra,xl,xe] Set of opcodes/operands to use (default: auto)\n" \
   "\n"                                                                 \
   "Output options\n"                                                   \
   " --out=[filename]  Write the file out\n"                            \
   " --fix-revb        Fix pointers to correct Rev.B save bug\n"        \
   " --strip-immediate Remove the immediate command from the end\n"     \
   " --strip-junk      Remove any junk from the end\n"                  \
   " --recreate-vnt    Create a valid VNT if needed\n"                  \
   " --wipe-vvt        Erase any saved variable values\n"               \
   " --merge-minus     Merge unary minus with scalar values\n"          \
   " --remove-unused   Remove unreferenced variables\n"             \
   ""                                                                   \
   "Add one or more filenames for BASIC programs to analyze\n"          \
   ""
struct optparse {
   char *opt;
   int *val;
};
int force=0;
struct optparse options[] = {
   { "--display-header", &display_header },
   { "--display-variables", &display_variables },
   { "--display-lines", &display_lines },
   { "--display-full-lines", &display_full_lines },
   { "--display-nonascii", &display_full_lines_with_nonascii },
   { "--display-immediate", &display_immediate_command },
   { "--display-junk", &display_post_junk_hexdump },
   { "--fix-revb", &fix_pointer_rev_b_bug },
   { "--strip-immediate", &strip_immediate },
   { "--strip-junk", &strip_end_data },
   { "--recreate-vnt", &recreate_vnt },
   { "--wipe-vvt", &wipe_vvt },
   { "--merge-minus", &merge_minus },
   { "--remove-unused", &remove_unreferenced_variables },
   { "--force",&force }, // Not in help options
};

int main(int argc,char *argv[])
{
   struct basic_program prog;
   int r=0;

   memset(&prog,0,sizeof(prog));
   prog.fd = -1;

   if ( argc == 1 )
   {
      prog.filename="stdin";
      prog.fd=0;
      prog.ofd = -1;
      r=process_one_file(&prog);
      return r;
   }

   while (argc > 1 )
   {
      for (unsigned int i=0;i<sizeof(options)/sizeof(options[0]); ++i )
      {
         if ( strncmp(argv[1],options[i].opt,strlen(options[i].opt)) == 0 )
         {
            argv[1]+=strlen(options[i].opt);
            if ( *argv[1] == '=' ) *options[i].val = atoi(argv[1]+1);
            else *options[i].val = 1;
            goto next_arg;
         }
      }
      if ( strncmp(argv[1],"--out=",sizeof("--out=")-1)==0 )
      {
         prog.outfilename = argv[1]+sizeof("--out=")-1;
         goto next_arg;
      }
      if ( strncmp(argv[1],"--mode=",sizeof("--mode=")-1)==0 )
      {
         char *m = argv[1]+sizeof("--mode=")-1;
         if ( 0 == strcmp(m,"auto") ) display_mode=auto_detect;
         else if ( 0 == strcmp(m,"atari") ) display_mode=atari_basic;
         else if ( 0 == strcmp(m,"turbo") ) display_mode=turbo_basic_xl;
         else if ( 0 == strcmp(m,"altirra") ) display_mode=altirra_basic;
         else if ( 0 == strcmp(m,"xl") ) display_mode=basic_xl;
         else if ( 0 == strcmp(m,"xe") ) display_mode=basic_xe;
         else if ( 0 == strcmp(m,"a+") ) display_mode=basic_ap;
         else
         {
            printf("Invalid display mode: %s\n",m);
            return 1;
         }
         goto next_arg;
      }
      if ( strcmp(argv[1],"--")==0 )
      {
         --argc;
         ++argv;
         break;
      }
      if ( strcmp(argv[1],"--help") == 0 )
      {
         printf("%s",USAGE);
         return 0;
      }
      if ( argv[1][0] == '-' )
      {
         printf("Illegal option: %s\n" USAGE,argv[1]);
         return 1;
      }
      break;
   next_arg:
      --argc;
      ++argv;
   }

   if ( strip_end_data && !fix_pointer_rev_b_bug )
   {
      // I don't understand why this is; apparently the Rev.B bug is more than just the pointer issue somehow
      // FIXME: Figure out why
      printf("Warning: Removing the end junk without fixing the Rev.B pointer issue will break the file\n");
      if ( !force ) exit(1);
   }

   while ( argc > 1 )
   {
      int r1;
      prog.fd = -1;
      prog.filename=argv[1];
      r1=process_one_file(&prog);
      if ( r1 ) r=r1;
      --argc;
      ++argv;
      memset(&prog,0,sizeof(prog));
   }
   return r;
}
