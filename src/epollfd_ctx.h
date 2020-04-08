#ifndef EPOLLFD_CTX_H_
#define EPOLLFD_CTX_H_

#define SHIM_SYS_SHIM_HELPERS
#include <sys/epoll.h>

#include <sys/tree.h>

#include <errno.h>
#include <stdint.h>

#include <poll.h>
#include <pthread.h>

struct registered_fds_node_;
typedef struct registered_fds_node_ RegisteredFDsNode;

typedef enum {
	EOF_STATE_READ_EOF = 0x01,
	EOF_STATE_WRITE_EOF = 0x02,
} EOFState;

struct registered_fds_node_ {
	RB_ENTRY(registered_fds_node_) entry;
	int fd;
	uint16_t flags;
	int eof_state;
	epoll_data_t data;
};

typedef RB_HEAD(registered_fds_set_, registered_fds_node_) RegisteredFDsSet;

RegisteredFDsNode *registered_fds_node_create(int fd, struct epoll_event *ev);
void registered_fds_node_destroy(RegisteredFDsNode *node);

typedef struct {
	int kq; // non owning
	pthread_mutex_t mutex;

	struct pollfd pfds[2];
	epoll_data_t pollfd_data;

	RegisteredFDsSet registered_fds;
} EpollFDCtx;

errno_t epollfd_ctx_init(EpollFDCtx *epollfd, int kq);
errno_t epollfd_ctx_terminate(EpollFDCtx *epollfd);

errno_t epollfd_ctx_ctl(EpollFDCtx *epollfd, int op, int fd2,
    struct epoll_event *ev);
errno_t epollfd_ctx_wait(EpollFDCtx *epollfd, struct epoll_event *ev, int cnt,
    int *actual_cnt);

#endif
