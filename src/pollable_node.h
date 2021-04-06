#ifndef POLLABLE_NODE_H
#define POLLABLE_NODE_H

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include <poll.h>

struct pollable_node_vtable;
typedef struct {
	void *ptr;
	struct pollable_node_vtable const *vtable;
} PollableNode;

typedef void (*pollable_node_poll_t)(void *pollable_node, uint32_t *revents);

struct pollable_node_vtable {
	pollable_node_poll_t poll_fun;
};

static inline void
pollable_node_poll(PollableNode pollable_node, uint32_t *revents)
{
	assert(pollable_node.ptr != NULL);
	assert(pollable_node.vtable != NULL);
	assert(pollable_node.vtable->poll_fun != NULL);

	pollable_node.vtable->poll_fun(pollable_node.ptr, revents);
}

#endif
