CC    := cc
CFLAGS := -std=c11 -Wall -g -fPIC -fstack-protector -fstack-protector-all -pthread -Wno-unused-variable -Wno-unused-but-set-variable
LIB   := -lfuse -lpthread -lrt -lfskit -lfskit_fuse -lpstat
INC   := -I. 
C_SRCS:= $(wildcard *.c)
OBJ   := $(patsubst %.c,%.o,$(C_SRCS))
DEFS  := -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_FILE_OFFSET_BITS=64 -D_XOPEN_SOURCE=700

EVENTFS := eventfs

DESTDIR ?= 
PREFIX ?= 
BINDIR ?= $(DESTDIR)/$(PREFIX)/bin

all: eventfs

eventfs: $(OBJ)
	$(CC) $(CFLAGS) -o "$@" $(OBJ) $(LIBINC) $(LIB)

install: eventfs
	mkdir -p $(BINDIR)
	cp -a $(EVENTFS) $(BINDIR)

%.o : %.c
	$(CC) $(CFLAGS) -o "$@" $(INC) -c "$<" $(DEFS)

.PHONY: clean
clean:
	/bin/rm -f $(OBJ) $(EVENTFS)
