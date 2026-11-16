CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
LDLIBS  += -lncurses
PREFIX  ?= /usr/local
DESTDIR ?=
MKDIR   ?= mkdir -p

PROG    = nixsk
BUILDDIR = build
BIN     = ${BUILDDIR}/${PROG}
SRCS    = src/nixsk.c

all: ${BIN}

${BIN}: ${SRCS}
	${MKDIR} ${BUILDDIR}
	${CC} ${CFLAGS} -o ${BIN} ${SRCS} ${LDLIBS}

install: ${BIN}
	${MKDIR} ${DESTDIR}${PREFIX}/bin
	install -m 755 ${BIN} ${DESTDIR}${PREFIX}/bin/${PROG}

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/${PROG}

clean:
	rm -rf ${BUILDDIR}

.PHONY: all install uninstall clean
