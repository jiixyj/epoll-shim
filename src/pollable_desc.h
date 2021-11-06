#ifndef POLLABLE_DESC_H
#define POLLABLE_DESC_H

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include <poll.h>

struct pollable_desc_vtable;
typedef struct {
	void *ptr;
	struct pollable_desc_vtable const *vtable;
} PollableDesc;

typedef void (*pollable_desc_poll_t)(void *pollable_desc, uint32_t *revents);

struct pollable_desc_vtable {
	pollable_desc_poll_t poll_fun;
};

static inline void
pollable_desc_poll(PollableDesc pollable_desc, uint32_t *revents)
{
	assert(pollable_desc.ptr != NULL);
	assert(pollable_desc.vtable != NULL);
	assert(pollable_desc.vtable->poll_fun != NULL);

	pollable_desc.vtable->poll_fun(pollable_desc.ptr, revents);
}

#endif
