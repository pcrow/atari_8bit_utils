/*
 *
 * Copyrights are held by the respective authors listed below.
 * Licensed for distribution under the GNU Public License version 2.0 or later.
 *
 * You need to use a sio2pc cable to run this.
 *
 *
 * Compilation:
 *	gcc -W -Wall -o sio2linux sio2linux.c
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
 *        Would it be possible to send SIO commands to other devices, ignoring
 *        the Atari, potentially using the sio2pc device to access a disk or other
 *        device directly, not using an Atari computer at all?
 *
 *	* Add a keyboard user interface to add/remove/swap disks during
 *	  run time.
 *
 *	* Enhance support for dynamic disk images that are live access to the
 *	  Linux file system.  It could use My-DOS style subdirectories.
 *        Support for creating new files could be added.
 *        Support for more than one file open at a time could be added.
 *        This really needs a full redesign where we track what directory entries
 *        have been sent, so it doesn't mess up if files are added or removed.
 *        Another option would be to also simulate SpartaDOS images.
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
 *        I'm not sure this is possible with my original sio2pc cable.
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
 *      * Add support for .ATX copy-protected images.
 *
 *      * Could we add support for the FujiNet N: device?
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

#define ARRAY_SIZE(_x) ((int)(sizeof(_x)/sizeof(_x[0])))

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
        struct host_mydos *mydos; /* If emulating a MyDOS image */
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
 * struct mydos_dir
 *
 * This tracks one subdirectory (or the root directory) in a MyDOS
 * host image.
 */
struct mydos_dir {
        char *host_dir;
        int entry_count; // 0..64
        char atari_name[64][8+3+1]; // +1 for NUL
        char *host_name[64]; // strdup, so remember to free
        int isdir[64]; // true if a subdirectory
        int subdir_index[64]; // Index into array for active subdirectories
        int secskip[64]; // If reading the second sector, add this many in referencing the host file
        int hostfd[64]; // file descriptor on host file system
        off_t file_bytes[64];
        int file_write[64]; // true if write permission
        struct timeval last_access; // Updated on any access to dir or file in dir
        // FIXME: variables for tracking files open for writing
        int isopen[64]; // true if file is open for writing
};

struct host_mydos {
        int mydos; // true if MyDOS extensions to DOS 2.0s are allowed
        int write; // true if write support enabled
        struct mydos_dir dir[63]; // Root is 0, 1..63 for subdirs
};

struct mydos_vtoc {
        unsigned char vtoc_sectors; // SD: X*2-3 ; DD: X-1 (extra SD sectors allocated in pairs)
        unsigned char total_sectors[2];
        unsigned char free_sectors[2];
        unsigned char unused[5];
        unsigned char bitmap[118]; // sectors 0-943; continues on sector 359, 358,... as needed
};

struct mydos_sort_dir {
        int order;
        char atari_name[8+3+1];
        char d_name[256];
};
        

#define MYDOS_SIZE_TO_SECTORS(_size) ((_size + 125)/125)
#define MYDOS_FILE_START(_dirnum,_filenum) ( !(_dirnum) ? (_filenum)+4 : (_dirnum) * 1024 + 1024 + 512 + (_filenum))
#define MYDOS_DIR_START(_dirnum,_filenum) ( !(_dirnum) ? (_filenum)*8+1024 : (_dirnum) * 1024 + 1024 + (_filenum)*8)

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
void init_mydos_drive(int disk,char *hostdir,int write);
void read_mydos_sector(int disk,int sec);

/*
 * Macros
 */
#define SEEK(n,i)	(disks[disk].seekcode==xfd)?SEEK0(n,i):((disks[i].seekcode=atr)?SEEK1(n,i):SEEK2(n,i))
#define SEEK0(n,i)	((n-1)*disks[i].secsize)
#define SEEK1(n,i)	(ATRHEAD + ((n<4)?((n-1)*128):(3*128+(n-4)*disks[i].secsize)))
#define SEEK2(n,i)	(ATRHEAD + ((n-1)*disks[i].secsize))
#define ATRHEAD		16
#define MAXDISKS	15 // Hard to access beyond 9
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
char sbuf[64];
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
	"  <file> disk image to mount as next disk (D1 through D15 in order)\n" \
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

        // Set default for serial port device
        {
                struct stat stat_mouse,stat_tty;
                serial="/dev/ttyS0";
                if (stat("/dev/mouse",&stat_mouse)==0) {
                        stat(serial,&stat_tty);
                        if (stat_mouse.st_rdev==stat_tty.st_rdev) {
                                printf("%s is the mouse, using ttyS1 as default serial device\n",serial);
                                serial="/dev/ttyS1";
                        }
                }
	}
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
                                if ( argv[i][0] == '/' )
                                        serial=argv[i];
                                else if ( isdigit(argv[i][0]) ) {
                                        serial=sbuf;
                                        sprintf(serial,"/dev/ttyS%s",argv[i]);
                                }
                                else {
                                        serial=sbuf;
                                        sprintf(serial,"/dev/%s",argv[i]);
                                }
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
 * In theory, this should work with only two sectors per file.  You always need
 * unique first sector for opening the file, but then the second sector could link
 * back to itself as files must be read sequentially.  If the sectors per file is
 * more than 5, then the calculations need to take into account skipping sectors 360-368.
 *
 * We need any subdirectories to have 8 sectors since they don't follow the sector
 * chaining system.
 *
 * Future: Consider a sort option
 */
#define SECSPERFILE 8
#define ROOTSKIP 2
#define RAW_FILENUM(_dirnum,_filenum) ((_dirnum)*64+(_filenum))
#define RAW_START_SEC(_dirnum,_filenum) (4 + RAW_FILENUM(_dirnum,_filenum)*SECSPERFILE)
#define START_SEC(_dirnum,_filenum) (RAW_START_SEC(_dirnum,_filenum+1) < 360 ? RAW_START_SEC(_dirnum,_filenum) | RAW_START_SEC(_dirnum,(_filenum)+ROOTSKIP))
#define SEC_TO_FILENUM(_sec) ((_sec) < 360?((_sec)-3)/SECSPERFILE:((_sec)-3)/SECSPERFILE-ROOTSKIP)
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
		free = total - SECSPERFILE*64;
		buf[4]=free/256;
		buf[3]=free%256;
		buf[0]=2; /* ??? */
		for(i=0;i<720;++i) {
			byte=10+i/8;
			bit=i%8;
			bit=7-bit;
			bit=1<<bit;
			if(i>=4+64*SECSPERFILE && i!=720 && (i<360 || i>368)) {
				buf[byte]|=bit;
			}
		}
		sendrawdata(buf,size);
		return;
	}
	if ( sec>=361 && sec<=368 ) { /* Create directory */
		rewinddir(disks[disk].dir);
		for(i=0;i<8*(sec-361);++i) {
			de=readdir(disks[disk].dir);
                        if ( !de ) {
				sendrawdata(buf,size);
				return;
			}
                        // skip hidden dot files
                        if ( de->d_name[0]=='.' ) {
                                --i;
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
			start=4+fn*SECSPERFILE;
			if ( de ) {
                                if ( de->d_name[0]=='.' ) {
                                        --i;
                                        continue;
                                }
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
	if ( sec>=4 && sec<4+64*SECSPERFILE ) { /* send file data */
		int fn;
		int off;
		off_t seekto;

		fn=(sec-4)/SECSPERFILE;
		off=sec-4-fn*SECSPERFILE;
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
			for(i=0;i<=fn;++i) {
                                de=readdir(disks[disk].dir);
                                if ( de && de->d_name[0]=='.' ) --i;
                        }
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
		if ( off==SECSPERFILE-1 ) {
			buf[126] -= (SECSPERFILE-1);
			disks[disk].secoff+=(SECSPERFILE-1);
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

        if ( disks[disk].mydos ) {
                read_mydos_sector(disk,sec);
                return;
        }
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
 */
static int get_atari(void)
{
	int fd;

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
                                init_mydos_drive(disk,path,1); // FIXME: set write from command-line option
                                printf( "D%d: %s simulated disk\n",disk+1,path);
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

	if (disks[disk].diskfd<0 && !disks[disk].mydos) {
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
	    case 0x39: if ( !quiet) printf( "D9: " ); disk = 8; break;
	    case 0x3A: if ( !quiet) printf( "D10: " ); disk = 9; break;
	    case 0x3B: if ( !quiet) printf( "D11: " ); disk = 10; break;
	    case 0x3C: if ( !quiet) printf( "D12: " ); disk = 11; break;
	    case 0x3D: if ( !quiet) printf( "D13: " ); disk = 12; break;
	    case 0x3E: if ( !quiet) printf( "D14: " ); disk = 13; break;
	    case 0x3F: if ( !quiet) printf( "D15: " ); disk = 14; break;
	    case 0x40: if ( !quiet) printf( "P: " ); printer = 1; break;
	    case 0x41: if ( !quiet) printf( "P2: " ); printer = 2; break;
	    case 0x42: if ( !quiet) printf( "P3: " ); printer = 3; break;
	    case 0x43: if ( !quiet) printf( "P4: " ); printer = 4; break;
	    case 0x44: if ( !quiet) printf( "P5: " ); printer = 5; break;
	    case 0x45: if ( !quiet) printf( "P6: " ); printer = 6; break;
	    case 0x46: if ( !quiet) printf( "P7: " ); printer = 7; break;
	    case 0x47: if ( !quiet) printf( "P8: " ); printer = 8; break;
	    case 0x48: if ( !quiet) printf( "P9: " ); printer = 9; break;
            case 0x4F: if ( !quiet) printf( "Poll: " ); break; // fart noise during boot
	    case 0x50: if ( !quiet) printf( "R1: " ); rs = 0; break;
	    case 0x51: if ( !quiet) printf( "R2: " ); rs = 1; break;
	    case 0x52: if ( !quiet) printf( "R3: " ); rs = 2; break;
	    case 0x53: if ( !quiet) printf( "R4: " ); rs = 3; break;
                default: if ( !quiet) printf( "0x%02x: ignored\n",buf[0]);return;
	}
	if (disk>=0&&!disks[disk].active) { if ( !quiet) printf( "[no image] " ); }
	if ( printer>=0 && printer != 9 ) {if ( !quiet) printf("[Printers not supported]\n"); return; }
	if (rs>=0) {if ( !quiet) printf("[Serial ports not supported]\n"); return; }

	sec = buf[2] + 256*buf[3];

	switch( buf[1] ) {
	    case 'B':
		;
		if ( !disks[disk].active ) {
			disks[disk].track[TRACK18(sec)].bad[OFF18(sec)]=1;
			if ( !quiet ) printf("announce bad sector %d: ",sec);
		}
                // fall through
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
                if ( printer == 9 ) {
                        unsigned char pbuf[80];
                        int i;
                        int sum=0;

                        if ( !quiet) printf("write: ");
                        usleep(ACK1);
                        ack('A');
                        for( i=0; i<40; i++ ) {
                                read( atari, &pbuf[i], 1 );	
                                sum = sum + pbuf[i];
                                sum = (sum & 0xff) + (sum >> 8);
                        }
                        read(atari,&i,1);
                        if ((i & 0xff) != (sum & 0xff) && !quiet) printf( "[BAD SUM]" );
                        usleep(ACK2);
                        ack('A');
                        usleep(COMPLETE1);
                        ack('C');
                        if ( !quiet ) printf(" %.40s",pbuf);
                        // for (i=0;i<40;++i) printf("%02X ",pbuf[i]);
                        // siomanage(pbuf); // Process command set by the Atari
                        break;
                }
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
                if ( printer == 9 )
                {
                        usleep(ACK1);
                        ack('A');
			static unsigned char status[] = { 0,0,0,0};
			usleep(COMPLETE1);
			ack('C');
			usleep(COMPLETE2);
			sendrawdata(status,sizeof(status));
                        break;
                }
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
 *
 * This is no longer used, as the ring indicator doesn't always work as desired
 */
void wait_for_cmd(int fd)
{
  int r;

  r=ioctl(fd,TIOCMIWAIT,TIOCM_RNG);
  (void) r; // not used
}

/************************************************************************/
/* afnamecpy()								*/
/* Convert a Unix filename to an Atari filename.			*/
/* Return 0 on failure.							*/
/************************************************************************/
int afnamecpy(char *an,const char *n)
{
	int i;
        if ( *n == '.' ) return 0; // Skip dot files
	for(i=0;i<11;++i) an[i]=' '; /* Space fill the Atari name */
	an[11]=0;
	for(i=0;i<8;++i) {
		if (!*n) return(1); /* Ok */
		if (*n=='.') break; /* Extension */
		if (*n==':') return(0); /* Illegal name */
		if (*n=='~') return(0); /* Illegal name */
		if (*n=='#') return(0); /* Illegal name */
		if (1) an[i]=toupper(*n);
		else an[i]= *n;
		++n;
	}
	if (*n=='.') ++n;
	for(i=8;i<11;++i) {
		if (!*n) return(1); /* Ok */
		if (*n=='.') return(0); /* Illegal name */
		if (*n==':') return(0); /* Illegal name */
		if (*n=='~') return(0); /* Illegal name */
		if (*n=='#') return(0); /* Illegal name */
		if (1) an[i]=toupper(*n);
		else an[i]= *n;
		++n;
	}
	if (*n) return(0); /* Extension too long or more than 11 characters */
	return(1);
}

/*
 * init_mydos_drive()
 *
 * Initialize a directory to appear as an Atari drive (D1: through D15:) in
 * MyDOS format.  Note that if there are no subdirectories, this will be DOS 2.0s
 * format.
 */
void init_mydos_drive(int disk,char *hostdir,int write)
{
        disks[disk].mydos = calloc(1,sizeof(struct host_mydos));
        if ( !disks[disk].mydos ) return; // memory full error; won't happen
        disks[disk].mydos->dir[0].host_dir = strdup(hostdir);
        disks[disk].mydos->write = write;
        disks[disk].mydos->mydos = 1; // FIXME: Allow it to be just DOS 2.0s from command line
        // No, on demand: mydos_read_dir(disks[disk].mydos,0); // Read the root directory
        disks[disk].active=1;
        disks[disk].secsize=128;
        disks[disk].seccount=65535; // all sectors are valid
        disks[disk].seekcode=direct;
        disks[disk].dirname=hostdir;
        disks[disk].filefd = -1;
        disks[disk].afileno = -1;
        return;
}

/*
 * mydos_readdir_compare()
 *
 * Comparison function for qsort
 */
int mydos_readdir_compare(const void *a,const void *b)
{
        return memcmp(a,b,sizeof(struct mydos_sort_dir));
}

/*
 * mydos_readdir_sorted()
 *
 * Read a directory into a dynamically allocated array, and sort it
 */
struct mydos_sort_dir *mydos_readdir_sorted(char *path,int *entries)
{
        struct mydos_sort_dir *dir_array;
        int array_len = 64;
        *entries = 0;

        dir_array = malloc(array_len * sizeof(dir_array[0]));
        if ( !dir_array )
        {
                return NULL;
        }

        // Read in all the entries
        DIR *dir = opendir(path);
        if ( !dir ) return NULL;
        struct dirent *de;
        while ( NULL != (de=readdir(dir)) )
        {
                if ( *entries == array_len )
                {
                        array_len += 64;
                        dir_array = realloc(dir_array,array_len * sizeof(dir_array[0]));
                        if ( !dir_array )
                        {
                                closedir(dir);
                                return NULL;
                        }
                }
                memset(&dir_array[*entries],0,sizeof(dir_array[0])); // Wipe any alignment pads just to be safe
                if ( 0 == afnamecpy(dir_array[*entries].atari_name,de->d_name) ) continue; // Skip invalid names
                //memcpy(dir_array[*entries].d_name,de->d_name,256); // size from man page
                strcpy(dir_array[*entries].d_name,de->d_name);
                // Special files that go first
                dir_array[*entries].order = 255; // Not a priority entry
                if ( strcmp(dir_array[*entries].atari_name,"DOS     SYS") == 0 ) dir_array[*entries].order = 1;
                if ( strcmp(dir_array[*entries].atari_name,"DUP     SYS") == 0 ) dir_array[*entries].order = 2;
                if ( strcmp(dir_array[*entries].atari_name,"AUTORUN SYS") == 0 ) dir_array[*entries].order = 3;
                if ( strcmp(dir_array[*entries].atari_name,"MEM     SAV") == 0 ) dir_array[*entries].order = 4;
                ++*entries;
        }
        closedir(dir);

        // Sort the directory entries
        if ( *entries > 1 )
        {
                qsort(dir_array,*entries,sizeof(dir_array[0]),mydos_readdir_compare);
        }

        // Return
        return dir_array;
}


/*
 * mydos_read_dir()
 *
 * Read a directory and set up MyDOS emulation.
 */
void mydos_read_dir(struct host_mydos *mydos,struct mydos_dir *mdir)
{
        struct mydos_sort_dir *dir_array;
        int entries;

        if ( !quiet ) printf("\n  Re-read dir: %s\n",mdir->host_dir);
        dir_array = mydos_readdir_sorted(mdir->host_dir,&entries);
        if ( !dir_array )
        {
                if ( !quiet ) printf("  Failed to read directory: %s\n",mdir->host_dir);
                return;
        }
        
	char path[MAXPATHLEN];
        struct stat sb;
        struct mydos_dir newdir; // Read dir here and then merge
        struct mydos_dir *nd = &newdir;
        if ( !mdir->entry_count ) nd=mdir;
        char *dirsave = mdir->host_dir;
        memset(nd,0,sizeof(*nd));
        nd->host_dir = dirsave;

        for (int i=0;i<entries;++i)
        {
                strcpy(path,mdir->host_dir);
                strcat(path,"/");
                strcat(path,dir_array[i].d_name);
                if ( access(path,R_OK) != 0 ) continue; // Ignore non-readable files
                if ( stat(path,&sb) != 0 ) continue; // Shouldn't fail to stat
                nd->hostfd[nd->entry_count] = -1;
                nd->file_bytes[nd->entry_count] = sb.st_size;
                nd->isdir[nd->entry_count] = (sb.st_mode & S_IFDIR) != 0;
                memcpy(nd->atari_name[nd->entry_count],dir_array[i].atari_name,sizeof(nd->atari_name[0]));
                if ( mydos->write && !access(path,W_OK) ) {
                        nd->file_write[nd->entry_count] = 1;
                }
                nd->host_name[nd->entry_count] = strdup(path);
                if ( !quiet ) printf("   %2d: %.8s.%.3s -> %s\n",nd->entry_count,nd->atari_name[nd->entry_count],&nd->atari_name[nd->entry_count][8],nd->host_name[nd->entry_count]);
                ++nd->entry_count;
                if ( nd->entry_count == 64 ) break; // dir full
        }
        free(dir_array);
        //for ( int i=0;i<64;++i ) { if ( nd->host_name[i] ) printf("   nd %d: %s\n",i,nd->host_name[i]); } // Debug
        if ( !quiet ) printf("  entry_count: %d\n",nd->entry_count);
        gettimeofday(&nd->last_access,NULL);
        // FIXME: sort directory?
        if ( nd == mdir ) return; // No need to merge new read with old

        if ( !quiet ) printf("  merging entries\n");
        /*
         * For each entry in the old directory, check to see if it's in the new directory.
         * If it's not in the new directory, remove the entry
         * If it's also in the new directory, copy over any updates and remove it from the new directory
         */
        for ( int i=0;i<64;++i )
        {
                if (0) next_i: continue;
                if ( !mdir->host_name[i] ) continue;
                for ( int j=0;j<64;++j )
                {
                        if ( !nd->host_name[j] ) continue;
                        if ( strcmp(mdir->host_name[i],nd->host_name[j]) == 0 )
                        {
                                // This entry is in both new and old, copy from new to old and clear new
                                if ( !quiet ) printf("  preserving entry previously found: %s\n",mdir->host_name[i]);
                                // update status
                                mdir->isdir[i] = nd->isdir[j];
                                mdir->file_bytes[i] = nd->file_bytes[j];
                                mdir->file_write[i] = nd->file_write[j];
                                free(nd->host_name[j]);
                                nd->host_name[j] = NULL;
                                goto next_i;
                        }
                }
                // This entry in mdir wasn't found; remove it
                if ( !quiet ) printf("  removing entry previously found: %s\n",mdir->host_name[i]);
                free(mdir->host_name[i]);
                mdir->host_name[i]=NULL;
                if ( mdir->hostfd[i] ) close(mdir->hostfd[i]);
                mdir->hostfd[i] = -1;
        }
        // for ( int i=0;i<64;++i ) { if ( mdir->host_name[i] ) printf("   mdir %d: %s\n",i,mdir->host_name[i]); } // DEBUG
        /*
         * For each entry in the new directory, find space in the old directory and copy it over.
         */
        for (int j=0;j<64;++j)
        {
                if ( !nd->host_name[j] ) continue;
                for ( int i=0;i<64;++i )
                {
                        if ( mdir->host_name[i] ) continue;
                        if ( !quiet ) printf("  adding entry not previously found: %s\n",nd->host_name[j]);
                        memcpy(mdir->atari_name[i],nd->atari_name[j],8+3+1);
                        mdir->host_name[i] = nd->host_name[j];
                        nd->host_name[j] = NULL;
                        mdir->isdir[i] = nd->isdir[j];
                        mdir->subdir_index[i] = nd->subdir_index[j];
                        mdir->secskip[i] = nd->secskip[j];
                        mdir->hostfd[i] = nd->hostfd[j];
                        mdir->file_bytes[i] = nd->file_bytes[j];
                        mdir->file_write[i] = nd->file_write[j];
                        mdir->isopen[i] = nd->isopen[j];
                        break;
                }
        }

        /*
         * set entry_count
         */
        mdir->entry_count = 0;
        for (int i=63;i>=0;--i)
        {
                if ( mdir->host_name[i] )
                {
                        mdir->entry_count = i+1;
                        break;
                }
        }
        return;
}

/*
 * mydos_dealloc_dir()
 *
 * Free all resources and clear out a directory
 */
void mydos_dealloc_dir(struct mydos_dir *dir)
{
        if ( dir->host_dir ) free(dir->host_dir);
        for (int i=0;i<64;++i) if ( dir->host_name[i] ) free(dir->host_name[i]);
        for (int i=0;i<64;++i) if ( dir->hostfd[i] ) close(dir->hostfd[i]);
        memset(dir,0,sizeof(*dir));
}

/*
 * mydos_alloc_subdir()
 *
 * We're accessing a new directory; assign it to a struct so that it can
 * be read and mapped.
 *
 * This gets tricky when we need to drop an old entry.
 *
 * Return the index in the array of directories for a new directory.
 */
int mydos_alloc_subdir(struct host_mydos *mydos,char *host_name)
{
        int oldest=0;
        // Easy: Find an unallocated directory
        for (int i=1;i<ARRAY_SIZE(mydos->dir);++i)
        {
                if ( !mydos->dir[i].host_dir )
                {
                        oldest = i;
                        break;
                }
        }

        // Find oldest directory with no open files
        if ( !oldest ) for (int i=1;i<ARRAY_SIZE(mydos->dir);++i)
        {
                if (0) next_i: continue;
                for (int j=0;j<64;++j)
                {
                        if ( mydos->dir[i].host_name[j] && ( mydos->dir[i].isopen[j] || mydos->dir[i].hostfd[j] >= 0 ) )
                        {
                                goto next_i;
                        }
                }
                if ( !oldest ) oldest = i;
                else
                {
                        if ( timercmp( &mydos->dir[i].last_access,  &mydos->dir[oldest].last_access, < ) ) oldest = i;
                }
        }

        // Find oldest directory with no write-open files
        if ( !oldest ) for (int i=1;i<ARRAY_SIZE(mydos->dir);++i)
        {
                if (0) next_i2: continue;
                for (int j=0;j<64;++j)
                {
                        if ( mydos->dir[i].host_name[j] && ( mydos->dir[i].hostfd[j] >= 0 ) )
                        {
                                goto next_i2;
                        }
                }
                if ( !oldest ) oldest = i;
                else
                {
                        if ( timercmp( &mydos->dir[i].last_access,  &mydos->dir[oldest].last_access, < ) ) oldest = i;
                }
        }

        // Find oldest directory even with open files
        if ( !oldest )
        {
                oldest = 1;
                for (int i=21;i<ARRAY_SIZE(mydos->dir);++i)
                {
                        if ( timercmp( &mydos->dir[i].last_access,  &mydos->dir[oldest].last_access, < ) ) oldest = i;
                }
        }
        mydos_dealloc_dir(&mydos->dir[oldest]);
        mydos->dir[oldest].host_dir = strdup(host_name);
        return oldest;
}


/*
 * read_mydos_sector()
 *
 * Send host data back as a MyDOS image.
 *
 * For the root directory, files 0-63 are two sectors each starting at sector 4.
 * Subdirectories of the root directory are 8 sectors each starting at sector 1024.
 * This leaves free sectors: 132--359 and 369--719 for writing.
 *
 * Active subdirectories start at 1024*n, and have 512 sectors for 64 possible
 * subdirectories and 128 bytes for 64 possible regular files.
 */
void read_mydos_sector(int disk,int sec)
{
        unsigned char buf[128];
        memset(buf,0,sizeof(buf));

        // Check for first sector of a regular file:
        int isfile = 1;
        int issecond = 0;
        int dirnum = sec/1024;
        int filenum;
        if ( dirnum == 1 ) isfile = 0; // No file sectors in 1024..2047
        if ( dirnum > 1 ) --dirnum;
        filenum = sec % 1024;
        if ( !dirnum ) filenum -= 4; // skip first three sectors for boot; entry 0 starts on sector 4
        else filenum -= 512; // skip first 512 bytes for subdirectories
        if ( filenum < 0 ) isfile = 0;
        if ( isfile && filenum >= 64 && filenum < 128 )
        {
                issecond = 1; // additional sector
                filenum -= 64;
        }
        if ( filenum < 0 || filenum >= 64 ) isfile = 0;
        if ( isfile && disks[disk].mydos->dir[dirnum].entry_count <= filenum ) isfile = 0;
        if ( isfile && disks[disk].mydos->dir[dirnum].isdir[filenum] ) isfile = 0;
        if ( isfile && !issecond )
        {
                // This is the first sector of a file
                if ( !quiet ) printf(" Read first sector of file: %d: %s\n",filenum,disks[disk].mydos->dir[dirnum].host_name[filenum]);
                gettimeofday(&disks[disk].mydos->dir[dirnum].last_access,NULL);
                disks[disk].mydos->dir[dirnum].secskip[filenum] = 0; // starting new read
                // Open the file (close it first if previously opened)
                if ( disks[disk].mydos->dir[dirnum].hostfd[filenum] >= 0 )
                {
                        close(disks[disk].mydos->dir[dirnum].hostfd[filenum]);
                }
                disks[disk].mydos->dir[dirnum].hostfd[filenum] = open(disks[disk].mydos->dir[dirnum].host_name[filenum],O_RDONLY);
                if ( disks[disk].mydos->dir[dirnum].hostfd[filenum] < 0 )
                {
                        printf(" Failed to open file: %s\n",disks[disk].mydos->dir[dirnum].host_name[filenum]);
                        sendrawdata(buf,128);
                        return;
                }
                // Read 125 bytes
                int r;
                r = read(disks[disk].mydos->dir[dirnum].hostfd[filenum],buf,125);
                if ( r < 0 )
                {
                        printf(" Failed to read file: %s\n",disks[disk].mydos->dir[dirnum].host_name[filenum]);
                        sendrawdata(buf,128);
                        return; // FIXME: handle error
                }
                buf[127] = r;
                if ( sec < 720 )
                {
                        buf[125] = filenum << 2;
                }
                if ( r == 125 )
                {
                        buf[125] |= (sec+64)/256;
                        buf[126] = (sec+64) & 0xff;
                }
                else
                {
                        close(disks[disk].mydos->dir[dirnum].hostfd[filenum]); // EOF; close file
                }
                sendrawdata(buf,128);
                return;
        }
        if ( issecond )
        {
                // This is a subsequent sector of a file
                gettimeofday(&disks[disk].mydos->dir[dirnum].last_access,NULL);
                ++disks[disk].mydos->dir[dirnum].secskip[filenum]; // Next time read past this
                // Read 125 bytes
                int r;
                r = read(disks[disk].mydos->dir[dirnum].hostfd[filenum],buf,125);
                if ( r < 0 ) return; // FIXME: handle error
                buf[127] = r;
                if ( sec < 720 )
                {
                        buf[125] = filenum << 2;
                }
                if ( r >= 125 )
                {
                        buf[125] |= sec / 256;
                        buf[126] =  sec & 0xff;
                }
                else
                {
                        close(disks[disk].mydos->dir[dirnum].hostfd[filenum]); // EOF; close file
                        disks[disk].mydos->dir[dirnum].secskip[filenum] = 0;
                }
                sendrawdata(buf,128);
                return;
        }

        // Check for a subdirectory
        int isdir = 1;
        int diroff;
        dirnum = sec/1024 - 1;
        filenum = (sec%1024)/8;
        diroff = sec % 8;
        if ( dirnum < 0 || dirnum >= 63 ) isdir = 0;
        if ( filenum >= 64 ) isdir = 0;
        if ( isdir && !disks[disk].mydos->dir[dirnum].isdir[filenum]) isdir = 0; // right location, but not a subdir entry
        if ( sec >= 361 && sec <=368 )
        {
                isdir = 1;
                dirnum = 0;
                diroff = sec - 361;
        }
        if ( isdir )
        {
                // Make sure the directory is allocated
                int reread = 0;
                if (sec > 368 && disks[disk].mydos->dir[dirnum].subdir_index[filenum] == 0)
                {
                        disks[disk].mydos->dir[dirnum].subdir_index[filenum] = mydos_alloc_subdir(disks[disk].mydos,disks[disk].mydos->dir[dirnum].host_name[filenum]);
                        reread = 1;
                }
                // Refresh directory if reading first sector
                if ( !diroff || reread )
                {
                        // Re-read the directory
                        mydos_read_dir(disks[disk].mydos,
                                       &disks[disk].mydos->dir[
                                               disks[disk].mydos->dir[dirnum].subdir_index[filenum]]);
                }

                gettimeofday(&disks[disk].mydos->dir[dirnum].last_access,NULL);
                // Reading a directory sector for a directory
                if ( sec >= 1024 )
                {
                        dirnum = disks[disk].mydos->dir[dirnum].subdir_index[filenum]; // Not the directory the directory is located in, but where it's really found
                }
                struct atari_dirent *ade = (void *)buf;
                struct mydos_dir *mdir = &disks[disk].mydos->dir[dirnum];
                if ( !quiet ) printf("Reading entries %d--%d of %d dirnum %d\n",diroff*8,diroff*8+7,mdir->entry_count,dirnum);
                for (int i=0+diroff*8;i<8+diroff*8;++i)
                {
                        if ( i >= mdir->entry_count ) break; // End of directory
                        // Fill in ade from mdir entry i
                        ade->flag = 0;
                        if ( !mdir->host_name[i] ) ade->flag = 0x80; // deleted
                        if ( mdir->host_name[i] )
                        {
                                if ( mdir->isdir[i] )
                                {
                                        ade->flag |= 0x10; // mydos_subidr
                                }
                                else
                                {
                                        ade->flag |= 0x42; // normal file, DOS 2 (not DOS 1)
                                        if ( dirnum ) ade->flag |= 0x04; // MyDOS extensions
                                }
                                if ( !mdir->file_write[i] )
                                {
                                        ade->flag |= 0x20; // locked
                                }
                                if ( mdir->isopen[i] )
                                {
                                        ade->flag |= 0x01; // open for write
                                }
                        }
                        if ( mdir->isdir[i] )
                        {
                                ade->countlo = 8;
                                ade->counthi = 0;
                                ade->startlo = (MYDOS_DIR_START(dirnum,i)+diroff) & 0xff;
                                ade->starthi = (MYDOS_DIR_START(dirnum,i)+diroff) / 0x100;
                        } else {
                                ade->countlo = MYDOS_SIZE_TO_SECTORS(mdir->file_bytes[i]) & 0xff;
                                ade->counthi = MYDOS_SIZE_TO_SECTORS(mdir->file_bytes[i]) / 0x100;
                                ade->startlo = (MYDOS_FILE_START(dirnum,i)+diroff) & 0xff;
                                ade->starthi = (MYDOS_FILE_START(dirnum,i)+diroff) / 0x100;
                        }
                        memcpy(ade->namelo,mdir->atari_name[i],8+3); // Also copies namehi
                        if ( !quiet ) printf("  Entry %d: flags %02x sector %d %.8s.%.3s\n",i,ade->flag,ade->starthi*256+ade->startlo,ade->namelo,ade->namehi);
                        // Next entry
                        ++ade;
                }
                sendrawdata(buf,128);
                return;
        }

        // Check for VTOC sector
        if ( sec == 360 )
        {
                struct mydos_vtoc *vtoc = (void *)buf;
                vtoc->vtoc_sectors = 1;
                vtoc->total_sectors[0] = 707 & 0xff;
                vtoc->total_sectors[1] = 707 / 0x100;
                vtoc->free_sectors[0] = (707 - 128) & 0xff;
                vtoc->free_sectors[1] = (707 - 128) / 0x100;
                if ( !disks[disk].mydos->write ) {
                        vtoc->free_sectors[0] = 0;
                        vtoc->free_sectors[1] = 0;
                }
                else
                {
                        // Set bit for each free sector
                        for ( int i=3+128+1;i<720;++i )
                        {
                                if ( i>=360 && i<=368 ) continue;
                                vtoc->bitmap[i/8] |= 1 << (7-(i%8));
                        }
                }
                sendrawdata(buf,128);
                return;
        }

        // Sector isn't active, return zeros (buf already set to zeros)
        sendrawdata(buf,128);
}

/*
 * write_mydos_sector()
 *
 * Receive data from the Atari for the host file system.
 *
 * Writes to directory entries can be used to:
 *   lock/unlock (chmod)
 *   rename
 *   delete
 *   open for append
 *   open for writing (truncate)
 *   create
 * Support for append may not be implemented
 *
 * We can track files open for writing, but not files open for reading, so it's
 * difficult to tell when we're done with a subdirectory.  We'll just keep the last
 * 62 directories read, then reuse the oldest one without a file open for writing.
 */
void write_mydos_sector(int disk,int sec,unsigned char *buf)
{
        (void)disk;// FIXME: get mydos struct from disk
        if ( sec == 360 ) return; // Ignore writes to sector 360
        
        // Check if sector is for a subdirectory
        if ( ( sec >= 361 && sec <= 368 ) || // root directory
             ( sec >= 1024 && (sec&1023) < 512 ) ) // subdirectory
        {
                (void)buf; // FIXME
                return;
        }

        // Check if sector is in the regular write area
        if ( sec >= 132 && sec <= 719 && !( sec >= 360 && sec <= 368 ) )
        {
                ; // FIXME - save file write data and correlate with file
                return;
        }
}

// If opening this in emacs, set tabs to be consistent.
// Note: I don't use 8 character indent anymore, but I'm keeping this for change tracking.
/* Local Variables: */
/* c-basic-offset: 8 */
/* End: */
