CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG -I. -I.. -I../.. -laio
CFLAGS = -Wall -g

.PHONY: all clean

build: all

all: aws

aws: ../../http-parser/http_parser.o aws.o ../sock_util.o

aws.o: aws.c ../sock_util.h ../../debug.h ../../util.h

../sock_util.o: ../sock_util.c ../sock_util.h ../../debug.h ../../util.h

../../http-parser/http_parser.o: ../../http-parser/http_parser.c ../../http-parser/http_parser.h
	make -C ../../http-parser/ http_parser.o

clean:
	-rm -f *~
	-rm -f *.o
	-rm -f ../sock_util.o
	-rm -f ../../http-parser/http_parser.o
	-rm -f aws

run_baby:
	gcc aws.c -c aws.o -I../.. -I. -I..
	gcc aws.o ../sock_util.o ../../http-parser/http_parser.o -laio -o aws
	
cp:
	cp aws ../../../checker-lin
	cp aws ../../../checker-lin/_test

check:
	./run.sh