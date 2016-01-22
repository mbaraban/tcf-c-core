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

#ifndef D_channel_ping
#define D_channel_ping

#include <tcf/framework/context.h>
#include <tcf/framework/protocol.h>

typedef void (*PingTimeoutCallBack)(void * /* callback_data */);
extern int add_channel_ping(Channel * channel, int period, int timeout, PingTimeoutCallBack cb, void * cb_data);

#endif /* D_channel_ping */
