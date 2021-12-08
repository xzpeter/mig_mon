CFLAGS=-O3 -Wall -Werror
LDFLAGS=-lpthread

.PHONY: clean

default: mig_mon

mig_mon: mig_mon.o

clean:
	@rm -rf *.o mig_mon
