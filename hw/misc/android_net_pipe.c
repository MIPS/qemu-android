/* Copyright (C) 2014 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** Description
**
** Implementation of 'tcp' and 'unix' Android pipes. These can be used
** to connect a guest process directly with a host TCP or Unix socket.
**
** For TCP, connections are limited to localhost (127.0.0.1) ports for
** security reasons (doing otherwise might allow any application to
** sneakily connect to the Internet when running under emulation).
**
** This is also used by the 'opengles' Android pipe service to send
** wire protocol data to the GPU emulation libraries.
*/

#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "hw/misc/android_pipe.h"
#include "hw/misc/android_opengles.h"

#include <errno.h>
//#include <sys/select.h>

/* Set to 1 or 2 for debug traces */
#define DEBUG 1

#if DEBUG >= 1
#  define D(...)   printf(__VA_ARGS__), printf("\n")
#else
#  define D(...)   ((void)0)
#endif

#if DEBUG >= 2
#  define DD(...)                       printf(__VA_ARGS__), printf("\n")
#  define DDASSERT(cond)                _ANDROID_ASSERT(cond, "Assertion failure: ", #cond)
#  define DDASSERT_INT_OP(cond,val,op)  _ANDROID_ASSERT_INT_OP(cond,val,op)
#else
#  define DD(...)                       ((void)0)
#  define DDASSERT(cond)                ((void)0)
#  define DDASSERT_INT_OP(cond,val,op)  ((void)0)
#endif

#define DDASSERT_INT_LT(cond,val)  DDASSERT_INT_OP(cond,val,<)
#define DDASSERT_INT_LTE(cond,val)  DDASSERT_INT_OP(cond,val,<=)
#define DDASSERT_INT_GT(cond,val)  DDASSERT_INT_OP(cond,val,>)
#define DDASSERT_INT_GTE(cond,val)  DDASSERT_INT_OP(cond,val,>=)
#define DDASSERT_INT_EQ(cond,val)  DDASSERT_INT_OP(cond,val,==)
#define DDASSERT_INT_NEQ(cond,val)  DDASSERT_INT_OP(cond,val,!=)

enum {
    STATE_INIT,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_CLOSING_GUEST,
    STATE_CLOSING_SOCKET
};

typedef struct {
    void *hwpipe;
    int state;
    int wakeWanted;
    int wakeActual;
    int fd;
} NetPipe;

/* Free a network pipe */
static void net_pipe_free(NetPipe *pipe)
{
    /* Close the socket */
    int fd = pipe->fd;
    if (fd >= 0) {
        qemu_set_fd_handler(fd, NULL, NULL, NULL);
        closesocket(fd);
        pipe->fd = -1;
    }
    g_free(pipe);
}

static void net_pipe_read_handler(void* opaque);
static void net_pipe_write_handler(void* opaque);

static void net_pipe_reset_state(NetPipe *pipe)
{
    IOHandler *read_handler = NULL;
    IOHandler *write_handler = NULL;

    if ((pipe->wakeWanted & PIPE_WAKE_WRITE) != 0) {
        write_handler = net_pipe_write_handler;
    }

    if (pipe->state == STATE_CONNECTED &&
        (pipe->wakeWanted & PIPE_WAKE_READ) != 0) {
       read_handler = net_pipe_read_handler;
    }
    qemu_set_fd_handler(pipe->fd, read_handler, write_handler, pipe);
}

static void net_pipe_close_from_socket(void *opaque)
{
    NetPipe *pipe = opaque;

    /* If the guest already ordered the pipe to be closed,
     * delete immediately */
    if (pipe->state == STATE_CLOSING_GUEST) {
        net_pipe_free(pipe);
        return;
    }

    /* Force the closure of the pipe channel - if a guest is blocked
     * waiting for a wake signal, it will receive an error. */
    if (pipe->hwpipe != NULL) {
        android_pipe_close(pipe->hwpipe);
        pipe->hwpipe = NULL;
    }

    pipe->state = STATE_CLOSING_SOCKET;
    net_pipe_reset_state(pipe);
}

/* Called when data arrives on the pipe's socket. */
static void net_pipe_read_handler(void* opaque)
{
    NetPipe *pipe = opaque;

    pipe->wakeActual |= PIPE_WAKE_READ;
    if ((pipe->wakeWanted & PIPE_WAKE_READ) != 0) {
        android_pipe_wake(pipe->hwpipe, pipe->wakeActual);
        pipe->wakeWanted &= ~PIPE_WAKE_READ;
    }
    net_pipe_reset_state(pipe);
}

/* Called when the pipe's file socket becomes writable. */
static void net_pipe_write_handler(void* opaque)
{
    NetPipe *pipe = opaque;

    pipe->wakeActual |= PIPE_WAKE_WRITE;
    if ((pipe->wakeWanted & PIPE_WAKE_WRITE) != 0) {
        android_pipe_wake(pipe->hwpipe, pipe->wakeActual);
        pipe->wakeWanted &= ~PIPE_WAKE_WRITE;
    }
    net_pipe_reset_state(pipe);
}

static void net_pipe_close_from_guest(void *opaque)
{
    NetPipe *pipe = opaque;
    net_pipe_free(pipe);
}

static int net_pipe_ready_send(NetPipe *pipe)
{
    if (pipe->state == STATE_CONNECTED) {
        return 0;
    } else if (pipe->state == STATE_CONNECTING) {
        return PIPE_ERROR_AGAIN;
    } else if (pipe->hwpipe == NULL) {
        return PIPE_ERROR_INVAL;
    } else {
        return PIPE_ERROR_IO;
    }
}

static int net_pipe_send_buffers(void* opaque,
                                 const AndroidPipeBuffer *buffers,
                                 int num_buffers)
{
    NetPipe *pipe = opaque;

    int ret = net_pipe_ready_send(pipe);
    if (ret != 0) {
        return ret;
    }

    const AndroidPipeBuffer* buff = buffers;
    const AndroidPipeBuffer* buff_end = buff + num_buffers;
    int count = 0;
    for (; buff < buff_end; ++buff) {
        count += buff->size;
    }

    buff = buffers;

    int buff_start = 0;
    ret = 0;
    while (count > 0) {
        int avail = buff->size - buff_start;
        ssize_t len = send(pipe->fd, (const void*)(buff->data + buff_start), avail, 0);

        if (len > 0) {
            /* the write succeeded */
            buff_start += len;
            if (buff_start >= buff->size) {
                buff++;
                buff_start = 0;
            }
            count -= (int)len;
            ret += (int)len;
            continue;
        }
        if (len == 0) {
            /* reached the end of stream? */
            if (ret == 0) {
                ret = PIPE_ERROR_IO;
            }
            break;
        }
        if (ret > 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ret = PIPE_ERROR_AGAIN;
        } else {
            ret = PIPE_ERROR_IO;
        }
        break;
    }
    return ret;
}

static int net_pipe_recv_buffers(void *opaque,
                                 AndroidPipeBuffer *buffers,
                                 int num_buffers)
{
    NetPipe *pipe = opaque;

    AndroidPipeBuffer* buff = buffers;
    AndroidPipeBuffer* buff_end = buff + num_buffers;

    int count = 0;
    for (; buff < buff_end; ++buff) {
        count += buff->size;
    }

    buff = buffers;

    int ret = 0;
    int buff_start = 0;
    while (count > 0) {
        int avail = buff->size - buff_start;
        ssize_t len = recv(pipe->fd, (void*)(buff->data + buff_start), avail, 0);

        if (len > 0) {
            /* the read succeeded. */
            buff_start += len;
            if (buff_start >= buff->size) {
                buff++;
                buff_start = 0;
            }
            count -= (int) len;
            ret += (int) len;
            continue;
        }

        if (len == 0) {
            /* reached the end of stream? */
            if (ret == 0)
                ret = PIPE_ERROR_IO;
            break;
        }

        if (ret > 0) {
            /* if data was already read, just return it. */
            break;
        }

        /* need to return an appropriate error code. */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ret = PIPE_ERROR_AGAIN;
        } else {
            ret = PIPE_ERROR_IO;
        }
        break;
    }
    return ret;
}

static unsigned net_pipe_poll(void *opaque)
{
    NetPipe *pipe = opaque;
#if 1
    return pipe->wakeActual;
#else
    unsigned result = 0;

    do {
        fd_set read_fds, write_fds;
        struct timeval tv = {0, 0};

        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(fd, &read_fds);
        FD_SET(fd, &write_fds);
        ret = select(fd + 1, &read_fds, &write_fds, NULL, &tv);
    } while (ret < 0 && errno == EINTR);

    if (ret == 1) {
        if (FD_ISSET(fd, &read_fds)) {
            result |= PIPE_WAKE_READ;
        }
        if (FD_ISSET(fd, &write_fds)) {
            result |= PIPE_WAKE_WRITE;
        }
    }
#endif
    return 0;
}

static void net_pipe_wake_on(void *opaque, int flags)
{
    NetPipe *pipe = opaque;

    DD("%s: flags=%d", __FUNCTION__, flags);

    pipe->wakeWanted |= flags;
    pipe->wakeActual &= ~flags;
    net_pipe_reset_state(pipe);
}

/* Called when the pipe finished connecting to its target. |fd| will be -1
 * to indicate that the connection failed. */
static void net_pipe_connect_handler(int fd, void* opaque)
{
    NetPipe *pipe = opaque;

    if (fd < 0) {
        net_pipe_close_from_socket(pipe);
        return;
    }

    pipe->state = STATE_CONNECTED;
    net_pipe_reset_state(pipe);
}

static void *net_pipe_init_from(void *hwpipe,
                                const char* args,
                                bool is_unix)
{
    if (args == NULL || args[0] == '\0') {
        D("%s: Missing address!", __FUNCTION__);
        return NULL;
    }

    NetPipe *pipe = (NetPipe *)g_new0(NetPipe, 1);
    pipe->hwpipe = hwpipe;
    pipe->state = STATE_CONNECTING;

    Error *err = NULL;

    if (is_unix) {
        D("%s: Unix path is '%s'", __FUNCTION__, args);
        pipe->fd = unix_nonblocking_connect(args,
                                            net_pipe_connect_handler,
                                            pipe,
                                            &err);
        if (pipe->fd < 0) {
            D("%s: Could not connect pipe to %s: %s\n",
            __FUNCTION__, args, error_get_pretty(err));
        }
    } else {
        D("%s: TCP port is '%s'", __FUNCTION__, args);

        char address[64];
        snprintf(address, sizeof(address), "127.0.0.1:%s", args);

        pipe->fd = inet_nonblocking_connect(address,
                                            net_pipe_connect_handler,
                                            pipe,
                                            &err);
        if (pipe->fd < 0) {
            D("%s: Could not connect pipe to %s: %s\n",
            __FUNCTION__, address, error_get_pretty(err));
        }
    }

    if (pipe->fd < 0) {
        error_free(err);
        g_free(pipe);
        return NULL;
    }

    return pipe;
}

static void *net_pipe_init_tcp(void *hwpipe, void *opaque, const char *args)
{
    return net_pipe_init_from(hwpipe, args, false);
}

#ifndef _WIN32
static void *net_pipe_init_unix(void *hwpipe, void *opaque, const char *args)
{
    return net_pipe_init_from(hwpipe, args, true);
}
#endif

static const AndroidPipeFuncs net_pipe_tcp_funcs = {
    net_pipe_init_tcp,
    net_pipe_close_from_guest,
    net_pipe_send_buffers,
    net_pipe_recv_buffers,
    net_pipe_poll,
    net_pipe_wake_on,
    NULL,  /* we can't save these */
    NULL,  /* we can't load these */
};

#ifndef _WIN32
static const AndroidPipeFuncs  net_pipe_unix_funcs = {
    net_pipe_init_unix,
    net_pipe_close_from_guest,
    net_pipe_send_buffers,
    net_pipe_recv_buffers,
    net_pipe_poll,
    net_pipe_wake_on,
    NULL,  /* we can't save these */
    NULL,  /* we can't load these */
};
#endif

static void *opengles_pipe_init(void *hwpipe,
                                void *opaque,
                                const char *args)
{
    char server_addr[PATH_MAX];
    android_gles_server_path(server_addr, sizeof(server_addr));
    NetPipe *pipe;
#ifndef _WIN32
    // Use a Unix pipe on Posix systems.
    pipe = (NetPipe *)net_pipe_init_unix(hwpipe, opaque, server_addr);
#else
    // Use a TCP pipe on Windows.
    pipe = (NetPipe *)net_pipe_init_tcp(hwpipe, opaque, server_addr);
#endif
    if (!pipe) {
        return NULL;
    }

    // Disable TCP nagle algorithm to improve through put of small
    // packets.
    socket_set_nodelay(pipe->fd);

#ifdef _WIN32
    // On Windows, adjust buffer sizes.
    int sndbuf = 128 * 1024;
    int len = sizeof(sndbuf);
    if (setsockopt(pipe->fd,
                    SOL_SOCKET,
                    SO_SNDBUF,
                    (char *)&sndbuf,
                    (int) sizeof(sndbuf)) == SOCKET_ERROR) {
        D("Failed to set SO_SNDBUF to %d error=0x%x\n",
            sndbuf, WSAGetLastError());
    }
#endif
    return pipe;
}

static const AndroidPipeFuncs opengles_pipe_funcs = {
    opengles_pipe_init,
    net_pipe_close_from_guest,
    net_pipe_send_buffers,
    net_pipe_recv_buffers,
    net_pipe_poll,
    net_pipe_wake_on,
    NULL,  /* we can't save these */
    NULL,  /* we can't save these */
};

void
android_net_pipe_init(void)
{
    android_pipe_add_type("tcp", NULL, &net_pipe_tcp_funcs);
#ifndef _WIN32
    android_pipe_add_type("unix", NULL, &net_pipe_unix_funcs);
#endif
    android_pipe_add_type("opengles", NULL, &opengles_pipe_funcs);
}

