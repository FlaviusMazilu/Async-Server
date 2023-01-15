CFLAGS = -Wall -DDEBUG -DLOG_LEVEL=LOG_DEBUG
build: aws
aws: http_parser.o sock_util.o aws.o
	gcc http_parser.o sock_util.o aws.o -laio -o aws

http_parser.o: http_parser.c
	gcc -c http_parser.c $(CFLAGS) 

sock_util.o: sock_util.c
	gcc -c sock_util.c $(CFLAGS)

aws.o: aws.c
	gcc -c aws.c $(CFLAGS)

clean:
	rm -rf sock_util.o http_parser.o aws.o aws

cp:
	cp aws ../../checker-lin/
	cp aws ../../checker-lin/_test

run:
	./run.sh

all:
	gcc -c http_parser.c $(CFLAGS)
	gcc -c sock_util.c $(CFLAGS)
	gcc -c aws.c $(CFLAGS)
	gcc http_parser.o sock_util.o aws.o -laio -o aws
	cp aws ../../checker-lin/
	cp aws ../../checker-lin/_test
	./run.sh

zip:
	zip FlavisMaziluTema3SO.zip -r aws.c http_parser.c sock_util.c README.md Makefile *.h README.md