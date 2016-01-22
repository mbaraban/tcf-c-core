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

#ifndef WR_DEVICEMGT_PROXY_H
#define WR_DEVICEMGT_PROXY_H

#include <tcf/framework/channel.h>


extern int deviceRegister(const char * url, const char * user, const char * id, const char * platform);
extern int gatewayRegister(const char * url, const char * id, const char ** devices);
extern void ini_device_mgt_lib(Protocol * protocol, TCFBroadcastGroup * bcg, int set_channel_ping, int period, int timeout);

#endif /* WR_DEVICEMGT_PROXY_H */
