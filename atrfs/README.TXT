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
  of the user mounting the file system.

  Permissions are always 755 for directories and 644 for files unless the
  file is locked, in which case it is 444.  You can use chmod to turn the
  write permissions on or off.

  If the ATR file is read-only, then all modifications will fail saying
  that it is a read-only file system.  Please unmount the file system
  before changing the permissions or otherwise modifying the underlying ATR
  file.

Special files (if enabled):
  .bootsectors: The raw boot sectors from the image.
  .bootinfo: A text file containing information from the boot sectors.
  .fsinfo: A text file with general file system and disk image info.

  All special files are read-only.

Info files (if enabled):
  Add ".info" to any regular file.  This will be a text file with
  additional information on the file.  It will not show up in directory
  listings and is read-only.


General:

The file system type is detected automatically.  If no valid file system is
detected, then the mount will fail.

The design of this is modular, so that support for any given file system is
a separate file, with the generic handling in atrfs.c.

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
to DOS 2.0s image.

The code allows subdirectories in all of them except DOS 2.5, as it is
possible to format a disk with DOS 2.0s and then use MyDOS on the image,
but with DOS 2.5, the extended VTOC sector is incompatible with MyDOS.

Note: DOS 2.0d is detected as MyDOS, as it's a proper subset.  The only
difference between the two is the code in the boot sectors is different.

Coding notes:

Handling the file numbers in the last three bytes of each sector can be a
pain, especially when moving files between directories, but also for other
operations.  So to make it simple, I use a function to remove all file
numbers, and another to add them back.  The adding back first checks if it
goes above 1023.  Hence, files are kept with maximal DOS 2 compatibility in
MyDOS images.  This is different from regular MyDOS which will simply not
use file numbers for newly created files.

Bugs:

This has had only minimal testing.  When creating file systems, it doesn't
write the correct boot code, as the code doesn't embed the full boot
sectors.  If you want to make a bootable image, then before using a newly
created image, load it in an emulator, format it with the correct DOS, and
write the DOS files to the disk.  Then mount it again with atrfs.


SpartaDOS:

Not implemented yet

DOS XE:

Not implemented yet

Atari DOS 3:

Not implemented yet (low priority)

Atari DOS 4 (designed for 1450XLD):

Not implemented yet (very low priority)

There were may other DOS versions released by third parties that could be
implemented here.  Games that use their own formats could also be supported.
I think I saw a game that used DOS 2 hacked to move the directory sectors.
If we can detect the format, we can add support.
