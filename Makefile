CC = gcc

# CFLAGS = -I./ -g -Wall
CFLAGS=-I./ -g
LIBS=-DHTTPS -lpthread -lssl -lcrypto
# LIB = -lpthread

src=$(wildcard *.c)
obj=$(patsubst %.c, %.o, $(src))

all:twebs cgi

$(obj):%.o:%.c
	$(CC) -o $@ -c $< $(CFLAGS)

twebs:$(obj)
	$(CC) -o $@ $^ $(LIBS)

cgi:
	(cd cgi-bin; make)

.PHONY:clean
clean:
	-rm -rf $(obj) access.log
