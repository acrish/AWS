CC = gcc
CFLAGS = -Wall -DDEBUG
LDFLAGS = -laio

build: aserver

aserver: aserver.o sock_util.o w_epoll.h http_parser.o

aserver.o: aserver.c aws.h

sock_util.o: sock_util.c sock_util.h

http_parser.o: http_parser.c

clean:
	rm -f *.o aserver
