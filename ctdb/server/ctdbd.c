/* 
   standalone ctdb daemon

   Copyright (C) Andrew Tridgell  2006

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/filesys.h"
#include "system/time.h"
#include "system/wait.h"
#include "system/network.h"
#include "system/syslog.h"

#include <popt.h>
#include <talloc.h>
/* Allow use of deprecated function tevent_loop_allow_nesting() */
#define TEVENT_DEPRECATED
#include <tevent.h>

#include "lib/util/debug.h"
#include "lib/util/samba_util.h"

#include "ctdb_private.h"

#include "common/reqid.h"
#include "common/system.h"
#include "common/common.h"
#include "common/logging.h"

static struct {
	const char *debuglevel;
	const char *transport;
	const char *myaddress;
	const char *logging;
	const char *recovery_lock;
	const char *db_dir;
	const char *db_dir_persistent;
	const char *db_dir_state;
	int         nosetsched;
	int         start_as_disabled;
	int         start_as_stopped;
	int         no_lmaster;
	int         no_recmaster;
	int	    script_log_level;
	int         max_persistent_check_errors;
} options = {
	.debuglevel = "NOTICE",
	.transport = "tcp",
	.logging = "file:" LOGDIR "/log.ctdb",
	.db_dir = CTDB_VARDIR "/volatile",
	.db_dir_persistent = CTDB_VARDIR "/persistent",
	.db_dir_state = CTDB_VARDIR "/state",
	.script_log_level = DEBUG_ERR,
};

int script_log_level;
bool fast_start;

/*
  called by the transport layer when a packet comes in
*/
static void ctdb_recv_pkt(struct ctdb_context *ctdb, uint8_t *data, uint32_t length)
{
	struct ctdb_req_header *hdr = (struct ctdb_req_header *)data;

	CTDB_INCREMENT_STAT(ctdb, node_packets_recv);

	/* up the counter for this source node, so we know its alive */
	if (ctdb_validate_pnn(ctdb, hdr->srcnode)) {
		/* as a special case, redirected calls don't increment the rx_cnt */
		if (hdr->operation != CTDB_REQ_CALL ||
		    ((struct ctdb_req_call_old *)hdr)->hopcount == 0) {
			ctdb->nodes[hdr->srcnode]->rx_cnt++;
		}
	}

	ctdb_input_pkt(ctdb, hdr);
}

static const struct ctdb_upcalls ctdb_upcalls = {
	.recv_pkt       = ctdb_recv_pkt,
	.node_dead      = ctdb_node_dead,
	.node_connected = ctdb_node_connected
};

static struct ctdb_context *ctdb_init(struct tevent_context *ev)
{
	int ret;
	struct ctdb_context *ctdb;

	ctdb = talloc_zero(ev, struct ctdb_context);
	if (ctdb == NULL) {
		DBG_ERR("Memory error\n");
		return NULL;
	}
	ctdb->ev  = ev;

	/* Wrap early to exercise code. */
	ret = reqid_init(ctdb, INT_MAX-200, &ctdb->idr);
	if (ret != 0) {
		D_ERR("reqid_init failed (%s)\n", strerror(ret));
		talloc_free(ctdb);
		return NULL;
	}

	ret = srvid_init(ctdb, &ctdb->srv);
	if (ret != 0) {
		D_ERR("srvid_init failed (%s)\n", strerror(ret));
		talloc_free(ctdb);
		return NULL;
	}

	ret = ctdb_set_socketname(ctdb, CTDB_SOCKET);
	if (ret != 0) {
		DBG_ERR("ctdb_set_socketname failed.\n");
		talloc_free(ctdb);
		return NULL;
	}

	gettimeofday(&ctdb->ctdbd_start_time, NULL);

	gettimeofday(&ctdb->last_recovery_started, NULL);
	gettimeofday(&ctdb->last_recovery_finished, NULL);

	ctdb->recovery_mode    = CTDB_RECOVERY_NORMAL;
	ctdb->recovery_master  = (uint32_t)-1;

	ctdb->upcalls = &ctdb_upcalls;

	ctdb->statistics.statistics_start_time = timeval_current();

	ctdb->capabilities = CTDB_CAP_DEFAULT;

	/*
	 * Initialise this node's PNN to the unknown value.  This will
	 * be set to the correct value by either ctdb_add_node() as
	 * part of loading the nodes file or by
	 * ctdb_tcp_listen_automatic() when the transport is
	 * initialised.  At some point we should de-optimise this and
	 * pull it out into ctdb_start_daemon() so it is done clearly
	 * and only in one place.
	 */
	ctdb->pnn = CTDB_UNKNOWN_PNN;

	ctdb->do_checkpublicip = true;

	return ctdb;
}


/*
  main program
*/
int main(int argc, const char *argv[])
{
	struct ctdb_context *ctdb = NULL;
	int interactive = 0;
	const char *ctdb_socket;

	struct poptOption popt_options[] = {
		POPT_AUTOHELP
		{ "debug", 'd', POPT_ARG_STRING, &options.debuglevel, 0, "debug level", NULL },
		{ "interactive", 'i', POPT_ARG_NONE, &interactive, 0, "don't fork", NULL },
		{ "logging", 0, POPT_ARG_STRING, &options.logging, 0, "logging method to be used", NULL },
		{ "listen", 0, POPT_ARG_STRING, &options.myaddress, 0, "address to listen on", "address" },
		{ "transport", 0, POPT_ARG_STRING, &options.transport, 0, "protocol transport", NULL },
		{ "dbdir", 0, POPT_ARG_STRING, &options.db_dir, 0, "directory for the tdb files", NULL },
		{ "dbdir-persistent", 0, POPT_ARG_STRING, &options.db_dir_persistent, 0, "directory for persistent tdb files", NULL },
		{ "dbdir-state", 0, POPT_ARG_STRING, &options.db_dir_state, 0, "directory for internal state tdb files", NULL },
		{ "reclock", 0, POPT_ARG_STRING, &options.recovery_lock, 0, "recovery lock", "lock" },
		{ "nosetsched", 0, POPT_ARG_NONE, &options.nosetsched, 0, "disable setscheduler SCHED_FIFO call, use mmap for tdbs", NULL },
		{ "start-as-disabled", 0, POPT_ARG_NONE, &options.start_as_disabled, 0, "Node starts in disabled state", NULL },
		{ "start-as-stopped", 0, POPT_ARG_NONE, &options.start_as_stopped, 0, "Node starts in stopped state", NULL },
		{ "no-lmaster", 0, POPT_ARG_NONE, &options.no_lmaster, 0, "disable lmaster role on this node", NULL },
		{ "no-recmaster", 0, POPT_ARG_NONE, &options.no_recmaster, 0, "disable recmaster role on this node", NULL },
		{ "script-log-level", 0, POPT_ARG_INT, &options.script_log_level, 0, "log level of event script output", NULL },
		{ "max-persistent-check-errors", 0, POPT_ARG_INT,
		  &options.max_persistent_check_errors, 0,
		  "max allowed persistent check errors (default 0)", NULL },
		POPT_TABLEEND
	};
	int opt, ret;
	const char **extra_argv;
	poptContext pc;
	struct tevent_context *ev;
	const char *ctdb_base;
	const char *t;

	/*
	 * Basic setup
	 */

	talloc_enable_null_tracking();

	fault_setup();

	ev = tevent_context_init(NULL);
	if (ev == NULL) {
		fprintf(stderr, "tevent_context_init() failed\n");
		exit(1);
	}
	tevent_loop_allow_nesting(ev);

	ctdb = ctdb_init(ev);
	if (ctdb == NULL) {
		fprintf(stderr, "Failed to init ctdb\n");
		exit(1);
	}

	/* Default value for CTDB_BASE - don't override */
	setenv("CTDB_BASE", CTDB_ETCDIR, 0);
	ctdb_base = getenv("CTDB_BASE");
	if (ctdb_base == NULL) {
		D_ERR("CTDB_BASE not set\n");
		exit(1);
	}

	/*
	 * Command-line option handling
	 */

	pc = poptGetContext(argv[0], argc, argv, popt_options, POPT_CONTEXT_KEEP_FIRST);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		default:
			fprintf(stderr, "Invalid option %s: %s\n", 
				poptBadOption(pc, 0), poptStrerror(opt));
			goto fail;
		}
	}

	/* If there are extra arguments then exit with usage message */
	extra_argv = poptGetArgs(pc);
	if (extra_argv) {
		extra_argv++;
		if (extra_argv[0])  {
			poptPrintHelp(pc, stdout, 0);
			goto fail;
		}
	}

	/*
	 * Logging setup/options
	 */

	/* Log to stderr when running as interactive */
	if (interactive) {
		options.logging = "file:";
	}

	if (strcmp(options.logging, "syslog") != 0) {
		/* This can help when CTDB logging is misconfigured */
		syslog(LOG_DAEMON|LOG_NOTICE,
		       "CTDB logging to location %s",
		       options.logging);
	}

	/* Initialize logging and set the debug level */
	if (!ctdb_logging_init(ctdb, options.logging, options.debuglevel)) {
		goto fail;
	}
	setenv("CTDB_LOGGING", options.logging, 1);
	setenv("CTDB_DEBUGLEVEL", debug_level_to_string(DEBUGLEVEL), 1);

	script_log_level = options.script_log_level;

	D_NOTICE("CTDB starting on node\n");

	/*
	 * Cluster setup/options
	 */

	ret = ctdb_set_transport(ctdb, options.transport);
	if (ret == -1) {
		D_ERR("ctdb_set_transport failed - %s\n", ctdb_errstr(ctdb));
		goto fail;
	}

	if (options.recovery_lock == NULL) {
		D_WARNING("Recovery lock not set\n");
	}
	ctdb->recovery_lock = options.recovery_lock;

	/* tell ctdb what address to listen on */
	if (options.myaddress) {
		ret = ctdb_set_address(ctdb, options.myaddress);
		if (ret == -1) {
			D_ERR("ctdb_set_address failed - %s\n",
			      ctdb_errstr(ctdb));
			goto fail;
		}
	}

	/* tell ctdb what nodes are available */
	ctdb->nodes_file = talloc_asprintf(ctdb, "%s/nodes", ctdb_base);
	if (ctdb->nodes_file == NULL) {
		DBG_ERR(" Out of memory\n");
		goto fail;
	}
	ctdb_load_nodes_file(ctdb);

	/*
	 * Database setup/options
	 */

	ctdb->db_directory = options.db_dir;
	mkdir_p_or_die(ctdb->db_directory, 0700);

	ctdb->db_directory_persistent = options.db_dir_persistent;
	mkdir_p_or_die(ctdb->db_directory_persistent, 0700);

	ctdb->db_directory_state = options.db_dir_state;
	mkdir_p_or_die(ctdb->db_directory_state, 0700);

	if (options.max_persistent_check_errors < 0) {
		ctdb->max_persistent_check_errors = 0xFFFFFFFFFFFFFFFFLL;
	} else {
		ctdb->max_persistent_check_errors =
			(uint64_t)options.max_persistent_check_errors;
	}

	/*
	 * Legacy setup/options
	 */

	ctdb->start_as_disabled = options.start_as_disabled;
	ctdb->start_as_stopped  = options.start_as_stopped;

	/* set ctdbd capabilities */
	if (options.no_lmaster != 0) {
		ctdb->capabilities &= ~CTDB_CAP_LMASTER;
	}
	if (options.no_recmaster != 0) {
		ctdb->capabilities &= ~CTDB_CAP_RECMASTER;
	}

	ctdb->do_setsched = (options.nosetsched != 1);

	/*
	 * Miscellaneous setup
	 */

	ctdb_tunables_set_defaults(ctdb);

	ctdb->event_script_dir = talloc_asprintf(ctdb,
						 "%s/events.d",
						 ctdb_base);
	if (ctdb->event_script_dir == NULL) {
		DBG_ERR("Out of memory\n");
		goto fail;
	}

	ctdb->notification_script = talloc_asprintf(ctdb,
						    "%s/notify.sh",
						    ctdb_base);
	if (ctdb->notification_script == NULL) {
		D_ERR("Unable to set notification script\n");
		goto fail;
	}

	/*
	 * Testing and debug options
	 */

	/* Environment variable overrides default */
	ctdbd_pidfile = getenv("CTDB_PIDFILE");
	if (ctdbd_pidfile == NULL) {
		ctdbd_pidfile = CTDB_RUNDIR "/ctdbd.pid";
	}

	/* Environment variable overrides default */
	ctdb_socket = getenv("CTDB_SOCKET");
	if (ctdb_socket == NULL) {
		ctdb_socket = CTDB_SOCKET;
	}
	ret = ctdb_set_socketname(ctdb, ctdb_socket);
	if (ret == -1) {
		D_ERR("ctdb_set_socketname() failed\n");
		goto fail;
	}

	t = getenv("CTDB_TEST_MODE");
	if (t != NULL) {
		ctdb->do_setsched = false;
		ctdb->do_checkpublicip = false;
		fast_start = true;
	}

	/* start the protocol running (as a child) */
	return ctdb_start_daemon(ctdb, interactive?false:true);

fail:
	talloc_free(ctdb);
	exit(1);
}
