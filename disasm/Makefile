CFLAGS = -g -O0 -W -Wall # -g and -O0 for easier gdb debugging
OBJ = disasm.o
HEADERS =
LIBS =

disasm: $(OBJ)
	gcc -o $@ $(OBJ) $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o disasm
