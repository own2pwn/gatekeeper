/*
 * Gatekeeper - DoS protection system.
 * Copyright (C) 2016 Digirati LTDA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>

#include "gatekeeper_lls.h"
#include "arp.h"
#include "cache.h"
#include "gatekeeper_launch.h"
#include "nd.h"

/* Length of time (in seconds) to wait between scans of the cache. */
#define LLS_CACHE_SCAN_INTERVAL 10

static struct lls_config lls_conf = {
	.arp_cache = {
		.key_len = sizeof(struct in_addr),
		.key_str_len = INET_ADDRSTRLEN,
		.name = "arp",
		.iface_enabled = iface_arp_enabled,
		.ip_str = ipv4_str,
		.ip_in_subnet = ipv4_in_subnet,
		.xmit_req = xmit_arp_req,
		.print_record = print_arp_record,
	},
	.nd_cache = {
		.key_len = sizeof(struct in6_addr),
		.key_str_len = INET6_ADDRSTRLEN,
		.name = "nd",
		.iface_enabled = iface_nd_enabled,
		.ip_str = ipv6_str,
		.ip_in_subnet = ipv6_in_subnet,
		.xmit_req = xmit_nd_req,
		.print_record = print_nd_record,
	},
};

static inline int
arp_enabled(struct lls_config *lls_conf)
{
	return lls_conf->arp_cache.iface_enabled(lls_conf->net,
			&lls_conf->net->front) ||
		lls_conf->arp_cache.iface_enabled(lls_conf->net,
			&lls_conf->net->back);
}

static inline int
nd_enabled(struct lls_config *lls_conf)
{
	return lls_conf->nd_cache.iface_enabled(lls_conf->net,
			&lls_conf->net->front) ||
		lls_conf->nd_cache.iface_enabled(lls_conf->net,
			&lls_conf->net->back);
}

struct lls_config *
get_lls_conf(void)
{
	return &lls_conf;
}

static int
cleanup_lls(void)
{
	if (nd_enabled(&lls_conf))
		lls_cache_destroy(&lls_conf.nd_cache);
	if (arp_enabled(&lls_conf))
		lls_cache_destroy(&lls_conf.arp_cache);
	destroy_mailbox(&lls_conf.requests);
	rte_timer_stop(&lls_conf.timer);
	return 0;
}

int
hold_arp(lls_req_cb cb, void *arg, struct in_addr *ip_be, unsigned int lcore_id)
{
	if (arp_enabled(&lls_conf)) {
		struct lls_hold_req hold_req = {
			.cache = &lls_conf.arp_cache,
			.hold = {
				.cb = cb,
				.arg = arg,
				.lcore_id = lcore_id,
			},
		};
		rte_memcpy(hold_req.ip_be, ip_be, sizeof(*ip_be));
		return lls_req(LLS_REQ_HOLD, &hold_req);
	}

	RTE_LOG(WARNING, GATEKEEPER,
		"lls: lcore %u called %s but ARP service is not enabled\n",
		lcore_id, __func__);
	return -1;
}

int
put_arp(struct in_addr *ip_be, unsigned int lcore_id)
{
	if (arp_enabled(&lls_conf)) {
		struct lls_put_req put_req = {
			.cache = &lls_conf.arp_cache,
			.lcore_id = lcore_id,
		};
		rte_memcpy(put_req.ip_be, ip_be, sizeof(*ip_be));
		return lls_req(LLS_REQ_PUT, &put_req);
	}

	RTE_LOG(WARNING, GATEKEEPER,
		"lls: lcore %u called %s but ARP service is not enabled\n",
		lcore_id, __func__);
	return -1;
}

int
hold_nd(lls_req_cb cb, void *arg, struct in6_addr *ip_be, unsigned int lcore_id)
{
	if (nd_enabled(&lls_conf)) {
		struct lls_hold_req hold_req = {
			.cache = &lls_conf.nd_cache,
			.hold = {
				.cb = cb,
				.arg = arg,
				.lcore_id = lcore_id,
			},
		};
		rte_memcpy(hold_req.ip_be, ip_be, sizeof(*ip_be));
		return lls_req(LLS_REQ_HOLD, &hold_req);
	}

	RTE_LOG(WARNING, GATEKEEPER,
		"lls: lcore %u called %s but ND service is not enabled\n",
		lcore_id, __func__);
	return -1;
}

int
put_nd(struct in6_addr *ip_be, unsigned int lcore_id)
{
	if (nd_enabled(&lls_conf)) {
		struct lls_put_req put_req = {
			.cache = &lls_conf.nd_cache,
			.lcore_id = lcore_id,
		};
		rte_memcpy(put_req.ip_be, ip_be, sizeof(*ip_be));
		return lls_req(LLS_REQ_PUT, &put_req);
	}

	RTE_LOG(WARNING, GATEKEEPER,
		"lls: lcore %u called %s but ND service is not enabled\n",
		lcore_id, __func__);
	return -1;
}

int
submit_nd(struct rte_mbuf *pkt, struct gatekeeper_if *iface)
{
	if (nd_enabled(&lls_conf)) {
		struct lls_nd_req nd_req = {
			.pkt = pkt,
			.iface = iface,
		};
		return lls_req(LLS_REQ_ND, &nd_req);
	}

	RTE_LOG(WARNING, GATEKEEPER,
		"lls: %s invoked but ND service is not enabled\n", __func__);
	return -1;
}

int
pkt_is_nd(struct ipacket *packet, struct gatekeeper_if *iface)
{
	struct icmpv6_hdr *icmpv6_hdr;

	if (packet->len < ND_NEIGH_PKT_MIN_LEN ||
			packet->flow.proto != ETHER_TYPE_IPv6 ||
			packet->next_hdr != IPPROTO_ICMPV6)
		return false;

	/*
	 * Make sure this is an ND neighbor message and that it was
	 * sent to us (our global address, link-local address, or
	 * either of the solicited-node multicast addresses.
	 */
	icmpv6_hdr = rte_pktmbuf_mtod_offset(packet->pkt, struct icmpv6_hdr *,
		sizeof(struct ether_hdr) + sizeof(struct ipv6_hdr));
	return (icmpv6_hdr->type == ND_NEIGHBOR_SOLICITATION ||
			icmpv6_hdr->type == ND_NEIGHBOR_ADVERTISEMENT) &&
		(ipv6_addrs_equal(packet->flow.f.v6.dst,
			iface->ll_ip6_addr.s6_addr) ||
		ipv6_addrs_equal(packet->flow.f.v6.dst,
			iface->ip6_addr.s6_addr) ||
		ipv6_addrs_equal(packet->flow.f.v6.dst,
			iface->ip6_mc_addr.s6_addr) ||
		ipv6_addrs_equal(packet->flow.f.v6.dst,
			iface->ll_ip6_mc_addr.s6_addr));
}

static void
lls_scan(__attribute__((unused)) struct rte_timer *timer, void *arg)
{
	struct lls_config *lls_conf = (struct lls_config *)arg;
	if (arp_enabled(lls_conf))
		lls_cache_scan(lls_conf, &lls_conf->arp_cache);
	if (nd_enabled(lls_conf))
		lls_cache_scan(lls_conf, &lls_conf->nd_cache);
}

static void
process_pkts(struct lls_config *lls_conf, struct gatekeeper_if *iface,
	uint16_t rx_queue, uint16_t tx_queue)
{
	struct rte_mbuf *bufs[GATEKEEPER_MAX_PKT_BURST];
	uint16_t num_rx = rte_eth_rx_burst(iface->id, rx_queue, bufs,
		GATEKEEPER_MAX_PKT_BURST);
	uint16_t i;

	for (i = 0; i < num_rx; i++) {
		struct ether_hdr *eth_hdr = rte_pktmbuf_mtod(bufs[i],
			struct ether_hdr *);

		/*
		 * The destination MAC address should be the broadcast
		 * address or match the interface's Ethernet address,
		 * because for round robin and LACP bonding the
		 * slave interfaces assume the MAC address of the
		 * bonded interface.
		 *
		 * See: http://dpdk.org/doc/guides/prog_guide/link_bonding_poll_mode_drv_lib.html#configuration
		 *
		 * XXX Is this check needed? By default, the NIC only
		 * accepts the assigned MAC address, broadcast address,
		 * and any MAC added (for example, for IPv6 Ethernet multicast).
		 */
		if (unlikely(!is_broadcast_ether_addr(&eth_hdr->d_addr) &&
			!is_same_ether_addr(&eth_hdr->d_addr,
				&iface->eth_mc_addr) &&
			!is_same_ether_addr(&eth_hdr->d_addr,
				&iface->ll_eth_mc_addr) &&
			!is_same_ether_addr(&eth_hdr->d_addr,
				&iface->eth_addr)))
			goto free_buf;

		switch (rte_be_to_cpu_16(eth_hdr->ether_type)) {
		case ETHER_TYPE_ARP:
			if (process_arp(lls_conf, iface, tx_queue,
					bufs[i], eth_hdr) == -1)
				goto free_buf;

			/* ARP reply was sent, so no free is needed. */
			continue;
		case ETHER_TYPE_IPv6:
			if (iface == &lls_conf->net->back) {
				/*
				 * Back interface can also see ND packets
				 * received here.
				 *
				 * TODO Move RSS on the back interface
				 * to a different block and pass ND
				 * packets to the LLS using DPDK
				 * packet classification and distribution.
				 * Then, handle any non-ARP and non-ND
				 * packets on the back interface. For now,
				 * just drop them.
				 */
				struct ipacket packet;
				int ret = extract_packet_info(bufs[i], &packet);
				if (ret < 0)
					goto free_buf;

				if (pkt_is_nd(&packet, iface)) {
					if (process_nd(lls_conf, iface,
							bufs[i]) == -1)
						goto free_buf;

					/* ND reply sent, so no free needed. */
					continue;
				}
			}
			/* FALLTHROUGH */
		default:
			RTE_LOG(ERR, GATEKEEPER, "lls: %s interface should not be seeing a packet with EtherType 0x%04hx\n",
				iface->name,
				rte_be_to_cpu_16(eth_hdr->ether_type));
			goto free_buf;
		}
free_buf:
		rte_pktmbuf_free(bufs[i]);
	}
}

static int
lls_proc(void *arg)
{
	struct lls_config *lls_conf = (struct lls_config *)arg;
	struct net_config *net_conf = lls_conf->net;

	RTE_LOG(NOTICE, GATEKEEPER,
		"lls: the LLS block is running at lcore = %u\n",
		lls_conf->lcore_id);

	while (likely(!exiting)) {
		/* Read in packets on front and back interfaces. */
		process_pkts(lls_conf, &net_conf->front,
			lls_conf->rx_queue_front, lls_conf->tx_queue_front);
		if (net_conf->back_iface_enabled)
			process_pkts(lls_conf, &net_conf->back,
				lls_conf->rx_queue_back,
				lls_conf->tx_queue_back);

		/* Process any requests. */
		if (likely(lls_process_reqs(lls_conf) == 0)) {
			/*
			 * If there are no requests to go through, then do a
			 * scan of the cache (if enough time has passed).
			 *
			 * XXX In theory, many new LLS changes could starve
			 * the ability to scan, but this will not likely
			 * happen. In fact, we may want to reduce the amount
			 * of times this is called, since reading the HPET
			 * timer is inefficient. See the timer application.
			 */
			rte_timer_manage();
		}
	}

	RTE_LOG(NOTICE, GATEKEEPER,
		"lls: the LLS block at lcore = %u is exiting\n",
		lls_conf->lcore_id);

	return cleanup_lls();
}

static int
assign_lls_queue_ids(struct lls_config *lls_conf)
{
	int ret = get_queue_id(&lls_conf->net->front, QUEUE_TYPE_RX,
		lls_conf->lcore_id);
	if (ret < 0)
		goto fail;
	lls_conf->rx_queue_front = ret;

	ret = get_queue_id(&lls_conf->net->front, QUEUE_TYPE_TX,
		lls_conf->lcore_id);
	if (ret < 0)
		goto fail;
	lls_conf->tx_queue_front = ret;

	if (lls_conf->net->back_iface_enabled) {
		ret = get_queue_id(&lls_conf->net->back, QUEUE_TYPE_RX,
			lls_conf->lcore_id);
		if (ret < 0)
			goto fail;
		lls_conf->rx_queue_back = ret;

		ret = get_queue_id(&lls_conf->net->back, QUEUE_TYPE_TX,
			lls_conf->lcore_id);
		if (ret < 0)
			goto fail;
		lls_conf->tx_queue_back = ret;
	}

	return 0;

fail:
	RTE_LOG(ERR, GATEKEEPER, "lls: cannot assign queues\n");
	return ret;
}

static int
lls_stage1(void *arg)
{
	struct lls_config *lls_conf = arg;
	return assign_lls_queue_ids(lls_conf);
}

static int
lls_stage2(void *arg)
{
	struct lls_config *lls_conf = arg;
	struct net_config *net_conf = lls_conf->net;
	int ret;

	if (lls_conf->arp_cache.iface_enabled(net_conf, &net_conf->front)) {
		ret = ethertype_filter_add(net_conf->front.id,
			ETHER_TYPE_ARP, lls_conf->rx_queue_front);
		if (ret < 0)
			return ret;
	}

	if (lls_conf->arp_cache.iface_enabled(net_conf, &net_conf->back)) {
		ret = ethertype_filter_add(net_conf->back.id,
			ETHER_TYPE_ARP, lls_conf->rx_queue_back);
		if (ret < 0)
			return ret;
	}

	/*
	 * Receive ND packets on the front interface from the
	 * GK and GT blocks, depending on whether we're running
	 * Gatekeeper or Grantor.
	 */

	/*
	 * TODO Have a different block set up RSS on the back interface,
	 * and classify + distribute ND packets to the LLS block using
	 * the DPDK libraries.
	 */
	if (lls_conf->nd_cache.iface_enabled(net_conf, &net_conf->back)) {
		uint8_t port_in = net_conf->back.id;
		uint16_t lls_queue = lls_conf->rx_queue_back;

		ret = gatekeeper_setup_rss(port_in, &lls_queue, 1);
		if (ret < 0)
			return ret;

		ret = gatekeeper_get_rss_config(port_in, &lls_conf->rss_conf);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int
run_lls(struct net_config *net_conf, struct lls_config *lls_conf)
{
	int ret;

	if (net_conf == NULL || lls_conf == NULL) {
		ret = -1;
		goto out;
	}

	ret = net_launch_at_stage1(net_conf, 1, 1, 1, 1, lls_stage1, lls_conf);
	if (ret < 0)
		goto out;

	ret = launch_at_stage2(lls_stage2, lls_conf);
	if (ret < 0)
		goto stage1;

	ret = launch_at_stage3("lls", lls_proc, lls_conf, lls_conf->lcore_id);
	if (ret < 0)
		goto stage2;

	/* Do LLS cache scan every LLS_CACHE_SCAN_INTERVAL seconds. */
	rte_timer_init(&lls_conf->timer);
	ret = rte_timer_reset(&lls_conf->timer,
		LLS_CACHE_SCAN_INTERVAL * rte_get_timer_hz(), PERIODICAL,
		lls_conf->lcore_id, lls_scan, lls_conf);
	if (ret < 0) {
		RTE_LOG(ERR, TIMER, "Cannot set LLS scan timer\n");
		goto stage3;
	}

	ret = init_mailbox("lls_req", MAILBOX_MAX_ENTRIES,
		sizeof(struct lls_request), lls_conf->lcore_id,
		&lls_conf->requests);
	if (ret < 0)
		goto timer;

	lls_conf->net = net_conf;
	if (arp_enabled(lls_conf)) {
		ret = lls_cache_init(lls_conf, &lls_conf->arp_cache);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"lls: ARP cache cannot be started\n");
			goto requests;
		}

		/* Set timeouts for front and back (if needed). */
		if (lls_conf->arp_cache.iface_enabled(net_conf,
				&net_conf->front))
			lls_conf->arp_cache.front_timeout_sec =
				net_conf->front.arp_cache_timeout_sec;
		if (lls_conf->arp_cache.iface_enabled(net_conf,
				&net_conf->back))
			lls_conf->arp_cache.back_timeout_sec =
				lls_conf->net->back.arp_cache_timeout_sec;
	}

	if (nd_enabled(lls_conf)) {
		ret = lls_cache_init(lls_conf, &lls_conf->nd_cache);
		if (ret < 0) {
			RTE_LOG(ERR, GATEKEEPER,
				"lls: ND cache cannot be started\n");
			goto arp;
		}

		/* Set timeouts for front and back (if needed). */
		if (lls_conf->nd_cache.iface_enabled(net_conf,
				&net_conf->front))
			lls_conf->nd_cache.front_timeout_sec =
				net_conf->front.nd_cache_timeout_sec;
		if (lls_conf->nd_cache.iface_enabled(net_conf, &net_conf->back))
			lls_conf->nd_cache.back_timeout_sec =
				lls_conf->net->back.nd_cache_timeout_sec;
	}

	return 0;

arp:
	if (arp_enabled(lls_conf))
		lls_cache_destroy(&lls_conf->arp_cache);
requests:
	destroy_mailbox(&lls_conf->requests);
timer:
	rte_timer_stop(&lls_conf->timer);
stage3:
	pop_n_at_stage3(1);
stage2:
	pop_n_at_stage2(1);
stage1:
	pop_n_at_stage1(1);
out:
	return ret;
}
