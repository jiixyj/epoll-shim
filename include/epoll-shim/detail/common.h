#ifndef SHIM_SYS_SHIM_HELPERS
#define SHIM_SYS_SHIM_HELPERS

#include <fcntl.h>  /* IWYU pragma: keep */
#include <unistd.h> /* IWYU pragma: keep */

extern int epoll_shim_close(int);
#define close epoll_shim_close

extern int epoll_shim_fcntl(int, int, ...);
#define SHIM_SYS_SHIM_HELPERS_SEL(PREFIX, _2, _1, SUFFIX, ...) PREFIX##_##SUFFIX
#define SHIM_SYS_SHIM_FCNTL_1(fd, cmd) fcntl((fd), (cmd))
#define SHIM_SYS_SHIM_FCNTL_N(fd, cmd, ...)                                \
	(((cmd) == F_SETFL) ? epoll_shim_fcntl((fd), (cmd), __VA_ARGS__) : \
				    fcntl((fd), (cmd), __VA_ARGS__))
#define fcntl(fd, ...)                                                       \
	SHIM_SYS_SHIM_HELPERS_SEL(SHIM_SYS_SHIM_FCNTL, __VA_ARGS__, N, 1, 1) \
	(fd, __VA_ARGS__)

#endif
