/************************************************************************/
/* atr2unix.c                                                           */
/*                                                                      */
/* Preston Crow                                                         */
/* Public Domain                                                        */
/*                                                                      */
/* Extract files from an Atari DOS, MyDOS, or SpartaDOS .atr file       */
/*                                                                      */
/* Version History                                                      */
/*  5 Jun 95  Version 1.0   Preston Crow <crow@cs.dartmouth.edu>        */
/*	      Initial public release                                    */
/* 20 Dec 95  Version 1.1   Chad Wagner <cmwagner@gate.net>             */
/*	      Ported to MS-DOS machines					*/
/* 10 Feb 98  Version 1.2   Preston Crow <crow@cs.dartmouth.edu>	*/
/*	      Expanded 256-byte sector support				*/
/*  2 Jan 22  Version 1.3   Preston Crow				*/
/*            Enhanced debugging                                        */
/* 13 Aug 23  Version 2.0   Preston Crow                                */
/*            SpartDOS support                                          */
/************************************************************************/

/************************************************************************/
/* Portability macros                                                   */
/* 1  Jun 95  crow@cs.dartmouth.edu (Preston Crow)                      */
/************************************************************************/
#if defined(__MSDOS) || defined(__MSDOS__) || defined(_MSDOS) || \
	 defined(_MSDOS_)
#define MSDOS /* icky, icky, icky! */
#endif

/************************************************************************/
/* Include files                                                        */
/************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#ifdef MSDOS
#include <dir.h>
#include <io.h>
#else
#include <unistd.h>
#endif

/************************************************************************/
/* Macros and Constants                                                 */
/************************************************************************/
#define ATRHEAD 16
#define USAGE "atr2unix [-dlmr-] atarifile.atr\n"                       \
        "    Flags:\n"                                                  \
        "\t-l Convert filenames to lower case\n"                        \
        "\t-m MyDOS format disk image\n"                                \
        "\t-s SpartaDOS format disk image\n"                            \
        "\t-- Next argument is not a flag\n"                            \
        "\t-d debugging\n"                                              \
        "\t-r={sector} Use non-standard root directory number\n"        \
        "\t-f Fake run; do not create any files\n"

#ifndef SEEK_SET
#define SEEK_SET 0 /* May be missing from stdio.h */
#endif
#define SEEK(n)		(ddshortinit?SEEK1(n):SEEK2(n))
#define SEEK1(n)	(ATRHEAD + ((n<4)?((n-1)*128):(3*128+(n-4)*secsize)))
#define SEEK2(n)	(ATRHEAD + ((n-1)*secsize))
#define BYTES2(n)	(n[0]+256*n[1]) // Read two-byte array as lo,hi
#define BYTES3(n)	(n[0]+256*n[1]+256*256*n[2])
#define FORMAT_SPARTA_DATE "%d-%s-%d %d:%02d:%02d"
#define PRINT_SPARTA_DATE(e) e.file_date[0],month_name[e.file_date[1]-1],e.file_date[2]+((e.file_date[2]>=78)?1900:2000),e.file_time[0],e.file_time[1],e.file_time[2]
static const char *month_name[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
#define TIMESTAMP_VALID(e) (e.file_date[0]<=31) && (e.file_date[0]>=1) && e.file_date[1]<=12 && e.file_date[1] >=1 && e.file_date[2]<100 && e.file_time[0]<24 && e.file_time[1]<60 && e.file_time[2]<60

/************************************************************************/
/* Data types                                                           */
/************************************************************************/
struct atari_dirent {
	unsigned char flag; /* set bits:  7->deleted  6->in use  5->locked  0->write open */
	unsigned char countlo; /* Number of sectors in file */
	unsigned char counthi;
	unsigned char startlo; /* First sector in file */
	unsigned char starthi;
	char namelo[8];
	char namehi[3];
};

struct atr_head {
	unsigned char h0; /* 0x96 */
	unsigned char h1; /* 0x02 */
	unsigned char seccount[2];
	unsigned char secsize[2];
	unsigned char hiseccount[2];
	unsigned char unused[8];
};

// https://atariwiki.org/wiki/attach/SpartaDOS/SpartaDOS%20X%204.48%20User%20Guide.pdf
// Disk structure is at page 151
struct sector1_sparta {
        // Initial fields used by the OS to boot
        unsigned char pad_zero; // Usually 00, could be 0x53 for 'S' for SD
        unsigned char boot_sectors; // Usually 3
        unsigned char boot_addr[2]; // Where to load the boot sectors in memory
        unsigned char dos_ini[2]; // I think this is a JSR address after loading boot sectors
        unsigned char exec_addr[2]; // execute address after boot loaded
        // Fields used by DOS, so specific to the version
        unsigned char pad_byte_8; // Skipped in the spec
        unsigned char dir[2]; // sector start of main directory sector map
        unsigned char sectors[2]; // number of sectors; should match atr_head
        unsigned char free[2]; // number of free sectors
        unsigned char bitmap_sectors; // number of bit map sectors
        unsigned char first_bitmap[2]; // first bit map sector
        unsigned char sec_num_allocation[2];
        unsigned char sec_num_dir_alloc[2];
        unsigned char volume_name[8];
        unsigned char track_count; // high bit indicates doulbe-sided; '01' indicates non-floppy
        unsigned char sector_size; // 0x00: 256-bytes, 0x01: 512-bytes, 0x80: 128-bytes
        unsigned char revision; // 0x11: SD1.1; 0x20: 2.x, 3.x, 4.1x, 4.2x; 0x21: SDSF 2.1
        unsigned char rev_specific[4]; // depends on the revision; not important here
        unsigned char vol_sequence;
        unsigned char vol_random;
        unsigned char sec_num_file_load_map[2];
        unsigned char lock; // Not used in 2.1; 0xff for lock in older versions
        // Boot code follows
};

// Each file or directory has a sector map; this is like the map in a Unix inode
// Sector maps are an array of sector numbers
// Entry 0 is the next sector of the sector map (if needed; otherwise zero)
// Entry 1 is the previous sector map of the file (zero if the first)
// Entries 2..end are the sequence of sectors; 0x00 indicates data is all zeros (sparse file)

// The Sparta directory header; the first 13 bytes
struct sparta_dir_header {
        unsigned char pad_0; // status not used for first entry
        unsigned char parent_dir_sector_map[2];
        unsigned char dir_length_bytes[3];
        unsigned char dir_name[8];
        unsigned char dir_ext_pad[3];
        unsigned char file_date[3]; // dd/mm/yy (directory creation date)
        unsigned char file_time[3]; // hh/mm/ss
};
struct sparta_dir_entry {
        unsigned char status; // bit 3: in use; bit 5: subdirectory
        unsigned char sector_map[2];
        unsigned char file_size_bytes[3];
        unsigned char file_name[8];
        unsigned char file_ext[3];
        unsigned char file_date[3]; // dd/mm/yy
        unsigned char file_time[3]; // hh/mm/ss
};


/************************************************************************/
/* Function Prototypes                                                  */
/************************************************************************/
int sparta_sanity(FILE *in,int verbose);
void read_sparta_dir(FILE *in,int sector);
void read_sparta_file(char *name,FILE *in,FILE *out,int sector,int file_size);
void read_dir(FILE *in,int sector);
void read_file(char *name,FILE *in,FILE *out,int sector,int count,int filenum);

/************************************************************************/
/* Global variables                                                     */
/************************************************************************/
int ddshortinit=0; /* True indicates double density with first 3 sectors 128 bytes */
int secsize,seccount;
int mydos=0;
int sparta=0;
int lowcase=0;
int debug=0;
int fake=0;
struct sector1_sparta sec1;

/************************************************************************/
/* main()                                                               */
/* Process command line                                                 */
/* Open input .ATR file                                                 */
/* Interpret .ATR header                                                */
/************************************************************************/
int main(int argc,char *argv[])
{
	FILE *in;
	struct atr_head head;
        int root=361;

	--argc; ++argv; /* Skip program name */

	/* Process flags */
	while (argc) {
		int done=0;

		if (**argv=='-') {
			++*argv;
			while(**argv) {
				switch(**argv) {
				      case 'm': /* MyDos disk */
					mydos=1;
					break;
                                      case 's':
                                        sparta=1;
                                        break;
				      case '-': /* Last option */
					done=1;
					break;
				      case 'l': /* strlwr names */
					lowcase=1;
					break;
				      case 'f': /* fake */
					fake=1;
					break;
				      case 'd': /* debugging */
					debug=1;
					break;
                                      case 'r': /* root directory sector */
                                        ++*argv;
                                        while ( **argv && !isdigit(**argv) ) ++*argv;
                                        root=atoi(*argv);
                                        while ( argv[0][1] ) ++*argv;
                                        break;
				      default:
					fprintf(stderr,USAGE);
					exit(1);
				}
				++*argv;
			}
			--argc; ++argv;
		}
		else break;
		if (done) break;
	}

	if (!argc) {
		fprintf(stderr,USAGE);
		exit(1);
	}
	in=fopen(*argv,"rb");
	if (!in) {
		fprintf(stderr,"Unable to open %s\n%s",*argv,USAGE);
		exit(1);
	}
	--argc; ++argv;
	if (argc) {
		if (chdir(*argv)) {
			fprintf(stderr,"Unable to change to directory: %s\n%s",*argv,USAGE);
			exit(1);
		}
	}

	if ( fread(&head,sizeof(head),1,in) != 1 )
        {
                printf("Unable to read ATR header\n");
                return 1;
        }
        if ( head.h0 != 0x96 || head.h1 != 0x02 )
        {
           if ( debug ) printf("File does not have ATR signature\n");
           return 1;
        }
	secsize=BYTES2(head.secsize);
        seccount=BYTES2(head.seccount) + (BYTES2(head.hiseccount) << 16); // image size in 16 bytes
        if ( secsize > 128 && seccount % secsize )
        {
                ddshortinit=1;
        }
        seccount=seccount * 16 / secsize;
        if ( debug ) printf("ATR image: %d sectors, %d bytes each\n",seccount,secsize);
	{
		struct stat buf;
                size_t expected;
		fstat(fileno(in),&buf);
		if (debug) {
			if (ddshortinit && secsize==256) printf("DD, but first 3 sectors SD\n");
			else if (secsize==256) printf("DD, including first 3 sectors\n");
		}
                expected = sizeof(head) + seccount * secsize - ((secsize - 128) * 3 * ddshortinit);
                if ( (size_t)buf.st_size != expected ) {
                        if ( debug ) {
                                int seccount_real;
                                printf("File size wrong; expected %u bytes, observed %u bytes\n",(unsigned int)expected,(unsigned int)buf.st_size);
                                seccount_real = (buf.st_size - sizeof(head)) / secsize;
                                if ( ddshortinit )
                                {
                                        if ( (size_t)buf.st_size <= sizeof(head) + 3 * 128 ) seccount_real = (buf.st_size - sizeof(head)) / 128;
                                        else seccount_real = 3 + (buf.st_size - sizeof(head) - 3*128)/secsize;
                                }
                                printf("Sectors expected: %d, observed: %d\n",seccount,seccount_real);
                        }
                }
	}
        if ( sparta )
        {
                if ( sparta_sanity(in,1) )
                {
                        printf("SpartaDOS disk volume: ");
                        for (int i=0;i<8;++i) printf("%c",sec1.volume_name[i]&0x7f); // Remove inverse video
                        printf("\n");
                        read_sparta_dir(in,BYTES2(sec1.dir));
                        return(0);
                }
                return 1;
        }
        if ( sparta_sanity(in,debug) )
        {
                printf("Note: Passes SpartaDOS sanity checks; consider using '-s' option if this fails\n");
        }
	read_dir(in,root);
	return(0);
}

/************************************************************************/
/* read_sector()                                                        */
/* Read one sector into a buffer                                        */
/*                                                                      */
/* Return 1 on success, 0 on error                                      */
/************************************************************************/
int read_sector(FILE *in,int sector,int readsize,void *buf)
{
        if (fseek(in,(long)SEEK(sector),SEEK_SET)) {
                fprintf(stderr,"Sector read error: sector %d seek failure\n",sector);
                return 0;
        }
        return fread(buf,readsize,1,in);
}

/************************************************************************/
/* display_entry()                                                      */
/* Display the contents of one directory entry for debugging            */
/************************************************************************/
void display_entry(int i,struct atari_dirent *f)
{
        struct atari_dirent empty;
        memset(&empty,0,sizeof(empty));
        if ( memcmp(&empty,f,sizeof(empty)) == 0 )
        {
                printf("%2d: [entry is all zeros]\n",i);
                return;
        }
        unsigned count = f->countlo + 256 * f->counthi;
        unsigned start = f->startlo + 256 * f->starthi;
        printf("%2d: %4d %4d %c%c%c%c%c%c%c%c.%c%c%c\n",i,count,start,
               f->namelo[0],f->namelo[1],f->namelo[2],f->namelo[3],f->namelo[4],f->namelo[5],f->namelo[6],f->namelo[7],
               f->namehi[0],f->namehi[1],f->namehi[2]);
}

/************************************************************************/
/* read_dir()                                                           */
/* Read the entries in a directory                                      */
/* Call read_file() for files, read_dir() for subdirectories            */
/************************************************************************/
void read_dir(FILE *in,int sector)
{
	int i,j,k;
	struct atari_dirent f;
	FILE *out;
	char name[13];

        if ( debug ) printf("Parsing directory sector %d\n",sector);

	for(i=0;i<64;++i) {
		fseek(in,(long)SEEK(sector)+i*sizeof(f)+(secsize-128)*(i/8),SEEK_SET);
		fread(&f,sizeof(f),1,in);
                if ( debug ) display_entry(i,&f);
                if ( fake ) continue;
		if (!f.flag) /* No more entries */
                {
                        if ( debug ) printf("Directory entry %d: zero indicates end of entries\n",i);
                        return;
                }
		if (f.flag&128) /* Deleted file */
                {
                        if ( debug ) printf("Directory entry %d: deleted flag\n",i);
                        continue;
                }
		for(j=0;j<8;++j) {
			name[j]=f.namelo[j];
			if (name[j]==' ') break;
		}
		name[j]='.';
		++j;
		for(k=0;k<3;++k,++j) {
			name[j]=f.namehi[k];
			if (name[j]==' ') break;
		}
		name[j]=0;
		if (name[j-1]=='.') name[j-1]=0;
		if(lowcase) for(j=0;name[j];++j) name[j]=tolower(name[j]);

		if (f.flag ==0x47 ) { /* Seems to work */
			printf("Warning:  File %s has flag bit 1 set--file ignored\n",name);
			continue;
		}
		if (mydos && f.flag&16) { /* Subdirectory */
			if (debug) printf("subdir %s (sec %d);\n",name,f.startlo+256*f.starthi);
#ifdef MSDOS
			mkdir(name);
#else
			mkdir(name,0777);
#endif
			chdir(name);
			read_dir(in,f.startlo+256*f.starthi);
			chdir("..");
		}
		else {
			out=fopen(name,"wb");
			if (!out) {
				fprintf(stderr,"Unable to create file:  %s\n",name);
				exit(2);
			}
			if (debug) printf("readfile %s (sec %d,count %d,flags %x);\n",name,f.startlo+256*f.starthi,f.countlo+256*f.counthi,f.flag);
			read_file(name,in,out,f.startlo+256*f.starthi,f.countlo+256*f.counthi,i);
			if (f.flag&32) { /* Make locked files read-only */
#ifdef MSDOS
				chmod(name,S_IREAD);
#else
				mode_t um;

				um=umask(022);
				umask(um);
				chmod(name,0444 & ~um);
#endif
			}
		}
	}
}

/************************************************************************/
/* read_file()                                                          */
/* Trace through the sector chain.                                      */
/* Complications: Are the file numbers or high bits on the sector       */
/*		  number?                                               */
/*		  What about the last block code for 256-byte sectors?  */
/************************************************************************/
void read_file(char *name,FILE *in,FILE *out,int sector,int count,int filenum)
{
	unsigned char buf[256];

	buf[secsize-1]=0;
	while(count) {
		if (sector<1) {
			fprintf(stderr,"Corrupted file (invalid sector %d): %s\n",sector,name);
			return;
		}
		if (buf[secsize-1]&128 && secsize==128) {
			fprintf(stderr,"Corrupted file (unexpected EOF): %s\n",name);
			return;
		}
		if (fseek(in,(long)SEEK(sector),SEEK_SET)) {
			fprintf(stderr,"Corrupted file (next sector %d): %s\n",sector,name);
			return;
		}
		fread(buf,secsize,1,in);
		fwrite(buf,buf[secsize-1],1,out);
		if (mydos) {
			sector=buf[secsize-2]+buf[secsize-3]*256;
		}
		else { /* DOS 2.0 */
			sector=buf[secsize-2]+(3&buf[secsize-3])*256;
			if (buf[secsize-3]>>2 != filenum) {
				fprintf(stderr,"Corrupted file (167: file number mismatch): %s\n",name);
				return;
			}
		}
		--count;
	}
	if (!(buf[secsize-1]&128) && secsize==128 && sector) {
		fprintf(stderr,"Corrupted file (expected EOF, code %d, next sector %d): %s\n",buf[secsize-1],sector,name);
		return;
	}

	fclose(out);
}

/************************************************************************/
/* sparta_sanity()                                                      */
/* Return true if first sector is compatible with SpartaDOS format      */
/************************************************************************/
int sparta_sanity(FILE *in,int verbose)
{
        if ( read_sector(in,1,sizeof(sec1),&sec1) != 1 )
        {
                fprintf(stderr,"Failed to read initial sector header\n");
                return 0;
        }
        if ( BYTES2(sec1.dir) > seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: Main directory sector map > sector count: %d > %d\n",BYTES2(sec1.dir), seccount);
                return 0;
        }
        if ( BYTES2(sec1.sectors) != seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: Sparta sector count != image sector count %d != %d\n",BYTES2(sec1.sectors), seccount);
                return 0;
        }
        if ( BYTES2(sec1.free) >= seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: Free sector count >= sector count %d != %d\n",BYTES2(sec1.free), seccount);
                return 0;
        }
        if ( !sec1.bitmap_sectors )
        {
                if ( verbose ) printf("Not SpartaDOS: No bitmap sectors\n");
                return 0;
        }
        if ( (sec1.bitmap_sectors-1)*8*secsize >= seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: Too many bitmap sectors %d\n",sec1.bitmap_sectors);
                return 0;
        }
        if ( BYTES2(sec1.first_bitmap) >= seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: first bitmap >= sector count %d != %d\n",BYTES2(sec1.first_bitmap), seccount);
                return 0;
        }
        if ( BYTES2(sec1.sec_num_allocation) >= seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: sector number alloc >= sector count %d != %d\n",BYTES2(sec1.sec_num_allocation), seccount);
                return 0;
        }
        if ( BYTES2(sec1.sec_num_dir_alloc) >= seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: sector number dir alloc >= sector count %d != %d\n",BYTES2(sec1.sec_num_dir_alloc), seccount);
                return 0;
        }
#if 0 // Observations are this can be invalid
        if ( BYTES2(sec1.sec_num_file_load_map) >= seccount )
        {
                if ( verbose ) printf("Not SpartaDOS: sector number file load map >= sector count %d != %d\n",BYTES2(sec1.sec_num_file_load_map), seccount);
                return 0;
        }
#endif
        return 1;
}

/************************************************************************/
/* read_sparta_sector_map()                                             */
/* Read the full sector map.                                            */
/* Returns an array dynamically allocated on the heap.                  */
/************************************************************************/
int *read_sparta_sector_map(FILE *in,int sector,int *entries)
{
        unsigned char *buf;
        int *map=NULL;
        int prev=0;
        int next;
        int count=0;

        if ( 0 && debug ) printf("Reading sector map starting at %d\n",sector);
        *entries=0;
        buf=malloc(secsize);
        if ( !buf )
        {
                fprintf(stderr,"Failed to allocate buffer for sector map\n");
                return NULL;
        }
        next = sector;

        while ( next )
        {
                ++count;
                map=realloc(map,sizeof(int) * count*((secsize-4)/2));
                if ( !map )
                {
                        fprintf(stderr,"Failed to allocate sector map buffer\n");
                        free(buf);
                        return NULL;
                }
                if ( read_sector(in,sector,secsize,buf) != 1 )
                {
                        fprintf(stderr,"Failed to read sector map from sector %d\n",sector);
                        free(buf);
                        free(map);
                        *entries=0;
                        return NULL;
                }
                if ( 0 && debug ) printf("Sector map starts: %02x %02x %02x %02x %02x %02x\n",buf[0],buf[1],buf[2],buf[3],buf[4],buf[5]);
                if ( BYTES2((buf+2)) != prev )
                {
                        fprintf(stderr,"Sector map linked list broken; prev should be %d, but is %d\n",prev,BYTES2((buf+2)));
                        free(buf);
                        free(map);
                        *entries=0;
                        return NULL;
                }
                prev=sector;
                next=BYTES2(buf);
                if ( next > seccount )
                {
                        fprintf(stderr,"Sector map linked list broken; next > seccount; %d >  %d\n",next,seccount);
                        free(buf);
                        free(map);
                        *entries=0;
                        return NULL;
                }
                for ( int i=0; i<(secsize-4)/2; ++i )
                {
                        map[*entries+i]=BYTES2((buf+4+i*2));
                }
                *entries += (secsize-4)/2;
        }
        free(buf);

        while ( *entries && map[*entries-1]==0 ) --*entries; // Ignore zero entries at the end
        return map;
}

/************************************************************************/
/* read_sparta_dir()                                                    */
/* Read the entries in a directory                                      */
/* Call read_sparta_file() for files, read_sparta_dir() for subdirs     */
/************************************************************************/
void read_sparta_dir(FILE *in,int sector)
{
        int entries;
        char *buf;
        int bufsec=0;
        int *map = read_sparta_sector_map(in,sector,&entries);
        if ( !map )
        {
                fprintf(stderr,"Failed to read directory map\n");
                return;
        }
        if ( 0 && debug ) printf("Sector map read: %d entries\n",entries);
        if ( 0 && debug ) for (int i=0;i<entries;++i ) printf("  %d\n",map[i]);
        // Sanity check map for directory; can't have zeros
        for (int i=0;i<entries;++i)
        {
                if ( map[i]==0 || map[i]>seccount )
                {
                        fprintf(stderr,"Invalid sector map: map[i] is %d\n",map[i]);
                        free(map);
                        return;
                }
        }

        // Read header
        struct sparta_dir_header head;
        if ( read_sector(in,map[0],sizeof(head),&head) != 1 )
        {
                fprintf(stderr,"Failed to read director header");
                free(map);
                return;
        }
        if ( debug )
        {
                printf("Directory header read: %.8s\n",head.dir_name);
                // printf("parent directory sector map: %d\n",BYTES2(head.parent_dir_sector_map));
                if ( TIMESTAMP_VALID(head) )
                        printf("Directory creation time stamp: "FORMAT_SPARTA_DATE"\n",PRINT_SPARTA_DATE(head));
                else
                        printf("Directory creation time stamp invalid\n");
        }
        // Read entries
        buf=malloc(secsize*2);
        if ( !buf )
        {
                fprintf(stderr,"Failed to allocate sector buffer\n");
                free(map);
                return;
        }
        if ( debug ) printf("Directory is %d bytes; %ld entries; %d sectors\n",BYTES3(head.dir_length_bytes),BYTES3(head.dir_length_bytes)/sizeof(struct sparta_dir_entry),(BYTES3(head.dir_length_bytes)+secsize-1)/secsize);
        for (unsigned int e=1;e<BYTES3(head.dir_length_bytes)/sizeof(struct sparta_dir_entry);++e)
        {
                struct sparta_dir_entry dir_entry;
                int sector2;
                int is_dir=0;

                // Read an entry
                // Entries can span sector boundaries, making it more complicated
                sector=map[e*sizeof(dir_entry)/secsize];
                sector2=map[((e+1)*sizeof(dir_entry)-1)/secsize];
                if ( 0 && debug ) printf("Directory entry %d is on sectors %d and %d\n",e,sector,sector2);
                if ( bufsec != sector )
                {
                        if ( read_sector(in,sector,secsize,buf) != 1 )
                        {
                                fprintf(stderr,"Failed to read sector %d\n",sector);
                                free(map);
                                free(buf);
                                return;
                        }
                }
                if ( sector2 != sector )
                {
                        if ( read_sector(in,sector2,secsize,buf+secsize) != 1 )
                        {
                                fprintf(stderr,"Failed to read sector %d\n",sector2);
                                free(map);
                                free(buf);
                                return;
                        }
                }
                memcpy(&dir_entry,buf+e*sizeof(dir_entry)%secsize,sizeof(dir_entry));
                if ( sector2 != sector )
                {
                        memcpy(buf,buf+secsize,secsize);
                        sector=sector2;
                }

                // Process an entry
                // FIXME: Make sure bit parsing is working correctly
                if ( !( dir_entry.status & (1<<3)) ) continue; // Not in use
                if (  dir_entry.status & (1<<5) ) is_dir=1; // Subdirectory

                char name[13];
                int j;
		for(j=0;j<8;++j) {
			name[j]=dir_entry.file_name[j];
			if (name[j]==' ') break;
		}
		name[j]='.';
		++j;
		for(int k=0;k<3;++k,++j) {
			name[j]=dir_entry.file_ext[k];
			if (name[j]==' ') break;
		}
		name[j]=0;

                if ( is_dir )
                {
			if (debug) printf("subdir %s (sec %d);\n",name,BYTES2(dir_entry.sector_map));
#ifdef MSDOS
			mkdir(name);
#else
			mkdir(name,0777);
#endif
			if ( chdir(name) != 0 )
                        {
                                fprintf(stderr,"Failed to chdir to %s\n",name);
                                free(map);
                                free(buf);
                                return;
                        }
			read_sparta_dir(in,BYTES2(dir_entry.sector_map));
			if ( chdir("..") != 0 )
                        {
                                fprintf(stderr,"Failed to chdir out of %s\n",name);
                                return;
                                free(map);
                                free(buf);
                        }
                }
                else
                {
			FILE *out=fopen(name,"wb");
			if (!out) {
				fprintf(stderr,"Unable to create file:  %s\n",name);
				exit(2);
			}
			if (debug) printf("readfile %s (sec %d,bytes %d,flags %x);\n",name,BYTES2(dir_entry.sector_map),BYTES3(dir_entry.file_size_bytes),dir_entry.status);
			read_sparta_file(name,in,out,BYTES2(dir_entry.sector_map),BYTES3(dir_entry.file_size_bytes));
			if (dir_entry.status&32) { /* Make locked files read-only */ // FIXME; wrong bit
#ifdef MSDOS
				chmod(name,S_IREAD);
#else
				mode_t um;

				um=umask(022);
				umask(um);
				chmod(name,0444 & ~um);
#endif
			}
                }
                // Set time stamp to match
                if ( TIMESTAMP_VALID(dir_entry) )
                {
#ifdef MSDOS
                        printf("Timestamp setting not implemented: %s -> " FORMAT_SPARTA_DATE "\n",name,PRINT_SPARTA_DATE(dir_entry));
#else
                        char cmdline[6+8+1+3+1+25+1+32]; // Add 32 extra
                        sprintf(cmdline,"touch %s -d '" FORMAT_SPARTA_DATE "'",name,PRINT_SPARTA_DATE(dir_entry));
                        if ( system(cmdline) != 0 )
                        {
                                fprintf(stderr,"Unable to set time stamp: %s -> " FORMAT_SPARTA_DATE "\n",name,PRINT_SPARTA_DATE(dir_entry));
                        }
#endif
                }
                else
                {
                        fprintf(stderr,"Timestamp for %s is invalid: %x %x %x %x %x %x\n",name,dir_entry.file_date[0],dir_entry.file_date[1],dir_entry.file_date[2],dir_entry.file_time[0],dir_entry.file_time[1],dir_entry.file_time[2]);
                }
        }
        free(map);
        free(buf);
}

/************************************************************************/
/* read_sparta_file()                                                   */
/* Trace through the sector chain.                                      */
/************************************************************************/
void read_sparta_file(char *name,FILE *in,FILE *out,int sector,int file_size)
{
        int entries;
        int *map = read_sparta_sector_map(in,sector,&entries);
        if ( !map )
        {
                fprintf(stderr,"Failed to read directory map\n");
                return;
        }
        // Sanity check map for directory; zeros legal
        for (int i=0;i<entries;++i)
        {
                if ( map[i]>seccount )
                {
                        fprintf(stderr,"Invalid sector map: map[i] is %d\n",map[i]);
                        free(map);
                        return;
                }
        }

        char *buf;
        int i=0;

        buf=malloc(secsize);
        if ( !buf )
        {
                fprintf(stderr,"Failed to allocate sector buffer\n");
                free(map);
                return;
        }

        while ( file_size )
        {
                if ( 0 && debug ) // very verbose
                {
                        if ( i>=entries ) printf("sector %d of file at end is unallocated zeros\n",i);
                        else if ( map[i]==0 ) printf("sector %d of file is unallocated zeros\n",i);
                        else printf("sector %d of file is sector %d on disk\n",i,map[i]);
                }
                if ( i >= entries || map[i]==0 )
                {
                        memset(buf,0,secsize);
                }
                else
                {
                        if ( read_sector(in,map[i],secsize,buf) != 1 )
                        {
                                free(map);
                                free(buf);
                                fprintf(stderr,"File read error: %s\n",name);
                                return;
                        }
                }
                int out_size = (secsize > file_size) ? file_size : secsize;
                fwrite(buf,out_size,1,out);

                ++i;
                file_size-=out_size;
	}
	fclose(out);
        free(map);
        free(buf);
}
