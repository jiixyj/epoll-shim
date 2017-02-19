LIB=		epoll-shim
SHLIB_MAJOR=	0
SRCS=		src/epoll.c src/timerfd.c src/signalfd.c src/common.c
INCS=		include/sys/epoll.h include/sys/timerfd.h include/sys/signalfd.h
VERSION_MAP=	Version.map

LIBDIR=		/usr/local/lib
INCSDIR=	/usr/local/include/libepoll-shim/sys

CFLAGS+=	-I${.CURDIR}/include -pthread -Wall -Wextra -Wno-missing-prototypes -Wno-padded -Wno-missing-variable-declarations -Wno-thread-safety-analysis
LDFLAGS+=	-pthread -lrt

distrib-dirs:
	mkdir -p "${DESTDIR}/${LIBDIR}"
	mkdir -p "${DESTDIR}/${INCSDIR}"

.include <bsd.lib.mk>
