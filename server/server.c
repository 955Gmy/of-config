
/* Copyright (c) 2015 Open Networking Foundation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <libnetconf_xml.h>

#include "common.h"

/* TODO: move this into configure */
#define CONFDIR "/etc/ofconfig/"

/* ietf-netconf-server transAPI structure from netconf-server-transapi.c */
extern struct transapi server_transapi;

/* OF-CONFIG datastore functions from ofconfig-datastore.c */
extern struct ncds_custom_funcs ofcds_funcs;

/* main loop flag */
volatile int mainloop = 0;

/* Print usage help */
static void
print_usage(char *progname)
{
    fprintf(stdout, "Usage: %s [-fh] [-v level]\n", progname);
    fprintf(stdout, " -f,--foreground        run in foreground\n");
    fprintf(stdout, " -h,--help              display help\n");
    fprintf(stdout, " -v,--verbose level     verbose output level\n");
    exit(0);
}

#define OPTSTRING "fhv:"

/*!
 * \brief Signal handler
 *
 * Handles received UNIX signals and sets value to control main loop
 *
 * \param sig 	signal number
 */
void
signal_handler(int sig)
{
    nc_verb_verbose("Signal %d received.", sig);

    switch (sig) {
    case SIGINT:
    case SIGTERM:
    case SIGQUIT:
    case SIGABRT:
    case SIGKILL:
        if (mainloop == 0) {
            /* first attempt */
            mainloop = 1;
        } else {
            /* second attempt */
            nc_verb_error("Hey! I need some time, be patient next time!");
            exit(EXIT_FAILURE);
        }
        break;
    default:
        nc_verb_error("Exiting on signal: %d", sig);
        exit(EXIT_FAILURE);
        break;
    }
}

int
main(int argc, char **argv)
{
    const char* optstring = "fhv:";
    const struct option longopts[] = {
        {"foreground", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {"verbose", required_argument, 0, 'v'},
        {0, 0, 0, 0}
    };
    int longindex, next_option;
    int daemonize = 1, verbose = 0;
    int retval = EXIT_SUCCESS, r;
    char *aux_string;

    struct sigaction action;
    sigset_t block_mask;

    struct {
        struct ncds_ds *server;
        ncds_id server_id;
    } ds = {
    NULL, -1};

#if 0
    conn_t *conn = NULL;
#endif

    /* initialize message system and set verbose and debug variables */
    if ((aux_string = getenv(ENVIRONMENT_VERBOSE)) == NULL) {
        /* default verbose level */
        verbose = NC_VERB_ERROR;
    } else {
        verbose = atoi(aux_string);
    }

    /* parse given options */
    while ((next_option = getopt_long(argc, argv, optstring, longopts,
                    &longindex)) != -1) {
        switch (next_option) {
        case 'f':
            daemonize = 0;
            break;
        case 'h':
            print_usage(argv[0]);
            break;
        case 'v':
            verbose = atoi(optarg);
            break;
        default:
            print_usage(argv[0]);
            break;
        }
    }

    /* set signal handler */
    sigfillset(&block_mask);
    action.sa_handler = signal_handler;
    action.sa_mask = block_mask;
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGKILL, &action, NULL);

    /* set verbose message printer callback */
    nc_callback_print(clb_print);

    /* normalize value if not from the enum */
    if (verbose < NC_VERB_ERROR) {
        nc_verbosity(NC_VERB_ERROR);
    } else if (verbose > NC_VERB_DEBUG) {
        nc_verbosity(NC_VERB_DEBUG);
    } else {
        nc_verbosity(verbose);
    }

    /* go to the background as a daemon */
    if (daemonize == 1) {
        if (daemon(0, 0) != 0) {
            nc_verb_error("Going to background failed (%s)", strerror(errno));
            return (EXIT_FAILURE);
        }
        openlog("netopeer-server", LOG_PID, LOG_DAEMON);
    } else {
        openlog("netopeer-server", LOG_PID | LOG_PERROR, LOG_DAEMON);
    }

    /* make sure we have sufficient rights to communicate with OVSDB */
    /* TODO */

    /* init libnetconf for a multilayer server */
    if ((r = nc_init(NC_INIT_ALL | NC_INIT_MULTILAYER)) < 0) {
        nc_verb_error("libnetconf initialization failed.");
        return (EXIT_FAILURE);
    }

#if 0
    /* Initiate communication subsystem for communication with agents */
    conn = comm_init(r);
    if (conn == NULL) {
        nc_verb_error("Communication subsystem not initiated.");
        return (EXIT_FAILURE);
    }
#endif

    /* prepare the ietf-netconf-server module */
    ncds_add_model(CONFDIR "/ietf-netconf-server/ietf-x509-cert-to-name.yin");
    ds.server = ncds_new_transapi_static(NCDS_TYPE_FILE,
                                         CONFDIR
                                         "/ietf-netconf-server/ietf-netconf-server.yin",
                                         &server_transapi);
    if (ds.server == NULL) {
        retval = EXIT_FAILURE;
        nc_verb_error("Creating ietf-netconf-server datastore failed.");
        goto cleanup;
    }
    ncds_file_set_path(ds.server,
                       CONFDIR "/ietf-netconf-server/datastore.xml");
    ncds_add_model(CONFDIR "/ietf-netconf-server/ietf-x509-cert-to-name.yin");
    ncds_feature_enable("ietf-netconf-server", "ssh");
    ncds_feature_enable("ietf-netconf-server", "inbound-ssh");
    if ((ds.server_id = ncds_init(ds.server)) < 0) {
        retval = EXIT_FAILURE;
        nc_verb_error
            ("Initiating ietf-netconf-server datastore failed (error code %d).",
             ds.ofc_id);
        goto cleanup;
    }

    if (ncds_consolidate() != 0) {
        retval = EXIT_FAILURE;
        nc_verb_error("Consoidating data models failed.");
        goto cleanup;
    }

    if (ncds_device_init(&(ds.server_id), NULL, 1) != 0) {
        retval = EXIT_FAILURE;
        nc_verb_error("Initiating ietf-netconf-server module failed.");
        goto cleanup;
    }

    nc_verb_verbose("OF-CONFIG server successfully initialized.");

    while (!mainloop) {
#if 0
        comm_loop(conn, 500);
#else
        sleep(1);
#endif
    }

cleanup:

    /* cleanup */
    nc_close();

    return (retval);
}
