CFLAGS=-O3 -g -Wall -Werror
LDFLAGS=-lpthread

.PHONY: clean cscope

default: mig_mon cscope

cscope:
	@cscope -bq *.c

mig_mon: mig_mon.o

clean:
	@rm -rf *.o mig_mon cscope*
