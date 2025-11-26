CC=gcc
CFLAGS=-Wall -Wextra -pthread -O2

all: controlador agente

controlador: controlador.c
	$(CC) $(CFLAGS) -o controlador controlador.c

agente: agente.c
	$(CC) $(CFLAGS) -o agente agente.c

clean:
	rm -f controlador agente

