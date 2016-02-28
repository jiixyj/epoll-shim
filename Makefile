LIB=		epoll-shim
SHLIBDIR?=	
SRCS=		src/epoll.c src/timerfd.c src/signalfd.c src/common.c
INCS=		include/sys/epoll.h include/sys/timerfd.h include/sys/signalfd.h
MAN=

CFLAGS+=	-I${.CURDIR}/include -fPIC -Weverything -Wno-missing-prototypes -Wno-padded -Wno-missing-variable-declarations -Wno-thread-safety-analysis

.include <bsd.lib.mk>
