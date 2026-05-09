##
## http://twitter.com/maximeb
##

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags libpng)
LDLIBS  += $(shell pkg-config --libs libpng) -lm

TARGET  = png2vtf
SRC     = png2vtf.c
HEADERS = vtf_format.h

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET) *.o *~ \#*

.PHONY: all clean
