ATRFS

This is a Linux file system implementation that allows you to mount
an Atari ATR disk image file as a file system under Linux.  You can then
use all your regular file system commands and tools to manipulate the
files.

Usage:
  atrfs [options] --name=[ATR image file path] [mount point]
    or
  atrfs --help

  There are a number of options, including to create a new image file.


Limitations:
  If it is not SpartaDOS or DOS XE, then all timestamps will be the
  timestamp of the ATR file at the time it was mounted.  Attempts to
  change the timestamps will report success to avoid noisy errors when
  copying files.

  File names are presented in 8.3 format, but you can specify file names as
  upto 11 characters, and it will infer a '.' after 8 characters.

  Obviously there is no user/group or ACL support.  The user/group is that
  of the user mounting the file system.  Any attempt to change ownership of
  files will report success despite doing nothing to avoid pointless
  warnings.

  Permissions are always 755 for directories and 644 for files unless the
  file is locked, in which case it is 444.  You can use chmod to turn the
  write permissions on or off.  It will only look at the write permission
  bit for the owner and ignore all other bits.

  If the ATR file is read-only, then all modifications will fail saying
  that it is a read-only file system.  Please unmount the file system
  before changing the permissions or otherwise modifying the underlying ATR
  file.

Special files:
  .bootsectors: The raw boot sectors from the image.
  .bootinfo: A text file containing information from the boot sectors.
  .fsinfo: A text file with general file system and disk image info.

  The above will appear in the directory unless you turn them off.  The
  files will still work even if you turn them off.

  All special files are read-only.

  Example use: hexdump -C .bootsectors

  Additionally, you can always access any sector with .sector# (where '#'
  is the number in decimal, or hex with '0x' or '$' preceding it.

  For file systems that use clusters, you can read raw clusters wth
  .cluster# just like with sectors.  This is supported in DOS3, DOS4, and
  DOS XE.

  If the file system time can not be determined, the standard special files
  will be created.  Also the directory will list the .sector files for each
  non-zero sector.

  The special files are only in the main directory for file systems that
  support subdirectories.

Info files
  Add ".info" to any regular file or directory.  This will be a text file with
  additional information on the file.  It will not show up in directory
  listings and is read-only.


General:

The file system type is detected automatically.  If no valid file system is
detected, it will create the standard special files, .sector files for each non-zero sector, and a few empty files reporting that it's unknown.

The design of this is modular, so that support for any given file system is
a separate file, with the generic handling in atrfs.c.  Due to the use of
common code, DOS 1, DOS 2, DOS 2.5, and MyDOS are all in the mydos.c
module.

File system detection is done by calling a "sanity" function for each
supported file system until one finds it compatible.  In adding new file
systems, the order in the file system enum may be important; add the
more-specific or picky file systems before more general systems if they are
similar enough to pass more than one sanity test.

In general, the code is designed for efficiency in coding, not efficiency
in execution.  Modern computers are fast, and these images are small, so
keeping it simple for coding is optimal.

If you use 'ls -i' to show the inode numbers, you'll see something
appropriate for the given file system, such as the starting sector number.

To-do:

For .info files, expand the types where it will provide some analysis.

I could include disassembly for the boot sectors in .bootinfo, with another
command-line option and also disassemble binary load files.  But there are other tools for disassembling code that run just fine.

Bugs:
 * It doesn't handle concurrency correctly, so simultaneous operations may
   cause weird faults.
 * Rename is not atomic in replacing the target file with the original file
 * .bootsectors returns the full boot sectors, which might not be useful for
   double-density images.


File system specific information:


DOS 1, DOS 2.0s, DOS 2.0d, DOS 2.5, and MyDOS 4.53:

These file systems are all very similar and handled by the mydos.c module.
DOS 2.5 is only detected if it's an enhanced-density disk with a second
VTOC sector on sector 1024.  Otherwise I believe DOS 2.5 images are identical
to DOS 2.0s images.

The code allows subdirectories in all of them except DOS 2.5 and DOS 1, as
it is possible to format a disk with DOS 2.0s and then use MyDOS on the
image, but with DOS 2.5, the extended VTOC sector is incompatible with
MyDOS.  DOS 1 is excluded because MyDOS does not have compatibilty with DOS
1 files.

Note: DOS 2.0d is detected as MyDOS, as it's a proper subset.  The only
difference between the two is the code in the boot sectors is different.

The boot sector code from each version of DOS is embedded in the code, so
the correct boot sectors are used when you create a file system.  Also, if
you write DOS.SYS to the main directory, it will update the boot sectors to
match that version of DOS (if possible) and set the flag and sector
pointers in the boot sector so that the image should boot.

Coding notes:

Handling the file numbers in the last three bytes of each sector can be a
pain, especially when moving files between directories, but also for other
operations.  So to make it simple, I use a function to remove all file
numbers, and another to add them back.  The adding back first checks if it
goes above 1023.  Hence, files are kept with maximal DOS 2 compatibility in
MyDOS images.  This is different from regular MyDOS which will simply not
use file numbers for newly created files.

Bugs:

This has had only minimal testing.


SpartaDOS:

This is implemented based on documentation I've found for SpartaDOS 1.1.  I
could use some help finding differences between SpartaDOS versions,
including boot sectors.

Note that file time stamps are preserved.  They are interpreted as being
from the current time zone.  SpartaDOS uses one byte for the last two digits of the year.  I don't know if there's a standard, but I arbitrarily decided that years 78 and up should have 1900 added, and years 00 though 77 are 2000 through 2077.  Writing files will use this convention, too, never using years > 100.

Bugs:

Probably.


DOS XE:

Read-only for now.

Atari DOS 3:

Read-only for now.

Atari DOS 4 (designed for 1450XLD):

Read-only for now.

LiteDOS:

Minimal testing.

Bugs:

Does not handle copying DOS to the disk with touching the boot sector to make it boot.


Thoughts:

There were may other DOS versions released by third parties that could be
implemented here.  Games that use their own formats could also be supported.
I think I saw a game that used DOS 2 hacked to move the directory sectors.
If we can detect the format, we can add support.
