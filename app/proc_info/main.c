/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_tailq.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_log.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_string_fns.h>

/* Maximum long option length for option parsing. */
#define MAX_LONG_OPT_SZ 64
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

#define MAX_STRING_LEN 256

/**< mask of enabled ports */
static uint32_t enabled_port_mask;
/**< Enable stats. */
static uint32_t enable_stats;
/**< Enable xstats. */
static uint32_t enable_xstats;
/**< Enable collectd format*/
static uint32_t enable_collectd_format;
/**< FD to send collectd format messages to STDOUT*/
static int stdout_fd;
/**< Host id process is running on */
static char host_id[MAX_LONG_OPT_SZ];
/**< Enable stats reset. */
static uint32_t reset_stats;
/**< Enable xstats reset. */
static uint32_t reset_xstats;
/**< Enable memory info. */
static uint32_t mem_info;

/**< display usage */
static void
proc_info_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK\n"
		"  -m to display DPDK memory zones, segments and TAILQ information\n"
		"  -p PORTMASK: hexadecimal bitmask of ports to retrieve stats for\n"
		"  --stats: to display port statistics, enabled by default\n"
		"  --xstats: to display extended port statistics, disabled by "
			"default\n"
		"  --stats-reset: to reset port statistics\n"
		"  --xstats-reset: to reset port extended statistics\n"
		"  --collectd-format: to print statistics to STDOUT in expected by collectd format\n"
		"  --host-id STRING: host id used to identify the system process is running on\n",
		prgname);
}

/*
 * Parse the portmask provided at run time.
 */
static int
parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	errno = 0;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0') ||
		(errno != 0)) {
		printf("%s ERROR parsing the port mask\n", __func__);
		return -1;
	}

	if (pm == 0)
		return -1;

	return pm;

}

static int
proc_info_preparse_args(int argc, char **argv)
{
	char *prgname = argv[0];
	int i;

	for (i = 0; i < argc; i++) {
		/* Print stats or xstats to STDOUT in collectd format */
		if (!strncmp(argv[i], "--collectd-format", MAX_LONG_OPT_SZ)) {
			enable_collectd_format = 1;
			stdout_fd = dup(STDOUT_FILENO);
			close(STDOUT_FILENO);
		}
		if (!strncmp(argv[i], "--host-id", MAX_LONG_OPT_SZ)) {
			if ((i + 1) == argc) {
				printf("Invalid host id or not specified\n");
				proc_info_usage(prgname);
				return -1;
			}
			strncpy(host_id, argv[i+1], sizeof(host_id));
		}
	}

	if (!strlen(host_id)) {
		int err = gethostname(host_id, MAX_LONG_OPT_SZ-1);

		if (err)
			strcpy(host_id, "unknown");
	}

	return 0;
}

/* Parse the argument given in the command line of the application */
static int
proc_info_parse_args(int argc, char **argv)
{
	int opt;
	int option_index;
	char *prgname = argv[0];
	static struct option long_option[] = {
		{"stats", 0, NULL, 0},
		{"stats-reset", 0, NULL, 0},
		{"xstats", 0, NULL, 0},
		{"xstats-reset", 0, NULL, 0},
		{"collectd-format", 0, NULL, 0},
		{"host-id", 0, NULL, 0},
		{NULL, 0, 0, 0}
	};

	if (argc == 1)
		proc_info_usage(prgname);

	/* Parse command line */
	while ((opt = getopt_long(argc, argv, "p:m",
			long_option, &option_index)) != EOF) {
		switch (opt) {
		/* portmask */
		case 'p':
			enabled_port_mask = parse_portmask(optarg);
			if (enabled_port_mask == 0) {
				printf("invalid portmask\n");
				proc_info_usage(prgname);
				return -1;
			}
			break;
		case 'm':
			mem_info = 1;
			break;
		case 0:
			/* Print stats */
			if (!strncmp(long_option[option_index].name, "stats",
					MAX_LONG_OPT_SZ))
				enable_stats = 1;
			/* Print xstats */
			else if (!strncmp(long_option[option_index].name, "xstats",
					MAX_LONG_OPT_SZ))
				enable_xstats = 1;
			/* Reset stats */
			if (!strncmp(long_option[option_index].name, "stats-reset",
					MAX_LONG_OPT_SZ))
				reset_stats = 1;
			/* Reset xstats */
			else if (!strncmp(long_option[option_index].name, "xstats-reset",
					MAX_LONG_OPT_SZ))
				reset_xstats = 1;
			break;

		default:
			proc_info_usage(prgname);
			return -1;
		}
	}
	return 0;
}

static void
meminfo_display(void)
{
	printf("----------- MEMORY_SEGMENTS -----------\n");
	rte_dump_physmem_layout(stdout);
	printf("--------- END_MEMORY_SEGMENTS ---------\n");

	printf("------------ MEMORY_ZONES -------------\n");
	rte_memzone_dump(stdout);
	printf("---------- END_MEMORY_ZONES -----------\n");

	printf("------------- TAIL_QUEUES -------------\n");
	rte_dump_tailq(stdout);
	printf("---------- END_TAIL_QUEUES ------------\n");
}

static void
nic_stats_display(uint8_t port_id)
{
	struct rte_eth_stats stats;
	uint8_t i;

	static const char *nic_stats_border = "########################";

	rte_eth_stats_get(port_id, &stats);
	printf("\n  %s NIC statistics for port %-2d %s\n",
		   nic_stats_border, port_id, nic_stats_border);

	printf("  RX-packets: %-10"PRIu64"  RX-errors:  %-10"PRIu64
	       "  RX-bytes:  %-10"PRIu64"\n", stats.ipackets, stats.ierrors,
	       stats.ibytes);
	printf("  RX-nombuf:  %-10"PRIu64"\n", stats.rx_nombuf);
	printf("  TX-packets: %-10"PRIu64"  TX-errors:  %-10"PRIu64
	       "  TX-bytes:  %-10"PRIu64"\n", stats.opackets, stats.oerrors,
	       stats.obytes);

	printf("\n");
	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS; i++) {
		printf("  Stats reg %2d RX-packets: %-10"PRIu64
		       "  RX-errors: %-10"PRIu64
		       "  RX-bytes: %-10"PRIu64"\n",
		       i, stats.q_ipackets[i], stats.q_errors[i], stats.q_ibytes[i]);
	}

	printf("\n");
	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS; i++) {
		printf("  Stats reg %2d TX-packets: %-10"PRIu64
		       "  TX-bytes: %-10"PRIu64"\n",
		       i, stats.q_opackets[i], stats.q_obytes[i]);
	}

	printf("  %s############################%s\n",
		   nic_stats_border, nic_stats_border);
}

static void
nic_stats_clear(uint8_t port_id)
{
	printf("\n Clearing NIC stats for port %d\n", port_id);
	rte_eth_stats_reset(port_id);
	printf("\n  NIC statistics for port %d cleared\n", port_id);
}

static void collectd_resolve_cnt_type(char *cnt_type, size_t cnt_type_len,
				      const char *cnt_name) {
	char *type_end = strrchr(cnt_name, '_');

	if ((type_end != NULL) &&
	    (strncmp(cnt_name, "rx_", strlen("rx_")) == 0)) {
		if (strncmp(type_end, "_errors", strlen("_errors")) == 0)
			strncpy(cnt_type, "if_rx_errors", cnt_type_len);
		else if (strncmp(type_end, "_dropped", strlen("_dropped")) == 0)
			strncpy(cnt_type, "if_rx_dropped", cnt_type_len);
		else if (strncmp(type_end, "_bytes", strlen("_bytes")) == 0)
			strncpy(cnt_type, "if_rx_octets", cnt_type_len);
		else if (strncmp(type_end, "_packets", strlen("_packets")) == 0)
			strncpy(cnt_type, "if_rx_packets", cnt_type_len);
		else if (strncmp(type_end, "_placement",
				 strlen("_placement")) == 0)
			strncpy(cnt_type, "if_rx_errors", cnt_type_len);
		else if (strncmp(type_end, "_buff", strlen("_buff")) == 0)
			strncpy(cnt_type, "if_rx_errors", cnt_type_len);
		else
			/* Does not fit obvious type: use a more generic one */
			strncpy(cnt_type, "derive", cnt_type_len);
	} else if ((type_end != NULL) &&
		(strncmp(cnt_name, "tx_", strlen("tx_"))) == 0) {
		if (strncmp(type_end, "_errors", strlen("_errors")) == 0)
			strncpy(cnt_type, "if_tx_errors", cnt_type_len);
		else if (strncmp(type_end, "_dropped", strlen("_dropped")) == 0)
			strncpy(cnt_type, "if_tx_dropped", cnt_type_len);
		else if (strncmp(type_end, "_bytes", strlen("_bytes")) == 0)
			strncpy(cnt_type, "if_tx_octets", cnt_type_len);
		else if (strncmp(type_end, "_packets", strlen("_packets")) == 0)
			strncpy(cnt_type, "if_tx_packets", cnt_type_len);
		else
			/* Does not fit obvious type: use a more generic one */
			strncpy(cnt_type, "derive", cnt_type_len);
	} else if ((type_end != NULL) &&
		   (strncmp(cnt_name, "flow_", strlen("flow_"))) == 0) {
		if (strncmp(type_end, "_filters", strlen("_filters")) == 0)
			strncpy(cnt_type, "operations", cnt_type_len);
		else if (strncmp(type_end, "_errors", strlen("_errors")) == 0)
			strncpy(cnt_type, "errors", cnt_type_len);
		else if (strncmp(type_end, "_filters", strlen("_filters")) == 0)
			strncpy(cnt_type, "filter_result", cnt_type_len);
	} else if ((type_end != NULL) &&
		   (strncmp(cnt_name, "mac_", strlen("mac_"))) == 0) {
		if (strncmp(type_end, "_errors", strlen("_errors")) == 0)
			strncpy(cnt_type, "errors", cnt_type_len);
	} else {
		/* Does not fit obvious type, or strrchr error: */
		/* use a more generic type */
		strncpy(cnt_type, "derive", cnt_type_len);
	}
}

static void
nic_xstats_display(uint8_t port_id)
{
	struct rte_eth_xstat_name *xstats_names;
	struct rte_eth_xstat *xstats;
	int len, ret, i;
	static const char *nic_stats_border = "########################";

	len = rte_eth_xstats_get_names(port_id, NULL, 0);
	if (len < 0) {
		printf("Cannot get xstats count\n");
		return;
	}
	xstats = malloc(sizeof(xstats[0]) * len);
	if (xstats == NULL) {
		printf("Cannot allocate memory for xstats\n");
		return;
	}

	xstats_names = malloc(sizeof(struct rte_eth_xstat_name) * len);
	if (xstats_names == NULL) {
		printf("Cannot allocate memory for xstat names\n");
		free(xstats);
		return;
	}
	if (len != rte_eth_xstats_get_names(
			port_id, xstats_names, len)) {
		printf("Cannot get xstat names\n");
		goto err;
	}

	printf("###### NIC extended statistics for port %-2d #########\n",
			   port_id);
	printf("%s############################\n",
			   nic_stats_border);
	ret = rte_eth_xstats_get(port_id, xstats, len);
	if (ret < 0 || ret > len) {
		printf("Cannot get xstats\n");
		goto err;
	}

	for (i = 0; i < len; i++) {
		if (enable_collectd_format) {
			char counter_type[MAX_STRING_LEN];
			char buf[MAX_STRING_LEN];

			collectd_resolve_cnt_type(counter_type,
						  sizeof(counter_type),
						  xstats_names[i].name);
			sprintf(buf, "PUTVAL %s/dpdkstat-port.%u/%s-%s N:%"
				PRIu64"\n", host_id, port_id, counter_type,
				xstats_names[i].name, xstats[i].value);
			write(stdout_fd, buf, strlen(buf));
		} else {
			printf("%s: %"PRIu64"\n", xstats_names[i].name,
			       xstats[i].value);
		}
	}

	printf("%s############################\n",
			   nic_stats_border);
err:
	free(xstats);
	free(xstats_names);
}

static void
nic_xstats_clear(uint8_t port_id)
{
	printf("\n Clearing NIC xstats for port %d\n", port_id);
	rte_eth_xstats_reset(port_id);
	printf("\n  NIC extended statistics for port %d cleared\n", port_id);
}

int
main(int argc, char **argv)
{
	int ret;
	int i;
	char c_flag[] = "-c1";
	char n_flag[] = "-n4";
	char mp_flag[] = "--proc-type=secondary";
	char *argp[argc + 3];
	uint8_t nb_ports;

	/* preparse app arguments */
	ret = proc_info_preparse_args(argc, argv);
	if (ret < 0) {
		printf("Failed to parse arguments\n");
		return -1;
	}

	argp[0] = argv[0];
	argp[1] = c_flag;
	argp[2] = n_flag;
	argp[3] = mp_flag;

	for (i = 1; i < argc; i++)
		argp[i + 3] = argv[i];

	argc += 3;

	ret = rte_eal_init(argc, argp);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");

	argc -= ret;
	argv += (ret - 3);

	if (!rte_eal_primary_proc_alive(NULL))
		rte_exit(EXIT_FAILURE, "No primary DPDK process is running.\n");

	/* parse app arguments */
	ret = proc_info_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid argument\n");

	if (mem_info) {
		meminfo_display();
		return 0;
	}

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	/* If no port mask was specified*/
	if (enabled_port_mask == 0)
		enabled_port_mask = 0xffff;

	for (i = 0; i < nb_ports; i++) {
		if (enabled_port_mask & (1 << i)) {
			if (enable_stats)
				nic_stats_display(i);
			else if (enable_xstats)
				nic_xstats_display(i);
			else if (reset_stats)
				nic_stats_clear(i);
			else if (reset_xstats)
				nic_xstats_clear(i);
		}
	}

	return 0;
}
