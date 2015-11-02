/*
 * Copyright (c) 2014 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */
/*
 * This module implements port forwarding service.
 */

#ifndef D_channel_ping
#define D_channel_ping

#include <tcf/framework/context.h>
#include <tcf/framework/protocol.h>

typedef void (*PingTimeoutCallBack)(void * /* callback_data */);
extern int add_channel_ping(Channel * channel, int period, int timeout, PingTimeoutCallBack cb, void * cb_data);

#endif /* D_channel_ping */
