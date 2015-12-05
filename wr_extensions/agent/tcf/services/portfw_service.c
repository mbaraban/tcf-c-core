/*
 * Copyright (c) 2011-2012 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */


/*
 * Target service implementation: systems (TCF name SystemsService)
 */

#include <tcf/config.h>
#include <tcf/framework/mdep-inet.h>
#include <tcf/framework/mdep-threads.h>
#include <assert.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/context.h>
#include <tcf/services/portfw_service.h>
#include <tcf/framework/errors.h>

#include <tcf/framework/json.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/asyncreq.h>
#include <tcf/services/streamsservice.h>

#if SERVICE_PortForward

#if defined(_WRS_KERNEL)
#define ENABLE_PortForward_Serial       0
#elif defined(WIN32)
#define ENABLE_PortForward_Serial       1
#else
#define ENABLE_PortForward_Serial       1
#endif

#if ENABLE_PortForward_Serial
#if !defined(WIN32)
#include <fcntl.h>
#include <ctype.h>
#if defined(__linux__)
#include <linux/serial.h>
#endif
#include <termios.h>
#endif
#endif

 
#define PORTFW_TCP_PORT         0
#define PORTFW_UDP_PORT         1
#define PORTFW_SER_PORT         2

#define IN_BUF_SIZE     32*1024
#define OUT_BUF_SIZE    32*1024

#define PORTFW_SERIAL_FC_NONE   0
#define PORTFW_SERIAL_FC_XON    1
#define PORTFW_SERIAL_FC_RTS    2
#define PORTFW_SERIAL_FC_DSR    3

#define PORTFW_SERIAL_PARITY_NONE       0
#define PORTFW_SERIAL_PARITY_ODD      1
#define PORTFW_SERIAL_PARITY_EVEN     2
#define PORTFW_SERIAL_PARITY_MARK     3
#define PORTFW_SERIAL_PARITY_SPACE    4

#if ENABLE_PortForward_Serial
typedef struct BaudConst {
    int baud_const;
    int baud_rate;
} BaudConst;

typedef struct DataBitConst
    {
    int data_bits_const;
    int data_bits;
} DataBitConst;

#ifdef WIN32
static BaudConst baud_table[] =
    { { CBR_110, 110 }, { CBR_300, 300 }, { CBR_600, 600 }, { CBR_1200, 1200 },
      { CBR_2400, 2400 }, { CBR_4800, 4800 },
      { CBR_9600, 9600 }, { CBR_14400, 14400},
      { CBR_19200, 19200 }, { CBR_38400, 38400 },
      { CBR_57600, 57600 }, { CBR_115200, 115200 },
      { CBR_128000, 128000 }, { CBR_256000, 256000 },
      { 0, 0 }
    };
#else  /* WIN32 */
static BaudConst baud_table[] =
    { { B150, 150 }, { B300, 300 }, { B600, 600 }, { B1200, 1200 },
      { B1800, 1800 }, { B2400, 2400 }, { B4800, 4800 },
      { B9600, 9600 }, { B19200, 19200 }, { B38400, 38400 },
#ifdef  B57600
      { B57600, 57600 },
#endif
#ifdef  B115200
      { B115200, 115200 },
#endif
#ifdef  B230400
      { B230400, 230400, },
#endif
#ifdef  B46080
      { B460800, 460800 },
#endif
      { 0, 0 }
    };

static DataBitConst data_bit_table[] =
    { {CS8, 8}, {CS7, 7}, {CS6, 6}, {CS5, 5}, {0,0} };
#endif /* WIN32 */
#endif  /* ENABLE_PortForward_Serial */

typedef struct PortFwConfig PortFwConfig;
typedef void (*ConnectCallBack)(PortFwConfig * /* config */, int /* error */);
typedef int (*UserSendReqFunc)(PortFwConfig * /* config */, void * /* buffer */, size_t /* length */);
typedef int (*UserRecvReqFunc)(PortFwConfig * /* config */, void * /* buffer */, size_t /* length */);
typedef void (*UserCloseFunc)(PortFwConfig * /* config */);

typedef struct PortConnectInfo {
    AsyncReqInfo cnct_req;
    ConnectCallBack cnct_callback;

    /* UDP/TCP specific parameters */
    struct sockaddr * addr_buf;
    int addr_len;
    int sock;

    /* Non  network parameters */
    UserSendReqFunc send_req_func;
    UserRecvReqFunc recv_req_func;
    UserCloseFunc close_func;

#if     ENABLE_PortForward_Serial
    /* Serial line specific parameters */

    /* Syntax of the param string for serial line is:
     * <baud rate>[-<data bits>[-<parity>[-<stop bits>[-<flow control>]]]]
     * eg: 9600-8-N-1-N or 9600-8-N-1 or 9600
     */

    struct {
        /* Serial line specific parameters */
        int stop_bits;  /* number of stop bits (1 or 2), 1 is default */
        int data_bits;  /* number of data bits (between 5 & 8), 8 is default */
        int parity;     /* parity, no parity is the default */
        int flow_ctrl;  /* flow control; no flow control is the dfault */
        int baud_rate;  /* baud rate, default to 115200 */
#ifdef WIN32
        HANDLE          handle;         /* serial device handle */
        OVERLAPPED      write_overlapped;       /* overlap struct for write ops  */
        OVERLAPPED      read_overlapped;        /* overlap struct for read ops  */
        COMMTIMEOUTS    saved_timeout;          /* saved serial line device timeout */
        DCB             saved_dcb;              /* saved serial line configuration */
#else
        int fd;
#endif
    } serial;
#endif
} PortConnectInfo;

struct PortFwConfig {
    LINK link;
    char id[256];
    Channel * channel;
    char token[256];
    int auto_connect;
    int verbose;
    int port_type;              /* port type */

    char * port_config;         /* port config string (tcp:<port  number>, udp:<port number>, serial:<device>) */
    char port_name[256];
    char * port_params;         /* port parameters */

    /* UDP/TCP specific parameters */
    unsigned short port;

    int connected;
    VirtualStream * in_vstream;
    VirtualStream * out_vstream;
    char outbuf[OUT_BUF_SIZE];
    size_t outbuf_len;
    char inbuf[IN_BUF_SIZE];
    size_t inbuf_pos;
    size_t inbuf_len;
    AsyncReqInfo send_req;
    int send_in_progress;
    AsyncReqInfo recv_req;
    int recv_in_progress;
    char in_stream_id[256];
    char out_stream_id[256];
    int shutdown_in_progress;

    PortConnectInfo port_info;
};

#define gdbconfl2gdbconfp(A)    ((PortFwConfig *)((char *)(A) - offsetof(PortFwConfig, link)))

static const char * PortForward = "PortForward";

static LINK portfw_config_list = TCF_LIST_INIT(portfw_config_list);
static const char * portfw_remote_inv_config_err = "Unable to find specified PortForward Remote configuration";

static TCFBroadcastGroup * broadcast_group = NULL;

static PortFwConfig * find_portfw_config(const char * name);
static void write_portfw_remote_connection_config(OutputStream * out, PortFwConfig * config);
static void write_port_to_stream(void * arg);
static int log_portfw;

static void set_socket_options(int sock) {
    int snd_buf = OUT_BUF_SIZE;
    int rcv_buf = IN_BUF_SIZE;
    struct linger optval;
    int i = 1;

    /*
     * set SO_LINGER & SO_REUSEADDR socket options so that it closes the
     * connections gracefully, when required to close.
     */

    optval.l_onoff = 1;
    optval.l_linger = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *) &optval,
            sizeof(optval)) != 0) {
        trace(LOG_ALWAYS, "Unable to set SO_LINGER socket option: %s",
                errno_to_str(errno));
    };

#if !(defined(_WIN32) || defined(__CYGWIN__))
    {
        const int i = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &i, sizeof(i))
                < 0) {
            trace(LOG_ALWAYS, "Unable to set SO_REUSEADDR socket option: ",
                    errno_to_str(errno));
        }
    }
#endif

    /* Set TCP_NODELAY socket option to optimize communication */

    i = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &i, sizeof(i))
            < 0) {
        trace(LOG_ALWAYS, "Can't set TCP_NODELAY option on a socket: %s",
                errno_to_str(errno));
    }

    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &i, sizeof(i))
            < 0) {
        trace(LOG_ALWAYS, "Can't set SO_KEEPALIVE option on a socket: %s",
                errno_to_str(errno));
    }

    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *) &snd_buf,
            sizeof(snd_buf)) < 0) {
        trace(LOG_ALWAYS, "setsockopt(SOL_SOCKET,SO_SNDBUF,%d) error: %s",
                snd_buf, errno_to_str(errno));
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *) &rcv_buf,
            sizeof(rcv_buf)) < 0) {
        trace(LOG_ALWAYS, "setsockopt(SOL_SOCKET,SO_RCVBUF,%d) error: %s",
                rcv_buf, errno_to_str(errno));
    }
}

static void tcp_connect_done(void * args) {
    PortFwConfig * config = (PortFwConfig *)((AsyncReqInfo *)args)->client_data;
    PortConnectInfo * info = &config->port_info;

    loc_free(info->cnct_req.u.con.addr);
    if (info->cnct_req.error) {
        info->cnct_callback(config, info->cnct_req.error);
    }
    else {
        config->connected = 1;
        set_socket_options(info->sock);
        info->cnct_callback(config, 0);
    }
}

static int tcp_connect(PortFwConfig * config, ConnectCallBack callback) {
    int error = 0;
    struct addrinfo hints;
    struct addrinfo * reslist = NULL;
    char * dev_string = loc_strdup(config->port_config);
    struct sockaddr * addr_buf = NULL;
    int addr_len;
    int sock;
    char * host_str;
    char * port_str;

    host_str = strchr(dev_string, ':');
    if (host_str == NULL) {
        error = EINVAL;
    }
    else {
        host_str++;
        port_str = strchr(host_str, ':');
        if (port_str == NULL) {
            error = EINVAL;
        }
    }
    if (!error) {
        port_str[0] = '\0';
        port_str++;
        if (strlen(host_str) == 0) host_str = NULL;
    }
    if (!error) {
        memset(&hints, 0, sizeof hints);

        hints.ai_family = PF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        error = loc_getaddrinfo(host_str, port_str, &hints, &reslist);
        if (error) error = set_gai_errno(error);
        if (!error) {
            struct addrinfo * res;
            for (res = reslist; res != NULL; res = res->ai_next) {
                addr_len = res->ai_addrlen;
                addr_buf = (struct sockaddr *)loc_alloc(res->ai_addrlen);
                memcpy(addr_buf, res->ai_addr, res->ai_addrlen);
                sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (sock < 0) {
                    error = errno;
                }
                else {
                    error = 0;
                    break;
                }
            }
            loc_freeaddrinfo(reslist);
        }
    }
    if (!error && addr_buf == NULL) error = ENOENT;
    if (error) {
            callback(config, error);
    }
    else {
        PortConnectInfo * info = &config->port_info;
        info->addr_buf = addr_buf;
        info->addr_len = addr_len;
        info->sock = sock;

        info->cnct_callback = callback;
        info->cnct_req.client_data = config;
        info->cnct_req.done = tcp_connect_done;
        info->cnct_req.type = AsyncReqConnect;
        info->cnct_req.u.con.sock = info->sock;
        info->cnct_req.u.con.addr = info->addr_buf;
        info->cnct_req.u.con.addrlen = info->addr_len;
        async_req_post(&info->cnct_req);
    }
    loc_free(dev_string);
    return 0;
}

static void disconnect_port(PortFwConfig * config) {
    if (config->port_info.sock != -1) {
        /* It seems we need to use shutdown to unblock threads blocked on recv/send */
        if (config->connected) shutdown(config->port_info.sock, SHUT_RDWR);
        if (closesocket(config->port_info.sock) == -1) perror ("closesocket");
        config->port_info.sock = -1;
        if (config->verbose) fprintf(stderr, "Connection with %s closed\n", config->port_name);
    }
    else if (config->port_info.close_func) {
        config->port_info.close_func(config);
    }
    config->connected = 0;
}
#if ENABLE_PortForward_Serial
/* SERIAL LINE SUPPORT */

static int serial_parse_params(PortFwConfig * config) {
    char * ptr;
    char * s;
    char * param = NULL;

    config->port_info.serial.baud_rate = 115200;
    config->port_info.serial.stop_bits = 1;
    config->port_info.serial.data_bits = 8;
    config->port_info.serial.parity = PORTFW_SERIAL_PARITY_NONE;
    config->port_info.serial.flow_ctrl = PORTFW_SERIAL_FC_NONE;

    if (config->port_params == NULL) return 0;
    s = config->port_params;

    /* BAUD RATE */
    while (*s && *s != '-') s++;
    if (s != config->port_params) {
        param = loc_strndup(config->port_params, s - config->port_params);
        config->port_info.serial.baud_rate = atoi(param);
        loc_free(param);
    }

    /* DATA BITS */
    if (*s) s++;
    ptr = s;
    while (*s && *s != '-') s++;
    if (s != ptr) {
        param = loc_strndup(ptr, s - ptr);
        config->port_info.serial.data_bits = atoi(param);
        loc_free(param);
    }


    /* PARITY */
    if (*s) s++;
    ptr = s;
    while (*s && *s != '-') s++;
    if (s != ptr) {
        param = loc_strndup(ptr, s - ptr);
        if (strlen(param) != 1) {
            errno = EINVAL;
            loc_free(param);
            return -1;
        }
        if (param[0] == 'O' || param[0] == 'o') config->port_info.serial.parity = PORTFW_SERIAL_PARITY_ODD;
        else if (param[0] == 'E' || param[0] == 'e') config->port_info.serial.parity = PORTFW_SERIAL_PARITY_EVEN;
        else if (param[0] == 'N' || param[0] == 'n') config->port_info.serial.parity = PORTFW_SERIAL_PARITY_NONE;
        else if (param[0] == 'S' || param[0] == 's') config->port_info.serial.parity = PORTFW_SERIAL_PARITY_SPACE;
        else if (param[0] == 'M' || param[0] == 'm') config->port_info.serial.parity = PORTFW_SERIAL_PARITY_MARK;
        else {
            errno = EINVAL;
            loc_free(param);
            return -1;
        }
        loc_free(param);
    }

    /* STOP BITS */
    if (*s) s++;
    ptr = s;
    while (*s && *s != '-') s++;
    if (s != ptr) {
        param = loc_strndup(ptr, s - ptr);
        config->port_info.serial.stop_bits = atoi(param);
        loc_free(param);
    }


    /* FLOW CONTROL */
    if (*s) s++;
    ptr = s;
    while (*s && *s != '-') s++;
    if (s != ptr) {
        param = loc_strndup(ptr, s - ptr);
        if (strlen(param) != 1) {
            errno = EINVAL;
            loc_free(param);
            return -1;
        }
        if (param[0] == 'N' || param[0] == 'n') config->port_info.serial.flow_ctrl = PORTFW_SERIAL_FC_NONE;
        else if (param[0] == 'S' || param[0] == 's') config->port_info.serial.flow_ctrl = PORTFW_SERIAL_FC_XON;
        else if (param[0] == 'R' || param[0] == 'r') config->port_info.serial.flow_ctrl = PORTFW_SERIAL_FC_RTS;
        else if (param[0] == 'D' || param[0] == 'd') config->port_info.serial.flow_ctrl = PORTFW_SERIAL_FC_DSR;
        else {
            errno = EINVAL;
            loc_free(param);
            return -1;
        }
        loc_free(param);
    }

    if (config->port_info.serial.data_bits < 5 || config->port_info.serial.data_bits > 8) {
        errno = EINVAL;
        return -1;
    }
    if (config->port_info.serial.stop_bits < 1 || config->port_info.serial.stop_bits > 2) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static void serial_connect_done(void * args) {
    PortFwConfig * config = (PortFwConfig *)((AsyncReqInfo *)args)->client_data;
    PortConnectInfo * info = &config->port_info;

    if (info->cnct_req.error) {
        info->cnct_callback(config, info->cnct_req.error);
    }
    else {
        config->connected = 1;
        info->cnct_callback(config, 0);
    }
}

static int serial_connect_req_func(void * reqdata) {
    PortFwConfig * config = (PortFwConfig *)reqdata;
    PortConnectInfo * info = &config->port_info;
    char * dev_name = config->port_config + strlen("serial:");
    int ix;
    int baud_const = 0;
#ifdef WIN32
    DCB                         dcb = {0};
    COMMTIMEOUTS                timeouts;
    HANDLE                      handle;
    char com_dev_name[128];
#endif
    trace(log_portfw, "Try to open serial device %s", dev_name);

    for (ix = 0; baud_table[ix].baud_rate != 0; ix++) {
        if (baud_table[ix].baud_rate == info->serial.baud_rate)
            baud_const = baud_table[ix].baud_const;
    }
#ifdef WIN32
    if (baud_const == 0) baud_const = info->serial.baud_rate;
#else
    if (baud_const == 0) {
        trace(log_portfw, "ERROR: Invalid baud rate");
        errno = EINVAL;
        return -1;
    }
#endif
    
#ifdef WIN32

    snprintf(com_dev_name, sizeof(com_dev_name), "\\\\.\\%s", dev_name);
    handle = CreateFile (com_dev_name,
                            GENERIC_READ | GENERIC_WRITE,
                            0,
                            0,
                            OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED,
                            (void *)NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        set_win32_errno(GetLastError());
        trace(log_portfw, "ERROR: Unable to open open serial device %s: %d", dev_name, GetLastError());
        return -1;
    }

    info->serial.read_overlapped.hEvent = CreateEvent (
            NULL,     /* security attributes */
            1,        /* manual reset (required by GetOverlappedResult()) */
            0,        /* non signalled initial state */
            NULL      /* event name */);
    
    if (info->serial.read_overlapped.hEvent == NULL) {
        set_win32_errno(GetLastError());
        CloseHandle (handle);
        return -1;
    }

    info->serial.write_overlapped.hEvent = CreateEvent (
        NULL,     /* security attributes */
        1,        /* manual reset (required by GetOverlappedResult()) */
        0,        /* non signalled initial state */
        NULL      /* event name */);

    if (info->serial.write_overlapped.hEvent == NULL) {
        set_win32_errno(GetLastError());
        CloseHandle (info->serial.read_overlapped.hEvent);
        CloseHandle (handle);
        return -1;
    }

    info->serial.saved_dcb.DCBlength= sizeof(DCB);
    if (!GetCommState(handle, &info->serial.saved_dcb))
        {
        set_win32_errno(GetLastError());
        CloseHandle (handle);
        CloseHandle (info->serial.read_overlapped.hEvent);
        CloseHandle (info->serial.write_overlapped.hEvent);
        return -1;
        }
    dcb = info->serial.saved_dcb;
    dcb.BaudRate = baud_const;
    dcb.ByteSize = info->serial.data_bits;

    if (info->serial.parity == PORTFW_SERIAL_PARITY_ODD) dcb.Parity = ODDPARITY;
    else if (info->serial.parity == PORTFW_SERIAL_PARITY_EVEN) dcb.Parity = EVENPARITY;
    else if (info->serial.parity == PORTFW_SERIAL_PARITY_NONE) dcb.Parity = NOPARITY;
    else if (info->serial.parity == PORTFW_SERIAL_PARITY_SPACE) dcb.Parity = SPACEPARITY;
    else if (info->serial.parity == PORTFW_SERIAL_PARITY_MARK) dcb.Parity = MARKPARITY;
    else {
        CloseHandle (handle);
        CloseHandle (info->serial.read_overlapped.hEvent);
        CloseHandle (info->serial.write_overlapped.hEvent);
        trace(log_portfw, "ERROR: Invalid parity");
        errno = EINVAL;
        return -1;
    }

    if (info->serial.stop_bits == 1)
        dcb.StopBits = ONESTOPBIT;
    else if (info->serial.stop_bits == 2)
        dcb.StopBits = TWOSTOPBITS;
    else {
        CloseHandle (handle);
        CloseHandle (info->serial.read_overlapped.hEvent);
        CloseHandle (info->serial.write_overlapped.hEvent);
        trace(log_portfw, "ERROR: Invalid stop bits number");
        errno = EINVAL;
        return -1;
    }

    /* Documented unsupported configurations */

    if (info->serial.stop_bits == 2 &&
         info->serial.data_bits == 5)
        {
        CloseHandle (handle);
        CloseHandle (info->serial.read_overlapped.hEvent);
        CloseHandle (info->serial.write_overlapped.hEvent);
        errno = EINVAL;
        trace(log_portfw, "ERROR: Invalid stop bits number");
        return -1;
        }

    dcb.fOutX    = 0;
    dcb.fInX     = 0;

    if (info->serial.flow_ctrl == PORTFW_SERIAL_FC_NONE) {
        dcb.fOutxCtsFlow    = 0;
        dcb.fOutxDsrFlow    = 0;
        dcb.fDtrControl     = DTR_CONTROL_DISABLE;
        dcb.fDsrSensitivity = 0;
        dcb.fRtsControl     = RTS_CONTROL_DISABLE;
    }
#ifdef SUPPORT_FLOW_CTRL
    else if (info->serial.flow_ctrl == PORTFW_SERIAL_FC_XON) {
        dcb.fOutxCtsFlow    = 0;
        dcb.fOutxDsrFlow    = 0;
        dcb.fDtrControl     = DTR_CONTROL_DISABLE;
        dcb.fDsrSensitivity = 0;
        dcb.fRtsControl     = RTS_CONTROL_DISABLE;
        dcb.fOutX = dcb.fInX = 1;
    }
    else if (info->serial.flow_ctrl == PORTFW_SERIAL_FC_RTS) {
        dcb.fOutxCtsFlow    = 1;
        dcb.fOutxDsrFlow    = 0;
        dcb.fDtrControl     = DTR_CONTROL_DISABLE;
        dcb.fDsrSensitivity = 0;
        dcb.fRtsControl     = RTS_CONTROL_ENABLE;
    }
    else if (info->serial.flow_ctrl == PORTFW_SERIAL_FC_DSR) {
        dcb.fOutxCtsFlow    = 0;
        dcb.fOutxDsrFlow    = 1;
        dcb.fDtrControl     = DTR_CONTROL_ENABLE;
        dcb.fDsrSensitivity = 1;
        dcb.fRtsControl     = RTS_CONTROL_DISABLE;
    }
#endif /* SUPPORT_FLOW_CTRL */
    else {
        CloseHandle (handle);
        CloseHandle (info->serial.read_overlapped.hEvent);
        CloseHandle (info->serial.write_overlapped.hEvent);
        trace(log_portfw, "ERROR: Invalid flow control");
        errno = EINVAL;
        return -1;
    }

    dcb.fAbortOnError = 0;

    if (!SetCommState(handle, &dcb)) {
        set_win32_errno(GetLastError());
        CloseHandle (handle);
        CloseHandle (info->serial.read_overlapped.hEvent);
        CloseHandle (info->serial.write_overlapped.hEvent);
        trace(log_portfw, "Unable to set comm state: %d", errno);
        return -1;
    }

    GetCommTimeouts (handle, &info->serial.saved_timeout);
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 2000;   /* set a timeout of 2s max */
    timeouts.WriteTotalTimeoutMultiplier = 15;        /* max = 15ms per character */
    timeouts.WriteTotalTimeoutConstant = 200;        /* const: 200ms */
    SetCommTimeouts (handle, &timeouts);
    info->serial.handle = handle;
#else        /* WIN32 */
    struct termios termIo;
#if defined(__linux__)
    struct serial_struct serial;
#endif
    int data_bits_const = CS8;

    for (ix = 0; data_bit_table[ix].data_bits != 0; ix++)        {
        if (data_bit_table[ix].data_bits == info->serial.data_bits)
            data_bits_const = data_bit_table[ix].data_bits_const;
    }

#if defined(__APPLE__)
    if ((info->serial.fd = open(dev_name, O_RDWR|O_NONBLOCK, 0)) < 0) {
#else
    if ((info->serial.fd = open(dev_name, O_RDWR, 0)) < 0) {
#endif
        trace(log_portfw, "Unable to open serial port: %d", errno);
        return -1;
    }
    if (tcgetattr(info->serial.fd, &termIo) == -1) {
        trace(log_portfw, "Unable to get serial port attributes: %d", errno);
        close(info->serial.fd);
        info->serial.fd = -1;
        return -1;
    }
    
    termIo.c_iflag = 0; /* no input procesing */
    termIo.c_oflag = 0; /* no output procesing */
    termIo.c_cflag = CREAD | data_bits_const | CLOCAL;

    if (info->serial.stop_bits == 2) termIo.c_cflag |= CSTOPB;

    if (info->serial.parity == PORTFW_SERIAL_PARITY_ODD) termIo.c_cflag |= PARODD;
    else if (info->serial.parity == PORTFW_SERIAL_PARITY_EVEN) termIo.c_cflag |= PARENB;
    else if (info->serial.parity != PORTFW_SERIAL_PARITY_NONE) {
        errno = EINVAL;
        close(info->serial.fd);
        info->serial.fd = -1;
        trace(log_portfw, "ERROR: Invalid parity");
        return -1;
    }

    termIo.c_lflag = 0; /* no local processing */
    
    /* time out value (<val> * 0.1s) */
    termIo.c_cc[VMIN] = 0;
    termIo.c_cc[VTIME] = 3 * 10;

#ifdef  ASYNC_LOW_LATENCY
    if (ioctl(info->serial.fd, TIOCGSERIAL, &serial) == 0)
        {
        serial.flags |= ASYNC_LOW_LATENCY;
        ioctl(info->serial.fd, TIOCSSERIAL, &serial);
        }
#endif  /* ASYNC_LOW_LATENCY */

    /* configure baud rate for input and output */

    cfsetispeed(&termIo, baud_const);
    cfsetospeed(&termIo, baud_const);

    /* set the attributes */

    tcsetattr(info->serial.fd, TCSAFLUSH, &termIo);

#if defined(__APPLE__)
    fcntl(info->serial.fd, F_SETFL, fcntl(info->serial.fd, F_GETFL) & ~O_NONBLOCK);
#endif  /* __APPLE__ */
#endif
    trace(log_portfw, "SUCCESS: serial port opened");
    return 0;
}

static void display_buffer(unsigned char * buffer, size_t size) {
    if ((log_mode & log_portfw) && log_file) {
        size_t i;
        char tmp_buffer[256];
        if (size > sizeof(tmp_buffer)) size = sizeof(tmp_buffer) - 1;
        for (i = 0; i < size; i++) {
            if (isprint(buffer[i])) tmp_buffer[i] = buffer[i];
            else tmp_buffer[i] = '.';
        }
        tmp_buffer[i] = 0;
        trace(log_portfw, tmp_buffer);
    }
}

static int serial_send_req_func(PortFwConfig * config, void * buffer, size_t length) {
    int ret;
#ifdef WIN32
    BOOL status;
    DWORD write_count;
    DWORD total_write_count = 0;
    HANDLE WriteEvent;
    WriteEvent = config->port_info.serial.write_overlapped.hEvent;
        
    do
        {
        memset (&config->port_info.serial.write_overlapped, 0, sizeof(config->port_info.serial.write_overlapped));
        config->port_info.serial.write_overlapped.hEvent = WriteEvent;
        ResetEvent (WriteEvent);
        status = WriteFile (config->port_info.serial.handle,
                            (char *)buffer + total_write_count,
                            length - total_write_count, &write_count,
                            &config->port_info.serial.write_overlapped);
        
        /* ...and wait till last char is sent. */

        if (status == FALSE) {
            DWORD error = GetLastError ();

            if (error == ERROR_IO_PENDING) {
                status = GetOverlappedResult (config->port_info.serial.handle,
                                              &config->port_info.serial.write_overlapped,
                                              &write_count, TRUE);
                if (!status) {
                    set_win32_errno(GetLastError());
                    ResetEvent (WriteEvent);
                    return -1;
                }
            }
            else {
                ResetEvent (WriteEvent);
                set_win32_errno(error);
                return -1;
            }
        }
        total_write_count += write_count;
    } while ((int) total_write_count < length);

    ResetEvent (WriteEvent);
    ret = length;
#else
    do {
        ret = write(config->port_info.serial.fd, buffer, length);
    }
    while (ret < 0 && (errno == EINTR));
#endif
    return ret; 
}

static int serial_recv_req_func(PortFwConfig * config, void * buffer, size_t length) {
#ifdef  WIN32
    BOOL fWaitingOnRead = FALSE;
    HANDLE handle = config->port_info.serial.handle;         /* serial device handle */
    OVERLAPPED * read_overlapped = &config->port_info.serial.read_overlapped;
    int total_read_count = 0;

    ResetEvent(read_overlapped->hEvent);
    while (1) {
        if (!fWaitingOnRead) {
            DWORD read_count;
            DWORD max_bytes = 0;
            DWORD com_errors;
            COMSTAT com_status; /* To get the pending input bytes */

            /* Get the number of pending chars. If there are more than 1 read all of them; otherwise wait
             * only for the first character. Waiting for more than 1 character if they are not available
             * would lead to block until the specified number of bytes has been received.
             */

            if (ClearCommError (handle, &com_errors, &com_status)) {
                max_bytes = com_status.cbInQue;
            }

            /* If there are enough received characters to transfer immediately to the client or if there are no
             * more pending read characters and we have some characters to transfer; do the transfer right now.
             * Note that READ_THRESHOLD is also used to avoid buffer overflow.
             */
            if (max_bytes == 0 && total_read_count != 0) {
                return total_read_count;
            }

            /* If there are no pending characters; try to read only one. Otherwise, we would block until
             * we have received the number of characters specified.
             */
            if (max_bytes == 0) max_bytes = 1;
            else {
                if ((total_read_count + max_bytes) > length) max_bytes = (length - total_read_count);
            }

            if (!ReadFile(handle, (char *)buffer + total_read_count, max_bytes, &read_count, read_overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    set_win32_errno(GetLastError());
                    return -1;
                }
                else {
                    fWaitingOnRead = TRUE;
                }
            }
            else {
                /* read completed immediately
                 * Read the character but do not make it available to the client right now; check if
                 * we can read more data first.
                 */

                total_read_count += read_count;
                fWaitingOnRead = FALSE;
            }
        }
        else {
            /* We are waiting for the result of an asynchronous read */

            DWORD dwRes = WaitForSingleObject(read_overlapped->hEvent, INFINITE);
            DWORD read_count;
            switch(dwRes) {
                case WAIT_OBJECT_0:
                    if (!GetOverlappedResult(handle, read_overlapped, &read_count, TRUE)) {
                        set_win32_errno(GetLastError());
                        return -1;
                    }
                    else {
                        /* Read completed successfully. */
                        total_read_count += read_count;

                        /*  Reset flag so that another operation can be issued. */
                        fWaitingOnRead = FALSE;
                    }
                    break;
                case WAIT_TIMEOUT:
                    /* Operation isn't complete yet. fWaitingOnRead flag isn't
                     * changed since I'll loop back around, and I don't want
                     * to issue another read until the first one finishes.
                     *
                     * This is a good time to do some background work.
                     */
                    break;

                default:
                    set_win32_errno(GetLastError());
                    return -1;
            }
        }
    }
    return total_read_count;
#else
    int ret;
    do
        {
        ret = read(config->port_info.serial.fd, buffer, length);
        }
    while (ret == 0 || (ret < 0 && (errno == EINTR)));
    return ret;
#endif
}

static void serial_close(PortFwConfig * config) {
#ifdef WIN32
    if (config->port_info.serial.handle == NULL) return;
    SetCommTimeouts (config->port_info.serial.handle, &config->port_info.serial.saved_timeout);
    SetCommState(config->port_info.serial.handle, &config->port_info.serial.saved_dcb);
    CloseHandle (config->port_info.serial.handle);
    config->port_info.serial.handle = NULL;
    if (config->port_info.serial.read_overlapped.hEvent) CloseHandle(config->port_info.serial.read_overlapped.hEvent);
    if (config->port_info.serial.write_overlapped.hEvent) CloseHandle(config->port_info.serial.write_overlapped.hEvent);
    config->port_info.serial.read_overlapped.hEvent = NULL;
    config->port_info.serial.write_overlapped.hEvent = NULL;
#else
    if (config->port_info.serial.fd == -1) return;
    close(config->port_info.serial.fd);
#endif
    if (config->verbose) fprintf(stderr, "Connection with %s closed\n", config->port_name);
}

static void serial_connect(PortFwConfig * config, ConnectCallBack callback) {
    int error = 0;

    if (serial_parse_params(config) == -1) {
        error = errno;
        callback(config, error);
        return;
    }
    else {
        PortConnectInfo * info = &config->port_info;

        info->send_req_func = serial_send_req_func;
        info->recv_req_func = serial_recv_req_func;
        info->close_func = serial_close;
        info->cnct_callback = callback;
        info->cnct_req.client_data = config;
        info->cnct_req.done = serial_connect_done;
        info->cnct_req.type = AsyncReqUser;
        info->cnct_req.u.user.func = serial_connect_req_func;
        info->cnct_req.u.user.data = config;
        async_req_post(&info->cnct_req);
    }
}
/* END SERIAL LINE SUPPORT */
#endif /* ENABLE_PortForward_Serial */

static void send_event_portfw_remote_config_added(PortFwConfig * config) {
    if (broadcast_group == NULL) return;
    else {
        OutputStream * out = &broadcast_group->out;
        write_stringz(out, "E");
        write_stringz(out, PortForward);
        write_stringz(out, "configAdded");
        write_portfw_remote_connection_config (out, config);
        write_stream(out, 0);
        write_stream(out, MARKER_EOM);
    }
}

static void send_event_portfw_remote_config_removed(PortFwConfig * config) {
    if (broadcast_group == NULL) return;
    else {
        OutputStream * out = &broadcast_group->out;
        write_stringz(out, "E");
        write_stringz(out, PortForward);
        write_stringz(out, "configRemoved");
        write_stream(out, '[');
        json_write_string(out, config->id);
        write_stream(out, ']');
        write_stream(out, 0);
        write_stream(out, MARKER_EOM);
    }
}

static void write_portfw_remote_connection_config(OutputStream * out, PortFwConfig * config) {

    write_stream(out, '{');

    json_write_string(out, "ID");
    write_stream(out, ':');
    json_write_string(out, config->id);
    
    write_stream(out, ',');
    json_write_string(out, "InputStream");
    write_stream(out, ':');
    json_write_string(out, config->in_stream_id);
    write_stream(out, ',');
    json_write_string(out, "OutputStream");
    write_stream(out, ':');
    json_write_string(out, config->out_stream_id);
    write_stream(out, '}');
}

static void portfw_cmd_get_config(char * token, Channel * c) {
    char id[256];
    PortFwConfig * config;
    int err = 0;
    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);
    if ((config = find_portfw_config(id)) == NULL) err = set_errno(ERR_OTHER,
            portfw_remote_inv_config_err);
    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, err);
    if (err == 0) {
        write_portfw_remote_connection_config(&c->out, config);
        write_stream(&c->out, 0);
    }
    else {
        write_stringz(&c->out, "null");
    }
    write_stream(&c->out, MARKER_EOM);
}

static void portfw_cmd_list(char * token, Channel * c) {
    LINK * l = portfw_config_list.next;
    int cnt = 0;
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);
    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, '[');
    while (l != &portfw_config_list) {
        PortFwConfig * config = gdbconfl2gdbconfp(l);
        if (cnt > 0) write_stream(&c->out, ',');
        json_write_string(&c->out, config->id);
        l = l->next;
        cnt++;
    }
    write_stream(&c->out, ']');
    write_stream(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}


static PortFwConfig * find_portfw_config(const char * id) {
    LINK * l = portfw_config_list.next;
    while (l != &portfw_config_list) {
        PortFwConfig * config = gdbconfl2gdbconfp(l);
        if (strcmp(id, config->id) == 0 && !config->shutdown_in_progress) {
            return config;
        }
        l = l->next;
    }
    return NULL ;

}

static int user_send_func(void * data) {
    PortFwConfig * config = (PortFwConfig *) data;
    return config->port_info.send_req_func(config, config->outbuf, config->outbuf_len);
}

static int user_recv_func(void * data) {
    PortFwConfig * config = (PortFwConfig *) data;
    return config->port_info.recv_req_func(config, config->inbuf, IN_BUF_SIZE);
}

static void write_stream_to_port(PortFwConfig * config) {
    int eos = 0;
    size_t read_size = 0;
    assert (config->send_in_progress == 0);

    if (virtual_stream_get_data(config->out_vstream,
                    config->outbuf + config->outbuf_len,
                OUT_BUF_SIZE - config->outbuf_len,
                &read_size, &eos) < 0) return;
    config->outbuf_len += read_size;

    if (read_size != 0) {
        size_t write_size = 0;
        write_size = config->outbuf_len;

        if (write_size != 0) {
            config->send_in_progress = 1;
            if (config->send_req.type != AsyncReqUser) {
                config->send_req.u.sio.bufp = config->outbuf;
                config->send_req.u.sio.bufsz = write_size;
            }
            async_req_post(&config->send_req);
        }
    }
    else
        config->send_in_progress = 0;
}

static void release_config(PortFwConfig * config) {
    loc_free(config->port_config);
    loc_free(config);
}

static void delete_config(PortFwConfig * config) {
    size_t wrote = 0;
    config->shutdown_in_progress = 1;
    if (config->connected) {
        disconnect_port(config);
    }
    if (config->send_in_progress || config->recv_in_progress) {
        return;
    }
    virtual_stream_add_data(config->in_vstream, NULL, 0, &wrote, 1);
    if (config->auto_connect) {
        virtual_stream_disconnect(config->channel, NULL, config->out_stream_id);
        virtual_stream_disconnect(config->channel, NULL, config->in_stream_id);
    }
    virtual_stream_delete(config->in_vstream);
    virtual_stream_delete(config->out_vstream);
    list_remove (&config->link);
    send_event_portfw_remote_config_removed(config);

    release_config(config);
}

static void done_recv_request(void * args) {
    AsyncReqInfo * req = (AsyncReqInfo *)args;
    PortFwConfig * config = (PortFwConfig *)(req)->client_data;
    int rval;

    if (req->type == AsyncReqUser) rval = req->u.user.rval;
    else rval = req->u.sio.rval;

    config->recv_in_progress = 0;
    if (rval == 0 || (rval == -1 && req->error != EINTR)
            || config->shutdown_in_progress) {
        delete_config(config);
        return;
    }
    if (rval == -1) {
        /* Interrupted system call */
        async_req_post(&config->recv_req);
        return;
    }
    config->inbuf_len = rval;
    if (rval > 0) display_buffer((unsigned char *)config->inbuf, rval);
    write_port_to_stream(config);
}

static void done_send_request(void * args) {
    AsyncReqInfo * req = (AsyncReqInfo *)args;
    PortFwConfig * config = (PortFwConfig *)(req)->client_data;
    int rval;

    config->send_in_progress = 0;
    if (req->type == AsyncReqUser) rval = req->u.user.rval;
    else rval = req->u.sio.rval;
    if (rval == 0 || (rval == -1 && req->error != EINTR)
            || config->shutdown_in_progress) {
        delete_config(config);
        return;
    }
    if (rval == -1) {
        /* Interrupted system call */
        async_req_post(&config->send_req);
        return;
    }
    if (rval > 0) display_buffer((unsigned char *)config->outbuf, rval);
    if (config->outbuf_len != rval) {
        memmove(config->outbuf, config->outbuf + rval, config->outbuf_len - rval);
        config->outbuf_len -= rval;
        async_req_post(&config->send_req);
        return;
    }
        
    config->outbuf_len -= rval;
    write_stream_to_port(config);
}

static void write_stream_to_port_callback(VirtualStream *stream, int event,
        void * arg) {
    PortFwConfig * config = arg;
    if (event == VS_EVENT_DATA_AVAILABLE) {
        if (config->connected == 0 || config->shutdown_in_progress) return;
        if (config->send_in_progress)
            return;
        write_stream_to_port(config);
    }
}

static void write_port_to_stream(void * arg) {
    PortFwConfig * config = (PortFwConfig *) arg;

    assert (config->recv_in_progress == 0);

    if (config->inbuf_len != 0) {
        size_t wrote = 0;
        virtual_stream_add_data(config->in_vstream,
                config->inbuf + config->inbuf_pos, config->inbuf_len, &wrote,
                0);
        config->inbuf_pos += wrote;
        config->inbuf_len -= wrote;
    }

    /* If all data has been consumed, start a new read request. Otherwise wait
     * for the full buffer to be consumed.
     */
    if (config->inbuf_len == 0) {
        config->inbuf_pos = 0;
        if (config->recv_req.type != AsyncReqUser) {
            config->recv_req.u.sio.bufp = config->inbuf;
            config->recv_req.u.sio.bufsz = IN_BUF_SIZE;
        }
        config->recv_in_progress = 1;
        async_req_post(&config->recv_req);
    }
}

static void write_port_to_stream_callback(VirtualStream *stream,
        int event, void * arg) {
    PortFwConfig * config = arg;
    if (config->connected == 0 || config->shutdown_in_progress) return;
    switch (event) {
    case VS_EVENT_SPACE_AVAILABLE:
        {
            if (config->recv_in_progress) return;
            write_port_to_stream(config);
        }
        break;
    case VS_EVENT_DATA_AVAILABLE:
    default:
        /*
         portfw_client_read(config);
         */
        str_exception(ERR_OTHER, "Unhandled stream callback\n");
    }
}

static void connect_callback(PortFwConfig * config, int error) {
    if (error) {
        if (config->verbose) fprintf(stderr, "Error opening %s: %d (%s)\n", config->port_name, error, errno_to_str(error));
    } 
    else {
        if (config->verbose) fprintf(stdout, "Successfully opened %s\n", config->port_name);
    }
    channel_unlock_with_msg(config->channel, PortForward);
    write_stringz(&config->channel->out, "R");
    write_stringz(&config->channel->out, config->token);
    write_errno(&config->channel->out, error);
    write_stream(&config->channel->out, MARKER_EOM);

    if (error == 0) {
        virtual_stream_create(PortForward, config->id, IN_BUF_SIZE,
                VS_ENABLE_REMOTE_READ, write_port_to_stream_callback, config,
                &config->in_vstream);
        virtual_stream_create(PortForward, config->id, OUT_BUF_SIZE,
                VS_ENABLE_REMOTE_WRITE, write_stream_to_port_callback, config,
                &config->out_vstream);
        virtual_stream_get_id(config->in_vstream, config->in_stream_id,
                sizeof(config->in_stream_id));
        virtual_stream_get_id(config->out_vstream, config->out_stream_id,
                sizeof(config->out_stream_id));
        if (config->auto_connect) {
            virtual_stream_connect(config->channel, NULL, config->out_stream_id);
            virtual_stream_connect(config->channel, NULL, config->in_stream_id);
        }

        list_add_first(&config->link, &portfw_config_list);

        send_event_portfw_remote_config_added(config);

        config->send_req.client_data = config;
        config->send_req.done = done_send_request;

        config->recv_req.client_data = config;
        config->recv_req.done = done_recv_request;

        if (config->port_info.sock != -1) {
            config->send_req.type = AsyncReqSend;
            config->send_req.u.sio.sock = config->port_info.sock;
            config->send_req.u.sio.flags = 0;

            config->recv_req.type = AsyncReqRecv;
            config->recv_req.u.sio.sock = config->port_info.sock;
            config->recv_req.u.sio.flags = 0;
            config->recv_req.u.sio.bufp = config->inbuf;
            config->recv_req.u.sio.bufsz = IN_BUF_SIZE;
        } else {
            config->send_req.type = AsyncReqUser;
            config->send_req.u.user.func = user_send_func;
            config->send_req.u.user.data = config;

            config->recv_req.type = AsyncReqUser;
            config->recv_req.u.user.func = user_recv_func;
            config->recv_req.u.user.data = config;
        }

        config->recv_in_progress = 1;
        async_req_post(&config->recv_req);
    }
    else {
        disconnect_port(config);
        release_config(config);
    }
}

static void read_portfw_property(InputStream * inp, const char * name,
        void * args) {
    PortFwConfig * config = args;
    if (strcmp(name, "ID") == 0) json_read_string(inp, config->id,
            sizeof(config->id));
    else if (strcmp(name, "Config") == 0) config->port_config =
            json_read_alloc_string(inp);
    else if (strcmp(name, "Params") == 0) config->port_params = json_read_alloc_string(inp);
    else if (strcmp(name, "Port") == 0 ||
                strcmp(name,  "RemotePort") == 0) config->port = (unsigned short) json_read_uint64(inp);
    else if (strcmp(name, "AutoConnect") == 0) config->auto_connect = json_read_boolean(inp);
    else if (strcmp(name, "Verbose") == 0) config->verbose = json_read_boolean(inp);
    else json_skip_object(inp);
}

static void portfw_cmd_create(char * token, Channel * c) {
    int err = 0;
    PortFwConfig * config = loc_alloc_zero(sizeof(PortFwConfig));
    config->port_info.sock = -1;

    json_read_struct(&c->inp, read_portfw_property, config);
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    if (config->id[0] == 0) str_exception(ERR_OTHER, "PortForward configuration must have an ID");

    if (find_portfw_config(config->id) != NULL) err = set_errno(ERR_OTHER,
            "PortForward Remote configuration already exists");

    if (config->port_config == NULL && config->port == 0) {
        err = set_errno (ERR_OTHER, "Need to specify either a port config string or a port number");
    }
    if (config->port_config != NULL && config->port != 0) {
        err = set_errno (ERR_OTHER, "Need to specify one of port config string or port number");
    }

    if (err == 0) {
        config->channel = c;
        strlcpy(config->token, token, sizeof(config->token));

        if (config->port_config == NULL) {
            config->port_config = loc_alloc(64);
            snprintf(config->port_config, 64, "tcp::%d", (int)config->port);
        }

        trace(log_portfw, "Received connection request for port %s", config->port_config);

        if (strncasecmp(config->port_config, "tcp:", strlen("tcp:")) == 0) {
            channel_lock_with_msg(c, PortForward);
            config->port_type = PORTFW_TCP_PORT;
            snprintf (config->port_name, sizeof(config->port_name), "TCP port \"%s\"", config->port_config + strlen("tcp:"));
            if (config->verbose) fprintf(stdout, "Received connection request for %s\n", config->port_name);
            tcp_connect(config, connect_callback);
        }
        else if (strncasecmp(config->port_config, "udp:", strlen("udp:")) == 0) {
            config->port_type = PORTFW_UDP_PORT;
            if (config->verbose) fprintf(stderr, "Unsupported protocol \"udp\"\n");
            err = EINVAL;
        }
#if ENABLE_PortForward_Serial
        else if (strncasecmp(config->port_config, "serial:", strlen("serial:")) == 0) {
            config->port_type = PORTFW_SER_PORT;
            snprintf (config->port_name, sizeof(config->port_name), "serial port \"%s\"", config->port_config + strlen("serial:"));
            if (config->verbose) fprintf(stdout, "Received connection request for %s\n", config->port_name);
            channel_lock_with_msg(c, PortForward);
            serial_connect(config, connect_callback);
        }
#endif
        else {
            if (config->verbose) fprintf(stderr, "Unsupported connection request for %s\n", config->port_config);
            err = EINVAL;
        }
   }

    if (err == 0) {
        return;
    } 
    else {
        release_config(config);
    }
    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, err);
    write_stream(&c->out, MARKER_EOM);
}

static void portfw_cmd_delete(char * token, Channel * c) {
    char id[256];
    int err = 0;
    PortFwConfig * config;

    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    if ((config = find_portfw_config(id)) == NULL) err = set_errno(ERR_OTHER,
            portfw_remote_inv_config_err);
    else {
        delete_config(config);
    }
    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, err);
    write_stream(&c->out, MARKER_EOM);
}


static void portfw_cmd_get_capabilities(char * token, Channel * c) {
    char id[256];
    PortFwConfig * config = NULL;
    OutputStream * out = &c->out;
    int err = 0;

    json_read_string(&c->inp, id, sizeof(id));
    json_test_char(&c->inp, MARKER_EOA);
    json_test_char(&c->inp, MARKER_EOM);

    if (strlen(id) > 0) {
        if ((config = find_portfw_config(id)) == NULL) err = set_errno(ERR_OTHER,
                portfw_remote_inv_config_err);
    }

    write_stringz(out, "R");
    write_stringz(out, token);
    write_errno(out, err);
    if (err) {
        write_stringz(&c->out, "null");
    }
    else {
        write_stream(out, '{');
        json_write_string(out, "TCP");
        write_stream(out, ':');
        json_write_boolean(out, 1);
        write_stream(out,',');
#if ENABLE_PortForward_Serial
        json_write_string(out, "Serial");
        write_stream(out, ':');
        json_write_boolean(out, 1);
        write_stream(out,',');
#endif /* ENABLE_PortForward_Serial */
        json_write_string(out, "AutoConnect");
        write_stream(out, ':');
        json_write_boolean(out, 1);
        write_stream(out, '}');
        write_stream(out, 0);
    }
    write_stream(out, MARKER_EOM);
}

static void channel_close_listener(Channel * c) {
    LINK * l = portfw_config_list.next;
    for (l = portfw_config_list.next; l != &portfw_config_list;) {
        PortFwConfig * config = gdbconfl2gdbconfp(l);
        l = l->next;
        if (config->channel == c) delete_config(config);
    }
}

void ini_portfw_remote_config_service(Protocol *proto, TCFBroadcastGroup * bcg) {
    static int ini_portfw = 0;

    if (ini_portfw == 0) {
        add_channel_close_listener(channel_close_listener);
        broadcast_group = bcg;
        ini_portfw = 1;
    }
    log_portfw = add_trace_mode(0, "portfw", "Port Forwarding");

    add_command_handler(proto, PortForward, "getConfig", portfw_cmd_get_config);
    add_command_handler(proto, PortForward, "list", portfw_cmd_list);
    add_command_handler(proto, PortForward, "create", portfw_cmd_create);
    add_command_handler(proto, PortForward, "delete", portfw_cmd_delete);
    add_command_handler(proto, PortForward, "getCapabilities", portfw_cmd_get_capabilities);
}

#endif
