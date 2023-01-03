/*
 *
 * Copyrights are held by the respective authors listed below.
 * Licensed for distribution under the GNU Public License version 2.0 or later.
 *
 * You need to use a sio2pc cable to run this.
 *
 *
 * Compilation:
 *	gcc -W -Wall -o sio2linux sio2linux-3.1.0.c
 *
 * Currently, this does not support the 'format' or 'verify' SIO
 * commands.
 *
 * TO-DO:
 *
 *	* Add support for missing drive commands.
 *
 *	* Add a watch/copy mode where it watches I/O to a real drive and
 *	  uses that to create a copy.  Hence, all you need to do to copy
 *	  a disk is read every sector from the Atari.
 *	  This might not be possible.  It seems that the read responses from
 *	  my 1050 aren't visible on the Linux serial port.  This may be
 *	  a limitation in my sio2pc cable.
 *
 *	* Add a keyboard user interface to add/remove/swap disks during
 *	  run time.
 *
 *	* Enhance support for dynamic disk images that are live access to the
 *	  Linux file system.  It could use My-DOS style subdirectories.
 *        Support for creating new files could be added.
 *        Support for more than one file open at a time could be added.
 *
 *	* Add support for cassette transfers, allowing BASIC programming with
 *	  fast I/O, but without the memory cost of DOS, as well as support for
 *	  the few programs that required a cassette drive, or for loading cassette
 *        images.  The 410 recorded at 600 baud, and apparently worked in a raw
 *	  streaming mode instead of the normal SIO command-frame mode.  My SIO2PC
 *	  cable does not forward the signal from pin 8 (cassette motor control), so
 *	  it would be difficult at best to get it to work correctly.  In any
 *	  case, emulating the raw 600 baud signal would require a significant
 *	  programming effort, which is not something I'm likely to do anytime
 *	  soon.
 *
 *	* Add support for printers
 *
 *	* Add 850 R: emulation
 *	  Loading of the R: handler by the 850 worked by having the 850
 *        act as D1: if it first saw ignored status requests for D1:.  The 850
 *        then sends a 3-sector boot program that sends 3 comands to the R1:
 *        device.  Apparently the boot program loads and installs the R: handler
 *        and then goes away.
 *
 *	* Add 1030 T: emulation
 *        The boot process is probably quite similar to the 850 mechanism.
 *
 * Version History:
 *
 * Version 3.1.0 13 Oct 2010    Preston Crow
 *
 *	Add support for ignoring the ring line, as some USB-to-serial converters
 *      don't handle this correctly.  Timings for Ack/Completion were adjusted to
 *      account for buffered writes.
 *
 * Version 3.0.1 22 Nov 2008    Preston Crow
 *
 *      Clear RTS before listening for a command.  This has no effect with an
 *      original SIO2PC cable, but it enables compatibility mode in some other
 *      cables so that they will behave as expected.
 *
 * Version 2.0.1 5 Sep 2005	Preston Crow
 *
 *      Fix bug in -s option for selecting serial port
 *
 * Version 2.0  19 Aug 2005	Preston Crow
 *
 *      Renamed to sio2linux.
 *      Clean up to read the commands properly.
 *	Add features: Create blank images, quiet mode, skip image, specify serial device
 *
 * Version 1.4	22 Mar 1998	Preston Crow
 *
 *	Added support for read-only images.  Any image that can't
 *	be opened for read/write will instead be opened read-only.
 *	Also, if a '-r' option appears before the image, it will
 *	be opened read-only.
 *
 *	Cleaned up a few things.  The system speed is now determined
 *	dynamically, though it still uses the Pentium cycle counter.
 *	A status request will now send write-protect information.
 *	Added a short usage blurb for when no options are specified.
 *
 *	It should be slightly more tollerant of other devices active
 *	on the SIO bus, but it could still confuse it.
 *
 * Version 1.3	20 Mar 1998	Preston Crow
 *
 *	The status command responds correctly for DD and ED images.
 *
 *	This version is fully functional.  Improvements beyond this
 *	release will focus on adding a nice user interface, and
 *	making it better at recognizing commands, so as to interact
 *	safely with real SIO devices.  A possible copy-protection
 *	mode may be nice, where the program watches all the activity
 *	on D1: while the program loads off of a real device, recording
 *	all data, timing, and status information.  Whether yet another
 *	file format should be used, or some existing format, is an open
 *	matter.
 *
 * Version 1.2	17 Mar 1998	Preston Crow
 *
 *	I've added in support for checking the ring status after reading
 *	a byte to determine if it is part of a command.  However, as this
 *	requires a separate system call, it may be too slow.  If that proves
 *	to be the case, it may be necessary to resort to direct assembly-
 *	language access to the port (though this would eliminate compatibility
 *	with non-Intel Linux systems).  That seems to not work well; many
 *	commands aren't recognized, at least when using the system call to
 *	check the ring status, so I've implemented a rolling buffer that will
 *	assume it has a command when the last five bytes have a valid checksum.
 *	That may cause problems if a non-SIO2PC drive is used.
 *
 *	It seems to work great for reading SD disk images right now.
 *	I haven't tested writing, but I suspect it will also work.
 *	It has problems when doing DD disk images.  I suspect the
 *	problem has to do with the status command returning hard-coded
 *	information.
 *
 *	The debugging output should be easier to read now, and should always
 *	be printed in the same order as the data is transmitted or received.
 *
 * Version 1.1	Preston Crow
 *	Lots of disk management added.
 *	In theory, it should handle almost any ATR or XFD disk image
 *	file now, both reading and writing.
 *	Unfortunately, it is quite broken right now.  I suspect timing
 *	problems, though it may be problems with incorrect ACK/COMPLETE
 *	signals or some sort of control signal separate from the data.
 *
 * Version 1.0	Pavel Machek <pavel@atrey.karlin.mff.cuni.cz>
 *
 *	This is Floppy EMULator - it will turn your linux machine into
 *	atari 800's floppy drive. Copyright 1997 Pavel Machek
 *	<pavel@atrey.karlin.mff.cuni.cz> distribute under GPL.
 */


/*
 * Standard include files
 */
#include <stdio.h>
#include <termio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/timeb.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

/*
 * Data structures
 */
struct atr_head {
	unsigned char h0; /* 0x96 */
	unsigned char h1; /* 0x02 */
	unsigned char seccountlo;
	unsigned char seccounthi;
	unsigned char secsizelo;
	unsigned char secsizehi;
	unsigned char hiseccountlo;
	unsigned char hiseccounthi;
	unsigned char unused[8];
};

enum seekcodes {
	xfd,	/* This is a xfd (raw sd) image */
	atr,	/* This is a regular ATR image */
	atrdd3, /* This is a dd ATR image, including the first 3 sectors */
	direct  /* This is a directory pretending to be a disk image */
};

struct atari_dirent {
	unsigned char flag; /* set bits:  
			       7->deleted
			       6->normal file
			       5->locked
			       4->MyDOS subdirectory
			       3->???
			       2->??? \ one for >720, one for >1024 ?
			       1->??? /  all for MyDOS?
			       0->write open */
	unsigned char countlo; /* Number of sectors in file */
	unsigned char counthi;
	unsigned char startlo; /* First sector in file */
	unsigned char starthi;
	char namelo[8];
	char namehi[3];
};

struct trackformat {
	int offset[18];		/* sector offset from start: offset[i]==i*100 if no skew */
	int bad[18];		/* Only set with special SIO2Linux command */
};

struct image {
	int secsize;		/* 128 or 256 */
	int seccount;		/* 720, 1040, or whatever */
	enum seekcodes seekcode;/* Image type */
	int diskfd;		/* file descriptor */
	int ro;			/* non-zero if read-only */
	int active;		/* non-zero if Linux is responding for this disk */
	int fakewrite;		/* non-zero if writes are accepted but dropped */
	int blank;		/* non-zero if disk can grow as needed */
	/*
	 * Stuff for directories as virtual disk images
	 */
	DIR *dir;		/* NULL if not a directory */
	int filefd;		/* fd of open file in directory */
	int afileno;		/* afileno (0-63) of open file */
	int secoff;		/* sector offset of open file */
	char *dirname;		/* directory name, used to append filenames */
	/*
	 * Stuff for real disks, so that we can analyze the format
	 */
	int lastsec;		/* last sector read for real disks */
	int prevsec;		/* sector before last for real disks */
	struct timeval lasttime;/* time that lastsec[] was read */
	struct trackformat track[40]; /* format information derived from observations */
};

/*
 * Prototypes
 */
static void err(const char *s);
static void raw(int fd);
static void ack(unsigned char c);
static void senddata(int disk,int sec);
static void sendrawdata(unsigned char *buf,int size);
static void recvdata(int disk,int sec);
static int get_atari(void);
void getcmd(unsigned char *buf);
static void loaddisk(char *path,int disk);
int firstgood(int disk,int sec);
void addtiming(int disk,int sec);
static void decode(unsigned char *buf);
void write_atr_head(int disk);
void snoopread(int disk,int sec);
int afnamecpy(char *an,const char *n);

/*
 * Macros
 */
#define SEEK(n,i)	(disks[disk].seekcode==xfd)?SEEK0(n,i):((disks[i].seekcode=atr)?SEEK1(n,i):SEEK2(n,i))
#define SEEK0(n,i)	((n-1)*disks[i].secsize)
#define SEEK1(n,i)	(ATRHEAD + ((n<4)?((n-1)*128):(3*128+(n-4)*disks[i].secsize)))
#define SEEK2(n,i)	(ATRHEAD + ((n-1)*disks[i].secsize))
#define ATRHEAD		16
#define MAXDISKS	8
#define TRACK18(n)	(((n)-1)/18) /* track of sector 'n' if 18 sectors per track (0-39) */
#define OFF18(n)	(((n)-1)%18) /* offset of sector 'n' in track (0-17) */
#define TRACKSTART(n)	((((n)-1)/18)*18+1)
#define RPM(_uspr)	(60*1000*1000/_uspr) /* microseconds for one revolution -> RPMs */
#define RPM3(_uspr)	((int)((60*1000ull*1000ull*1000ull/_uspr)%1000ull)) /* Fractional RPMS to 3 decimal points */
/*
 * Default Timings from SIO2PC:
 *	Time before first ACK:	   85us
 *	Time before second ACK:  1020us
 *	Time before COMPLETE:	  255us
 *	Time after COMPLETE:	  425us
 */
#define ACK1 2000 /* Atari may wait 650-950us to raise the command line; device permitted 0-16ms */
#define ACK2 1020 /* 850ms min */
#define COMPLETE1 500 /* 250 is the min, but add more for transmission delays */
#define COMPLETE2 425

/*
 * Global variables
 */
struct image disks[MAXDISKS];
int atari; /* fd of the serial port hooked up to the SIO2PC cable */
/* Config options */
int snoop; /* If true, display detailed data on unmapped drives */
int quiet; /* If true, don't display per-I/O data */
int noring; /* If true, the serial port ring detect doesn't work */
char *serial;
int uspr=208333; /* microseconds per revolution, default for 288 RPM */
int speed=19200; /* Baud rate */

/*
 * main()
 *
 * Read the command line, open the disk images, connect to the Atari,
 * and listen for commands.
 *
 * This never terminates.
 */
int main(int argc,char *argv[])
{
	int i;
	int numdisks=0;

	/*
	 * Parse command-line options
	 */
#define USAGE \
        "Options:\n" \
	"  -r     next parameter is read-only image\n"\
	"  -f     next parameter is image, fake accepting writes (no change to image)\n"\
	"  -s     next parameter is serial device to use (default: /dev/ttyS0)\n"\
	"  -b     next parameter is blank single-density image to create\n" \
	"  -B     next parameter is blank double-density image to create\n" \
	"  -x     skip next drive image\n" \
	"  -n     no ring detect on serial port (some USB converters)\n" \
	"  <file> disk image to mount as next disk (D1 through D8 in order)\n" \
        "  <dir>  directory to mount as next disk\n"

	if (argc==1) {
		fprintf(stderr,"SIO2Linux:  The Atari floppy drive emulator\n");
		fprintf(stderr,USAGE);
		fprintf(stderr,"Example:\n  %s boot.atr -x -b d3.atr\n(D1: is boot.atr, D2: is ignored, D3: is a new blank image)\n",argv[0]);
		exit(1);
	}

	setvbuf(stdout,NULL,_IONBF,0);
	setvbuf(stderr,NULL,_IONBF,0);

	memset(disks,0,sizeof(disks));
	for(i=0;i<MAXDISKS;++i) {
		disks[i].diskfd= -1;
	}
	serial="/dev/ttyS0";
	for(i=1;i<argc;i++) {
		if (*(argv[i]) == '-') {
			switch( (argv[i])[1] ) {
			    case 'q':
				++quiet;
				break;
			    case 'x':
				++numdisks;
				break;
                           case 'n':
                              noring=1;
                              break;
			    case 'B': /* double-density blank disk */
				disks[numdisks].secsize=128;
				/* fall through */
			    case 'b': /* single-density blank disk */
				disks[numdisks].secsize+=128;
				disks[numdisks].seccount=3; /* Will grow */
				disks[numdisks].blank=1;
				if ( i+1==argc ) {
					fprintf(stderr, "Must have a parameter for '-b'\n" );
					exit(1);
				}
				break;
			    case 'r':
				disks[numdisks].ro=1;
				if ( i+1==argc ) {
					fprintf(stderr, "Must have a parameter for '-f'\n" );
					exit(1);
				}
				break;
			    case 'f': /* Fake writes (no change to disk) */
				disks[numdisks].fakewrite=1;
				if ( i+1==argc ) {
					fprintf(stderr, "Must have a parameter for '-f'\n" );
					exit(1);
				}
				break;
			    case 's':
				++i;
				if ( i==argc ) {
					fprintf(stderr, "Must have a parameter for '-s'\n" );
					exit(1);
				}
				serial=argv[i];
				break;
			    default:
				err( "Bad command line argument." );
			}
		}
		else {
			loaddisk(argv[i],numdisks);
			numdisks++;
		}
	}

	atari=get_atari();

	/*
	 * Main control loop
	 *
	 * Read a command and deal with it
	 * The command frame is 5 bytes.
	 */
	while( 1 ) {
		unsigned char buf[5];

		getcmd(buf);
		decode(buf);
	}
}

static void err(const char *s)
{
	fprintf(stderr,"%d:", errno );
	fprintf(stderr,"%s\n", s );
	exit(1);
}

static void raw(int fd)
{
	struct termios it;

	if (tcgetattr(fd,&it)<0) {
		perror("tcgetattr failed");
		err( "get attr" );
	}
	it.c_lflag &= 0; /* ~(ICANON|ISIG|ECHO); */
	it.c_iflag &= 0; /* ~(INPCK|ISTRIP|IXON); */
	/* it.c_iflag |= IGNPAR; */
	it.c_oflag &=0; /* ~(OPOST); */
	it.c_cc[VMIN] = 1;
	it.c_cc[VTIME] = 0;

	if (cfsetospeed( &it, B19200 )<0) err( "set o speed" );
	if (cfsetispeed( &it, B19200 )<0) err( "set i speed" );
	if (tcsetattr(fd,TCSANOW,&it)<0) err( "set attr" );
}

static void ack(unsigned char c)
{
	if ( !quiet) printf("[");
	if (write( atari, &c, 1 )<=0) err( "ack failed\n" );
	if ( !quiet) printf("%c]",c);
}

/*
 * senddirdata()
 *
 * The directory is simulated by having the starting sector number of each
 * file be 4+afileno*5.
 * The file is then mapped to 5 sectors, such that the first sector is always
 * the start of the file, and the 5th sector links to the 2nd sector, with the
 * actual file offset being determined based on the assumption of a sequential
 * read.
 */
void senddirdata(int disk,int sec)
{
	int size;
	unsigned char buf[256];
	int total;
	int free;
	int i;
	int byte;
	int bit;
	struct dirent *de;
	int r;
	char path[MAXPATHLEN];

	memset(buf,0,sizeof(buf));
	size=disks[disk].secsize;
	total=disks[disk].seccount-3-1-8-1;
	if ( sec <= 3 ) {
		sendrawdata(buf,size);
		return;
	}
	if ( sec==360 ) { /* Create sector map */
		buf[2]=total/256;
		buf[1]=total%256;
		free = total - 5*64;
		buf[4]=free/256;
		buf[3]=free%256;
		buf[0]=2; /* ??? */
		for(i=0;i<720;++i) {
			byte=10+i/8;
			bit=i%8;
			bit=7-bit;
			bit=1<<bit;
			if(i>=4+64*5 && i!=720 && (i<360 || i>368)) {
				buf[byte]|=bit;
			}
		}
		sendrawdata(buf,size);
		return;
	}
	if ( sec>=361 && sec<=368 ) { /* Create directory */
		rewinddir(disks[disk].dir);
		readdir(disks[disk].dir);
		readdir(disks[disk].dir);
		for(i=0;i<8*(sec-361);++i) {
			if (!readdir(disks[disk].dir)) {
				sendrawdata(buf,size);
				return;
			}
		}
		for(i=0;i<8;++i) {
			int start;
			int count;
			int fn;
			struct stat sb;
			struct atari_dirent ad;

			de=readdir(disks[disk].dir);
			fn=(sec-361)*8+i;
			start=4+fn*5;
			if ( de ) {
				memset(&ad,0,sizeof(ad));
				strcpy(path,disks[disk].dirname);
				strcat(path,"/");
				strcat(path,de->d_name);
				r=stat(path,&sb);
				count=(sb.st_size+125)/125;
				ad.countlo=count%256;
				ad.counthi=count/256;
				ad.startlo=start%256;
				ad.starthi=start/256;
				ad.flag=0x80; /* If unable to convert name, deleted file */
				if ( !r && afnamecpy(ad.namelo,de->d_name) ) {
					ad.flag=0x42;
				}
				memcpy(buf+16*i,&ad,sizeof(ad));
			}
			else break;
		}
		sendrawdata(buf,size);
		return;
	}
	if ( sec>=4 && sec<4+64*5 ) { /* send file data */
		int fn;
		int off;
		off_t seekto;

		fn=(sec-4)/5;
		off=sec-4-fn*5;
		if ( off ) {
			/* This file had better be open already */
			if ( fn != disks[disk].afileno ) {
				if ( !quiet ) printf("-no data-");
				memset(buf,0,size);
				sendrawdata(buf,size);
				return;
			}
			seekto=(disks[disk].secoff+off)*125;
		}
		else {
			if ( disks[disk].afileno ) close(disks[disk].afileno);
			disks[disk].secoff=0;
			disks[disk].afileno=fn;
			rewinddir(disks[disk].dir);
			readdir(disks[disk].dir);
			readdir(disks[disk].dir);
			for(i=0;i<=fn;++i) de=readdir(disks[disk].dir);
			strcpy(path,disks[disk].dirname);
			strcat(path,"/");
			strcat(path,de->d_name);
			disks[disk].filefd=open(path,O_RDONLY);
			seekto=0;
		}
		r=lseek(disks[disk].filefd,seekto,SEEK_SET);
		if ( r<0 ) {
			if ( !quiet ) printf("-lseek errno %d-",errno);
			memset(buf,0,size);
			sendrawdata(buf,size);
			return;
		}
		r=read(disks[disk].filefd,buf,125);
		buf[125]=fn<<2;
		buf[126]=sec+1;
		if ( off==4 ) {
			buf[126] -= 4;
			disks[disk].secoff+=4;
		}
		buf[127]=r;
		if ( r<125 ) {
			buf[126]=0;
		}
		sendrawdata(buf,size);
		return;
	}
	memset(buf,0,size);
	sendrawdata(buf,size);
	return;
}

static void senddata(int disk,int sec)
{
	unsigned char buf[256];
	int size;
	off_t check,to;
	int i;

	if ( disks[disk].dir ) {
		senddirdata(disk,sec);
		return;
	}
	size=disks[disk].secsize;
	if (sec<=3) size=128;

	if ( sec > disks[disk].seccount ) {
		memset(buf,0,size);
	}
	else {
		to=SEEK(sec,disk);
		check=lseek(disks[disk].diskfd,to,SEEK_SET);
		if (check!=to) {
			if (errno) perror("lseek");
			fprintf(stderr,"lseek failed, went to %ld instead of %ld\n",check,to);
			exit(1);
		}
		/* printf("-%d-",check); */
		i=read(disks[disk].diskfd,buf,size);
		if (i!=size) {
			if (i<0) perror("read");
			fprintf(stderr,"Incomplete read\n");
			exit(1);
		}
	}
	sendrawdata(buf,size);
}

static void sendrawdata(unsigned char *buf,int size)
{
	int i, sum = 0;
	int c=0;
        struct timeval t1,t2;
        int usecs,expected;

        /*
         * Compute checksum
         */
	for( i=0; i<size; i++ ) {
		sum+=buf[i];
		sum = (sum&0xff) + (sum>>8);
        }


        gettimeofday(&t1,NULL);
        /*
         * Send buffer; let the port queue as much as it can handle
         */
	for( i=0; i<size; ) {
		c=write(atari,&buf[i],size-i);
		if (c<0) {
			if (errno) perror("write");
			fprintf(stderr,"write failed after %d bytes\n",i);
			exit(1);
		}
                i+=c;
	}
        gettimeofday(&t2,NULL);

        usecs=(t2.tv_sec-t1.tv_sec)*1000*1000;
        usecs += t2.tv_usec;
        usecs -= t1.tv_usec;

        expected=(1000 * 1000 * 10 * size ) / speed; // 8 bits per byte, plus start/stop bits

        if ( usecs < expected )
        {
                usleep(expected - usecs); // Don't write faster than the port can send
        }

	c=write( atari, &sum, 1 );
	if (c!=1) {
		if (errno) perror("write");
		fprintf(stderr,"write failed\n");
		exit(1);
	}
	if ( !quiet ) printf("-%d bytes+sum-",size);
}

static void recvdata(int disk,int sec)
{
	int i, sum = 0;
	unsigned char mybuf[ 2048 ];
	int size;

	size=disks[disk].secsize;
	if (sec<=3) size=128;

	for( i=0; i<size; i++ ) {
		read( atari, &mybuf[i], 1 );	
		sum = sum + mybuf[i];
		sum = (sum & 0xff) + (sum >> 8);
	}
	read(atari,&i,1);
	if ((i & 0xff) != (sum & 0xff) && !quiet) printf( "[BAD SUM]" );
	else if (disks[disk].fakewrite) {
		if ( !quiet) printf("[write discarded]");
	}
	else {
		lseek(disks[disk].diskfd,SEEK(sec,disk),SEEK_SET);
		i=write(disks[disk].diskfd,mybuf,size);
		if (i!=size) if ( !quiet) printf("[write failed: %d]",i);
		if ( disks[disk].blank && sec>disks[disk].seccount ) {
			disks[disk].seccount=sec;
			write_atr_head(disk);
		}
	}
	if ( !quiet) printf("-%d bytes+sum recvd-",size);
}

void snoopread(int disk,int sec)
{
	int i, sum = 0;
	unsigned char mybuf[ 2048 ];
	int size;
	int r;

	size=disks[disk].secsize;
	if (sec<=3 || size<128 ) size=128;
	r=read(atari,&i,1);
	if ( r!=1 ) {
		fprintf(stderr,"snoop read failed\n");
		return;
	}
	if ( !quiet ) printf("[%c]",i);
	if ( i!='A' ) {
		return;
	}
	r=read(atari,&i,1);
	if ( r!=1 ) {
		fprintf(stderr,"snoop read failed\n");
		return;
	}
	if ( !quiet ) printf("[%c]",i);
	if ( i!='C' ) {
		return;
	}
	for( i=0; i<size; i++ ) {
		read( atari, &mybuf[i], 1 );	
		sum = sum + mybuf[i];
		sum = (sum & 0xff) + (sum >> 8);
	}
	read(atari,&i,1);
	if ((i & 0xff) != (sum & 0xff)) {
		if (!quiet) printf( "[BAD SUM]" );
		return;
	}
}

void write_atr_head(int disk)
{
	struct atr_head buf;
	int paragraphs;

	lseek(disks[disk].diskfd,0,SEEK_SET);

	memset(&buf,0,sizeof(buf));
	buf.h0=0x96;
	buf.h1=0x02;
	paragraphs=disks[disk].seccount*(disks[disk].secsize/16) - (disks[disk].secsize-128)/16;
	buf.seccountlo=(paragraphs&0xff);
	buf.seccounthi=((paragraphs>>8)&0xff);
	buf.hiseccountlo=((paragraphs>>16)&0xff);
	buf.hiseccounthi=((paragraphs>>24)&0xff);
	buf.secsizelo=(disks[disk].secsize&0xff);
	buf.secsizehi=((disks[disk].secsize>>8)&0xff);
	write(disks[disk].diskfd,&buf,16);
}

/*
 * get_atari()
 *
 * Open the serial device and return the file descriptor.
 * It assumes that it is /dev/ttyS0 unless there's a symlink
 * from /dev/mouse to that, in which case /dev/ttyS1 is used.
 */
static int get_atari(void)
{
	int fd;
	struct stat stat_mouse,stat_tty;

	if (stat("/dev/mouse",&stat_mouse)==0) {
		stat(serial,&stat_tty);
		if (stat_mouse.st_rdev==stat_tty.st_rdev) {
			printf("/dev/ttyS0 is the mouse, using ttyS1\n");
			serial="/dev/ttyS1";
		}
	}

	fd = open(serial,O_RDWR);
	if (fd<0) {
		fprintf(stderr,"Can't open %s\n",serial);
		exit(1);
	}
	raw(fd); /* Set up port parameters */
	return(fd);
}

/*
 * getcmd()
 *
 * Read one 5-byte command
 *
 * The Atari will activate the command line while sending
 * the 5-byte command.
 */
void getcmd(unsigned char *buf)
{
	int i,r;

	/*
	 * Clear RTS (override hw flow control)
	 * [Necessary to get some of the cables to work.]
	 * See: http://www.atarimax.com/flashcart/forum/viewtopic.php?p=2426
	 */
	i = TIOCM_RTS;
	if ( ioctl(atari, TIOCMBIC, &i) < 0 )
        {
                perror("ioctl(TIOCMBIC) failed");
        }
        
        /*
         * Wait for a command
         */
        if ( !noring && ioctl(atari,TIOCMIWAIT,TIOCM_RNG) < 0 ) /* Wait for a command */
        {
                perror("ioctl(TIOCMIWIAT,TIOCM_RNG) failed");
        }

        if ( tcflush(atari,TCIFLUSH) < 0 ) /* Clear out pre-command garbage */
        {
                perror("tcflush(TCIFLUSH) failed");
        }

        /*
         * Read 5 bytes
         * This should take 2.6ms.  *** FIXME *** set an alarm
         * Use setitimer(ITIMER_REAL,(struct itimerval),NULL)
         */
        i=0;
        while (1) {
                for ( ; i<5; ++i )
                {
                        r=read(atari,buf+i,1);
                        if ( r <=0 )
                        {
                                perror("read from serial port failed");
                                fprintf(stderr,"read returned %d\n",r);
                                exit(1);
                        }
                }

		/*
		 * Compute the checksum
		 */
		{
			int sum=0;

			for(i=0;i<4;++i) {
				sum+=buf[i];
				sum = (sum&0xff) + (sum>>8);
			}
			if (buf[4]==sum) {
				return; /* Match; normal return */
			}
		}

		/*
		 * Error -- bad checksum
		 */
                if ( !quiet ) printf("%02x garbage\n",buf[0]);
                buf[0]=buf[1];
                buf[1]=buf[2];
                buf[2]=buf[3];
                buf[3]=buf[4];
                i=4; // Read one more byte and recompute checksum
        }
}

/*
 * loaddisk()
 *
 * Ready a disk image.
 * The type of file (xfd/atr) is determined by the file size.
 */
static void loaddisk(char *path,int disk)
{
	int exists=0;
	if (disk>=MAXDISKS) {
		fprintf(stderr,"Attempt to load invalid disk number %d\n",disk+1);
		exit(1);
	}

	if ( disks[disk].blank ) {
		disks[disk].diskfd=open(path,O_RDWR,0644);
		if ( disks[disk].diskfd>=0 ) {
			exists=1;
		}
		else {
			disks[disk].diskfd=open(path,O_RDWR|O_CREAT,0644);
			disks[disk].seekcode=atr;
		}
	}
	else {
		disks[disk].diskfd=open(path,(disks[disk].ro||disks[disk].fakewrite)?O_RDONLY:O_RDWR);
		if (disks[disk].diskfd<0 && !disks[disk].ro && !disks[disk].fakewrite) {
			if ( errno == EACCES ) {
				disks[disk].ro=1;
				disks[disk].diskfd=open(path,O_RDONLY);
			}
			else if ( errno == EISDIR ) {
				disks[disk].filefd = -1;
				disks[disk].afileno = -1;
				disks[disk].dir=opendir(path);
				if ( !disks[disk].dir ) {
					fprintf(stderr,"Unable to open directory %s; drive %d disabled\n",path,disk);
					return;
				}
				disks[disk].active=1;
				disks[disk].secsize=128;
				disks[disk].seccount=720;
				disks[disk].seekcode=direct;
				disks[disk].dirname=path;
				printf( "D%d: %s simulated disk (%d %d-byte sectors)\n",disk+1,path,disks[disk].seccount,disks[disk].secsize);
				return;
			}
		}
	}

	if (disks[disk].diskfd<0) {
		fprintf(stderr,"Unable to open disk image %s; drive %d disabled\n",path,disk);
		return;
	}
	disks[disk].active=1;
	if ( !disks[disk].blank || exists ) {

	/*
	 * Determine the file type based on the size
	 */
	disks[disk].secsize=128;
	{
		struct stat buf;

		fstat(disks[disk].diskfd,&buf);
		disks[disk].seekcode=atrdd3;
		if (((buf.st_size-ATRHEAD)%256)==128) disks[disk].seekcode=atr;
		if (((buf.st_size)%128)==0) disks[disk].seekcode=xfd;
		disks[disk].seccount=buf.st_size/disks[disk].secsize;
	}

	/*
	 * Read disk geometry
	 */
	if (disks[disk].seekcode!=xfd) {
		struct atr_head atr;
		long paragraphs;

		read(disks[disk].diskfd,&atr,sizeof(atr));
		disks[disk].secsize=atr.secsizelo+256*atr.secsizehi;
		paragraphs=atr.seccountlo+atr.seccounthi*256+
			atr.hiseccountlo*256*256+atr.hiseccounthi*256*256*256;
		if (disks[disk].secsize==128) {
			disks[disk].seccount=paragraphs/8;
		}
		else {
			paragraphs+=(3*128/16);
			disks[disk].seccount=paragraphs/16;
		}
	}

	}
	else {
		write_atr_head(disk);
	}
	printf( "D%d: %s opened%s (%d %d-byte sectors)\n",disk+1,path,disks[disk].ro?" read-only":"",disks[disk].seccount,disks[disk].secsize);
}

/*
 * firstgood()
 *
 * Return the first non-bad sector in the same track.
 */
int firstgood(int disk,int sec)
{
	int i;

	for(i=TRACKSTART(sec);i<sec;++i) {
		if ( !disks[disk].track[TRACK18(i)].bad[OFF18(i)] ) return(i);
	}
	return(sec);
}

/*
 * addtiming()
 *
 * We've just seen a read issued to a non-managed disk, so compute the location
 * of the last sector relative to the one previous to it from the time elapsed
 * between the reads, assuming that the Atari is reading as fast as it can.
 *
 * This is only useful for copy-protected disks, so we can assume 18-sectors per
 * track.
 */
void addtiming(int disk,int sec)
{
	struct timeval newtime;
	int diff;
	int revs;
	int secs;
	int secpct; /* percentage to next sector */
	int usps;
	int fgs;

	if ( sec > 720 ) return;
	gettimeofday(&newtime,NULL);
	if ( !disks[disk].prevsec || TRACK18(disks[disk].prevsec)!=TRACK18(disks[disk].lastsec) ) {
		goto done;
	}

	diff=newtime.tv_sec-disks[disk].lasttime.tv_sec;
	if ( diff > 1 ) goto done; /* more than a second */

	diff *= 1000000;
	diff += newtime.tv_usec;
	diff -= disks[disk].lasttime.tv_usec;

	if ( disks[disk].prevsec==disks[disk].lastsec ) {
		uspr = diff; /* Observed microsceonds for one revolution */
		if ( !quiet ) printf(" %d.%03d RPMs ",RPM(uspr),RPM3(uspr));
		goto done;
	}
	usps = uspr/18;

	revs=diff/uspr;
	secs=(diff-revs*uspr)/usps;
	secpct = (diff - revs*uspr - secs*usps) * 100 / usps;

	if ( revs>1 ) {
		if ( !quiet ) printf(" %d revolutions (%d us) [delayed read]",revs,diff);
		goto done;
	}

	fgs = firstgood(disk,disks[disk].lastsec);
	if ( disks[disk].lastsec != fgs ) {
		/* Not the first good sector on the track */
		if ( disks[disk].prevsec==fgs ||
		     disks[disk].track[TRACK18(disks[disk].prevsec)].offset[OFF18(disks[disk].prevsec)]) {
			/* We can measure directly or indirectly from the first good sector */
			disks[disk].track[TRACK18(disks[disk].lastsec)].offset[OFF18(disks[disk].lastsec)] =
				( disks[disk].track[TRACK18(disks[disk].prevsec)].offset[OFF18(disks[disk].prevsec)] + secs*100 + secpct ) % 1800;
			if ( !quiet ) printf(" sec %d is %d.%02d sectors after sec %d [RECORDED]",
					     disks[disk].lastsec,
					     disks[disk].track[TRACK18(disks[disk].lastsec)].offset[OFF18(disks[disk].lastsec)]/100,
					     disks[disk].track[TRACK18(disks[disk].lastsec)].offset[OFF18(disks[disk].lastsec)]%100,
					     fgs
					     );
			goto done;
		}
	}

	if ( !quiet ) {
		printf(" sec %d is %d.%02d sectors after sec %d", disks[disk].lastsec, secs, secpct, disks[disk].prevsec );
		printf(" fgs:%d",fgs);
	}


 done:
	disks[disk].prevsec = disks[disk].lastsec;
	disks[disk].lastsec = sec;
	disks[disk].lasttime = newtime;
}

/*
 * decode()
 *
 * Given a command frame (5-bytes), decode it and
 * do whatever needs to be done.
 */
static void decode(unsigned char *buf)
{
	int disk = -1, rs = -1, printer = -1;
	int sec;

	if ( !quiet) printf( "%02x %02x %02x %02x %02x ",buf[0],buf[1],buf[2],buf[3],buf[4]);

	switch( buf[0] ) {
	    case 0x31: if ( !quiet) printf( "D1: " ); disk = 0; break;
	    case 0x32: if ( !quiet) printf( "D2: " ); disk = 1; break;
	    case 0x33: if ( !quiet) printf( "D3: " ); disk = 2; break;
	    case 0x34: if ( !quiet) printf( "D4: " ); disk = 3; break;
	    case 0x35: if ( !quiet) printf( "D5: " ); disk = 4; break;
	    case 0x36: if ( !quiet) printf( "D6: " ); disk = 5; break;
	    case 0x37: if ( !quiet) printf( "D7: " ); disk = 6; break;
	    case 0x38: if ( !quiet) printf( "D8: " ); disk = 7; break;
	    case 0x40: if ( !quiet) printf( "P: " ); printer = 0; break;
	    case 0x41: if ( !quiet) printf( "P1: " ); printer = 0; break;
	    case 0x42: if ( !quiet) printf( "P2: " ); printer = 1; break;
	    case 0x43: if ( !quiet) printf( "P3: " ); printer = 2; break;
	    case 0x44: if ( !quiet) printf( "P4: " ); printer = 3; break;
	    case 0x45: if ( !quiet) printf( "P5: " ); printer = 4; break;
	    case 0x46: if ( !quiet) printf( "P6: " ); printer = 5; break;
	    case 0x47: if ( !quiet) printf( "P7: " ); printer = 6; break;
	    case 0x48: if ( !quiet) printf( "P8: " ); printer = 7; break;
	    case 0x50: if ( !quiet) printf( "R1: " ); rs = 0; break;
	    case 0x51: if ( !quiet) printf( "R2: " ); rs = 1; break;
	    case 0x52: if ( !quiet) printf( "R3: " ); rs = 2; break;
	    case 0x53: if ( !quiet) printf( "R4: " ); rs = 3; break;
	    default: if ( !quiet) printf( "???: ignored\n");return;
	}
	if (disk>=0&&!disks[disk].active) { if ( !quiet) printf( "[no image] " ); }
	if (printer>=0) {if ( !quiet) printf("[Printers not supported]\n"); return; }
	if (rs>=0) {if ( !quiet) printf("[Serial ports not supported]\n"); return; }

	sec = buf[2] + 256*buf[3];

	switch( buf[1] ) {
	    case 'B':
		;
		if ( !disks[disk].active ) {
			disks[disk].track[TRACK18(sec)].bad[OFF18(sec)]=1;
			if ( !quiet ) printf("announce bad sector %d: ",sec);
		}
	    case 'R':
		if ( !quiet) printf("read sector %d: ",sec);
		if ( !disks[disk].active ) {
			addtiming(disk,sec);
			if ( snoop ) snoopread(disk,sec);
			break;
		}
		usleep(ACK1);
		ack('A');
		usleep(COMPLETE1);
		ack('C');
		usleep(COMPLETE2);
		senddata(disk,sec);
		break;
	    case 'W': 
		if ( !quiet) printf("write sector %d: ",sec);
		if ( !disks[disk].active ) break;
		usleep(ACK1);
		if (disks[disk].ro) {
			ack('N');
			if ( !quiet) printf("[Read-only image]");
			break;
		}
		ack('A');
		recvdata(disk,sec);
		usleep(ACK2);
		ack('A');
		usleep(COMPLETE1);
		ack('C');
		break;
	    case 'P': 
		if ( !quiet) printf("put sector %d: ",sec); 
		if ( !disks[disk].active ) break;
		usleep(ACK1);
		if (disks[disk].ro) {
			ack('N');
			if ( !quiet) printf("[Read-only image]");
			break;
		}
		ack('A');
		recvdata(disk, sec);
		usleep(ACK2);
		ack('A');
		usleep(COMPLETE1);
		ack('C');
		break;
	    case 'S': 
		if ( !quiet) printf( "status:" ); 
		if ( !disks[disk].active ) break;
		usleep(ACK1);
		ack('A');
		{
			/*
			 * Bob Woolley wrote on comp.sys.atari.8bit:
			 *
			 * at your end of the process, the bytes are
			 * CMD status, H/W status, Timeout and unused.
			 * CMD is the $2EA value previously
			 * memtioned. Bit 7 indicates an ED disk.  Bits
			 * 6 and 5 ($6x) indicate DD. Bit 3 indicates
			 * write protected. Bits 0-2 indicate different
			 * error conditions.  H/W is the FDD controller
			 * chip status.  Timeout is the device timeout
			 * value for CIO to use if it wants.
			 *
			 * So, I expect you want to send a $60 as the
			 * first byte if you want the OS to think you
			 * are in DD. OK?
			 */
			static unsigned char status[] = { 0x10, 0x00, 1, 0 };
			status[0]=(disks[disk].secsize==128?0x10:0x60);
			if (disks[disk].secsize==128 && disks[disk].seccount>720) status[0]=0x80;
			if (disks[disk].ro) {
				status[0] |= 8;
			}
			else {
				status[0] &= ~8;
			}
			usleep(COMPLETE1);
			ack('C');
			usleep(COMPLETE2);
			sendrawdata(status,sizeof(status));
		}
		break;
	    case 'N':
		if ( !quiet) printf("815 configuration block read");
		if ( !disks[disk].active ) break;
		/* We get 19 of these from DOS 2.0 when you hit reset */
		usleep(ACK1);
		ack('A');
		usleep(COMPLETE1);
		ack('C');
		{
			unsigned char status[12];

			memset(status,0,sizeof(status));
			status[0]=1; /* 1 big track */
			status[1]=1; /* Why not? */
			status[2]=disks[disk].seccount>>8;
			status[3]=disks[disk].seccount&0xff;
			status[5]=((disks[disk].secsize==256)?4:0);
			status[6]=disks[disk].secsize>>8;
			status[7]=disks[disk].secsize&0xff;
			sendrawdata(status,sizeof(status));
		}
		break;
	    case 'O':
		if ( !quiet) printf("815 configuration block write (ignored)");
		if ( !disks[disk].active ) break;
		usleep(ACK1);
		ack('A');
		{
			int i;
			char s;
			int sum=0;

			for( i=0; i<12; i++ ) {
				read( atari, &s, 1 );	
				if ( !quiet) printf(" %02x",s);
				sum = sum + s;
				sum = (sum & 0xff) + (sum >> 8);
			}
			read(atari,&s,1);
			if ((s & 0xff) != (sum & 0xff)) if ( !quiet) printf( "[BAD SUM %02x]",sum );
			if ( !quiet) printf(" ");
		}
		usleep(ACK2);
		ack('A');
		usleep(COMPLETE1);
		ack('C');
		break;
	    case '"':
		if ( !quiet) printf( "format enhanced " ); 
		if ( !disks[disk].active ) break;
		/*** FIXME *** Acknowledge and zero disk image ***/
		usleep(ACK1);
		ack('A');
		usleep(COMPLETE1);
		ack('C');
		usleep(COMPLETE2);
		senddata(disk,99999);
		break;
	    case '!':
		if ( !quiet) printf( "format " ); 
		if ( !disks[disk].active ) break;
		/*** FIXME *** Acknowledge and zero disk image ***/
		usleep(ACK1);
		ack('A');
		usleep(ACK1);
		ack('C');
		break;
	    case 0x20: 
		if ( !quiet) printf( "download " ); 
		if ( !disks[disk].active ) break;
		break;
	    case 0x54: 
		if ( !quiet) printf( "readaddr " ); 
		if ( !disks[disk].active ) break;
		break;
	    case 0x51: 
		if ( !quiet) printf( "readspin " ); 
		if ( !disks[disk].active ) break;
		break;
	    case 0x55: 
		if ( !quiet) printf( "motoron " ); 
		if ( !disks[disk].active ) break;
		break;
	    case 0x56: 
		if ( !quiet) printf( "verify " ); 
		if ( !disks[disk].active ) break;
		break;
	    default:
		if ( !quiet) printf( "??? " );
		if ( !disks[disk].active ) break;
		break;
	}
	if ( !quiet) printf( "\n" );
}

/*
 * wait_for_cmd()
 *
 * Wait for the ring indicator to specify that a command block is being sent
 */
void wait_for_cmd(int fd)
{
  int r;

  r=ioctl(fd,TIOCMIWAIT,TIOCM_RNG);
}

/************************************************************************/
/* afnamecpy()								*/
/* Convert a Unix filename to an Atari filename.			*/
/* Return 0 on failure.							*/
/************************************************************************/
int afnamecpy(char *an,const char *n)
{
	int i;
	for(i=0;i<11;++i) an[i]=' '; /* Space fill the Atari name */
	an[11]=0;
	for(i=0;i<8;++i) {
		if (!*n) return(1); /* Ok */
		if (*n=='.') break; /* Extension */
		if (*n==':') return(0); /* Illegal name */
		if (1) an[i]=toupper(*n);
		else an[i]= *n;
		++n;
	}
	if (*n=='.') ++n;
	for(i=8;i<11;++i) {
		if (!*n) return(1); /* Ok */
		if (*n=='.') return(0); /* Illegal name */
		if (*n==':') return(0); /* Illegal name */
		if (1) an[i]=toupper(*n);
		else an[i]= *n;
		++n;
	}
	if (*n) return(0); /* Extension too long or more than 11 characters */
	return(1);
}
