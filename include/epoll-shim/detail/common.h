#ifndef EPOLL_SHIM_DETAIL_COMMON_H_
#define EPOLL_SHIM_DETAIL_COMMON_H_

#include <fcntl.h>
#include <unistd.h>

extern int epoll_shim_close(int);
#define close(...) epoll_shim_close(__VA_ARGS__)

extern int epoll_shim_fcntl(int, int, ...);
#define fcntl(...) epoll_shim_fcntl(__VA_ARGS__)

#endif
