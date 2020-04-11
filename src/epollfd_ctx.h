#ifndef EPOLLFD_CTX_H_
#define EPOLLFD_CTX_H_

#define SHIM_SYS_SHIM_HELPERS
#include <sys/epoll.h>

#include <sys/tree.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <poll.h>
#include <pthread.h>

struct registered_fds_node_;
typedef struct registered_fds_node_ RegisteredFDsNode;

typedef enum {
	EOF_STATE_READ_EOF = 0x01,
	EOF_STATE_WRITE_EOF = 0x02,
} EOFState;

typedef enum {
	NODE_TYPE_FIFO = 1,
	NODE_TYPE_SOCKET = 2,
	NODE_TYPE_KQUEUE = 3,
	NODE_TYPE_OTHER = 4,
	NODE_TYPE_POLL = 5,
} NodeType;

struct registered_fds_node_ {
	RB_ENTRY(registered_fds_node_) entry;
	RegisteredFDsNode *del_list;

	int fd;
	epoll_data_t data;

	bool is_registered;

	bool has_evfilt_read;
	bool has_evfilt_write;

	bool got_evfilt_read;
	bool got_evfilt_write;

	NodeType node_type;
	union {
		struct {
			bool is_nycss;
		} socket;
		struct {
			bool readable;
			bool writable;
		} fifo;
	} node_data;
	int eof_state;

	uint16_t events;
	uint32_t revents;

	bool is_edge_triggered;
	bool is_oneshot;
};

typedef RB_HEAD(registered_fds_set_, registered_fds_node_) RegisteredFDsSet;

typedef struct {
	int kq; // non owning
	pthread_mutex_t mutex;

	RegisteredFDsNode *poll_node;

	RegisteredFDsSet registered_fds;
	size_t registered_fds_size;

	struct kevent *kevs;
	size_t kevs_length;
} EpollFDCtx;

errno_t epollfd_ctx_init(EpollFDCtx *epollfd, int kq);
errno_t epollfd_ctx_terminate(EpollFDCtx *epollfd);

void epollfd_ctx_fill_pollfds(EpollFDCtx *epollfd, struct pollfd pfds[2]);

errno_t epollfd_ctx_ctl(EpollFDCtx *epollfd, int op, int fd2,
    struct epoll_event *ev);
errno_t epollfd_ctx_wait(EpollFDCtx *epollfd, struct epoll_event *ev, int cnt,
    int *actual_cnt);

#endif
