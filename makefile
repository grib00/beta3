CC=gcc
CFLAGS=-std=gnu99 -Wall
LDLIBS=-ljack -lm

beta3: beta3.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f beta3 *.o

