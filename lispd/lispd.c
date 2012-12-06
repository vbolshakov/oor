/*
 * lispd.c 
 *
 * This file is part of LISP Mobile Node Implementation.
 * lispd Implementation
 * 
 * Copyright (C) 2011 Cisco Systems, Inc, 2011. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Please send any bug reports or fixes you make to the email address(es):
 *    LISP-MN developers <devel@lispmob.org>
 *
 * Written or modified by:
 *    David Meyer       <dmm@cisco.com>
 *    Preethi Natarajan <prenatar@cisco.com>
 *    Albert Cabellos   <acabello@ac.upc.edu>
 *    Lorand Jakab      <ljakab@ac.upc.edu>
 *
 */

#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <net/if.h>
#include "lispd.h"
#include "lispd_config.h"
#include "lispd_ipc.h"
#include "lispd_iface_list.h"
#include "lispd_iface_mgmt.h"
#include "lispd_lib.h"
#include "lispd_local_db.h"
#include "lispd_map_cache_db.h"
#include "lispd_map_register.h"
#include "lispd_map_request.h"
#include "lispd_timers.h"
#include "lispd_tun.h"
#include "lispd_input.h"
#include "lispd_output.h"
#include "lispd_iface_list.h"

#include "lispd_map_cache_db.h"


void event_loop();
void signal_handler(int);
int build_timers_event_socket();
int process_timer_signal();
void exit_cleanup(void);

/*
 *      global (more or less) vars
 *
 */

/*
 *      database and map cache
 */

lispd_database_t  *lispd_database       = NULL;
lispd_map_cache_t *lispd_map_cache      = NULL;

/*
 *      next gen database
 */

patricia_tree_t *AF4_database           = NULL;
patricia_tree_t *AF6_database           = NULL;

/*
 *      data cache
 */

datacache_t     *datacache;

/*
 *      config paramaters
 */

lispd_addr_list_t          *map_resolvers  = 0;
lispd_addr_list_t          *proxy_itrs  = 0;
lispd_weighted_addr_list_t *proxy_etrs  = 0;
lispd_map_server_list_t    *map_servers = 0;
char    *config_file                    = "/etc/lispd.conf";
char    *map_resolver                   = NULL;
char    *map_server                     = NULL;
char    *proxy_etr                      = NULL;
char    *proxy_itr                      = NULL;
int      debug                          = 0;
int      daemonize                      = 0;
int      map_request_retries            = DEFAULT_MAP_REQUEST_RETRIES;
int      control_port                   = LISP_CONTROL_PORT;
uint32_t iseed  = 0;            /* initial random number generator */
/*
 *      various globals
 */

char   msg[128];                                /* syslog msg buffer */
pid_t  pid                              = 0;    /* child pid */
pid_t  sid                              = 0;
/*
 *      sockets (fds)
 */
int     ipv4_data_input_fd            = 0;
int     ipv6_data_input_fd            = 0;
int     ipv4_control_input_fd           = 0;
int     ipv6_control_input_fd           = 0;
int     netlink_fd                      = 0;
fd_set  readfds;
struct  sockaddr_nl dst_addr;
struct  sockaddr_nl src_addr;
nlsock_handle nlh;

/*
 *      timers (fds)
 */

static int signal_pipe[2]; // We don't have signalfd in bionic, fake it.
int     timers_fd                       = 0;

#ifdef LISPMOBMH
/* timer to rate control smr's in multihoming scenarios */
int 	smr_timer_fd					= 0;
#endif

/*
 * Interface on which control messages
 * are sent
 */
lispd_iface_elt *default_ctrl_iface_v4  = NULL;
lispd_iface_elt *default_ctrl_iface_v6  = NULL;
lisp_addr_t source_rloc;

int main(int argc, char **argv) 
{
    lisp_addr_t tun_addr;

    /*
     *  Check for superuser privileges
     */

    if (geteuid()) {
        printf("Running %s requires superuser privileges! Exiting...\n", LISPD);
        exit(EXIT_FAILURE);
    }

    /*
     *  Initialize the random number generator
     */

    iseed = (unsigned int) time (NULL);
    srandom(iseed);

    /*
     * Set up signal handlers
     */

    signal(SIGHUP,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);
    signal(SIGQUIT, signal_handler);

    /*
     *  set up syslog now, checking to see if we're daemonizing...
     */

    set_up_syslog();


    /*
     *  Setup LISP and routing netlink sockets
     */
/*

    if (!setup_netlink_iface()) {
        syslog(LOG_DAEMON, "Can't set up netlink socket for interface events");
        exit(EXIT_FAILURE);
    }
*/
    syslog(LOG_DAEMON, "Netlink sockets created");

    /*
     *  set up databases
     */

    db_init();
    map_cache_init();

    /*
     *  create timers
     */

    if (build_timers_event_socket() == 0)
    {
        syslog(LOG_ERR, " Error programing the timer signal. Exiting...");
        exit(EXIT_FAILURE);
    }
    init_timers();

    /*
     *  Parse command line options
     */

    handle_lispd_command_line(argc, argv);


    /*
     *  Now do the config file
     */

#ifdef OPENWRT

    handle_uci_lispd_config_file("/etc/config", "lispd");

#else

    handle_lispd_config_file();

#endif
    
    




    dump_map_cache();

    dump_local_eids();

    dump_iface_list();

    /*
     * now build the v4/v6 receive sockets
     */

//     if (build_receive_sockets() == 0)
//         exit(EXIT_FAILURE);
//
//
//
//
// #ifdef LISPMOBMH
//     if ((smr_timer_fd = timerfd_create(CLOCK_REALTIME, 0)) == -1)
//         syslog(LOG_INFO, "Could not create the SMR timer controller");
//     /*Make sure the timer starts with coherent values*/
//     stop_smr_timeout();
// #endif
//
//
//     /*
//      *  see if we need to daemonize, and if so, do it
//      */
//
//     if (daemonize) {
//         syslog(LOG_INFO, "Starting the daemonizing process");
//         if ((pid = fork()) < 0) {
//             exit(EXIT_FAILURE);
//         }
//         umask(0);
//         if (pid > 0)
//             exit(EXIT_SUCCESS);
//         if ((sid = setsid()) < 0)
//             exit(EXIT_FAILURE);
//         if ((chdir("/")) < 0)
//             exit(EXIT_FAILURE);
//         close(STDIN_FILENO);
//         close(STDOUT_FILENO);
//         close(STDERR_FILENO);
//     }
//
//     /*
//      *  Dump routing table so we can get the gateway address for source routing
//      */
//
//     if (!dump_routing_table(AF_INET, RT_TABLE_MAIN))
//         syslog(LOG_INFO, "Dumping main routing table failed");
//


    syslog(LOG_INFO, "*************** Creating tun interface... ***************");

    //char *device = "eth0";
    char *tun_dev_name = TUN_IFACE_NAME;



    create_tun(tun_dev_name,
                TUN_RECEIVE_SIZE,
                TUN_MTU,
                &tun_receive_fd,
                &tun_ifindex,
                &tun_receive_buf);

    //tun_addr = get_main_eid(AF_INET);
    get_lisp_addr_from_char("127.0.0.127",&tun_addr);

    tun_bring_up_iface_v4_eid(tun_addr,tun_dev_name);

    //tun_add_v6_eid_to_iface(get_main_eid(AF_INET6),tun_dev_name,tun_ifindex);

    lisp_addr_t dest;
    //lisp_addr_t src;
    //lisp_addr_t gw;
    uint32_t prefix_len;
    uint32_t metric;
    

    prefix_len = 1;
    metric = 3;
    //get_lisp_addr_from_char("0.0.0.0",&gw);
    //get_lisp_addr_from_char("0.0.0.0",&src);

    
    get_lisp_addr_from_char("0.0.0.0",&dest);
    
    add_route_v4(tun_ifindex,
                 &dest,
                 NULL,
                 NULL,
                 prefix_len,
                 metric);

    
    get_lisp_addr_from_char("128.0.0.0",&dest);
    
    add_route_v4(tun_ifindex,
                 &dest,
                 NULL,
                 NULL,
                 prefix_len,
                 metric);
    
    //install_default_route(tun_ifindex,AF_INET6);

    open_iface_binded_sockets();

    set_default_output_ifaces();


    //data_out_socket = open_device_binded_raw_socket(device,AF_INET);
    //open_device_binded_raw_socket(device,AF_INET6);


    syslog(LOG_INFO, "*************** Created tun interface *****************");

    ipv4_control_input_fd = open_control_input_socket(AF_INET);
    printf("socket control lisp input: %d\n",ipv4_control_input_fd);

    ipv4_data_input_fd = open_data_input_socket(AF_INET);
    printf("socket data lisp input: %d\n",ipv4_data_input_fd);

    
    /*
     *  Register to the Map-Server(s)
     */

    map_register (NULL,NULL);

    event_loop();

    syslog(LOG_INFO, "Exiting...");         /* event_loop returned bad */
    closelog();
    return(0);
}

/*
 *      main event loop
 *
 *      should never return (in theory)
 */

void event_loop()
{
    int    max_fd;
    fd_set readfds;
    int    retval;
    
    /*
     *  calculate the max_fd for select.
     */
    
    max_fd = ipv4_data_input_fd;
    max_fd = (max_fd > ipv4_control_input_fd)   ? max_fd : ipv4_control_input_fd;
    max_fd = (max_fd > tun_receive_fd)          ? max_fd : tun_receive_fd;
    max_fd = (max_fd > timers_fd)               ? max_fd : timers_fd;
    for (;;) {
        
        FD_ZERO(&readfds);
        FD_SET(tun_receive_fd, &readfds);
        FD_SET(ipv4_data_input_fd, &readfds);
        FD_SET(ipv4_control_input_fd, &readfds);
        FD_SET(timers_fd, &readfds);
        
        retval = have_input(max_fd, &readfds);
        if (retval == -1) {
            break;           /* doom */
        }
        if (retval == BAD) {
            continue;        /* interrupted */
        }
        
        if (FD_ISSET(ipv4_data_input_fd, &readfds)) {
            //printf("Recieved packet in the data input buffer (4341)\n");
            process_input_packet(ipv4_data_input_fd, tun_receive_fd);
        }
        if (FD_ISSET(ipv4_control_input_fd, &readfds)) {
            //printf("Recieved packet in the control input buffer (4342)\n");
            process_lisp_ctr_msg(ipv4_control_input_fd, AF_INET);
        }
        if (FD_ISSET(tun_receive_fd, &readfds)) {
            //printf("Recieved packet in the tun buffer\n");
            process_output_packet(tun_receive_fd, tun_receive_buf, TUN_RECEIVE_SIZE);
        }
        if (FD_ISSET(timers_fd,&readfds)){
            process_timer_signal();
        }
    }

    
    
//     int    max_fd;
//     fd_set readfds;
//     time_t curr,prev; //Modified by acabello
// 
//     
//     /*
//      *  calculate the max_fd for select. Is there a better way
//      *  to do this?
//      */
//     max_fd = (ipv4_data_input_fd > ipv6_data_input_fd) ? ipv4_data_input_fd : ipv6_data_input_fd;
//     max_fd = (max_fd > netlink_fd)           ? max_fd : netlink_fd;
//     max_fd = (max_fd > nlh.fd)               ? max_fd : nlh.fd;
//     max_fd = (max_fd > timers_fd)            ? max_fd : timers_fd;
// #ifdef LISPMOBMH
//     max_fd = (max_fd > smr_timer_fd)		 ? max_fd : smr_timer_fd;
// #endif
//     // Modified by acabello
//     prev=time(NULL);
// 
//     for (EVER) {
//         FD_ZERO(&readfds);
//         FD_SET(ipv4_data_input_fd,&readfds);
//         FD_SET(ipv6_data_input_fd,&readfds);
//         FD_SET(netlink_fd,&readfds);
//         FD_SET(nlh.fd, &readfds);
//         FD_SET(timers_fd, &readfds);
// #ifdef LISPMOBMH
//         FD_SET(smr_timer_fd,&readfds);
// #endif
//         if (have_input(max_fd,&readfds) == -1)
//             break;                              /* news is bad */
//         if (FD_ISSET(ipv4_data_input_fd,&readfds))
//             process_lisp_msg(ipv4_data_input_fd, AF_INET);
//         if (FD_ISSET(ipv6_data_input_fd,&readfds))
//             process_lisp_msg(ipv6_data_input_fd, AF_INET6);
//         if (FD_ISSET(netlink_fd,&readfds))
//             process_netlink_msg();
//         if (FD_ISSET(nlh.fd,&readfds)) 
//             process_netlink_iface();
//         if (FD_ISSET(timers_fd,&readfds))
//             process_timer_signal();
// #ifdef LISPMOBMH
//         if (FD_ISSET(smr_timer_fd,&readfds))
//                 smr_on_timeout();
// #endif
//         // Modified by acabello
//         // Each second expire_datacache
//         // This can be improved by using threading and timer_create()
//         curr=time(NULL);
//         if ((curr-prev)>LISPD_EXPIRE_TIMEOUT) {
//                 expire_datacache();
//                 prev=time(NULL);
//             }
//     }

}

/*
 *      signal_handler --
 *
 */

void signal_handler(int sig) {
    switch (sig) {
    case SIGHUP:
        /* TODO: SIGHUP should trigger reloading the configuration file */
        syslog(LOG_WARNING, "Received SIGHUP signal.");
        break;
    case SIGTERM:
        /* SIGTERM is the default signal sent by 'kill'. Exit cleanly */
        syslog(LOG_WARNING, "Received SIGTERM signal. Cleaning up...");
        exit_cleanup();
        break;
    case SIGINT:
        /* SIGINT is sent by pressing Ctrl-C. Exit cleanly */
        syslog(LOG_WARNING, "Terminal interrupt. Cleaning up...");
        exit_cleanup();
        break;
    default:
        syslog(LOG_WARNING,"Unhandled signal (%d)", sig);
        exit(EXIT_FAILURE);
    }
}



int process_timer_signal()
{
    int sig;
    int  bytes;

    bytes = read(timers_fd, &sig, sizeof(sig));

    if (bytes != sizeof(sig)) {
        syslog(LOG_WARNING, "process_event_signal(): nothing to read");
        return(-1);
    }

    if (sig == SIGRTMIN) {
        handle_timers();
    }
    return(0);
}



/*
 * event_sig_handler
 *
 * Forward signal to the fd for handling in the event loop
 */
static void event_sig_handler(int sig)
{
    if (write(signal_pipe[1], &sig, sizeof(sig)) != sizeof(sig)) {
        syslog(LOG_ERR, "write signal %d: %s", sig, strerror(errno));
    }
}


/*
 * build_timer_event_socket
 *
 * Set up the event handler socket. This is
 * used to serialize events like timer expirations that
 * we would rather deal with synchronously. This avoids
 * having to deal with all sorts of locking and multithreading
 * nonsense.
 */
int build_timers_event_socket()
{
    int flags;
    struct sigaction sa;

    if (pipe(signal_pipe) == -1) {
        syslog(LOG_ERR, "signal pipe setup failed %s", strerror(errno));
        return 0;
    }
    timers_fd = signal_pipe[0];

    if ((flags = fcntl(timers_fd, F_GETFL, 0)) == -1) {
        syslog(LOG_ERR, "fcntl() F_GETFL failed %s", strerror(errno));
        return 0;
    }
    if (fcntl(timers_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        syslog(LOG_ERR, "fcntl() set O_NONBLOCK failed %s", strerror(errno));
        return 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = event_sig_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction() failed %s", strerror(errno));
    }
    return(1);
}

/*
 *  exit_cleanup()
 *
 *  remove lisp modules (and restore network settings)
 */

void exit_cleanup(void) {

    /*Need iterator to remove state associated to each interface*/
    //iface_list_elt *list_iterator = NULL;

    /* Close timer file descriptors */
    close(timers_fd);

    /* Close receive sockets */
    close(tun_receive_fd);
    close(ipv4_data_input_fd);
    //close(ipv6_data_input_fd);
    close(ipv4_control_input_fd);
    //close(ipv6_control_input_fd);

    /* Close syslog */
    closelog();

    exit(EXIT_SUCCESS);
}




/*
 * Editor modelines
 *
 * vi: set shiftwidth=4 tabstop=4 expandtab:
 * :indentSize=4:tabSize=4:noTabs=true:
 */
