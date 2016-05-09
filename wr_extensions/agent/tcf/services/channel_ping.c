/*******************************************************************************
 * Copyright (c) 2015, 2016 Wind River Systems, Inc. and others.
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

#include <stdio.h>
#include <tcf/framework/proxy.h>
#include <tcf/framework/channel.h>
#include <tcf/framework/trace.h>
#include <tcf/services/filesystem.h>
#include <tcf/services/linenumbers.h>
#include <tcf/services/symbols.h>
#include <tcf/framework/json.h>
#include <tcf/framework/events.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/myalloc.h>
#include <tcf/services/channel_ping.h>

typedef struct ChannelPing {
    LINK link;
    Channel * channel;
    int timeout;
    int period;
    PingTimeoutCallBack cb;
    void * cb_data;
} ChannelPing;

static LINK channels_ping_list = TCF_LIST_INIT(channels_ping_list);
#define link2channelping(A)  ((ChannelPing *)((char *)(A) - offsetof(ChannelPing, link)))

static void channel_ping_cb(Channel * channel, void * client_data, int error);

static ChannelPing * find_channel(Channel * channel) {
    LINK * qhp = &channels_ping_list;
    LINK * qp = qhp->next;

    while (qp != qhp) {
        ChannelPing * channel_ping = link2channelping(qp);
	if (channel_ping->channel == channel) return channel_ping;
        qp = qp->next;
    }
    return NULL;
}

static void event_channel_closed(Channel * c) {
    ChannelPing * channel_ping = find_channel(c);
    if (channel_ping == NULL) return;
    channel_unlock(channel_ping->channel);
    list_remove(&channel_ping->link);
    loc_free(channel_ping);
}

/* ping timeout callback. When this callback is called, a ping timeout
 * has been reached on the channel. Close the channel to prevent clients
 * from blocking on this connection.
 */
static void channel_ping_timeout_cb(void * client_data) {
    Channel * channel = client_data;
    ChannelPing * channel_ping = find_channel(channel);
    if (channel_ping == NULL) return;
    if (channel_ping->cb != NULL)
        channel_ping->cb (channel_ping->cb_data);
    channel_close(channel);
}

/* callback used to send the ping request to the channel. Once the
 * request has been sent to the channel, this routine also post a 
 * callback with a delay to detect a ping timeout; this callback 
 * will be canceled if the ping request is received meanwhile
 */

static void channel_ping_post_event_cb(void * client_data) {
    Channel * channel = client_data;
    ChannelPing * channel_ping;
    channel_ping = find_channel(channel);
    if (channel_ping == NULL) return;
    protocol_send_command(channel, "Locator", "sync", channel_ping_cb, channel);
    write_stream(&channel->out, MARKER_EOM);
    post_event_with_delay(channel_ping_timeout_cb, channel, channel_ping->timeout); 
}

/* ping request callback. This callback will be called when we receive the
 * ping request answer or when the channel is closed (because the connection
 * is closed or because of a redirect call). If the channel is closed, 
 * abort any ping request; otherwise cancel ping timeout callback and post
 * (with a delay) another ping request.
 */

static void channel_ping_cb(Channel * channel, void * client_data, int error) {
    Trap trap;
    ChannelPing * channel_ping = find_channel(channel);
    if (channel_ping == NULL) return;
    if (error) {
        cancel_event(channel_ping_timeout_cb, channel, 0);
        return;
    }
    cancel_event(channel_ping_timeout_cb, channel, 0);
    if (set_trap(&trap)) {
        read_errno(&channel->inp);
        json_test_char(&channel->inp, MARKER_EOM);
        clear_trap(&trap);
        post_event_with_delay(channel_ping_post_event_cb, channel, channel_ping->timeout);
    }
}

int add_channel_ping(Channel * channel, int period, int timeout, PingTimeoutCallBack cb, void * cb_data) {
    int i;
    int service_loc = 0;
    static int ini_channel_ping = 0;

    if (find_channel(channel) != NULL) return -1;

    if (ini_channel_ping == 0) {
        add_channel_close_listener(event_channel_closed);
	ini_channel_ping = 1;
    }
    
    if (timeout == 0) return 0;
    if (period == 0) period = 2;
    for (i = 0; i < channel->peer_service_cnt; i++) {
        char * nm = channel->peer_service_list[i];
        if (strcmp(nm, "Locator") == 0) {
            service_loc = 1;
        }
    }

    if (service_loc) {
	ChannelPing * args = loc_alloc_zero (sizeof(ChannelPing));
	args->channel = channel;
	args->timeout = timeout * 1000000;
	args->period = period * 1000000;
	args->cb = cb;
	args->cb_data = cb_data;
	channel_lock(channel);
	list_add_last(&args->link, &channels_ping_list);
	post_event_with_delay(channel_ping_post_event_cb, channel, args->timeout);
	return 0;
    }
    return -1;
}
