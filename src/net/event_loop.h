#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#ifdef __APPLE__
    #include <sys/event.h>
    #include <sys/time.h>
    #define EL_BACKEND "kqueue"
#else
    #include <sys/epoll.h>
    #define EL_BACKEND "epoll"
#endif

#define EL_MAX_EVENTS 64

/*
Structure for api abstraction 
(holds user information e.g the connection information)
*/
typedef struct {
    void* ptr;
} ElEvent;

/* API */

/* create event loop — returns loop fd, -1 on error */
int  el_create(void);

/*
add fd to loop
ptr is what you get back in ElEvent.ptr when fd is ready
*/
int  el_add(int loop_fd, int fd, void* ptr);

/* remove df from loop */
int  el_remove(int loop_fd, int fd);

/*
wait for events
timeout_ms: -1 = block forever, 0 = return immediately
returns number of ready events, -1 on error
*/
int  el_wait(int loop_fd, ElEvent* events, int max, int timeout_ms);

/* destroy event loop */
void el_destroy(int loop_fd);

#endif