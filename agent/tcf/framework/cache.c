/*******************************************************************************
 * Copyright (c) 2009, 2014 Wind River Systems, Inc. and others.
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
 *******************************************************************************/

/*
 * Abstract asynchronous data cache support.
 */

#include <tcf/config.h>
#include <assert.h>
#include <string.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/events.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/cache.h>

typedef struct WaitingCacheClient {
    unsigned id;
    CacheClient * client;
    Channel * channel;
    void * args;
    size_t args_size;
    int args_copy;
#ifndef NDEBUG
    time_t time_stamp;
    const char * file;
    int line;
#endif
} WaitingCacheClient;

static WaitingCacheClient current_client = {0, 0, 0, 0, 0, 0};
static int client_exited = 0;
static int cache_miss_cnt = 0;
static WaitingCacheClient * wait_list_buf;
static unsigned wait_list_max;
static unsigned id_cnt = 0;
static LINK cache_list = TCF_LIST_INIT(cache_list);
static Channel * def_channel = NULL;
static const char * channel_lock_msg = "Cache client lock";

static CacheTransactionListener ** listeners = NULL;
static unsigned listeners_cnt = 0;
static unsigned listeners_max = 0;

#define link_all2cache(x) ((AbstractCache *)((char *)(x) - offsetof(AbstractCache, link)))

#ifndef NDEBUG
/* Print cache items that are waiting too long to be filled.
 * In most cases such items indicate a bug in the agent code. */
static int cache_timer_posted = 0;

static void cache_timer(void * x) {
    LINK * l;
    time_t time_now = time(NULL);

    assert(cache_timer_posted);
    cache_timer_posted = 0;
    for (l = cache_list.next; l != &cache_list; l = l->next) {
        unsigned i;
        AbstractCache * cache = link_all2cache(l);
        assert(cache->wait_list_cnt > 0);
        for (i = 0; i < cache->wait_list_cnt; i++) {
            WaitingCacheClient * client = cache->wait_list_buf + i;
            if (time_now - client->time_stamp >= 30) {
                /* Client is waiting longer than 30 sec - it might be a bug */
                trace(LOG_ALWAYS, "Stalled cache at %s:%d", client->file, client->line);
            }
        }
    }
    if (!list_is_empty(&cache_list)) {
        post_event_with_delay(cache_timer, NULL, 5000000);
        cache_timer_posted = 1;
    }
}
#endif

static void run_cache_client(int retry) {
    Trap trap;
    unsigned i;

    cache_miss_cnt = 0;
    client_exited = 0;
    def_channel = NULL;
    for (i = 0; i < listeners_cnt; i++) listeners[i](retry ? CTLE_RETRY : CTLE_START);
    if (set_trap(&trap)) {
        current_client.client(current_client.args);
        clear_trap(&trap);
        assert(cache_miss_cnt == 0);
        assert(client_exited);
    }
    else if (get_error_code(trap.error) != ERR_CACHE_MISS || client_exited || cache_miss_cnt == 0) {
        trace(LOG_ALWAYS, "Unhandled exception in data cache client: %d %s", trap.error, errno_to_str(trap.error));
    }
    for (i = 0; i < listeners_cnt; i++) listeners[i](client_exited ? CTLE_COMMIT : CTLE_ABORT);
    if (cache_miss_cnt == 0 && current_client.args_copy) loc_free(current_client.args);
    memset(&current_client, 0, sizeof(current_client));
    cache_miss_cnt = 0;
    client_exited = 0;
    def_channel = NULL;
}

void cache_enter(CacheClient * client, Channel * channel, void * args, size_t args_size) {
    assert(is_dispatch_thread());
    assert(client != NULL);
    assert(current_client.id == 0);
    assert(current_client.client == NULL);
    current_client.id = id_cnt++;
    if (current_client.id == 0) current_client.id = id_cnt++;
    current_client.client = client;
    current_client.channel = channel;
    current_client.args = args;
    current_client.args_size = args_size;
    current_client.args_copy = 0;
    run_cache_client(0);
}

void cache_exit(void) {
    assert(is_dispatch_thread());
    assert(current_client.client != NULL);
    assert(!client_exited);
    if (cache_miss_cnt > 0) exception(ERR_CACHE_MISS);
    client_exited = 1;
}

#ifdef NDEBUG
void cache_wait(AbstractCache * cache) {
#else
void cache_wait_dbg(const char * file, int line, AbstractCache * cache) {
#endif
    assert(is_dispatch_thread());
    assert(client_exited == 0);
    if (current_client.client != NULL && cache_miss_cnt == 0) {
        if (cache->wait_list_cnt >= cache->wait_list_max) {
            cache->wait_list_max += 8;
            cache->wait_list_buf = (WaitingCacheClient *)loc_realloc(cache->wait_list_buf, cache->wait_list_max * sizeof(WaitingCacheClient));
        }
        if (current_client.args != NULL && !current_client.args_copy) {
            void * mem = loc_alloc(current_client.args_size);
            memcpy(mem, current_client.args, current_client.args_size);
            current_client.args = mem;
            current_client.args_copy = 1;
        }
#ifndef NDEBUG
        current_client.file = file;
        current_client.line = line;
        current_client.time_stamp = time(NULL);
        if (!cache_timer_posted) {
            post_event_with_delay(cache_timer, NULL, 5000000);
            cache_timer_posted = 1;
        }
#endif
        if (cache->wait_list_cnt == 0) list_add_last(&cache->link, &cache_list);
        if (current_client.channel != NULL) channel_lock_with_msg(current_client.channel, channel_lock_msg);
        cache->wait_list_buf[cache->wait_list_cnt++] = current_client;
    }
#ifndef NDEBUG
    else if (current_client.client == NULL) {
        trace(LOG_ALWAYS, "Illegal cache access at %s:%d", file, line);
    }
#endif
    cache_miss_cnt++;
    exception(ERR_CACHE_MISS);
}

void cache_notify(AbstractCache * cache) {
    unsigned i;
    unsigned cnt = cache->wait_list_cnt;

    assert(is_dispatch_thread());
    if (cnt == 0) return;
    list_remove(&cache->link);
    cache->wait_list_cnt = 0;
    if (wait_list_max < cnt) {
        wait_list_max = cnt;
        wait_list_buf = (WaitingCacheClient *)loc_realloc(wait_list_buf, cnt * sizeof(WaitingCacheClient));
    }
    memcpy(wait_list_buf, cache->wait_list_buf, cnt * sizeof(WaitingCacheClient));
    for (i = 0; i < cnt; i++) {
        current_client = wait_list_buf[i];
        run_cache_client(1);
        if (wait_list_buf[i].channel != NULL) channel_unlock_with_msg(wait_list_buf[i].channel, channel_lock_msg);
    }
}

static void cache_notify_event(void * args) {
    unsigned i;
    WaitingCacheClient * buf = (WaitingCacheClient *)args;
    for (i = 0; buf[i].client != NULL; i++) {
        current_client = buf[i];
        run_cache_client(1);
        if (buf[i].channel != NULL) channel_unlock_with_msg(buf[i].channel, channel_lock_msg);
    }
    loc_free(buf);
}

void cache_notify_later(AbstractCache * cache) {
    unsigned cnt = cache->wait_list_cnt;
    unsigned max = cache->wait_list_max;
    WaitingCacheClient * buf = cache->wait_list_buf;

    assert(is_dispatch_thread());
    if (cnt == 0) return;
    list_remove(&cache->link);
    cache->wait_list_buf = NULL;
    cache->wait_list_cnt = 0;
    cache->wait_list_max = 0;
    if (max <= cnt) {
        max = cnt + 1;
        buf = (WaitingCacheClient *)loc_realloc(buf, max * sizeof(WaitingCacheClient));
    }
    memset(buf + cnt, 0, sizeof(WaitingCacheClient));
    post_event(cache_notify_event, buf);
}

Channel * cache_channel(void) {
    if (current_client.channel != NULL) return current_client.channel;
    if (current_client.client != NULL) return def_channel;
    return NULL;
}

void cache_set_def_channel(Channel * channel) {
    def_channel = channel;
}

unsigned cache_transaction_id(void) {
    return current_client.id;
}

unsigned cache_miss_count(void) {
    return cache_miss_cnt;
}

void add_cache_transaction_listener(CacheTransactionListener * l) {
    if (listeners_cnt >= listeners_max) {
        listeners_max += 8;
        listeners = (CacheTransactionListener **)loc_realloc(listeners,
            sizeof(CacheTransactionListener *) * listeners_max);
    }
    listeners[listeners_cnt++] = l;
}

void cache_dispose(AbstractCache * cache) {
    assert(is_dispatch_thread());
    assert(cache->wait_list_cnt == 0);
    assert(list_is_empty(&cache->link));
    loc_free(cache->wait_list_buf);
    memset(cache, 0, sizeof(*cache));
}
