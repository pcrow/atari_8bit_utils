
CFLAGS = -g -O0 -W -Wall # -g and -O0 for easier gdb debugging

#
# This was written using the Fuse 3.x release.  This may be a
# problem for some users, particularly on Macs.
# I've gotten this to at least compile on older versions with
# just changing defines here and a bit of tweaking of the code.
#
# This is the sort of thing autoconf is designed to handle, but
# I'm not setting that up for this with just one issue.
#
use_modern_fuse=yes # Comment this out for older version
ifdef use_modern_fuse
CFLAGS += -DFUSE_USE_VERSION=30
CFLAGS += "-DFUSE_INCLUDE=<fuse3/fuse.h>"
LIBS = -lfuse3
else
CFLAGS += -DFUSE_USE_VERSION=25 # Probably works on older numbers; set as needed
CFLAGS += "-DFUSE_INCLUDE=<fuse.h>"
LIBS = -lfuse
endif

CFLAGS += -D_FILE_OFFSET_BITS=64 # Important for older FUSE versions
OBJ = atrfs.o generic.o common.o special.o info.o mydos.o sparta.o dosxe.o dos3.o dos4.o litedos.o apt.o unknown.o
HEADERS = atrfs.h
CFLAGS += -Wno-deprecated-declarations # MD5 is deprecated in OpenSSL 3.0
LIBS += -lcrypto
atrfs: $(OBJ)
	gcc -o $@ $(OBJ) $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o atrfs
