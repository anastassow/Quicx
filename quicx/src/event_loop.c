#include "event_loop.h"
#include <unistd.h>

#ifdef __APPLE__

int el_create(void) {
    return kqueue();
}

int el_add(int loop_fd, int fd, void* ptr) {
    struct kevent ev;
    EV_SET(&ev,
           fd,
           EVFILT_READ,
           EV_ADD,
           0, 0,
           ptr);
    return kevent(loop_fd, &ev, 1, NULL, 0, NULL);
}

int el_remove(int loop_fd, int fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    return kevent(loop_fd, &ev, 1, NULL, 0, NULL);
}

int el_wait(int loop_fd, ElEvent* events, int max, int timeout_ms) {
    struct kevent kevents[EL_MAX_EVENTS];

    struct timespec  ts;
    struct timespec* tsp = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec  =  timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        tsp        = &ts;
    }

    int n = kevent(loop_fd, NULL, 0, kevents, max, tsp);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++)
        events[i].ptr = kevents[i].udata;

    return n;
}

void el_destroy(int loop_fd) {
    close(loop_fd);
}

#else 

int el_create(void) {
    return epoll_create1(0);
}

int el_add(int loop_fd, int fd, void* ptr) {
    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = ptr;
    return epoll_ctl(loop_fd, EPOLL_CTL_ADD, fd, &ev);
}

int el_remove(int loop_fd, int fd) {
    return epoll_ctl(loop_fd, EPOLL_CTL_DEL, fd, NULL);
}

int el_wait(int loop_fd, ElEvent* events, int max, int timeout_ms) {
    struct epoll_event eevents[EL_MAX_EVENTS];

    int n = epoll_wait(loop_fd, eevents, max, timeout_ms);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++)
        events[i].ptr = eevents[i].data.ptr;

    return n;
}

void el_destroy(int loop_fd) {
    close(loop_fd);
}

#endif