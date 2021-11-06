#ifndef EPOLL_SHIM_DETAIL_READ_H_
#define EPOLL_SHIM_DETAIL_READ_H_

#include <unistd.h>

extern ssize_t epoll_shim_read(int, void *, size_t);
#define read(...) epoll_shim_read(__VA_ARGS__)

#endif
