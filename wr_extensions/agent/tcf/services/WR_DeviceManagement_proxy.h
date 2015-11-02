/*
 * Copyright (c) 2013-2014 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */

/*
 * TCF service filesystem - proxy version.
 *
 */

#ifndef WR_DEVICEMGT_PROXY_H
#define WR_DEVICEMGT_PROXY_H

#include <tcf/framework/channel.h>


extern int deviceRegister(const char * url, const char * user, const char * id, const char * platform);
extern int gatewayRegister(const char * url, const char * id, const char ** devices);
extern void ini_device_mgt_lib(Protocol * protocol, TCFBroadcastGroup * bcg, int set_channel_ping, int period, int timeout);

#endif /* WR_DEVICEMGT_PROXY_H */
