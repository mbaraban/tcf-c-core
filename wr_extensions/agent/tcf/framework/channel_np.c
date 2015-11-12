/*******************************************************************************
 * Copyright (c) 2007, 2014 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *     Michael Sills-Lavoie(École Polytechnique de Montréal)  - ZeroCopy support
 *              *                         *            - tcp_splice_block_stream
 *******************************************************************************/

/*
 * Implements input and output stream over TCP/IP transport.
 */

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#include <tcf/config.h>

#if ENABLE_WebSocket

#include <fcntl.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <tcf/framework/mdep-threads.h>
#include <tcf/framework/mdep-fs.h>
#include <tcf/framework/mdep-inet.h>
#include <tcf/framework/tcf.h>
#include <tcf/framework/channel.h>
#include <tcf/framework/channel_np.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/protocol.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/events.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/json.h>
#include <tcf/framework/peer.h>
#include <tcf/framework/ip_ifc.h>
#include <tcf/framework/asyncreq.h>
#include <tcf/framework/inputbuf.h>
#include <tcf/framework/outputbuf.h>
#include <tcf/services/discovery.h>
#define NOPOLL_APIS_ONLY
#include <nopoll.h>

#ifdef WIN32
#ifndef ECONNREFUSED
#define ECONNREFUSED    WSAECONNREFUSED
#endif
#endif


#ifndef MSG_MORE
#define MSG_MORE 0
#endif

#define BUF_SIZE OUTPUT_QUEUE_BUF_SIZE
#define CHANNEL_MAGIC 0x27208956
#define MAX_IFC 10
#if !defined(ENABLE_OutputQueue)
#  if ENABLE_ContextProxy || defined(_WIN32) || defined(__CYGWIN__) || defined(__linux__)
#    define ENABLE_OutputQueue 1
#  else
#    define ENABLE_OutputQueue 0
#  endif
#endif

#if ENABLE_WebSocket_SOCKS_V5
static char * socks_v5_host = NULL;
static char * socks_v5_port = NULL;
#endif


typedef struct ChannelNP ChannelNP;

struct ChannelNP {
    Channel chan;           /* Public channel information - must be first */
    int magic;              /* Magic number */
    NOPOLL_SOCKET socket;   /* Socket file descriptor */
    noPollConn * np_socket;
    int lock_cnt;           /* Stream lock count, when > 0 channel cannot be deleted */
    int read_pending;       /* Read request is pending */
    unsigned char * read_buf;
    size_t read_buf_size;
    int read_done;

#if ENABLE_Splice
    int pipefd[2];          /* Pipe used to splice data between a fd and the channel */
#endif /* ENABLE_Splice */

    /* Input stream buffer */
    InputBuf ibuf;

    /* Output stream state */
    unsigned char * out_bin_block;
    OutputBuffer * obuf;
    int out_errno;
    int out_flush_cnt;      /* Number of posted lazy flush events */
    int out_eom_cnt;        /* Number of end-of-message markers in the output buffer */
#if ENABLE_OutputQueue
    OutputQueue out_queue;
    AsyncReqInfo wr_req;
#endif /* ENABLE_OutputQueue */

    /* Async read request */
    AsyncReqInfo rd_req;
    int is_ssl;
};

typedef struct ServerNP ServerNP;

struct ServerNP {
    ChannelServer serv;
    int sock;
    LINK servlink;
    AsyncReqInfo accreq;
    noPollCtx  * np_ctx;
    noPollConn * np_listener;
    noPollConn * np_sock;
    int is_ssl;
};

typedef struct NpIOReq NpIOReq;
struct NpIOReq {
    noPollConn * np_sock;
    void * bufp;
    size_t bufsz;
    char * error_msg;
};

#define channel2np(A)  ((ChannelNP *)((char *)(A) - offsetof(ChannelNP, chan)))
#define inp2channel(A)  ((Channel *)((char *)(A) - offsetof(Channel, inp)))
#define out2channel(A)  ((Channel *)((char *)(A) - offsetof(Channel, out)))
#define server2np(A)   ((ServerNP *)((char *)(A) - offsetof(ServerNP, serv)))
#define servlink2np(A) ((ServerNP *)((char *)(A) - offsetof(ServerNP, servlink)))
#define ibuf2np(A)     ((ChannelNP *)((char *)(A) - offsetof(ChannelNP, ibuf)))
#define obuf2np(A)     ((ChannelNP *)((char *)(A) - offsetof(ChannelNP, out_queue)))

static LINK server_list;
static void np_channel_read_done(void * x);
static void handle_channel_msg(void * x);

static void delete_channel(ChannelNP * c) {
    trace(LOG_PROTOCOL, "Deleting channel %#lx", c);
    assert(c->lock_cnt == 0);
    assert(c->out_flush_cnt == 0);
    assert(c->magic == CHANNEL_MAGIC);
    assert(c->read_pending == 0);
    assert(c->ibuf.handling_msg != HandleMsgTriggered);
    channel_clear_broadcast_group(&c->chan);
    if (c->np_socket >= 0) {
        nopoll_conn_close(c->np_socket);
        c->np_socket = NULL;
    }
    list_remove(&c->chan.chanlink);
    if (list_is_empty(&channel_root) && list_is_empty(&channel_server_root))
        shutdown_set_stopped(&channel_shutdown);
    c->magic = 0;
#if ENABLE_OutputQueue
    output_queue_clear(&c->out_queue);
#endif /* ENABLE_OutputQueue */
#if ENABLE_Splice
    close(c->pipefd[0]);
    close(c->pipefd[1]);
#endif /* ENABLE_Splice */
    output_queue_free_obuf(c->obuf);
    loc_free(c->ibuf.buf);
    loc_free(c->chan.peer_name);
    loc_free(c);
}

static void np_lock(Channel * channel) {
    ChannelNP * c = channel2np(channel);
    assert(is_dispatch_thread());
    assert(c->magic == CHANNEL_MAGIC);
    c->lock_cnt++;
}

static void np_unlock(Channel * channel) {
    ChannelNP * c = channel2np(channel);
    assert(is_dispatch_thread());
    assert(c->magic == CHANNEL_MAGIC);
    assert(c->lock_cnt > 0);
    c->lock_cnt--;
    if (c->lock_cnt == 0) {
        assert(!c->read_pending);
        delete_channel(c);
    }
}

static int np_is_closed(Channel * channel) {
    ChannelNP * c = channel2np(channel);
    assert(is_dispatch_thread());
    assert(c->magic == CHANNEL_MAGIC);
    assert(c->lock_cnt > 0);
    return c->chan.state == ChannelStateDisconnected;
}

#if ENABLE_OutputQueue
static void done_write_request(void * args) {
    ChannelNP * c = (ChannelNP *)((AsyncReqInfo *)args)->client_data;
    int size = 0;
    int error = 0;

    assert(args == &c->wr_req);
    assert(c->socket != NOPOLL_INVALID_SOCKET);
    loc_free(((AsyncReqInfo *)args)->u.user.data);

    if (c->wr_req.u.user.rval < 0) error = c->wr_req.error;
    size = c->wr_req.u.user.rval;
    output_queue_done(&c->out_queue, error, size);
    if (error) c->out_errno = error;
    if (output_queue_is_empty(&c->out_queue) &&
        c->chan.state == ChannelStateDisconnected) nopoll_conn_shutdown(c->np_socket);
    np_unlock(&c->chan);
    trace(LOG_PROTOCOL, "done_write_request");
}

static int np_wt_write_request (void * args) {
    NpIOReq * req = (NpIOReq *)args;
    int rval;
    if(!nopoll_conn_is_ok(req->np_sock)) rval = -1;
    else {
        trace(LOG_PROTOCOL, "write request size:%d",req->bufsz);
        rval = nopoll_conn_send_binary(req->np_sock, req->bufp, req->bufsz);
        nopoll_conn_flush_writes (req->np_sock, 10, 0);
/*        assert(nopoll_conn_pending_write_bytes(req->np_sock) == 0);*/
    }
    if (rval < 0) errno = ERR_OTHER;
    return rval;
}

static void post_write_request(OutputBuffer * bf) {
    ChannelNP * c = obuf2np(bf->queue);

    assert(c->socket != NOPOLL_INVALID_SOCKET);

    c->wr_req.client_data = c;
    c->wr_req.done = done_write_request;
    {
        NpIOReq * req = loc_alloc( sizeof(NpIOReq));
        req->bufp = bf->buf + bf->buf_pos;
        req->bufsz = bf->buf_len - bf->buf_pos;
        req->np_sock = c->np_socket;
        c->wr_req.type = AsyncReqUser;
        c->wr_req.u.user.data = req;
        c->wr_req.u.user.func = np_wt_write_request;
        async_req_post(&c->wr_req);
    }
    np_lock(&c->chan);
}
#endif /* ENABLE_OutputQueue */

static void np_flush_with_flags(ChannelNP * c, int flags) {
    unsigned char * p = c->obuf->buf;
    assert(is_dispatch_thread());
    assert(c->magic == CHANNEL_MAGIC);
    assert(c->chan.out.end == p + sizeof(c->obuf->buf));
    assert(c->out_bin_block == NULL);
    assert(c->chan.out.cur >= p);
    assert(c->chan.out.cur <= p + sizeof(c->obuf->buf));
    if (c->chan.out.cur == p) return;
    if (c->chan.state != ChannelStateDisconnected && c->out_errno == 0) {
#if ENABLE_OutputQueue
        c->obuf->buf_len = c->chan.out.cur - p;
        c->out_queue.post_io_request = post_write_request;
        trace(LOG_PROTOCOL, "Outbuf add size:%d",c->obuf->buf_len);

        output_queue_add_obuf(&c->out_queue, c->obuf);
        c->obuf = output_queue_alloc_obuf();
        c->chan.out.end = c->obuf->buf + sizeof(c->obuf->buf);
#else
        while (p < c->chan.out.cur) {
            int rval = 0;
            size_t sz = c->chan.out.cur - p;

            if (!nopoll_conn_is_ok(c->np_socket)) rval = -1;
            if (rval >= 0)
                {
                rval = nopoll_conn_send_binary(c->np_socket, (const char *)p, sz);
                nopoll_conn_flush_writes (c->np_socket, 10, 0);
/*        assert(nopoll_conn_pending_write_bytes(c->np_socket) == 0);*/
                }
            if (rval < 0) errno = ERR_OTHER;
            if (rval < 0) {
                int err = errno;
                trace(LOG_PROTOCOL, "Can't send() on channel %#lx: %s", c, errno_to_str(err));
                c->out_errno = err;
                c->chan.out.cur = c->obuf->buf;
                c->out_eom_cnt = 0;
                return;
            }
            p += rval;
        }
        assert(p == c->chan.out.cur);
#endif
    }
    c->chan.out.cur = c->obuf->buf;
    c->out_eom_cnt = 0;
}

static void np_flush_event(void * x) {
    ChannelNP * c = (ChannelNP *)x;
    assert(c->magic == CHANNEL_MAGIC);
    if (--c->out_flush_cnt == 0) {
        int congestion_level = c->chan.congestion_level;
        if (congestion_level > 0) usleep(congestion_level * 2500);
        np_flush_with_flags(c, 0);
        np_unlock(&c->chan);
    }
    else if (c->out_eom_cnt > 3) {
        np_flush_with_flags(c, 0);
    }
}

static void np_bin_block_start(ChannelNP * c) {
    *c->chan.out.cur++ = ESC;
    *c->chan.out.cur++ = 3;
#if BUF_SIZE > 0x4000
    *c->chan.out.cur++ = 0;
#endif
    *c->chan.out.cur++ = 0;
    *c->chan.out.cur++ = 0;
    c->out_bin_block = c->chan.out.cur;
}

static void np_bin_block_end(ChannelNP * c) {
    size_t len = c->chan.out.cur - c->out_bin_block;
    if (len == 0) {
#if BUF_SIZE > 0x4000
        c->chan.out.cur -= 5;
#else
        c->chan.out.cur -= 4;
#endif
    }
    else {
#if BUF_SIZE > 0x4000
        *(c->out_bin_block - 3) = (len & 0x7fu) | 0x80u;
        *(c->out_bin_block - 2) = ((len >> 7) & 0x7fu) | 0x80u;
        *(c->out_bin_block - 1) = (unsigned char)(len >> 14);
#else
        *(c->out_bin_block - 2) = (len & 0x7fu) | 0x80u;
        *(c->out_bin_block - 1) = (unsigned char)(len >> 7);
#endif
    }
    c->out_bin_block = NULL;
}

static void np_write_stream(OutputStream * out, int byte) {
    ChannelNP * c = channel2np(out2channel(out));
    assert(c->magic == CHANNEL_MAGIC);
    if (!c->chan.out.supports_zero_copy || c->chan.out.cur >= c->chan.out.end - 32 || byte < 0) {
        if (c->out_bin_block != NULL) np_bin_block_end(c);
        if (c->chan.out.cur == c->chan.out.end) np_flush_with_flags(c, MSG_MORE);
        if (byte < 0 || byte == ESC) {
            char esc = 0;
            *c->chan.out.cur++ = ESC;
            if (byte == ESC) esc = 0;
            else if (byte == MARKER_EOM) esc = 1;
            else if (byte == MARKER_EOS) esc = 2;
            else assert(0);
            if (c->chan.out.cur == c->chan.out.end) np_flush_with_flags(c, MSG_MORE);
            *c->chan.out.cur++ = esc;
            if (byte == MARKER_EOM) {
                c->out_eom_cnt++;
                if (c->out_flush_cnt < 2) {
                    if (c->out_flush_cnt++ == 0) np_lock(&c->chan);
                    post_event_with_delay(np_flush_event, c, 0);
                }
            }
            return;
        }
    }
    else if (c->out_bin_block == NULL) {
        np_bin_block_start(c);
    }
    *c->chan.out.cur++ = (char)byte;
}

static void np_write_block_stream(OutputStream * out, const char * bytes, size_t size) {
    unsigned char * src = (unsigned char *)bytes;
    ChannelNP * c = channel2np(out2channel(out));
    while (size > 0) {
        size_t n = out->end - out->cur;
        if (n > size) n = size;
        if (n == 0) {
            np_write_stream(out, *src++);
            size--;
        }
        else if (c->out_bin_block) {
            memcpy(out->cur, src, n);
            out->cur += n;
            size -= n;
            src += n;
        }
        else if (*src != ESC) {
            unsigned char * dst = out->cur;
            unsigned char * end = dst + n;
            do {
                unsigned char ch = *src;
                if (ch == ESC) break;
                *dst++ = ch;
                src++;
            }
            while (dst < end);
            size -= dst - out->cur;
            out->cur = dst;
        }
        else {
            np_write_stream(out, *src++);
            size--;
        }
    }
}

static ssize_t np_splice_block_stream(OutputStream * out, int fd, size_t size, int64_t * offset) {
    assert(is_dispatch_thread());
    if (size == 0) return 0;
    {
        ssize_t rd;
        char buffer[BUF_SIZE];
        if (size > BUF_SIZE) size = BUF_SIZE;
        if (offset != NULL) {
            rd = pread(fd, buffer, size, (off_t)*offset);
            if (rd > 0) *offset += rd;
        }
        else {
            rd = read(fd, buffer, size);
        }
        if (rd > 0) np_write_block_stream(out, buffer, rd);
        return rd;
    }
}

static int np_wt_read_request (void * args) {
    NpIOReq * req = (NpIOReq  *)args;
    int rval = 0;
    trace(LOG_PROTOCOL, "read request size:%d",req->bufsz);

    while (rval == 0) {
        rval = nopoll_conn_read (req->np_sock, req->bufp, req->bufsz, nopoll_false, 10);
        if ((rval <= 0) && !nopoll_conn_is_ok(req->np_sock)) {
            rval = 0;                /* Treat error as EOF */
            break;
        }
        else if (rval < 0) {
            errno = ERR_OTHER;
            req->error_msg = loc_strdup("Failure reading socket");
            break;
        }
    }
    trace(LOG_PROTOCOL, "read size:%d",rval);
    return rval;
}

static void np_post_read(InputBuf * ibuf, unsigned char * buf, size_t size) {
    ChannelNP * c = ibuf2np(ibuf);
    NpIOReq * req;

    if (c->read_pending) return;
    req = loc_alloc(sizeof(NpIOReq));
    req->bufp = buf;
    req->bufsz = size;
    req->np_sock = c->np_socket;
    c->read_pending = 1;
    c->read_buf = buf;
    c->read_buf_size = size;
    c->rd_req.u.user.func = np_wt_read_request;
    c->rd_req.u.user.data = req;
    c->rd_req.type = AsyncReqUser;
    async_req_post(&c->rd_req);
    trace(LOG_PROTOCOL, "post read request ");

}

static void np_wait_read(InputBuf * ibuf) {
    ChannelNP * c = ibuf2np(ibuf);

    /* Wait for read to complete */
    assert(c->lock_cnt > 0);
    assert(c->read_pending != 0);
    cancel_event(np_channel_read_done, &c->rd_req, 1);
    np_channel_read_done(&c->rd_req);
}

static int np_read_stream(InputStream * inp) {
    Channel * channel = inp2channel(inp);
    ChannelNP * c = channel2np(channel);

    assert(c->lock_cnt > 0);
    if (inp->cur < inp->end) return *inp->cur++;
    return ibuf_get_more(&c->ibuf, 0);
}

static int np_peek_stream(InputStream * inp) {
    Channel * channel = inp2channel(inp);
    ChannelNP * c = channel2np(channel);

    assert(c->lock_cnt > 0);
    if (inp->cur < inp->end) return *inp->cur;
    return ibuf_get_more(&c->ibuf, 1);
}

static void send_eof_and_close(Channel * channel, int err) {
    ChannelNP * c = channel2np(channel);

    assert(c->magic == CHANNEL_MAGIC);
    if (channel->state == ChannelStateDisconnected) return;
    ibuf_flush(&c->ibuf);
    if (c->ibuf.handling_msg == HandleMsgTriggered) {
        /* Cancel pending message handling */
        cancel_event(handle_channel_msg, c, 0);
        c->ibuf.handling_msg = HandleMsgIdle;
    }
    write_stream(&c->chan.out, MARKER_EOS);
    write_errno(&c->chan.out, err);
    write_stream(&c->chan.out, MARKER_EOM);
    np_flush_with_flags(c, 0);
#if ENABLE_OutputQueue
    if (output_queue_is_empty(&c->out_queue))
#endif
        nopoll_conn_shutdown(c->np_socket);
    c->chan.state = ChannelStateDisconnected;
    np_post_read(&c->ibuf, c->ibuf.buf, c->ibuf.buf_size);
    notify_channel_closed(channel);
    if (channel->disconnected) {
        channel->disconnected(channel);
    }
    else {
        trace(LOG_PROTOCOL, "channel %#lx disconnected", c);
        if (channel->protocol != NULL) protocol_release(channel->protocol);
    }
    channel->protocol = NULL;
}

static void handle_channel_msg(void * x) {
    Trap trap;
    ChannelNP * c = (ChannelNP *)x;
    int has_msg;

    assert(is_dispatch_thread());
    assert(c->magic == CHANNEL_MAGIC);
    assert(c->ibuf.handling_msg == HandleMsgTriggered);
    assert(c->ibuf.message_count);

    has_msg = ibuf_start_message(&c->ibuf);
    if (has_msg <= 0) {
        if (has_msg < 0 && c->chan.state != ChannelStateDisconnected) {
            trace(LOG_PROTOCOL, "Socket is shutdown by remote peer, channel %#lx %s", c, c->chan.peer_name);
            channel_close(&c->chan);
        }
    }
    else if (set_trap(&trap)) {
        if (c->chan.receive) {
            c->chan.receive(&c->chan);
        }
        else {
            handle_protocol_message(&c->chan);
            assert(c->out_bin_block == NULL);
        }
        clear_trap(&trap);
    }
    else {
        trace(LOG_ALWAYS, "Exception in message handler: %s", errno_to_str(trap.error));
        send_eof_and_close(&c->chan, trap.error);
    }
}

static void channel_check_pending(Channel * channel) {
    ChannelNP * c = channel2np(channel);

    assert(is_dispatch_thread());
    if (c->ibuf.handling_msg == HandleMsgIdle && c->ibuf.message_count) {
        post_event(handle_channel_msg, c);
        c->ibuf.handling_msg = HandleMsgTriggered;
    }
}

static void np_trigger_message(InputBuf * ibuf) {
    ChannelNP * c = ibuf2np(ibuf);

    assert(is_dispatch_thread());
    assert(c->ibuf.message_count > 0);
    if (c->ibuf.handling_msg == HandleMsgIdle) {
        post_event(handle_channel_msg, c);
        c->ibuf.handling_msg = HandleMsgTriggered;
    }
}

static int channel_get_message_count(Channel * channel) {
    ChannelNP * c = channel2np(channel);
    assert(is_dispatch_thread());
    if (c->ibuf.handling_msg != HandleMsgTriggered) return 0;
    return c->ibuf.message_count;
}

static void np_channel_read_done(void * x) {
    AsyncReqInfo * req = (AsyncReqInfo *)x;
    ChannelNP * c = (ChannelNP *)req->client_data;
    ssize_t len = 0;

    assert(is_dispatch_thread());
    assert(c->magic == CHANNEL_MAGIC);
    assert(c->read_pending != 0);
    assert(c->lock_cnt > 0);
    loc_free(req->u.user.data);

    c->read_pending = 0;
    /* some data is available retrieve it */
    {
        len = c->rd_req.u.user.rval;
        if (req->error) {
            if (c->chan.state != ChannelStateDisconnected) {
                trace(LOG_ALWAYS, "Can't read from socket: %s", errno_to_str(req->error));
            }
            len = 0; /* Treat error as EOF */
        }
    }
    if (c->chan.state != ChannelStateDisconnected) {
        ibuf_read_done(&c->ibuf, len);
    }
    else if (len > 0) {
        np_post_read(&c->ibuf, c->ibuf.buf, c->ibuf.buf_size);
    }
    else {
        np_unlock(&c->chan);
    }
}

static void start_channel(Channel * channel) {
    ChannelNP * c = channel2np(channel);

    assert(is_dispatch_thread());
    assert(c->magic == CHANNEL_MAGIC);
    assert(c->socket != NOPOLL_INVALID_SOCKET);
    notify_channel_created(&c->chan);
    if (c->chan.connecting) {
        c->chan.connecting(&c->chan);
    }
    else {
        trace(LOG_PROTOCOL, "channel server connecting");
        send_hello_message(&c->chan);
    }
    ibuf_trigger_read(&c->ibuf);
}

static ChannelNP * create_channel(noPollConn * np_sock, int en_ssl, int server) {
    const int i = 1;
    ChannelNP * c;
    int sock = nopoll_conn_socket(np_sock);

    assert(np_sock >= 0);
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&i, sizeof(i)) < 0) {
        int error = errno;
        trace(LOG_ALWAYS, "Can't set TCP_NODELAY option on a socket: %s", errno_to_str(error));
        errno = error;
        return NULL;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *)&i, sizeof(i)) < 0) {
        int error = errno;
        trace(LOG_ALWAYS, "Can't set SO_KEEPALIVE option on a socket: %s", errno_to_str(error));
        errno = error;
        return NULL;
    }

    c = (ChannelNP *)loc_alloc_zero(sizeof *c);
#if ENABLE_Splice
    if (pipe(c->pipefd) == -1) {
        int err = errno;
        loc_free(c);
        trace(LOG_ALWAYS, "Cannot create channel pipe : %s", errno_to_str(err));
        errno = err;
        return NULL;
    }
#endif /* ENABLE_Splice */
    c->magic = CHANNEL_MAGIC;
    c->is_ssl = en_ssl;
    c->chan.inp.read = np_read_stream;
    c->chan.inp.peek = np_peek_stream;
    c->obuf = output_queue_alloc_obuf();
    c->chan.out.cur = c->obuf->buf;
    c->chan.out.end = c->obuf->buf + sizeof(c->obuf->buf);
    c->chan.out.write = np_write_stream;
    c->chan.out.write_block = np_write_block_stream;
    c->chan.out.splice_block = np_splice_block_stream;
    list_add_last(&c->chan.chanlink, &channel_root);
    shutdown_set_normal(&channel_shutdown);
    c->chan.state = ChannelStateStartWait;
    c->chan.incoming = server;
    c->chan.start_comm = start_channel;
    c->chan.check_pending = channel_check_pending;
    c->chan.message_count = channel_get_message_count;
    c->chan.lock = np_lock;
    c->chan.unlock = np_unlock;
    c->chan.is_closed = np_is_closed;
    c->chan.close = send_eof_and_close;
    ibuf_init(&c->ibuf, &c->chan.inp);
    c->ibuf.post_read = np_post_read;
    c->ibuf.wait_read = np_wait_read;
    c->ibuf.trigger_message = np_trigger_message;
    c->socket = nopoll_conn_socket(np_sock);
    c->np_socket = np_sock;
    c->lock_cnt = 1;
    c->rd_req.done = np_channel_read_done;
    c->rd_req.client_data = c;
    c->rd_req.type = AsyncReqSelect;
#if ENABLE_OutputQueue
    output_queue_ini(&c->out_queue);
#endif
    return c;
}

static void refresh_peer_server(int sock, PeerServer * ps) {
    unsigned i;
    const char * transport = peer_server_getprop(ps, "TransportName", NULL);
    assert(transport != NULL);
    if (strcmp(transport, "UNIX") == 0) {
        char str_id[64];
        PeerServer * ps2 = peer_server_alloc();
        ps2->flags = ps->flags | PS_FLAG_LOCAL | PS_FLAG_DISCOVERABLE;
        for (i = 0; i < ps->ind; i++) {
            peer_server_addprop(ps2, loc_strdup(ps->list[i].name), loc_strdup(ps->list[i].value));
        }
        snprintf(str_id, sizeof(str_id), "%s:%s", transport, peer_server_getprop(ps, "Host", ""));
        for (i = 0; str_id[i]; i++) {
            /* Character '/' is prohibited in a peer ID string */
            if (str_id[i] == '/') str_id[i] = '|';
        }
        peer_server_addprop(ps2, loc_strdup("ID"), loc_strdup(str_id));
        peer_server_add(ps2, PEER_DATA_RETENTION_PERIOD * 2);
    }
    else {
        struct sockaddr_in sin;
#if defined(_WRS_KERNEL)
        int sinlen;
#else
        socklen_t sinlen;
#endif
        const char *str_port = peer_server_getprop(ps, "Port", NULL);
        int ifcind;
        struct in_addr src_addr;
        ip_ifc_info ifclist[MAX_IFC];
        sinlen = sizeof sin;
        if (getsockname(sock, (struct sockaddr *)&sin, &sinlen) != 0) {
            trace(LOG_ALWAYS, "refresh_peer_server: getsockname error: %s", errno_to_str(errno));
            return;
        }
        ifcind = build_ifclist(sock, MAX_IFC, ifclist);
        while (ifcind-- > 0) {
            char str_host[64];
            char str_id[64];
            PeerServer * ps2;
            if (sin.sin_addr.s_addr != INADDR_ANY &&
                (ifclist[ifcind].addr & ifclist[ifcind].mask) !=
                (sin.sin_addr.s_addr & ifclist[ifcind].mask)) {
                continue;
            }
            src_addr.s_addr = ifclist[ifcind].addr;
            ps2 = peer_server_alloc();
            ps2->flags = ps->flags | PS_FLAG_LOCAL | PS_FLAG_DISCOVERABLE;
            for (i = 0; i < ps->ind; i++) {
                peer_server_addprop(ps2, loc_strdup(ps->list[i].name), loc_strdup(ps->list[i].value));
            }
            inet_ntop(AF_INET, &src_addr, str_host, sizeof(str_host));
            snprintf(str_id, sizeof(str_id), "%s:%s:%s", transport, str_host, str_port);
            peer_server_addprop(ps2, loc_strdup("ID"), loc_strdup(str_id));
            peer_server_addprop(ps2, loc_strdup("Host"), loc_strdup(str_host));
            peer_server_addprop(ps2, loc_strdup("Port"), loc_strdup(str_port));
            peer_server_add(ps2, PEER_DATA_RETENTION_PERIOD * 2);
        }
    }
}

static void refresh_all_peer_servers(void * x) {
    LINK * l = server_list.next;
    while (l != &server_list) {
        ServerNP * si = servlink2np(l);
        refresh_peer_server(si->sock, si->serv.ps);
        l = l->next;
    }
    post_event_with_delay(refresh_all_peer_servers, NULL, PEER_DATA_REFRESH_PERIOD * 1000000);
}

static void set_peer_addr(ChannelNP * c, struct sockaddr * addr, int addr_len) {
    /* Create a human readable channel name that uniquely identifies remote peer */
    char name[128];
    char nbuf[128];
    assert(addr->sa_family == AF_INET);
    snprintf(name, sizeof(name), "%s:%s:%d",
                c->is_ssl ? "WSS" : "WS",
                inet_ntop(addr->sa_family,
                  &((struct sockaddr_in *)addr)->sin_addr, nbuf, sizeof(nbuf)),
                  ntohs(((struct sockaddr_in *)addr)->sin_port));
    c->chan.peer_name = loc_strdup(name);
}

static void np_server_accept_done(void * x) {
    AsyncReqInfo * req = (AsyncReqInfo *)x;
    ServerNP * si = (ServerNP *)req->client_data;
    if (si->sock < 0) {
        /* Server closed. */
        loc_free(si);
        return;
    }
    if (req->error) {
        trace(LOG_ALWAYS, "Socket accept failed: %s", errno_to_str(req->error));
    }
    else {
        ChannelNP * c = create_channel(si->np_sock, si->is_ssl, 1);
        if (c == NULL) {
            trace(LOG_ALWAYS, "Cannot create channel for accepted connection: %s", errno_to_str(errno));
            closesocket(req->u.acc.rval);
        }
        else {
            struct sockaddr * addr_buf; /* Socket remote address */
            socklen_t addr_len;
#if defined(SOCK_MAXADDRLEN)
            addr_len = SOCK_MAXADDRLEN;
#else
            addr_len = 0x1000;
#endif
            addr_buf = (struct sockaddr *)loc_alloc(addr_len);
            if (getpeername(nopoll_conn_socket (si->np_sock), addr_buf, &addr_len) < 0) {
                trace(LOG_ALWAYS, "Unable to get peer remote name: %s", errno_to_str(errno));
                closesocket(req->u.acc.rval);
            }
            else {
                set_peer_addr(c, addr_buf, addr_len);
                si->serv.new_conn(&si->serv, &c->chan);
            }
            loc_free(addr_buf);
        }
    }
    async_req_post(req);
}

static void server_close(ChannelServer * serv) {
    ServerNP * s = server2np(serv);

    assert(is_dispatch_thread());
    if (s->sock < 0) return;
    list_remove(&s->serv.servlink);
    if (list_is_empty(&channel_root) && list_is_empty(&channel_server_root))
        shutdown_set_stopped(&channel_shutdown);
    list_remove(&s->servlink);
    peer_server_free(s->serv.ps);
    nopoll_conn_close(s->np_sock);
    nopoll_ctx_unref(s->np_ctx);
    s->sock = -1;
}

/* Our own version of np_conn_wait_until_connection_ready() since this API
 * does not work with SSL enabled in our configuration (when we do not use
 * the nopoll wait loop. 
 * Workaround this limitation but creating a similar loop that makes use 
 * of nopoll_conn_get_msg() before connection is ready...
 */

static int channel_np_wait_until_connection_ready(noPollConn * conn, int timeout, int is_ssl) {
        long int total_timeout = timeout;
        int socket = nopoll_conn_socket(conn);
        struct timeval tv;
        fd_set readfds;
        fd_set writefds;
        fd_set errorfds;

        /* check if the connection already finished its connection
           handshake */
        do {
            int rc;
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            FD_ZERO(&errorfds);
            FD_SET(socket, &readfds);

            tv.tv_usec = 10 * 1000;
            tv.tv_sec = 0;

            /* Wait for some event to occur on file descriptor */

            rc = select(socket + 1, &readfds, &writefds, &errorfds, &tv);

            if (rc == -1) break;

            /* For SSL connection, we need to call nopoll_conn_get_msg
             * in order to handle SSL accept. One may expect this is
             * done in nopoll_conn_is_ready()...
             */

            if (is_ssl) nopoll_conn_get_msg (conn);

            /* check if the connection is ok */
            if (! nopoll_conn_is_ok (conn)) 
                return nopoll_false;

            /* reduce the amount of time we have to wait. This computation
             * is not fully accurate but overall it is okay*/
            total_timeout = total_timeout - 10;
        } while (! nopoll_conn_is_ready (conn) && (total_timeout > 0));

        /* report if the connection is ok */
        return nopoll_conn_is_ready (conn);
}

/* following _wt_ functions are called from a worker thread so caution is required
 * to keep its operations thread safe
 */
static int np_wt_accept (void * args) {
    int rc;
    ServerNP * si = (ServerNP *)args;
    si->np_sock = nopoll_conn_accept (si->np_ctx, si->np_listener);

    (void) nopoll_conn_set_sock_block(nopoll_conn_socket(si->np_sock), nopoll_false);
    rc = channel_np_wait_until_connection_ready(si->np_sock, 1000, si->is_ssl);
    (void) nopoll_conn_set_sock_block(nopoll_conn_socket(si->np_sock), nopoll_true);

    if (rc == 0) {
        errno = EINVAL;
        return -1;
    }
    assert (rc);
    assert(nopoll_conn_is_ready(si->np_sock));
    return (si->np_sock != NULL);
}

static ChannelServer * channel_server_create(PeerServer * ps, noPollCtx * np_ctx, noPollConn * np_listener, int is_ssl) {
    ServerNP * si = (ServerNP *)loc_alloc_zero(sizeof *si);
    /* TODO: need to investigate usage of sizeof(sockaddr_storage) for address buffer size */
    si->serv.close = server_close;
    si->sock = nopoll_conn_socket(np_listener);
    si->serv.ps = ps;
    if (server_list.next == NULL) {
        list_init(&server_list);
        post_event_with_delay(refresh_all_peer_servers, NULL, PEER_DATA_REFRESH_PERIOD * 1000000);
    }
    list_add_last(&si->serv.servlink, &channel_server_root);
    shutdown_set_normal(&channel_shutdown);
    list_add_last(&si->servlink, &server_list);
    refresh_peer_server(si->sock, ps);


    si->accreq.done = np_server_accept_done;
    si->accreq.u.user.data = si;
    si->accreq.u.user.func = np_wt_accept;
    si->accreq.client_data = si;
    si->accreq.type = AsyncReqUser;

    si->np_listener = np_listener;
    si->np_ctx = np_ctx;
    si->is_ssl = is_ssl;
    async_req_post(&si->accreq);
    return &si->serv;
}

static int np_poll_initialized = 0;

static void * local_mutex_init(void) {
#ifdef PTHREAD_MUTEX_RECURSIVE
    pthread_mutexattr_t attr;
#endif
    pthread_mutex_t * mutex = loc_alloc(sizeof(pthread_mutex_t));
#ifdef PTHREAD_MUTEX_RECURSIVE
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init (mutex, &attr);
#else
    pthread_mutex_init (mutex, NULL);
#endif
    return mutex;
}

static void local_mutex_destroy (void * args) {
    if (args == 0) return;
    pthread_mutex_destroy((pthread_mutex_t *)args);
    loc_free(args);
}

static void local_mutex_lock (void * args) {
    if (args) {
        pthread_mutex_lock((pthread_mutex_t *)args);
    }
}

static void local_mutex_unlock (void * args) {
    if (args) {
        pthread_mutex_unlock((pthread_mutex_t *)args);
    }
}

static void ini_nopoll() {
    if(np_poll_initialized) return;
    nopoll_thread_handlers (local_mutex_init,
                    local_mutex_destroy,
                    local_mutex_lock,
                    local_mutex_unlock);
#if ENABLE_WebSocket_SOCKS_V5
    if (socks_v5_host != NULL) {
        nopoll_conn_set_socks_v5_proxy(socks_v5_host, socks_v5_port);
    }
#endif
    np_poll_initialized = 1;
}

ChannelServer * channel_np_server(PeerServer * ps) {
/*    const char * host = peer_server_getprop(ps, "Host", NULL);*/
    const char * port = peer_server_getprop(ps, "Port", NULL);
    char port_str[16];
    const char * host = peer_server_getprop(ps, "Host", NULL);
    const char * certificate = peer_server_getprop(ps, "CertificateFile", NULL);
    const char * key = peer_server_getprop(ps, "KeyFile", NULL);
    noPollCtx * np_ctx;
    noPollConn * np_listener;
    int ssl;

    assert(is_dispatch_thread());
    if (port == NULL) {
        sprintf(port_str, "%d", DISCOVERY_TCF_PORT);
        port = port_str;
    }

    ini_nopoll();
    /* create the nopoll ctx */
    np_ctx = nopoll_ctx_new ();
    if(np_ctx == NULL) {
        trace(LOG_ALWAYS, "Unable to create nopoll context: %s", errno_to_str(errno));
        return NULL;
    }
    /*nopoll_log_enable(np_ctx, nopoll_true);*/

    ssl = strcmp(peer_server_getprop(ps, "TransportName", ""), "WSS") == 0;
    if (ssl) np_listener = nopoll_listener_tls_new (np_ctx, host == NULL ? "0.0.0.0" : host, port);
    else np_listener = nopoll_listener_new (np_ctx, host == NULL ? "0.0.0.0" : host, port);


    if (np_listener == NULL) {
        trace(LOG_ALWAYS, "Unable to create nopoll listener: %s", errno_to_str(errno));
        nopoll_ctx_unref(np_ctx);
        return NULL;
    }

    /* configure certificates to be used by this listener */
    if (ssl) {
        errno = 0;
        if (! nopoll_listener_set_certificate (np_listener, certificate, key, NULL)) {
            printf ("ERROR: unable to configure certificates for TLS websocket..\n");
            if (errno == 0) errno = EINVAL;
            nopoll_conn_close (np_listener);
            nopoll_ctx_unref(np_ctx);
            return NULL;
        }

        /* register certificates at context level */
        errno = 0;
        if (! nopoll_ctx_set_certificate (np_ctx, NULL, certificate, key, NULL)) {
            printf ("ERROR: unable to setup certificates at context level..\n");
            if (errno == 0) errno = EINVAL;
            nopoll_conn_close (np_listener);
            nopoll_ctx_unref(np_ctx);
            return NULL;
        }
    }

    peer_server_addprop(ps, loc_strdup("Port"), loc_strdup(port));
    return channel_server_create(ps, np_ctx, np_listener, ssl);
}

typedef struct ChannelConnectInfo {
    ChannelConnectCallBack callback;
    void * callback_args;
    int is_ssl;
    AsyncReqInfo req;
    char * port;
    char * host;
    const char * get_url;
    const char * host_name;
    noPollConn * np_sock;
    noPollCtx * np_ctx;
    struct sockaddr * addr_buf;
    socklen_t addr_len;
} ChannelConnectInfo;

static void channel_np_connect_done(void * args) {
    ChannelConnectInfo * info = (ChannelConnectInfo *)((AsyncReqInfo *)args)->client_data;
    if (info->req.error) {
        if (info->req.error == EPERM) {
            info->req.error = set_fmt_errno(ERR_OTHER, "Failed to handshake the connection h:%s p:%s", info->host, info->port);
        }
        else if (info->req.error == ECONNREFUSED) {
            info->req.error = set_fmt_errno(ERR_OTHER, "Failed to establish connection h:%s p:%s", info->host, info->port);
        }
	if (info->np_sock) nopoll_conn_close(info->np_sock);
        info->callback(info->callback_args, info->req.error, NULL);
    }
    else {
        ChannelNP * c = create_channel(info->np_sock, info->is_ssl, 0);
        if (c == NULL) {
	    if (info->np_sock) nopoll_conn_close(info->np_sock);
            info->callback(info->callback_args, errno, NULL);
        }
        else {
            set_peer_addr(c, info->addr_buf, info->addr_len);
            info->callback(info->callback_args, 0, &c->chan);
        }
    }
    nopoll_ctx_unref(info->np_ctx);
    loc_free(info->host);
    loc_free(info->port);
    loc_free(info->addr_buf);
    loc_free(info);
}

static int np_wt_connect(void * args) {
    ChannelConnectInfo * info = (ChannelConnectInfo *)args;
    noPollConn * conn;
    
    if (info->is_ssl) {
        noPollConnOpts * opts = NULL;
#ifdef _WRS_KERNEL
        /* For VxWorks SSL peer certificate verification does not work; let's
         * disable this for now.
         */
        opts = nopoll_conn_opts_new ();
        nopoll_conn_opts_ssl_peer_verify (opts, nopoll_false);
#endif
        conn = nopoll_conn_tls_new (info->np_ctx, opts, info->host, info->port, NULL, info->get_url, info->host_name, NULL);
    } else conn = nopoll_conn_new (info->np_ctx, info->host, info->port, NULL, NULL, NULL, NULL);

    if (! nopoll_conn_is_ok (conn)) {
        nopoll_conn_close(conn);
        errno = ECONNREFUSED;
        return -1;
    }

    /* nopoll_conn_wait_until_connection_ready() can return true even if
     * the connection is not ready but simply ok; no clue why. Let's check
     * again that the connection is ready.
     */

    if (! nopoll_conn_wait_until_connection_ready (conn, 10) || ! nopoll_conn_is_ready(conn)) {
        nopoll_conn_close(conn);
        errno = EPERM;
        return -1;
    }

    assert (nopoll_conn_is_ready (conn));
    assert (nopoll_conn_is_ok (conn));

    /* Set the socket in blocking mode */
    (void) nopoll_conn_set_sock_block(nopoll_conn_socket(conn), nopoll_true);

    info->np_sock = conn;
    return 0;
}

void channel_np_connect(PeerServer * ps, ChannelConnectCallBack callback, void * callback_args) {
    const char * host = peer_server_getprop(ps, "Host", NULL);
    const char * port = peer_server_getprop(ps, "Port", NULL);
    const char * get_url = peer_server_getprop(ps, "GetUrl", NULL);
    const char * host_name = peer_server_getprop(ps, "HostName", NULL);
    ChannelConnectInfo * info = NULL;
    char port_str[16];
    struct addrinfo hints;
    struct addrinfo * reslist = NULL;
    int error;

    ini_nopoll();
    if (port == NULL) {
        sprintf(port_str, "%d", DISCOVERY_TCF_PORT);
        port = port_str;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = PF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    error = loc_getaddrinfo(host, port, &hints, &reslist);
    if (error) error = set_gai_errno(error);
    if (!error) {
        struct addrinfo * res;
        info = (ChannelConnectInfo *)loc_alloc_zero(sizeof(ChannelConnectInfo));
        for (res = reslist; res != NULL; res = res->ai_next) {
            info->addr_len = res->ai_addrlen;
            info->addr_buf = (struct sockaddr *)loc_alloc(res->ai_addrlen);
            memcpy(info->addr_buf, res->ai_addr, res->ai_addrlen);
            error = 0;
            break;
        }
        loc_freeaddrinfo(reslist);
    }

    if (!error && info->addr_buf == NULL) error = ENOENT;
    if (error) {
            if (info != NULL) {
            loc_free(info->addr_buf);
            loc_free(info);
        }
        callback(callback_args, error, NULL);
    } else {
	noPollCtx * np_ctx_client;
	np_ctx_client = nopoll_ctx_new();
	/*nopoll_log_enable(np_ctx_client, nopoll_true);*/

        info->callback = callback;
        info->callback_args = callback_args;
        info->is_ssl = strcmp(peer_server_getprop(ps, "TransportName", ""), "WSS") == 0;
        info->req.client_data = info;
        info->req.done = channel_np_connect_done;
        info->req.type = AsyncReqUser;
        info->req.u.user.func = np_wt_connect;
        info->req.u.user.data = info;
        info->host = loc_strdup(host == NULL ? "127.0.0.1" : host);
        info->port = loc_strdup(port);
        info->get_url = get_url;
        info->host_name = host_name;
        info->np_ctx = np_ctx_client;
        async_req_post(&info->req);
    }
}

#if ENABLE_WebSocket_SOCKS_V5
int parse_socks_v5_proxy(const char * proxy) {
    int error = 0;
    if (proxy != NULL) {
        const char * str;
        str = strchr(proxy, ':');
        if (str == NULL || strchr (str + 1, ':') != NULL) {
            error = set_errno (ERR_OTHER, "Invalid format for SocksV5 WebSocket proxy");
            return -1;
        }
        socks_v5_host = loc_alloc_zero(str - proxy + 1);
        strncpy(socks_v5_host, proxy, str - proxy);
        socks_v5_port = loc_strdup(str + 1);
    }
    return 0;
}
#endif  /* ENABLE_WebSocket_SOCKS_V5 */

void ini_np_channel() {
    add_channel_transport("WS", channel_np_server, channel_np_connect);
    add_channel_transport("WSS", channel_np_server, channel_np_connect);
}
#endif /* ENABLE_WebSocket */
