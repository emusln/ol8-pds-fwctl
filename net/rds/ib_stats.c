/*
 * Copyright (c) 2006 Oracle.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <linux/percpu.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>

#include "rds.h"
#include "ib.h"

DEFINE_PER_CPU(struct rds_ib_statistics, rds_ib_stats) ____cacheline_aligned;

static char *rds_ib_stat_names[] = {
	"ib_connect_raced",
	"ib_listen_closed_stale",
	"ib_evt_handler_call",
	"ib_tasklet_call",
	"ib_tx_cq_event",
	"ib_tx_ring_full",
	"ib_tx_throttle",
	"ib_tx_sg_mapping_failure",
	"ib_tx_stalled",
	"ib_tx_credit_updates",
	"ib_rx_cq_event",
	"ib_rx_ring_empty",
	"ib_rx_refill_from_cm",
	"ib_rx_refill_from_cq",
	"ib_rx_refill_from_thread",
	"ib_rx_refill_lock_taken",
	"ib_rx_alloc_limit",
	"ib_rx_total_frags",
	"ib_rx_total_incs",
	"ib_rx_credit_updates",
	"ib_rx_cache_get",
	"ib_rx_cache_put",
	"ib_rx_cache_put_alloc",
	"ib_rx_cache_put_free",
	"ib_rx_cache_alloc",
	"ib_rx_cache_free",
	"ib_rx_cache_get_percpu",
	"ib_rx_cache_get_ready",
	"ib_rx_cache_get_miss",
	"ib_rx_cache_put_percpu",
	"ib_ack_sent",
	"ib_ack_send_failure",
	"ib_ack_send_delayed",
	"ib_ack_send_piggybacked",
	"ib_ack_received",
	"ib_rdma_mr_8k_alloc",
	"ib_rdma_mr_8k_free",
	"ib_rdma_mr_8k_used",
	"ib_rdma_mr_8k_pool_flush",
	"ib_rdma_mr_8k_pool_depleted",
	"ib_rdma_mr_1m_alloc",
	"ib_rdma_mr_1m_free",
	"ib_rdma_mr_1m_used",
	"ib_rdma_mr_1m_pool_flush",
	"ib_rdma_mr_1m_pool_depleted",
	"ib_rdma_flush_mr_pool_avoided",
	"ib_atomic_cswp",
	"ib_atomic_fadd",
	"ib_srq_lows",
	"ib_srq_refills",
	"ib_srq_empty_refills",
	"ib_recv_cache_added",
	"ib_recv_cache_removed",
	"ib_recv_nmb_cache_added",
	"ib_recv_nmb_cache_removed",
	"ib_yield_yielding",
	"ib_yield_right_of_way",
	"ib_yield_stale",
	"ib_yield_expired",
	"ib_yield_accepting",
	"ib_yield_success",
	"ib_cm_watchdog_triggered",
	"ib_frwr_registrations",
	"ib_frwr_invalidates",
	"ib_frwr_conn_qp_timeout",
	"ib_frwr_freg_qp_timeout",
	"ib_rx_limit_reached",
};

unsigned int rds_ib_stats_info_copy(struct rds_info_iterator *iter,
				    unsigned int avail)
{
	struct rds_ib_statistics stats = {0, };
	uint64_t *src;
	uint64_t *sum;
	size_t i;
	int cpu;

	if (avail < ARRAY_SIZE(rds_ib_stat_names))
		goto out;

	for_each_possible_cpu(cpu) {
		src = (uint64_t *)&(per_cpu(rds_ib_stats, cpu));
		sum = (uint64_t *)&stats;
		for (i = 0; i < sizeof(stats) / sizeof(uint64_t); i++)
			*(sum++) += *(src++);
	}

	rds_stats_info_copy(iter, (uint64_t *)&stats, rds_ib_stat_names,
			    ARRAY_SIZE(rds_ib_stat_names));
out:
	return ARRAY_SIZE(rds_ib_stat_names);
}

void rds_ib_stats_print(const char *where)
{
	struct rds_ib_statistics ibstats = {};
	uint64_t *src;
	uint64_t *sum;
	size_t i;
	int cpu;
	size_t nibstats = sizeof(ibstats) / sizeof(uint64_t);

	for_each_possible_cpu(cpu) {
		src = (uint64_t *)&(per_cpu(rds_ib_stats, cpu));
		sum = (uint64_t *)&ibstats;
		for (i = 0; i < nibstats; i++)
			*(sum++) += *(src++);
	}

	sum = (uint64_t *)&ibstats;
	for (i = 0; i < nibstats; i++)
		if (sum[i])
			pr_info("%s %s %lld\n", where, rds_ib_stat_names[i], sum[i]);
}
EXPORT_SYMBOL_GPL(rds_ib_stats_print);