/*
 * Copyright (C) 2018-2021 Jolla Ltd.
 * Copyright (C) 2018-2021 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "test_binder.h"
#include "gbinder_local_object.h"
#include "gbinder_system.h"

#define GLOG_MODULE_NAME test_binder_log
#include <gutil_log.h>
GLOG_MODULE_DEFINE2("test_binder", gutil_log_default);

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/syscall.h>

#define gettid() syscall(SYS_gettid)

static GHashTable* test_fd_map = NULL;
static GHashTable* test_node_map = NULL;
static GPrivate test_looper;

G_LOCK_DEFINE_STATIC(test_binder);
static GCond test_node_map_cond;

#define PUBLIC (0)
#define PRIVATE (1)
#define public_fd  node[PUBLIC].fd
#define private_fd node[PRIVATE].fd

#define BINDER_VERSION _IOWR('b', 9, gint32)
#define BINDER_SET_MAX_THREADS _IOW('b', 5, guint32)

#define B_TYPE_LARGE 0x85
#define BINDER_TYPE_BINDER  GBINDER_FOURCC('s', 'b', '*', B_TYPE_LARGE)
#define BINDER_TYPE_HANDLE  GBINDER_FOURCC('s', 'h', '*', B_TYPE_LARGE)

#define TF_ONE_WAY     0x01
#define TF_ROOT_OBJECT 0x04
#define TF_STATUS_CODE 0x08
#define TF_ACCEPT_FDS  0x10

typedef struct test_binder_io TestBinderIo;

typedef
void
(*TestBinderPushDataFunc)(
    int fd,
    const void* data);

typedef struct test_binder_submit_thread {
    GThread* thread;
    GCond cond;
    GMutex mutex;
    gboolean run;
    GSList* queue;
    TestBinder* binder;
} TestBinderSubmitThread;

typedef struct test_binder_node TestBinderNode;
struct test_binder_node {
    int fd;
    char* path;
    TestBinder* binder;
    TestBinderNode* other;
    TEST_LOOPER looper_enabled;
    gpointer tx_thread;
    gint looper_count;
    GMutex mutex; /* Protects reads and next_cmd */
    guint32* next_cmd;
};

typedef struct test_binder_fd {
    int fd;
    GHashTable* destroy_map;
    TestBinderNode* node;
} TestBinderFd;

typedef struct test_binder {
    gint refcount;
    const TestBinderIo* io;
    GHashTable* object_map; /* GBinderLocalObject* => handle */
    GHashTable* handle_map; /* handle => GBinderLocalObject* */
    TestBinderSubmitThread* submit_thread;
    GMutex mutex;
    gboolean passthrough;
    TestBinderNode node[2];
} TestBinder;

struct test_binder_io {
    int version;
    int write_read_request;
    int (*handle_write_read)(TestBinderFd* fd, void* data);
};

typedef struct binder_write_read_64 {
    guint64 write_size;
    guint64 write_consumed;
    guint64 write_buffer;
    guint64 read_size;
    guint64 read_consumed;
    guint64 read_buffer;
} BinderWriteRead64;

typedef struct binder_transaction_data_64 {
    guint64 handle;
    guint64 cookie;
    guint32 code;
    guint32 flags;
    gint32 sender_pid;
    gint32 sender_euid;
    guint64 data_size;
    guint64 offsets_size;
    guint64 data_buffer;
    guint64 data_offsets;
} BinderTransactionData64;

typedef struct binder_transaction_data_sg_64 {
    BinderTransactionData64 tx;
    guint64 buffers_size;
} BinderTransactionDataSg64;

typedef struct binder_pre_cookie_64 {
    guint64 ptr;
    guint64 cookie;
} BinderPtrCookie64;

typedef struct binder_handle_cookie_64 {
  guint32 handle;
  guint64 cookie;
} __attribute__((packed)) BinderHandleCookie64;

typedef struct binder_object_64 {
  guint32 type;
  guint32 flags;
  guint64 object;
  guint64 cookie;
} BinderObject64;

#define BC_TRANSACTION_64       _IOW('c', 0, BinderTransactionData64)
#define BC_REPLY_64             _IOW('c', 1, BinderTransactionData64)
#define BC_FREE_BUFFER_64       _IOW('c', 3, guint64)
#define BC_INCREFS              _IOW('c', 4, guint32)
#define BC_ACQUIRE              _IOW('c', 5, guint32)
#define BC_RELEASE              _IOW('c', 6, guint32)
#define BC_DECREFS              _IOW('c', 7, guint32)
#define BC_ENTER_LOOPER          _IO('c', 12)
#define BC_EXIT_LOOPER           _IO('c', 13)
#define BC_REQUEST_DEATH_NOTIFICATION_64 _IOW('c', 14, BinderHandleCookie64)
#define BC_CLEAR_DEATH_NOTIFICATION_64   _IOW('c', 15, BinderHandleCookie64)
#define BC_TRANSACTION_SG_64    _IOW('c', 17, BinderTransactionDataSg64)
#define BC_REPLY_SG_64          _IOW('c', 18, BinderTransactionDataSg64)

#define BR_TRANSACTION_64       _IOR('r', 2, BinderTransactionData64)
#define BR_REPLY_64             _IOR('r', 3, BinderTransactionData64)
#define BR_DEAD_REPLY            _IO('r', 5)
#define BR_TRANSACTION_COMPLETE  _IO('r', 6)
#define BR_INCREFS_64           _IOR('r', 7, BinderPtrCookie64)
#define BR_ACQUIRE_64           _IOR('r', 8, BinderPtrCookie64)
#define BR_RELEASE_64           _IOR('r', 9, BinderPtrCookie64)
#define BR_DECREFS_64           _IOR('r', 10, BinderPtrCookie64)
#define BR_NOOP                  _IO('r', 12)
#define BR_DEAD_BINDER_64       _IOR('r', 15, guint64)
#define BR_FAILED_REPLY          _IO('r', 17)

static
gpointer
test_binder_submit_thread_proc(
    gpointer data)
{
    TestBinderSubmitThread* submit = data;
    TestBinder* binder = submit->binder;
    GMutex* mutex = &submit->mutex;
    GCond* cond = &submit->cond;

    GDEBUG("Submit thread started");
    g_mutex_lock(mutex);
    while (submit->run) {
        GBytes* next = NULL;

        while (submit->run && !next) {
            if (submit->queue) {
                next = submit->queue->data;
                submit->queue = g_slist_remove(submit->queue, next);
                break;
            } else {
                g_cond_wait(cond, mutex);
            }
        }

        if (next) {
            int bytes_available = 0;
            int err = ioctl(binder->public_fd, FIONREAD, &bytes_available);

            /* Wait until the queue is empty */
            g_assert(err >= 0);
            while (bytes_available > 0 && submit->run) {
                /* Wait a bit between polls */
                g_cond_wait_until(cond, mutex, g_get_monotonic_time () +
                    100 * G_TIME_SPAN_MILLISECOND);
                err = ioctl(binder->public_fd, FIONREAD, &bytes_available);
                g_assert(err >= 0);
            }

            if (submit->run) {
                gsize len;
                gconstpointer data = g_bytes_get_data(next, &len);

                GDEBUG("Submitting command 0x%08x", *(guint32*)data);
                g_assert(write(binder->private_fd, data, len) == len);
            }
            g_bytes_unref(next);
        }
    }
    g_mutex_unlock(mutex);
    GDEBUG("Submit thread exiting");
    return NULL;
}

static
TestBinderSubmitThread*
test_binder_submit_thread_new(
    TestBinder* binder)
{
    TestBinderSubmitThread* submit = g_new0(TestBinderSubmitThread, 1);

    submit->run = TRUE;
    submit->binder = binder;
    g_cond_init(&submit->cond);
    g_mutex_init(&submit->mutex);
    submit->thread = g_thread_new(binder->node->path,
        test_binder_submit_thread_proc, submit);
    return submit;
}

static
void
test_binder_submit_thread_free(
    TestBinderSubmitThread* submit)
{
    if (submit) {
        g_mutex_lock(&submit->mutex);
        submit->run = FALSE;
        g_cond_signal(&submit->cond);
        g_mutex_unlock(&submit->mutex);
        g_thread_join(submit->thread);

        g_slist_free_full(submit->queue, (GDestroyNotify) g_bytes_unref);
        g_cond_clear(&submit->cond);
        g_mutex_clear(&submit->mutex);
        g_free(submit);
    }
}

static
void
test_binder_submit_later(
    TestBinderSubmitThread* submit,
    const void* data)
{
    const guint32* cmd = data;

    g_mutex_lock(&submit->mutex);
    submit->queue = g_slist_append(submit->queue,
        g_bytes_new(cmd, sizeof(*cmd) + _IOC_SIZE(*cmd)));
    g_cond_signal(&submit->cond);
    g_mutex_unlock(&submit->mutex);
}

static
void
test_io_free_buffer(
    TestBinderFd* fd,
    void* ptr)
{
    if (ptr) {
        GDestroyNotify destroy;

        G_LOCK(test_binder);
        destroy = g_hash_table_lookup(fd->destroy_map, ptr);
        if (destroy) {
            g_hash_table_remove(fd->destroy_map, ptr);
            destroy(ptr);
        } else {
            g_free(ptr);
        }
        G_UNLOCK(test_binder);
    }
}

void
test_binder_exit_wait(
    void)
{
    G_LOCK(test_binder);
    while (test_node_map) {
        GDEBUG("Waiting for loopers to exit...");
        g_cond_wait(&test_node_map_cond, &G_LOCK_NAME(test_binder));
    }
    G_UNLOCK(test_binder);
}

static
guint64
test_io_passthough_fix_handle(
    TestBinder* binder,
    guint64 handle)
{
    gpointer key = GSIZE_TO_POINTER(handle);

    /* Invoked under lock */
    if (g_hash_table_contains(binder->object_map, key)) {
        gpointer value = g_hash_table_lookup(binder->object_map, key);

        handle = GPOINTER_TO_SIZE(value);
        GDEBUG("Object %p => handle %u", key, (guint) handle);
    } else if (g_hash_table_contains(binder->handle_map, key)) {
        gpointer obj = g_hash_table_lookup(binder->handle_map, key);

        GDEBUG("Handle %u => object %p", (guint) handle, obj);
        handle = GPOINTER_TO_SIZE(obj);
    }
    return handle;
}

static
int
test_binder_bytes_available(
    int fd)
{
    int bytes_available = 0;
    int err = ioctl(fd, FIONREAD, &bytes_available);

    return (err >= 0) ? bytes_available : err;
}

static
int
test_binder_node_read_all(
    int fd,
    void* buf,
    int nbytes)
{
    int out = 0;
    guint8* ptr = buf;

    while (nbytes > 0) {
        out = read(fd, ptr, nbytes);
        if (out < 0) {
            break;
        } else {
            g_assert_cmpint(out, <= ,nbytes);
            ptr += out;
            nbytes -= out;
        }
    }
    return out;
}

static
guint32*
test_binder_node_read(
    TestBinderNode* node,
    gsize max_bytes,
    int* bytes_read)
{
    guint32* out = NULL;

    /* Ensures that we never read partial commands */
    g_mutex_lock(&node->mutex);
    if (node->next_cmd) {
        const guint total = 4 + _IOC_SIZE(node->next_cmd[0]);

        /* Alread have one ready */
        if (max_bytes >= total) {
            out = node->next_cmd;
            node->next_cmd = NULL;
            *bytes_read = total;
        } else {
            GDEBUG("Buffer full (%u > %d)", total, (int)max_bytes);
            *bytes_read = 0;
        }
    } else {
        /* Read the next one from the socket */
        int available = test_binder_bytes_available(node->fd);

        if (available >= 4) {
            guint32 cmd;
            int err = test_binder_node_read_all(node->fd, &cmd, sizeof(cmd));

            if (err < 0) {
                GDEBUG("Read error %d", err);
                *bytes_read = err;
            } else {
                guint datasize = _IOC_SIZE(cmd);
                guint total = 4 + datasize;
                guint32* buf = g_malloc(total);

                buf[0] = cmd;
                err = test_binder_node_read_all(node->fd, buf + 1, datasize);
                if (err < 0) {
                    GDEBUG("Read error %d", err);
                    *bytes_read = err;
                } else {
                    buf[0] = cmd;
                    if (max_bytes >= total) {
                        out = buf;
                        *bytes_read = total;
                    } else {
                        const guint32 noop = BR_NOOP;

                        GDEBUG("Stashed cmd (%u > %d)", total, (int)max_bytes);
                        node->next_cmd = buf;
                        *bytes_read = 0;

                        /*
                         * Make sure looper comes back for it and doesn't
                         * get stuck in poll() forever.
                         */
                        g_assert_cmpint(write(node->other->fd, &noop,
                            sizeof(noop)), == ,sizeof(noop));
                    }
                }
            }
        } else {
            /* Not enough data to read */
            *bytes_read = 0;
        }
    }
    g_mutex_unlock(&node->mutex);
    return out;
}

static
gssize
test_io_passthough_write_64(
    TestBinderFd* fd,
    const void* bytes,
    gsize bytes_to_write)
{
    const guint code = *(guint32*)bytes;
    TestBinderNode* node = fd->node;
    TestBinder* binder = node->binder;
    BinderTransactionData64* tx = NULL;
    gssize bytes_written;
    guint32* buf;
    guint32* cmd;
    guint32 bytes_to_really_write;
    void* data;

    /* Just ignore some codes for now */
    switch (code) {
    case BC_ACQUIRE:
    case BC_RELEASE:
    case BC_REQUEST_DEATH_NOTIFICATION_64:
    case BC_CLEAR_DEATH_NOTIFICATION_64:
        return bytes_to_write;
    default:
        break;
    }

    buf = cmd = g_memdup(bytes, bytes_to_write);
    data = cmd + 1;
    switch (*cmd) {
    case BR_TRANSACTION_64:
        *cmd = BC_TRANSACTION_64;
        tx = data;
        break;
    case BC_TRANSACTION_64:
    case BC_TRANSACTION_SG_64:
        if (g_atomic_pointer_compare_and_exchange(&node->tx_thread, NULL,
            g_thread_self())) {
            GDEBUG("Transaction thread %ld", gettid());
        }
        *cmd = BR_TRANSACTION_64;
        tx = data;
        break;
    case BR_REPLY_64:
        *cmd = BC_REPLY_64;
        tx = data;
        break;
    case BC_REPLY_64:
    case BC_REPLY_SG_64:
        /*
         * Prepend BR_TRANSACTION_COMPLETE. The whole thing has to be written
         * with a single write so that it gets read with a single read.
         */
        GDEBUG("Thread %ld inserting BR_TRANSACTION_COMPLETE", gettid());
        buf = g_realloc(buf, bytes_to_write + 4);
        cmd = buf + 1;
        data = cmd + 1;
        memmove(cmd, buf, bytes_to_write);
        *buf = BR_TRANSACTION_COMPLETE;
        *cmd = BR_REPLY_64;
        tx = data;
        break;
    }
    if (tx) {
        guint32* data_buffer = g_memdup(GSIZE_TO_POINTER(tx->data_buffer),
            tx->data_size);
        void* data_offsets = g_memdup(GSIZE_TO_POINTER(tx->data_offsets),
            tx->offsets_size);

        G_LOCK(test_binder);
        tx->handle = test_io_passthough_fix_handle(binder, tx->handle);
        if (data_buffer && tx->data_size > sizeof(BinderObject64)) {
            /* Replace BINDER_TYPE_BINDER with BINDER_TYPE_HANDLE */
            guint32* data_ptr = data_buffer;
            const guint32* data_end = data_buffer + ((tx->data_size -
                sizeof(BinderObject64))/sizeof(guint32) + 1);

            /*
             * Objects are supposed to be aligned at 32-bit boundary, so we
             * can scan the data buffer with 4-byte step.
             */
            for (data_ptr = data_buffer; data_ptr < data_end; data_ptr++) {
                if (*data_ptr == BINDER_TYPE_BINDER) {
                    BinderObject64* object = (BinderObject64*) data_ptr;

                    if (object->object) {
                        object->type = BINDER_TYPE_HANDLE;
                        object->object = test_io_passthough_fix_handle(binder,
                            object->object);
                    }
                    data_ptr += sizeof(object)/sizeof(guint32) - 1;
                }
            }
        }
        g_hash_table_replace(fd->destroy_map, data_offsets, NULL);
        G_UNLOCK(test_binder);
        tx->data_buffer = GPOINTER_TO_SIZE(data_buffer);
        tx->data_offsets = GPOINTER_TO_SIZE(data_offsets);
    }

    /* Real number of bytes to write may have changed */
    bytes_to_really_write = ((guint8*)data - (guint8*)buf) + _IOC_SIZE(*cmd);
    bytes_written = write(fd->fd, buf, bytes_to_really_write);
    g_free(buf);
    g_assert(bytes_written == bytes_to_really_write || bytes_written <= 0);
    return (bytes_written >= 0) ? bytes_to_write : bytes_written;
}

static
int
test_io_handle_write_read_64(
    TestBinderFd* fd,
    void* data)
{
    TestBinderNode* node = fd->node;
    TestBinder* binder = node->binder;
    BinderWriteRead64* wr = data;
    gssize bytes_left = wr->write_size - wr->write_consumed;
    const guint8* write_ptr = (void*)(gsize)(wr->write_buffer +
        wr->write_consumed);
    gpointer tx_thread = g_atomic_pointer_get(&node->tx_thread);
    gboolean can_read;

    while (bytes_left >= sizeof(guint32)) {
        const guint cmd = *(guint32*)write_ptr;
        const guint cmdsize = _IOC_SIZE(cmd);
        const void* cmddata = write_ptr + sizeof(guint32);
        const gsize bytes_to_write = sizeof(guint32) + cmdsize;
        int looper;

        GASSERT(bytes_left >= bytes_to_write);
        if (bytes_left >= bytes_to_write) {
            gssize bytes_written = bytes_to_write;

            switch (cmd) {
            case BC_FREE_BUFFER_64:
                test_io_free_buffer(fd, GSIZE_TO_POINTER(*(guint64*)cmddata));
                break;
            case BC_ENTER_LOOPER:
                g_assert(g_private_get(&test_looper) >= 0);
                looper = g_atomic_int_add(&node->looper_count, 1) + 1;
                g_private_set(&test_looper, GINT_TO_POINTER(looper));
                GDEBUG("Thread %ld is looper #%d", gettid(), looper);
                break;
            case BC_EXIT_LOOPER:
                GDEBUG("Thread %ld is no longer a looper", gettid());
                g_atomic_int_add(&node->looper_count, -1);
                g_private_set(&test_looper, NULL);
                break;
            default:
                if (binder->passthrough) {
                    bytes_written = test_io_passthough_write_64(fd,
                        write_ptr, bytes_to_write);
                }
                break;
            }
            if (bytes_written >= 0) {
                wr->write_consumed += bytes_written;
                write_ptr += bytes_written;
                bytes_left -= bytes_written;
            } else {
                GDEBUG("Write failed, %s", strerror(errno));
                return -1;
            }
        } else {
            /* Partial command in the buffer */
            errno = EINVAL;
            return -1;
        }
    }

    if (tx_thread && tx_thread != g_thread_self()) {
        can_read = FALSE;
    } else {
        const gint looper = GPOINTER_TO_INT(g_private_get(&test_looper));

        if (looper <= 0) {
            /* Main or pooled client thread */
            can_read = TRUE;
        } else {
            can_read = FALSE;
            switch (node->looper_enabled) {
            case TEST_LOOPER_DISABLE:
                break;
            case TEST_LOOPER_ENABLE:
                can_read = (looper > 0);
                break;
            case TEST_LOOPER_ENABLE_ONE:
                can_read = (looper == 1);
                break;
            }
        }
    }

    if (can_read && (wr->read_size > wr->read_consumed)) {
        int nbytes = 0;
        int avail = wr->read_size - wr->read_consumed;
        int total = 0;
        gboolean transaction_complete = FALSE;
        guint8* buf = GSIZE_TO_POINTER(wr->read_buffer + wr->read_consumed);
        guint32* cmd;

        while ((cmd = test_binder_node_read(node, avail, &nbytes)) != NULL) {
            g_assert_cmpint(nbytes, <= ,avail);
            if (cmd[0] == BR_TRANSACTION_COMPLETE) {
                transaction_complete = TRUE;
            }
            memcpy(buf, cmd, nbytes);
            wr->read_consumed += nbytes;
            total += nbytes;
            avail -= nbytes;
            buf += nbytes;
            g_free(cmd);
        }

        if (transaction_complete && g_atomic_pointer_compare_and_exchange
           (&node->tx_thread, g_thread_self(), NULL)) {
            GDEBUG("Thread %ld done with the transaction", gettid());
        }

        if (nbytes < 0) {
            /* Error */
            return nbytes;
        } else if (!total) {
            /* Nothing was read */
            usleep(100000); /* 100 ms */
        }
    } else if (wr->read_size > 0) {
        usleep(100000); /* 100 ms */
    }
    return 0;
}

static const TestBinderIo test_io_64 = {
    .version = 8,
    .write_read_request = _IOWR('b', 1, BinderWriteRead64),
    .handle_write_read = test_io_handle_write_read_64
};

static
int
test_binder_ioctl_version(
    TestBinder* binder,
    int* version)
{
    *version = binder->io->version;
    return 0;
}

static
TestBinderFd*
test_binder_fd_from_fd(
    int fd)
{
    TestBinderFd* binder_fd = NULL;

    G_LOCK(test_binder);
    GASSERT(test_fd_map);
    if (test_fd_map) {
        binder_fd = g_hash_table_lookup(test_fd_map, GINT_TO_POINTER(fd));
        GASSERT(binder_fd);
    }
    G_UNLOCK(test_binder);

    return binder_fd;
}

static
TestBinder*
test_binder_from_fd(
    int fd)
{
    TestBinderFd* binder_fd = test_binder_fd_from_fd(fd);

    return binder_fd ? binder_fd->node->binder : NULL;
}

static
void
test_io_destroy_none(
    gpointer data)
{
    GDEBUG("Not freeing %p", data);
}

void
test_binder_set_looper_enabled(
    int fd,
    TEST_LOOPER value)
{
    TestBinderFd* binder_fd = test_binder_fd_from_fd(fd);

    g_assert(binder_fd);
    binder_fd->node->looper_enabled = value;
}

void
test_binder_set_passthrough(
    int fd,
    gboolean passthrough)
{
    TestBinder* binder = test_binder_from_fd(fd);

    g_assert(binder);
    binder->passthrough = passthrough;
}

void
test_binder_set_destroy(
    int fd,
    gpointer ptr,
    GDestroyNotify destroy)
{
    TestBinderFd* binder_fd = test_binder_fd_from_fd(fd);

    if (binder_fd) {
        G_LOCK(test_binder);
        g_hash_table_replace(binder_fd->destroy_map, ptr,
            destroy ? destroy : test_io_destroy_none);
        G_UNLOCK(test_binder);
    }
}

static
void
test_binder_push_data(
    int fd,
    const void* data)
{
    const guint32* cmd = data;
    const int len = sizeof(*cmd) + _IOC_SIZE(*cmd);
    TestBinderFd* binder_fd = test_binder_fd_from_fd(fd);
    TestBinderNode* node = binder_fd->node;

    g_assert(write(node->other->fd, data, len) == len);
}

static
void
test_binder_push_data_later(
    int fd,
    const void* data)
{
    TestBinder* binder = test_binder_from_fd(fd);

    g_assert(binder);
    if (!binder->submit_thread) {
        binder->submit_thread = test_binder_submit_thread_new(binder);
    }
    test_binder_submit_later(binder->submit_thread, data);
}

void
test_binder_push_ptr_cookie(
    int fd,
    guint32 cmd,
    void* ptr)
{
    guint8 buf[sizeof(guint32) + sizeof(BinderPtrCookie64)];
    BinderPtrCookie64* data = (void*)(buf + sizeof(cmd));

    memcpy(buf, &cmd, sizeof(cmd));
    memset(data, 0, sizeof(*data));
    data->ptr = (gsize)ptr;
    test_binder_push_data(fd, buf);
}

void
test_binder_br_noop(
    int fd)
{
    guint32 cmd = BR_NOOP;

    test_binder_push_data(fd, &cmd);
}

void
test_binder_br_increfs(
    int fd,
    void* ptr)
{
    test_binder_push_ptr_cookie(fd, BR_INCREFS_64, ptr);
}

void
test_binder_br_acquire(
    int fd,
    void* ptr)
{
    test_binder_push_ptr_cookie(fd, BR_ACQUIRE_64, ptr);
}

void
test_binder_br_release(
    int fd,
    void* ptr)
{
    test_binder_push_ptr_cookie(fd, BR_RELEASE_64, ptr);
}

void
test_binder_br_decrefs(
    int fd,
    void* ptr)
{
    test_binder_push_ptr_cookie(fd, BR_DECREFS_64, ptr);
}

void
test_binder_br_transaction_complete(
    int fd)
{
    guint32 cmd = BR_TRANSACTION_COMPLETE;

    test_binder_push_data(fd, &cmd);
}

void
test_binder_br_transaction_complete_later(
    int fd)
{
    guint32 cmd = BR_TRANSACTION_COMPLETE;

    test_binder_push_data_later(fd, &cmd);
}

void
test_binder_br_dead_binder(
    int fd,
    guint handle)
{
    const guint64 handle64 = handle;
    guint32 buf[3];

    buf[0] = BR_DEAD_BINDER_64;
    memcpy(buf + 1, &handle64, sizeof(handle64));

    test_binder_push_data(fd, buf);
}

void
test_binder_br_dead_reply(
    int fd)
{
    guint32 cmd = BR_DEAD_REPLY;

    test_binder_push_data(fd, &cmd);
}

void
test_binder_br_failed_reply(
    int fd)
{
    guint32 cmd = BR_FAILED_REPLY;

    test_binder_push_data(fd, &cmd);
}

static
void
test_binder_fill_transaction_data(
    BinderTransactionData64* tr,
    guint64 handle,
    guint32 code,
    const GByteArray* bytes)
{
    memset(tr, 0, sizeof(*tr));
    tr->handle = handle;
    tr->code = code;
    tr->data_size = bytes ? bytes->len : 0;
    tr->sender_pid = getpid();
    tr->sender_euid = geteuid();
    /* This memory should eventually get deallocated with BC_FREE_BUFFER_64 */
    tr->data_buffer = (gsize)g_memdup(bytes ? (void*)bytes->data : (void*)"",
        tr->data_size);
}

void
test_binder_br_transaction(
    int fd,
    void* target,
    guint32 code,
    const GByteArray* bytes)
{
    guint32 cmd = BR_TRANSACTION_64;
    guint8 buf[sizeof(guint32) + sizeof(BinderTransactionData64)];

    memcpy(buf, &cmd, sizeof(cmd));
    test_binder_fill_transaction_data((void*)(buf + sizeof(cmd)),
        (gsize)target, code, bytes);

    test_binder_push_data(fd, buf);
}

static
void
test_binder_br_reply1(
    int fd,
    guint32 handle,
    guint32 code,
    const GByteArray* bytes,
    TestBinderPushDataFunc push)
{
    guint32 cmd = BR_REPLY_64;
    guint8 buf[sizeof(guint32) + sizeof(BinderTransactionData64)];

    memcpy(buf, &cmd, sizeof(cmd));
    test_binder_fill_transaction_data((void*)(buf + sizeof(cmd)),
        handle, code, bytes);

    push(fd, buf);
}

void
test_binder_br_reply(
    int fd,
    guint32 handle,
    guint32 code,
    const GByteArray* bytes)
{
    test_binder_br_reply1(fd, handle, code, bytes, test_binder_push_data);
}

void
test_binder_br_reply_later(
    int fd,
    guint32 handle,
    guint32 code,
    const GByteArray* bytes)
{
    test_binder_br_reply1(fd, handle, code, bytes, test_binder_push_data_later);
}

void
test_binder_br_reply_status1(
    int fd,
    gint32 status,
    TestBinderPushDataFunc push)
{
    guint8 buf[sizeof(guint32) + sizeof(BinderTransactionData64)];
    guint32* cmd = (void*)buf;
    BinderTransactionData64* tr = (void*)(buf + sizeof(*cmd));

    memset(buf, 0, sizeof(buf));
    *cmd = BR_REPLY_64;
    tr->flags = TF_STATUS_CODE;
    tr->data_size = sizeof(status);
    /* This memory should eventually get deallocated with BC_FREE_BUFFER_64 */
    tr->data_buffer = (gsize)g_memdup(&status, sizeof(status));

    push(fd, buf);
}

void
test_binder_br_reply_status(
    int fd,
    gint32 status)
{
    test_binder_br_reply_status1(fd, status, test_binder_push_data);
}

void
test_binder_br_reply_status_later(
    int fd,
    gint32 status)
{
    test_binder_br_reply_status1(fd, status, test_binder_push_data_later);
}

static
void
test_binder_node_clear(
    TestBinderNode* node)
{
    GDEBUG("Done with %s", node->path);
    g_hash_table_remove(test_node_map, node->path);
    if (!g_hash_table_size(test_node_map)) {
        g_hash_table_unref(test_node_map);
        g_cond_broadcast(&test_node_map_cond);
        test_node_map = NULL;
    }
    close(node->fd);
    g_mutex_clear(&node->mutex);
    g_free(node->next_cmd);
    g_free(node->path);
}

static
TestBinder*
test_binder_ref(
    TestBinder* binder)
{
    if (binder) {
        g_atomic_int_inc(&binder->refcount);
    }
    return binder;
}

static
void
test_binder_unregister_objects_internal(
    TestBinder* binder,
    gboolean need_lock)
{
    GSList* objects = NULL;
    GHashTableIter it;
    gpointer value;

    if (need_lock) {
        G_LOCK(test_binder);
    }
    g_assert(binder);
    g_hash_table_iter_init(&it, binder->handle_map);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        objects = g_slist_append(objects, value);
    }
    g_hash_table_remove_all(binder->object_map);
    g_hash_table_remove_all(binder->handle_map);
    if (need_lock) {
        G_UNLOCK(test_binder);
    }
    /* Unref GBinderLocalObjects outside the lock */
    g_slist_free_full(objects, (GDestroyNotify) gbinder_local_object_unref);
}

static
void
test_binder_unref_internal(
    TestBinder* binder,
    gboolean need_lock)
{
    if (binder && g_atomic_int_dec_and_test(&binder->refcount)) {
        if (need_lock) {
            G_LOCK(test_binder);
        }
        test_binder_node_clear(binder->node + 0);
        test_binder_node_clear(binder->node + 1);
        if (need_lock) {
            G_UNLOCK(test_binder);
        }
        test_binder_submit_thread_free(binder->submit_thread);
        test_binder_unregister_objects_internal(binder, need_lock);
        g_hash_table_destroy(binder->object_map);
        g_hash_table_destroy(binder->handle_map);
        g_mutex_clear(&binder->mutex);
        g_free(binder);
    }
}

guint
test_binder_register_object(
    int fd,
    GBinderLocalObject* obj,
    guint h)
{
    TestBinder* binder = test_binder_from_fd(fd);

    g_assert(binder);
    g_assert(obj);

    G_LOCK(test_binder);
    g_assert(!g_hash_table_contains(binder->object_map, obj));
    g_assert(!g_hash_table_contains(binder->handle_map, GINT_TO_POINTER(h)));
    if (h == AUTO_HANDLE) {
        h = 1;
        while (g_hash_table_contains(binder->handle_map, GINT_TO_POINTER(h)) ||
            g_hash_table_contains(binder->object_map, GINT_TO_POINTER(h))) {
            h++;
        }
    }
    GDEBUG("Object %p <=> handle %u", obj, h);
    g_hash_table_insert(binder->handle_map, GINT_TO_POINTER(h), obj);
    g_hash_table_insert(binder->object_map, obj, GINT_TO_POINTER(h));
    G_UNLOCK(test_binder);

    gbinder_local_object_ref(obj);
    return h;
}

void
test_binder_unregister_objects(
    int fd)
{
    test_binder_unregister_objects_internal(test_binder_from_fd(fd), TRUE);
}

static
void
test_fd_map_free(
    gpointer entry)
{
    TestBinderFd* binder_fd = entry;
    GHashTableIter it;
    gpointer key, value;

    g_hash_table_iter_init(&it, binder_fd->destroy_map);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        if (value) {
            ((GDestroyNotify)value)(key);
        } else {
            g_free(key);
        }
    }
    g_hash_table_destroy(binder_fd->destroy_map);
    test_binder_unref_internal(binder_fd->node->binder, FALSE);
    close(binder_fd->fd);
    g_free(binder_fd);
}

static
TestBinderFd*
test_binder_fd_new(
    TestBinderNode* node)
{
    TestBinderFd* binder_fd = g_new0(TestBinderFd, 1);

    /* Assume it's being created by the main thread */
    g_assert(GPOINTER_TO_INT(g_private_get(&test_looper)) <= 0);
    g_private_set(&test_looper, GINT_TO_POINTER(-1));
    test_binder_ref(node->binder);
    binder_fd->node = node;
    binder_fd->fd = dup(node->fd);
    binder_fd->destroy_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    return binder_fd;
}

int
gbinder_system_open(
    const char* path,
    int flags)
{
    static const char binder_suffix[] = "binder";
    static const char binder_private_suffix[] = "binder-private";
    static const char private_suffix[] = "-private";

    if (path && g_str_has_prefix(path, "/dev/") &&
        (g_str_has_suffix(path, binder_suffix) ||
         g_str_has_suffix(path, binder_private_suffix))) {
        TestBinderFd* fd;
        TestBinderNode* node;

        G_LOCK(test_binder);
        node = test_node_map ? g_hash_table_lookup(test_node_map, path) : NULL;
        if (!node) {
            int i, fds[2];
            TestBinder* binder = g_new0(TestBinder, 1);

            binder->io = &test_io_64;
            g_mutex_init(&binder->mutex);
            g_assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

            if (g_str_has_suffix(path, binder_suffix)) {
                node = binder->node + PUBLIC;
                node->path = g_strdup(path);
                binder->node[PRIVATE].path = g_strconcat(path,
                    private_suffix, NULL);
            } else {
                node = binder->node + PRIVATE;
                node->path = g_strdup(path);
                binder->node[PUBLIC].path = g_strndup(path,
                    strlen(path) - strlen(private_suffix) - 1);
            }

            if (!test_node_map) {
                test_node_map = g_hash_table_new(g_str_hash, g_str_equal);
            }
            for (i = 0; i < 2; i++) {
                TestBinderNode* this_node = binder->node + i;

                g_mutex_init(&this_node->mutex);
                this_node->binder = binder;
                this_node->fd = fds[i];
                this_node->other = binder->node + ((i + 1) % 2);
                g_hash_table_replace(test_node_map, this_node->path, this_node);
            }
            binder->object_map = g_hash_table_new
                (g_direct_hash, g_direct_equal);
            binder->handle_map = g_hash_table_new
                (g_direct_hash, g_direct_equal);
            GDEBUG("Created %s (%d) <=> %s (%d) binder",
                binder->node[0].path, binder->node[0].fd,
                binder->node[1].path, binder->node[1].fd);
        }
        fd = test_binder_fd_new(node);
        if (!test_fd_map) {
            test_fd_map = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                NULL, test_fd_map_free);
        }
        g_hash_table_replace(test_fd_map, GINT_TO_POINTER(fd->fd), fd);
        G_UNLOCK(test_binder);

        return fd->fd;
    } else {
        errno = ENOENT;
        return -1;
    }
}

int
gbinder_system_close(
    int fd)
{
    int ret;

    G_LOCK(test_binder);
    if (g_hash_table_remove(test_fd_map, GINT_TO_POINTER(fd))) {
        if (!g_hash_table_size(test_fd_map)) {
            g_hash_table_unref(test_fd_map);
            test_fd_map = NULL;
        }
        ret = 0;
    } else {
        errno = EBADF;
        ret = -1;
    }
    G_UNLOCK(test_binder);

    return ret;
}

int
gbinder_system_ioctl(
    int fd,
    int request,
    void* data)
{
    TestBinderFd* binder_fd = test_binder_fd_from_fd(fd);

    if (binder_fd) {
        TestBinder* binder = binder_fd->node->binder;
        const TestBinderIo* io = binder->io;

        switch (request) {
        case BINDER_VERSION:
            return test_binder_ioctl_version(binder, data);
        case BINDER_SET_MAX_THREADS:
            return 0;
        default:
            if (request == io->write_read_request) {
                return io->handle_write_read(binder_fd, data);
            } else {
                errno = EINVAL;
                return -1;
            }
        }
    }
    errno = EBADF;
    return -1;
}

void*
gbinder_system_mmap(
   size_t length,
   int prot,
   int flags,
   int fd)
{
    return g_malloc(length);
}

int
gbinder_system_munmap(
    void* addr,
    size_t length)
{
    g_free(addr);
    return 0;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
