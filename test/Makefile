CC      = gcc
CFLAGS  = -DHOST_TEST -I../main -Wall -Wextra -g

.PHONY: all clean run

all: cosmo_test

cosmo_test: cosmo_test.c ../main/cosmo.c ../main/cosmo.h
	$(CC) $(CFLAGS) cosmo_test.c ../main/cosmo.c -o cosmo_test

run: cosmo_test
	./cosmo_test

clean:
	rm -f cosmo_test
