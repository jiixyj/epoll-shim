#ifndef SHIM_EPOLL_SHIM_RUNTIME_H
#define SHIM_EPOLL_SHIM_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

int /* errno_t */ epoll_shim__start_realtime_step_detection(void);

#ifdef __cplusplus
}
#endif

#endif
