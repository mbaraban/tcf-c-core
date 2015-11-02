/*******************************************************************************
 * Copyright (c) 2007, 2011 Wind River Systems, Inc. and others.
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
 * TCP channel interface
 */

#ifndef D_channel_np
#define D_channel_np

#include <tcf/config.h>
#include <tcf/framework/channel.h>

#if ENABLE_WebSocket
/*
 * Start TCP (Internet) channel listener.
 * On error returns NULL and sets errno.
 */
extern ChannelServer * channel_np_server(PeerServer * server);

/*
 * Connect client side over TCP (Internet domain).
 *
 * On error returns NULL and sets errno.
 */
extern void channel_np_connect(PeerServer * server, ChannelConnectCallBack callback, void * callback_args);

/*
 * Generate SSL certificate to be used with SSL channels.
 */
extern void generate_ssl_certificate(void);

extern void ini_np_channel(void);

#endif /* D_channel_np */
#endif /* ENABLE_WebSocket */
