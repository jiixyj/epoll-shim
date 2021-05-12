#ifndef SHIM_SYS_SHIM_HELPERS_WRITE
#define SHIM_SYS_SHIM_HELPERS_WRITE

#include <unistd.h> /* IWYU pragma: keep */

extern ssize_t epoll_shim_write(int, void const *, size_t);
#define write(fd, buf, nbytes) epoll_shim_write(fd, buf, nbytes)

#endif
