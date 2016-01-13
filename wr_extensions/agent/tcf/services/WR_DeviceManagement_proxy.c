/*
 * Copyright (c) 2013-2014, 2016 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */

/*
 * TCF service filesystem - proxy version.
 *
 */

#include <tcf/config.h>

#include <fcntl.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <process.h> /* for getpid() and the exec..() family */
#elif defined(__APPLE__)
#include <netinet/in.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <ifaddrs.h>
#endif

#include <tcf/framework/mdep-inet.h>
#include <tcf/framework/protocol.h>
#include <tcf/framework/events.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/json.h>
#include <tcf/framework/myalloc.h>
#include <tcf/services/filesystem.h>
#include <tcf/framework/mdep-threads.h>
#include <tcf/framework/mdep-inet.h>
#include <tcf/services/channel_ping.h>
#include "WR_DeviceManagement_proxy.h"

#ifndef DEVICE_SERVER_NAME
#define DEVICE_SERVER_NAME "Helix App Cloud"
#endif

#define        DEVICES        "Devices"
#define        CHANNEL        "Channel"
#define MKSTR(MACRO) MKSTR_FIRST_PASS(MACRO)
#define MKSTR_FIRST_PASS(MACRO) #MACRO

#define REGISTER_RETRY_DELAY	2000000


typedef struct ChannelRedirection {
    Channel * channel;
    char token[256];
    char id[256];
    int redirected;
} ChannelRedirection;

typedef struct ConnectInfo {
    int is_device;
    struct ConnectInfo * next;
    PeerServer * ps;
    Channel * c1;
    int c1_connected;
    char * url;
    char * id;

    /* Device specific connnection parameters */
    ChannelRedirection * redir;
    unsigned int redir_cnt;
    unsigned int redir_max;
    char * user;
    char * platform;

    /* Gateway specific connnection parameters */
    const char ** devices;
} ConnectInfo;

typedef struct ChannelRedirArgs {
    char token[256];
    char id[256];
    ConnectInfo * info;
} ChannelRedirArgs;

static Protocol * proto;
static TCFBroadcastGroup * broadcast_group;
static ConnectInfo * dev_mgr_connections;

static int channel_ping = 0;
static int channel_ping_period = 0;
static int channel_ping_timeout = 0;

static void device_mgr_connect_done(void * args, int error, Channel * c);
static int get_mac_address(unsigned char ** mac_address);

static void abort_connect(ConnectInfo * info) {
    Channel * c;
    if (info == NULL) return;

    if (dev_mgr_connections == info) dev_mgr_connections = info->next;
    else {
        ConnectInfo * x;
        for (x = dev_mgr_connections; x != NULL && x->next != info; x = x->next)
            ;
        assert (x != NULL && x->next == info);
        x->next = info->next;
    }
    if (info->ps) peer_server_free(info->ps);
    free(info->id);
    free(info->url);
    free(info->user);
    free(info->platform);
    if (info->c1) {
    	c = info->c1;
        protocol_release(c->protocol);
	info->c1 = NULL;
        channel_unlock(c);
        if (!is_channel_closed(c)) channel_close(c);
    }
    free(info);
    info = NULL;
}

static void device_mgr_reconnect_event(void * args) {
    ConnectInfo * info = (ConnectInfo *)args;
    channel_connect(info->ps, device_mgr_connect_done, info);
}

static void device_mgr_reconnect(ConnectInfo * info) {
    if (info == NULL) return;
    if (info->c1) {
        protocol_release(info->c1->protocol);
        channel_unlock(info->c1);
        if (!is_channel_closed(info->c1)) channel_close(info->c1);
        info->c1 = NULL;
    }
    info->c1_connected = 0;
    post_event_with_delay(device_mgr_reconnect_event, info, REGISTER_RETRY_DELAY);
}

static void validate_tcfcmd(Channel * c, void * args, int error) {
    Trap trap;
    ConnectInfo * info = (ConnectInfo *)args;

    if (set_trap(&trap)) {
        if (!error) {
            error = read_errno(&c->inp);

            json_test_char(&c->inp, MARKER_EOA);
            json_test_char(&c->inp, MARKER_EOM);
        }
        clear_trap(&trap);
    } else {
        error = trap.error;
    }
    if (error) {
        channel_close(c);
    } else {
        fprintf(stderr, "Connection with %s server has been established\n", DEVICE_SERVER_NAME);
	info->c1_connected = 1;
    }
}

static void send_register_device_cmd(Channel * c, ConnectInfo * info) {
    protocol_send_command(c, DEVICES, "register", (ReplyHandlerCB) validate_tcfcmd, info);
    write_stream(&c->out, '{');
    json_write_string(&c->out, "ID");
    write_stream(&c->out, ':');
    json_write_string(&c->out, info->id);

    write_stream(&c->out, ',');
    json_write_string(&c->out, "PortRedirections");
    write_stream(&c->out, ':');
    write_stream(&c->out, '[');
    write_stream(&c->out, '{');
    json_write_string(&c->out, "Name");
    write_stream(&c->out, ':');
    json_write_string(&c->out, "ssh");
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Port");
    write_stream(&c->out, ':');
    json_write_ulong(&c->out, 22);
    write_stream(&c->out, '}');
    write_stream(&c->out, ',');
    write_stream(&c->out, '{');
    json_write_string(&c->out, "Name");
    write_stream(&c->out, ':');
    json_write_string(&c->out, "telnet");
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Port");
    write_stream(&c->out, ':');
    json_write_ulong(&c->out, 23);
    write_stream(&c->out, '}');
    write_stream(&c->out, ']');
    if (info->platform) {
        write_stream(&c->out, ',');
        json_write_string(&c->out, "PlatformID");
        write_stream(&c->out, ':');
        json_write_string(&c->out, info->platform);
    }
    if (info->user) {
        write_stream(&c->out, ',');
        json_write_string(&c->out, "User");
        write_stream(&c->out, ':');
        json_write_string(&c->out, info->user);
    }
    write_stream(&c->out, '}');
    write_stream(&c->out, MARKER_EOA);
    write_stream(&c->out, MARKER_EOM);
}

static void send_register_gateway_cmd(Channel * c, ConnectInfo * info) {
    const char ** device;
    int cnt = 0;
    protocol_send_command(c, DEVICES, "register", (ReplyHandlerCB) validate_tcfcmd, info);
    write_stream(&c->out, '{');
    json_write_string(&c->out, "ID");
    write_stream(&c->out, ':');
    json_write_string(&c->out, info->id);
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Gateway");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Devices");
    write_stream(&c->out, ':');
    write_stream(&c->out, '[');
    for (device = info->devices; (device != NULL) && (*device != (char *)NULL); device++) {
        const char * s = *device;
        char * value;
        char * name;
        const char * ptr;
        if (cnt > 0) write_stream(&c->out, ',');
        write_stream(&c->out, '{');
        while (*s && *s != ',') s++;
        json_write_string(&c->out, "Config");
        write_stream(&c->out, ':');
        value = loc_strndup(*device, s - *device);
        json_write_string(&c->out, value);
        loc_free(value);
        while (*s == ',') {
            s++;
            ptr = s;
            while (*s && *s != '=') s++;
            if (*s != '=' || s == ptr) {
                s = ptr - 1;
                break;
            }
            name = loc_strndup(ptr, s - ptr);
            s++;
            ptr = s;
            while (*s && *s != ',') s++;
            value = loc_strndup(ptr, s - ptr);

            write_stream(&c->out, ',');
            json_write_string(&c->out,name);
            write_stream(&c->out, ':');
            json_write_string(&c->out, value);
            loc_free(name);
            loc_free(value);
        }
        write_stream(&c->out, '}');
        cnt++;
    }
    write_stream(&c->out, ']');
    write_stream(&c->out, '}');
    write_stream(&c->out, MARKER_EOA);
    write_stream(&c->out, MARKER_EOM);
}

static void channel_connected(Channel * c) {
    int i;
    int service_device = 0;
    ConnectInfo * info;

    /* install ping in order to reconnect when the channel is closed */
    if (channel_ping) {
         if (add_channel_ping(c, channel_ping_period, channel_ping_timeout, NULL, NULL) == -1)
             fprintf(stderr, "Fail to start ping from device to device manager");
    }
    for (info = dev_mgr_connections; info != NULL && info->c1 != c; info = info->next);

    if (info) {
        for (i = 0; i < c->peer_service_cnt; i++) {
            char * nm = c->peer_service_list[i];
            if (strcmp(nm, DEVICES) == 0) {
                service_device = 1;
                break;
            }
        }
        if (service_device == 0) {
            /* The remote TCF server does not provide necessary features.
             * Try to connect again later on in case the remote TCF 
             * server is restarted or replaced with correct version.
             */
            channel_close(c);
        }
        else {
            if (info->id == NULL) {
                unsigned char * mac_address = NULL;
#ifndef	_WRS_KERNEL
                int pid = getpid();
#else
                int pid = 0;
#endif
                if (get_mac_address(&mac_address) == -1) {
                    fprintf (stderr, "Error reading target MAC address.\n");
                    channel_close(c);
                    return;
                }
                info->id = malloc(32);
                snprintf(info->id, 32, "%02x:%02x:%02x:%02x:%02x:%02x_%x", mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4], mac_address[5], pid);
                free(mac_address);
            }
            if (info->is_device) send_register_device_cmd(c, info);
            else send_register_gateway_cmd(c, info);
	}
    }
}

static void channel_disconnected(Channel * c) {
    ConnectInfo * info;
    for (info = dev_mgr_connections; info != NULL && info->c1 != c; info = info->next);
    if (info) {
        if (info->c1_connected) {
	    fprintf (stderr, "Connection with %s server has been lost\n", DEVICE_SERVER_NAME);
            fprintf(stderr, "Reconnecting to %s server...\n", DEVICE_SERVER_NAME);
        }
        device_mgr_reconnect(info);
    }
}

static void send_event_connected_channel(Channel * channel, ChannelRedirection * redir) {
    OutputStream * out = &channel->out;

    write_stringz(out, "E");
    write_stringz(out, CHANNEL);
    write_stringz(out, "connected");

    json_write_string(out, redir->id);
    write_stream(out, 0);
    write_stream(out, MARKER_EOM);


    channel->state = ChannelStateHelloSent;

    /* Notify listeners that the state of the target channel has
     * changed, to give them a chance to cleanup and be ready
     * for the upcoming channel redirection listener callback in
     * proxy_connected() when target hello message arrives.
     * This is required for the context proxy to be cleanup when the
     * ValueAdd is redirected twice to get access to the target agent.
     * Otherwise, it keeps the list of contexts of the first
     * redirection. */
    notify_channel_closed(channel);
}

static void channel_redir_connected(Channel * c) {
    int error = 0;
    unsigned int i;
    ConnectInfo * info;
    for (info = dev_mgr_connections; info != NULL; info = info->next) {
        if (!info->is_device) continue;
        for (i = 0; i < info->redir_cnt; i++) {
            if (info->redir[i].channel == c) break; 
        }
        if (i != info->redir_cnt) break;
    }
    if (info == NULL || info->c1 == NULL) return;
    if (info->redir[i].redirected) return;
    info->redir[i].redirected = 1;

    write_stringz(&info->c1->out, "R");
    write_stringz(&info->c1->out, info->redir[i].token);
    write_errno(&info->c1->out, error);
    write_stream(&info->c1->out, 0);
    write_stream(&info->c1->out, MARKER_EOM);

    send_event_connected_channel(c, &info->redir[i]);
}

static void channel_redir_disconnected(Channel * c) {
    unsigned int i;
    ConnectInfo * info;
    for (info = dev_mgr_connections; info != NULL; info = info->next) {
        if (!info->is_device) continue;
        for (i = 0; i < info->redir_cnt; i++) {
            if (info->redir[i].channel == c) break; 
        }
        if (i != info->redir_cnt) break;
    }
    if (info == NULL) return;
    info->redir_cnt--;
    for (; i < info->redir_cnt; i++) {
        info->redir[i] = info->redir[i+1];
    }
    protocol_release(c->protocol);
    channel_unlock(c);
    if (!is_channel_closed(c)) channel_close(c);
}

static void device_mgr_connect_done(void * args, int error, Channel * c) {
    ConnectInfo * info = (ConnectInfo *)args;
    assert (info->c1 == NULL);
    if (error) {
        if (error == ERR_INV_TRANSPORT) {
            fprintf(stderr, "Invalid Device Manager URL provided. Abort connection\n");
            abort_connect(info);
            return;
        }
        device_mgr_reconnect(info);
    }
    else {
        c->connected = channel_connected;
        c->disconnected = channel_disconnected;
        c->protocol = proto;
        protocol_reference(proto);
        channel_set_broadcast_group(c, broadcast_group);
        channel_start(c);
        channel_lock(c);
        info->c1 = c;
    }
}
static void redir_connect_done(void * args, int error, Channel * c) {
    ChannelRedirArgs * redir_args = args;
    ConnectInfo * info = redir_args->info;

    if (info->c1 == NULL) {
        /* Connection with Device Manager has been lost; abort */
        loc_free(args);
	return;
    }

    /* This is a redirection request */
    if (error) {
        write_stringz(&info->c1->out, "R");
        write_stringz(&info->c1->out, redir_args->token);
        write_errno(&info->c1->out, error);
        write_stream(&info->c1->out, 0);
        write_stream(&info->c1->out, MARKER_EOM);
    } else {
        info->redir_cnt ++;
        if (info->redir_cnt > info->redir_max) info->redir_max = info->redir_cnt;
        info->redir = loc_realloc(info->redir, info->redir_max * sizeof(ChannelRedirection));
        info->redir[info->redir_cnt - 1].channel = c;
        info->redir[info->redir_cnt - 1].redirected = 0;
        strcpy (info->redir[info->redir_cnt - 1].token, redir_args->token);
        strcpy (info->redir[info->redir_cnt - 1].id, redir_args->id);
        c->connected = channel_redir_connected;
        c->disconnected = channel_redir_disconnected;
        c->protocol = proto;
        protocol_reference(proto);
        channel_set_broadcast_group(c, broadcast_group);
        channel_start(c);
        channel_lock(c);
    }
    loc_free(redir_args);
}

static void device_mgr_connect (void * args) {
    ConnectInfo * info = (ConnectInfo *)args;
    if (info == NULL || info->url == NULL) return;
    assert (info->ps == NULL);
    if (info->ps != NULL) return;
    info->ps = channel_peer_from_url(info->url);
    if (info->ps == NULL) {
        fprintf(stderr, "Invalid Device Manager URL provided. Abort connection...\n");
        abort_connect(info);
        return;
    }
    info->next = dev_mgr_connections;
    dev_mgr_connections = info;
    fprintf(stderr, "Connecting to %s server...\n", DEVICE_SERVER_NAME);
    channel_connect(info->ps, device_mgr_connect_done, info);
}

static void redir_connect (void * args) {
    ConnectInfo * info = ((ChannelRedirArgs *)args)->info;
    assert (info != NULL && info->ps != NULL);
    if (info == NULL || info->ps == NULL) return;
    channel_connect(info->ps, redir_connect_done, args);
}

static int get_mac_address(unsigned char ** mac_address) {
#ifdef  WIN32
    PIP_ADAPTER_INFO pAdapterInfo;
    PIP_ADAPTER_INFO pAdapter = NULL;
    DWORD dwRetVal = 0;
    int success = 0;
    ULONG ulOutBufLen = sizeof (IP_ADAPTER_INFO);
    pAdapterInfo = (IP_ADAPTER_INFO *) malloc(sizeof (IP_ADAPTER_INFO));
    if (pAdapterInfo == NULL) {
        return -1;
    }
    // Make an initial call to GetAdaptersInfo to get
    // the necessary size into the ulOutBufLen variable
    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *) malloc(ulOutBufLen);
        if (pAdapterInfo == NULL) {
            return -1;
        }
    }

    if ((dwRetVal = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
        pAdapter = pAdapterInfo;
        while (pAdapter) {
            if (pAdapter->Type == MIB_IF_TYPE_ETHERNET) {
                *mac_address = malloc(6);
                memcpy(*mac_address, pAdapter->Address, 6);
                success = 1;
                break;
            }
            pAdapter = pAdapter->Next;
        }
    }
    if (pAdapterInfo)
        free(pAdapterInfo);
    if (success) return 0;
    else return -1;
#elif defined(__linux__)
    struct ifreq ifr;
    struct ifconf ifc;
    struct ifreq * it;
    const struct ifreq * end;
    char buf[1024];
    int success = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        return -1;
    }

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1) {
        close (sock);
        return -1;
    }

    it = ifc.ifc_req;
    end = it + (ifc.ifc_len / sizeof(struct ifreq));

    for (; it != end; ++it) {
        strcpy(ifr.ifr_name, it->ifr_name);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (! (ifr.ifr_flags & IFF_LOOPBACK)) {
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
		    *mac_address = malloc(6);
		    memcpy(*mac_address, ifr.ifr_ifru.ifru_addr.sa_data, 6);
                    success = 1;
                    break;
                }
            }
        }
    }

    close (sock);
    if (success) return 0;
    else return -1;
#elif defined(__APPLE__)
    struct ifaddrs * iflist, * iface;
    int success = 0;
    if (getifaddrs (&iflist) == -1) return -1;
    for (iface = iflist; iface; iface = iface->ifa_next) {
        if ((iface->ifa_addr->sa_family == AF_LINK) &&
                iface->ifa_addr) {
            struct sockaddr_dl * sdl = (struct sockaddr_dl *)iface->ifa_addr;
            if (sdl->sdl_type != IFT_ETHER) continue;
            if (iface->ifa_flags & IFF_LOOPBACK) continue;
            *mac_address = malloc(sdl->sdl_alen);
            memcpy(*mac_address, LLADDR(sdl), sdl->sdl_alen);
            success = 1;
            break;
        }
    }
    freeifaddrs (iflist);
    if (success) return 0;
    else return -1;
#else
    return -1;
#endif
}

int deviceRegister(const char * url, const char * user, const char * id, const char * platform) {
    ConnectInfo * info;
    if (url == NULL) {
        printf ("Usage: deviceRegister(<device id>, [<user>], [<device id>], [<platform id>], [<peer url>])\n");
        return -1;
    }
    for (info = dev_mgr_connections; info != NULL; info = info->next) {
        if (info->is_device) break;
    }
    if (info != NULL) return -1;

    info = (ConnectInfo *)calloc(1, sizeof(ConnectInfo));

    if (id != NULL) info->id = strdup(id);
    if (user != NULL) info->user = strdup(user);
    if (platform != NULL) info->platform = strdup(platform);
#ifdef	VSB_DIR
    else info->platform = strdup(MKSTR(VSB_DIR));
#endif

    info->url = strdup(url);
    info->c1 = NULL;
    info->is_device = 1;
    post_event(device_mgr_connect, info);
    return 0;
}

int gatewayRegister(const char * url, const char * id, const char ** devices) {
    ConnectInfo * info;
    if (url == NULL) {
        fprintf (stderr, "Usage: gatewayRegister(<gateway url>, <gateway id>, <devices>");
        return -1;
    }
    for (info = dev_mgr_connections; info != NULL; info = info->next) {
        if (!info->is_device) break;
    }
    if (info != NULL) return -1;

    info = (ConnectInfo *)calloc(1, sizeof(ConnectInfo));

    if (id != NULL) info->id = strdup(id);
    info->devices = devices;
    info->url = strdup(url);
    info->c1 = NULL;
    post_event(device_mgr_connect, info);
    return 0;
}

static void command_connect(char * token, Channel * c) {
    char id [256];
    char url [256];
    int error = 0;
    ConnectInfo * info;

    json_read_string(&c->inp, id, sizeof(id));
    json_read_string(&c->inp, url, sizeof(url));
    json_test_char(&c->inp, MARKER_EOA);
    json_test_char(&c->inp, MARKER_EOM);

    for (info = dev_mgr_connections; info != NULL; info = info->next) {
        if (info->is_device) break;
    }
    if (info == NULL || info->c1 != c) {
    	error = set_errno(ERR_OTHER, "Invalid channel");
    }
    
    if (error == 0) {
        ChannelRedirArgs * args = loc_alloc_zero(sizeof(ChannelRedirArgs));
	strcpy(args->id, id);
	strcpy(args->token, token);
        args->info = info;
	post_event(redir_connect, args);
    } else {
	write_stringz(&c->out, "R");
	write_stringz(&c->out, token);
	write_errno(&c->out, error);
	write_stream(&c->out, 0);
	write_stream(&c->out, MARKER_EOM);
    }
}

void ini_device_mgt_lib(Protocol * protocol, TCFBroadcastGroup * bcg, int set_channel_ping, int period, int timeout) {
    proto = protocol;
    broadcast_group = bcg;
    channel_ping = set_channel_ping;
    channel_ping_period = period;
    channel_ping_timeout = timeout; 
    add_command_handler(proto, CHANNEL, "connect", command_connect);
}
