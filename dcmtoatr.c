/*
** dcmtoatr.c -- written by Chad Wagner
**
** I just wanted to throw in some personal comments, I wanted to thank
** Preston Crow, for his work on portability issues, as well as some of
** the ideas he put into this program, Bob Puff, for his hard work on
** The Disk Communicator, an excellent means for transferring disk images,
** and I feel it is the de facto standard, and should remain as such, and
** Jason Duerstock, for writing DCMTODSK and his documents on the DCM
** format, which did help me resolve some of the things I wasn't aware of.
** I am sure there is a few others.  Unfortunately I only got one response
** from Bob Puff, he was attempting to help, but I believe he is very busy
** person, and just didn't have the time.
**
** Revision History:
** 31 May 95  cmwagner@gate.net
**  1 Jun 95  cmwagner@gate.net
**       added in some portability macros from dcm.c
**       added in read_atari16() function from dcm.c
**       wrote write_atari16() function
**       did a general clean-up of the code
**  1 Jun 95  crow@cs.dartmouth.edu
**       did a clean-up of the code, hopefully resolving any
**         portability issues
**  2 Jun 95  cmwagner@gate.net and crow@cs.dartmouth.edu
**       Allow for multi-file DCM files, after they've been
**         combined into a single file
**  5 Jun 95  cmwagner@gate.net
**       Fixed decoding routines to handle double density diskettes
** 26 Jun 95  cmwagner@gate.net
**       Added in support for creating DCMTOXFD, default compile is
**         for DCMTOATR
**  3 Sep 95  cmwagner@gate.net
**       Added in -x switch, which creates an XFD image, instead of the
**         default ATR image.
**       added in soffset() routine, changed around the SOFFSET define,
**         so that xfd images could be created, without compiling a
**         different version.
**       26 Jun 95 modification is no longer relevant because of this
**         modification.
**       wrote some more comments, about what certain functions are doing,
**         when they were created, and who wrote them.
** 28 Sep 95  lovegrov@student.umass.edu
**       Fixed bug in soffset() function, apparently sector 4 would return
**         an offset of 16 for ATR images or 0 for XFD images, which is not
**         correct.
*/

/* Include files */
#include <stdio.h>
#include <string.h>

/* prototypes */
void decode_C1(void);
void decode_C3(void);
void decode_C4(void);
void decode_C6(void);
void decode_C7(void);
void decode_FA(void);
int  read_atari16(FILE *);
void write_atari16(FILE *,int);
int  read_offset(FILE *);
void read_sector(FILE *);
void write_sector(FILE *);
long soffset(void);

/*
** Portability macros
** 1  Jun 95  crow@cs.dartmouth.edu (Preston Crow)
*/
#if defined(__MSDOS) || defined(__MSDOS__) || defined(_MSDOS) || defined(_MSDOS_)
#define MSDOS /* icky, icky, icky! */
#endif
#ifndef SEEK_SET
#define SEEK_SET 0 /* May be missing from stdio.h */
#endif

/* version of this program, as seen in usage message */
#define VERSION "1.4"

/* Global variables */
FILE		*fin,*fout;
unsigned int	secsize;
unsigned short	cursec=0,maxsec=0;
unsigned char	createdisk=0,working=0,last=0,density,buf[256],atr=16;
int		doprint=1; /* True for diagnostic output */

/*
** main()
*/
int main(int argc,char **argv)
{
	unsigned char	archivetype; /* Block type for first block */
	unsigned char	blocktype; /* Current block type */
	unsigned char	tmp; /* Temporary for read without clobber on eof */
	unsigned char	imgin[256],imgout[256]; /* file names */
	unsigned char	done=0; /* completion flag */
	char		*self; /* program name from argv[0] */

#ifdef MSDOS
	if ((self = strrchr(argv[0],'\\')) == NULL)
#else
	if ((self = strrchr(argv[0],'/')) == NULL)
#endif
	  {
		  self = argv[0];
	  }
	else
	  self++; /* Skip the slash */

	--argc;++argv; /* Don't look at the filename anymore */
	/* Process switches */
	if (argc) while (*argv[0]=='-') {
		int nomore=0;

		++argv[0]; /* skip the '-' */
		while(*argv[0]) {
			switch(*argv[0]) {
			      case '-':
				nomore=1;
				break;
			      case 'q':
			      case 'Q':
				doprint = !doprint;
				break;
					case 'x':
					case 'X':
				atr = 16 - atr;
				break;
			      default:
				fprintf(stderr,"Unsupported switch:  %c\n",*argv[0]);
				fprintf(stderr,"%s "VERSION" by cmwagner@gate.net\n",self);
				fprintf(stderr,"%s [-qx] input[.dcm] [output[.atr]]\n",self);
				exit(1);
			}
			++argv[0]; /* We've processed this flag */
		}
		--argc;++argv; /* Done processing these flags */
		if(nomore) break; /* Filename may begin with '-' */
	}

	if (argc<1 || argc>2) {
		fprintf(stderr,"%s "VERSION" by cmwagner@gate.net\n",self);
		fprintf(stderr,"%s [-qx] input[.dcm] [output[.atr]]\n",self);
		exit(1);
	}

	strcpy(imgin,argv[0]);
	if (strrchr(imgin,'.') == NULL)
		strcat(imgin,".dcm");

	if (argc==2)
		strcpy(imgout,argv[1]);
	else {
		char *p;

		strcpy(imgout,imgin);
		if ((p = strrchr(imgout,'.')) != NULL)
			*p = 0;
	}
	if (strrchr(imgout,'.') == NULL)
	if (atr) {
		strcat(imgout,".atr");
	} else {
		strcat(imgout,".xfd");
	}

	if ((fin = fopen(imgin,"rb")) == NULL) {
		fprintf(stderr,"I couldn't open \"%s\" for reading.\n",imgin);
		exit(1);
	}

	archivetype = blocktype = fgetc(fin);
	switch(blocktype) {
		case 0xF9:
		case 0xFA:
			break;
		default:
			fprintf(stderr,"0x%02X is an unknown header block.\n",blocktype);
			exit(1);
	}

	if ((fout = fopen(imgout,"rb")) != NULL) {
		fprintf(stderr,"I can't use \"%s\" for output, it already exists.\n",imgout);
		exit(1);
	} else {
		fout = fopen(imgout,"wb");
	}

	rewind(fin);

	do {
		if(doprint) printf("\rCurrent sector: %4u",cursec);

#ifdef MSDOS
		if (kbhit()) {
			if (getch() == 27) {
				fprintf(stderr,"\nProcessing terminated by user.\n");
				exit(1);
			}
		}
#endif
                if (feof(fin)) {
			fflush(stdout); /* Possible buffered I/O confusion fix */
                        if ((!last) && (blocktype == 0x45) && (archivetype == 0xF9)) {
				fprintf(stderr,"\nMulti-part archive error.\n");
				fprintf(stderr,"To process these files, you must first combine the files into a single file.\n");
#ifdef MSDOS
				fprintf(stderr,"\tCOPY /B file1.dcm+file2.dcm+file3.dcm newfile.dcm\n");
#else
				fprintf(stderr,"\tcat file1.dcm file2.dcm file3.dcm > newfile.dcm\n");
#endif
                        }
			else {
				fprintf(stderr,"\nEOF before end block.\n");
			}
                        exit(1);
                }

		tmp = fgetc(fin); /* blocktype is needed on EOF error--don't corrupt it */
		if (feof(fin)) continue; /* Will abort on the check at the top of the loop */
		blocktype = tmp;
		switch(blocktype) {
		      case 0xF9:
		      case 0xFA:
			/* New block */
			decode_FA();
			break;
		      case 0x45:
			/* End block */
			working=0;
			if (last) {
				if (doprint) printf("\r%s has been successfully decompressed.\n",imgout);
				fclose(fin);
				fclose(fout);
				done=1; /* Normal exit */
			}
			break;
		      case 0x41:
		      case 0xC1:
			decode_C1();
			break;
		      case 0x43:
		      case 0xC3:
			decode_C3();
			break;
		      case 0x44:
		      case 0xC4:
			decode_C4();
			break;
		      case 0x46:
		      case 0xC6:
			decode_C6();
			break;
		      case 0x47:
		      case 0xC7:
			decode_C7();
			break;
		      default:
			fprintf(stderr,"\n0x%02X is an unknown block type.  File may be "
				"corrupt.\n",blocktype);
			exit(1);
		} /* end case */

		if ((blocktype != 0x45) && (blocktype != 0xFA) &&
		(blocktype != 0xF9)) {
			if (!(blocktype & 0x80)) {
				cursec=read_atari16(fin);
				fseek(fout,soffset(),SEEK_SET);
			} else {
				cursec++;
			}
		}
	} while(!done); /* end do */
	return(0); /* Should never be executed */
}

void decode_C1(void)
{
	int	secoff,tmpoff,c;

	tmpoff=read_offset(fin);
	c=fgetc(fin);
	for (secoff=0; secoff<secsize; secoff++) {
		buf[secoff]=c;
	}
	c=tmpoff;
	for (secoff=0; secoff<tmpoff; secoff++) {
		c--;
		buf[c]=fgetc(fin);
	}
	write_sector(fout);
}

void decode_C3(void)
{
	int	secoff,tmpoff,c;

	secoff=0;
	do {
		if (secoff)
			tmpoff=read_offset(fin);
		else
			tmpoff=fgetc(fin);
		for (; secoff<tmpoff; secoff++) {
			buf[secoff]=fgetc(fin);
		}
		if (secoff == secsize)
			break;
		tmpoff=read_offset(fin);
		c=fgetc(fin);
		for (; secoff<tmpoff; secoff++) {
			buf[secoff] = c;
		}
	} while(secoff < secsize);
	write_sector(fout);
}

void decode_C4(void)
{
	int	secoff,tmpoff;

	tmpoff=read_offset(fin);
	for (secoff=tmpoff; secoff<secsize; secoff++) {
		buf[secoff]=fgetc(fin);
	}
	write_sector(fout);
}

void decode_C6(void)
{
	write_sector(fout);
}

void decode_C7(void)
{
	read_sector(fin);
	write_sector(fout);
}

void decode_FA(void)
{
	unsigned char c;

	if (working) {
		fprintf(stderr,"\nTrying to start section but last section never had "
		"an end section block.\n");
		exit(1);
	}
	c=fgetc(fin);
	density=((c & 0x70) >> 4);
	last=((c & 0x80) >> 7);
	switch(density) {
	      case 0:
		maxsec=720;
		secsize=128;
		break;
	      case 2:
		maxsec=720;
		secsize=256;
		break;
	      case 4:
		maxsec=1040;
		secsize=128;
		break;
	      default:
		fprintf(stderr,"\nDensity type is unknown, density type=%u\n",density);
		exit(1);
	}

	if (createdisk == 0) {
		createdisk = 1;
		if (atr) {
			/* write out atr header */
			/* special code, 0x0296 */
			write_atari16(fout,0x296);
			/* image size (low) */
			write_atari16(fout,(short)(((long)maxsec * secsize) >> 4));
			/* sector size */
			write_atari16(fout,secsize);
			/* image size (high) */
			write_atari16(fout,(short)(((long)maxsec * secsize) >> 20));
			/* 8 bytes unused */
			write_atari16(fout,0);
			write_atari16(fout,0);
			write_atari16(fout,0);
			write_atari16(fout,0);
		}
		memset(buf,0,256);
		for (cursec=0; cursec<maxsec; cursec++) {
			fwrite(buf,secsize,1,fout);
		}
	}
	cursec=read_atari16(fin);
	fseek(fout,soffset(),SEEK_SET);
	working=1;
}

/*
** read_atari16()
** Read a 16-bit integer with Atari byte-ordering.
** 1  Jun 95  crow@cs.dartmouth.edu (Preston Crow)
*/
int read_atari16(FILE *fin)
{
	int ch_low,ch_high; /* fgetc() is type int, not char */

	ch_low = fgetc(fin);
	ch_high = fgetc(fin);
	return(ch_low + 256*ch_high);
}

/*
** write_atari16()
** Write a 16-bit integer with Atari byte-ordering
** 1  Jun 95  cmwagner@gate.net (Chad Wagner)
*/
void write_atari16(FILE *fout,int n)
{
	unsigned char ch_low,ch_high;

	ch_low = (unsigned char)(n&0xff);
	ch_high = (unsigned char)(n/256);
	fputc(ch_low,fout);
	fputc(ch_high,fout);
}

/*
** read_offset()
** Simple routine that 'reads' the offset from an RLE encoded block, if the
**   offset is 0, then it returns it as 256.
** 5  Jun 95  cmwagner@gate.net (Chad Wagner)
*/
int read_offset(FILE *fin)
{
	int ch; /* fgetc() is type int, not char */

	ch = fgetc(fin);
	if (ch == 0)
		ch = 256;

	return(ch);
}

/*
** read_sector()
** Simple routine that reads in a sector, based on it's location, and the
**  sector size.  Sectors 1-3, are 128 bytes, all other sectors are secsize.
** 5  Jun 95  cmwagner@gate.net (Chad Wagner)
*/
void read_sector(FILE *fin)
{
	fread(buf,(cursec < 4 ? 128 : secsize),1,fin);
}

/*
** write_sector()
** Simple routine that writes in a sector, based on it's location, and the
**  sector size.  Sectors 1-3, are 128 bytes, all other sectors are secsize.
** 5  Jun 95  cmwagner@gate.net (Chad Wagner)
*/
void write_sector(FILE *fout)
{
	fwrite(buf,(cursec < 4 ? 128 : secsize),1,fout);
}

/*
** soffset()
** calculates offsets within ATR or XFD images, for seeking.
** 28 Sep 95  lovegrov@student.umass.edu (Mike White)
*/
long soffset()
{
	return (long)atr + (cursec < 4 ? ((long)cursec - 1) * 128 :
			 ((long)cursec - 4) * secsize + 384);
}
