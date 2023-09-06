
HEADERS = atrfs.h
CFLAGS = -g -O0 -W -Wall # -g and -O0 for easier gdb debugging
OBJ = atrfs.o special.o info.o mydos.o sparta.o unknown.o
atrfs: $(OBJ)
	gcc -o $@ $(OBJ) -lfuse3

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o atrfs
