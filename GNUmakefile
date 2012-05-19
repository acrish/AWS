CC = gcc
CFLAGS = -Wall -DDEBUG
LDFLAGS = -laio

build: aws

aws: aws.o sock_util.o w_epoll.h http_parser.o mytests.o

aws.o: aws.c aws.h

sock_util.o: sock_util.c sock_util.h

http_parser.o: http_parser.c

mytests.o: mytests.c

clean:
	rm -f *.o aws
