LIB=		epoll-shim
SHLIB_MAJOR=	0
SRCS=		src/epoll.c src/timerfd.c src/signalfd.c src/common.c
INCS=		include/sys/epoll.h include/sys/timerfd.h include/sys/signalfd.h
INCSDIR=	${INCLUDEDIR}/libepoll-shim/sys

CFLAGS+=	-I${.CURDIR}/include -Weverything -Wno-missing-prototypes -Wno-padded -Wno-missing-variable-declarations -Wno-thread-safety-analysis

distrib-dirs:
	mkdir -p "${DESTDIR}/${LIBDIR}"
	mkdir -p "${DESTDIR}/${INCSDIR}"

.include <bsd.lib.mk>
