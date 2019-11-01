#ifndef SHIM_SYS_EVENTFD_H
#define SHIM_SYS_EVENTFD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <fcntl.h>

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK

int eventfd(unsigned int, int);
int eventfd_read(int, eventfd_t *);
int eventfd_write(int, eventfd_t);


#ifndef SHIM_SYS_SHIM_HELPERS
#define SHIM_SYS_SHIM_HELPERS
#include <unistd.h> /* IWYU pragma: keep */

extern int epoll_shim_close(int);
#define close epoll_shim_close
#endif

#ifndef SHIM_SYS_SHIM_HELPERS_READ
#define SHIM_SYS_SHIM_HELPERS_READ
extern ssize_t epoll_shim_read(int, void *, size_t);
#define read epoll_shim_read
#endif

#ifndef SHIM_SYS_SHIM_HELPERS_WRITE
#define SHIM_SYS_SHIM_HELPERS_WRITE
extern ssize_t epoll_shim_write(int, void const*, size_t);
#define write epoll_shim_write
#endif


#ifdef __cplusplus
}
#endif

#endif /* sys/eventfd.h */
