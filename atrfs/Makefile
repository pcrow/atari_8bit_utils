
HEADERS = atrfs.h
CFLAGS = -g -O2 -W -Wall
OBJ = atrfs.o special.o info.o mydos.o
atrfs: $(OBJ)
	gcc -o $@ $(OBJ) -lfuse3

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o atrfs
