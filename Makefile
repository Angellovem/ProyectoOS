CC=gcc
CFLAGS=-Wall -Wextra -pthread -O2

all: controlador

controlador: controlador.c
	$(CC) $(CFLAGS) -o controlador controlador.c

clean:
	rm -f controlador


