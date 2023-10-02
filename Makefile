CFLAGS := -Wall -O1 -g

.PHONY: default clean

default: dump tweak decode decode-amd


dump: LDLIBS := -lEGL -lGL
dump: dump.o minidc.o siphash.o

tweak: LDLIBS := -lEGL -lGL
tweak: tweak.o minidc.o siphash.o

dump.o: dump.c minidc.h
tweak.o: tweak.c minidc.h
minidc.o: minidc.c minidc.h siphash.h
siphash.o: siphash.c siphash.h

decode: decode.c

decode-amd: decode-amd.c


clean:
	-rm -f dump tweak decode decode-amd
	-rm -f dump.o tweak.o minidc.o siphash.o
