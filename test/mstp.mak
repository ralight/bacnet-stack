#Makefile to build test case
CC      = gcc
SRC_DIR = ../src
INCLUDES = -I$(SRC_DIR) -I. -I../ports/linux
DEFINES = -DBIG_ENDIAN=0 -DTEST -DTEST_MSTP

CFLAGS  = -Wall $(INCLUDES) $(DEFINES) -g

SRCS = $(SRC_DIR)/bacnet/datalink/mstp.c \
	$(SRC_DIR)/bacnet/datalink/mstptext.c \
	$(SRC_DIR)/bacnet/indtext.c \
	$(SRC_DIR)/bacnet/datalink/crc.c \
	$(SRC_DIR)/bacnet/basic/sys/ringbuf.c \
	ctest.c

TARGET = mstp

all: ${TARGET}

OBJS = ${SRCS:.c=.o}

${TARGET}: ${OBJS}
	${CC} -o $@ ${OBJS}

.c.o:
	${CC} -c ${CFLAGS} $*.c -o $@

depend:
	rm -f .depend
	${CC} -MM ${CFLAGS} *.c >> .depend

clean:
	rm -rf core ${TARGET} $(OBJS)

include: .depend
