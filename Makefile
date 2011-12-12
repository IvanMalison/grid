CC = gcc
CFLAGS = -g

.PHONY: clean

all: server client

server: network.o server.o runner.o hash.o
	gcc -g -lpthread -o server network.o server.o runner.o hash.o

client: client.o network.o

network.o: network.c network.h constants.h

server.o: server.c network.h rpc.c server.h rpc.c constants.h

runner.o: runner.c runner.h constants.h server.h

client.o: client.c network.h constants.h

hash.o: hash.h hash.c

clean:
	rm *.o
	rm server