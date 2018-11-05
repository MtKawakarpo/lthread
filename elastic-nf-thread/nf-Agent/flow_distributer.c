/*
* Created by Zhilong Zheng
*/

#include <stdio.h>

#include <rte_common.h>

#include <rte_eal.h>
#include <rte_compat.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <unistd.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_timer.h>
#include <rte_cycles.h>

#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_atomic.h>
#include <rte_atomic_64.h>

#include "flow_distributer.h"

#define RTE_LOGTYPE_ELASTICTHREAD          RTE_LOGTYPE_USER1
#define MAX_PKTS_BURST_RX 32
#define MAX_PKTS_BURST_TX 32

#define MAX_NFS_PER_DIRECTOR 4

static rte_atomic16_t is_rx_configged;

// 全局数据结构
struct flow *flows;
int flow_count;
struct flow_table *flow_table;
//struct rte_ring *nf_rxring_mapping[MAX_NF_NB];
//struct nf_tx_conf *nf_txconf_mapping[MAX_NF_NB];
int txconf_count;
struct nf_flow_stats *last_stats;
struct nf_flow_stats *cur_stats;
int reset_stats;

void flows_init(uint32_t nb_flows) {
    int i;
    flow_count = 0;

    printf("-------- Init flows --------\n");
    flows = rte_calloc("flows", nb_flows, sizeof(*flows), 0);
    if (flows == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for flows\n");

    for (i = 0; i < nb_flows; ++i) {
        flows[i].flow_id = -1;
        flows[i].flow_hash = -1;
    }

    printf("-------- Init flows successfully! --------\n");

}

struct flow* add_flow(int flow_hash, int nf_id) {
    flows[flow_count].flow_hash = flow_hash;
    flows[flow_count].nf_id = nf_id;
    flow_count++;
    return &flows[flow_count-1];
}


void pktmbuf_free_bulk(struct rte_mbuf *mbuf_table[], unsigned n)
{
    unsigned int i;

    for (i = 0; i < n; i++)
        rte_pktmbuf_free(mbuf_table[i]);
}

void flow_table_init() {
    int i;

    flows_init(1024);

    printf("-------- Init flow table --------\n");
    flow_table = rte_calloc("flowtable", 1, sizeof(*flow_table), 0);
    if (flow_table == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for flows\n");

    for (i = 0; i < MAX_TABLE_ENTRIES; ++i) {
        flow_table->entries[i] = rte_calloc("flowentries", 1, sizeof(flow_table->entries[i]), 0);
        flow_table->entries[i]->m_flow = NULL;
        if (flow_table->entries[i] == NULL)
            rte_exit(EXIT_FAILURE, "Cannot allocate memory for flows\n");
    }
    flow_table->nb_entries = 0;
    txconf_count = 0;
    rte_atomic16_init(&is_rx_configged);
    rte_atomic16_set(&is_rx_configged, 0);

    printf("-------- Init flow table successfully! --------\n");

}

// TODO: 后面按dpdk的hash改，这里先做简化
void flow_table_add_entry(uint32_t hash, int nf_id) {
    printf("--------- Add a flow to flow table -----------\n");
    flow_table->entries[hash % MAX_TABLE_ENTRIES]->packet_hash = hash ;
    flow_table->entries[hash % MAX_TABLE_ENTRIES]->m_flow = add_flow(hash, nf_id);
    ++(flow_table->nb_entries);
}

struct flow *flow_table_get_flow(uint32_t hash) {
    return flow_table->entries[hash % MAX_TABLE_ENTRIES]->m_flow ;
}

void bind_nf_to_rxring(int nf_id, struct rte_ring *rx_ring) {
    printf("--- Bind NF %d to a rx ring\n", nf_id);
    nf_rxring_mapping[nf_id] = rx_ring;
}

int flow_director_rx_thread(struct port_info *args) {
    uint32_t i;
    int ret;
    uint32_t nb_rx_pkts = 0;

    uint32_t ports_nb = 0;
    struct rte_mbuf *pkts[MAX_PKTS_BURST_RX];

//    ports_nb = portinfo.port_nb;
    uint8_t port_id = args->port_id;
    uint16_t queue_id = args->queue_id;

    struct rte_mbuf *garbage_mbufs[MAX_PKTS_BURST_RX * 2];
    uint32_t garbage_count = 0;
    uint32_t garbase_min = 64;

    struct rte_mbuf *pkt;
    struct ipv4_hdr* _ipv4_hdr;
    struct udp_hdr *_udp_hdr;

    struct flow *tmp_flow;
    uint32_t nf_id = 0;

    // 初始化时，所有ring初始位NULL
    for (i = 0; i < MAX_NF_NB; ++i)
        nf_rxring_mapping[i] = NULL;

    // Rx上初始化一个monitor
//    extern struct nf_flow_stats *last_stats;
//    extern struct nf_flow_stats *cur_stats;

    if (!rte_atomic16_read(&is_rx_configged)) {
        last_stats = rte_calloc("last_stats", MAX_NF_NB, sizeof(*last_stats), 0);
        if (last_stats == NULL)
            rte_exit(EXIT_FAILURE, "Cannot allocate memory for last_flow_stats\n");
        cur_stats = rte_calloc("cur_stats", MAX_NF_NB, sizeof(*cur_stats), 0);
        if (cur_stats == NULL)
            rte_exit(EXIT_FAILURE, "Cannot allocate memory for cur_flow_stats\n");

        for (i = 0; i < MAX_NF_NB; ++i) {
            last_stats[i].total_pkts = 0;
            last_stats[i].processed_pkts = 0;
            last_stats[i].dropped_pkts = 0;
        }
        for (i = 0; i < MAX_NF_NB; ++i) {
            cur_stats[i].total_pkts = 0;
            cur_stats[i].processed_pkts = 0;
            cur_stats[i].dropped_pkts = 0;
        }
        rte_atomic16_set(&is_rx_configged, 1);
        printf("RX monitor 初始化成功\n");

    }

    reset_stats = 0;

    RTE_LOG(INFO, ELASTICTHREAD, "%s() started on lcore %u and receiving port %u on queue %u\n", __func__,
            rte_lcore_id(), port_id, queue_id);
//    if (ports_nb <= 0)
//        rte_exit(EXIT_FAILURE, "number of ports is %u, which is not enough!\n");

    while (1) {

//        queue_id++;
//        queue_id%=10;
        if (reset_stats && (port_id == 0)) {
            for (i = 0; i < MAX_NF_NB; ++i) {
                cur_stats[i].processed_pkts = 0;
                cur_stats[i].dropped_pkts = 0;
            }
            reset_stats = 0;
        }

        nb_rx_pkts = rte_eth_rx_burst(port_id, queue_id,
                                      pkts, MAX_PKTS_BURST_RX);

        if (unlikely(nb_rx_pkts == 0)) {
            RTE_LOG(DEBUG, ELASTICTHREAD, "%s() received zero packets\n", __func__);
            continue;
        }

//        printf("port %d recved %d packets\n", port_id, nb_rx_pkts);

        // Classfy flows
        for (i = 0; i < nb_rx_pkts; ++i) {
//            _ipv4_hdr = onvm_pkt_ipv4_hdr(pkts[i]);
            pkt = (struct rte_mbuf*)pkts[i];

            _ipv4_hdr = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *, sizeof(struct ether_hdr));

//            _udp_hdr = (struct udp_hdr *)((unsigned char *) _ipv4_hdr +
//                                          sizeof(struct ipv4_hdr));
//            if (_udp_hdr == NULL) {
//                printf("An invalid udp header!\n");
//            }

            tmp_flow = flow_table_get_flow(_ipv4_hdr->src_addr);

            if (tmp_flow == NULL) {

//                printf(" new flow %d\n", _ipv4_hdr->src_addr);

                garbage_mbufs[garbage_count] = pkts[i];
                garbage_count++;
                continue;
            }


            nf_id = tmp_flow->nf_id;

//            printf("flow %d -> nf id: %d\n", _ipv4_hdr->src_addr,  nf_id);

            struct rte_ring * nf_rx_ring = nf_rxring_mapping[nf_id];
            if (nf_rx_ring == NULL) {
                garbage_mbufs[garbage_count] = pkts[i];
                garbage_count++;
                cur_stats[nf_id].total_pkts++;
                continue;
            }

//            ret = rte_ring_sp_enqueue(nf_rx_ring, (void *)pkts[i]);
            ret = rte_ring_enqueue(nf_rx_ring, (void *)pkts[i]);
//            if (worker_id != 0 && worker_id != 1)
//                ret = 1;
            if (unlikely(ret != 0)) {
//                printf("rx send pkts failed\n");
                garbage_mbufs[garbage_count] = pkts[i];
                garbage_count++;
                cur_stats[nf_id].dropped_pkts++;
                continue;
            }
            cur_stats[nf_id].processed_pkts++;
        }
        if (garbage_count >= garbase_min) {
            pktmbuf_free_bulk(garbage_mbufs, garbage_count);
            garbage_count = 0;
        }

//        ret = rte_ring_enqueue_burst(workers[0].rx_q, (void *) pkts,
//                                     nb_rx_pkts, NULL);
//        printf("rx queue attri: %d\n", workers[worker_id].rx_q->prod.sp_enqueue);
//        portinfo.rx_stats[port_id].enqueue_pkts += enqueue_count;

    }
    RTE_LOG(DEBUG, ELASTICTHREAD, "Flow director exited\n");
    return 0;
}

void reset_nf_stats() {
    reset_stats = 1;
}

void monitor_update(int period) {
    int i;
//    printf("monitor update\n");

    for (i = 0; i < MAX_NF_NB; ++i) {
        cur_stats[i].processed_pps = cur_stats[i].processed_pkts / period;
        cur_stats[i].dropped_pps = cur_stats[i].dropped_pkts / period;
        if ((cur_stats[i].dropped_pkts + cur_stats[i].processed_pkts) == 0)
            cur_stats[i].dropped_ratio = 0;
        else
            cur_stats[i].dropped_ratio = cur_stats[i].dropped_pkts / (cur_stats[i].dropped_pkts + cur_stats[i].processed_pkts);
    }

    reset_nf_stats();  // 重置信息
//    printf("monitor update 成功\n");
}

uint64_t get_processed_pps_with_nf_id (int nf_id) {
    return cur_stats[nf_id].processed_pps;
}
uint64_t get_dropped_pps_with_nf_id (int nf_id) {
    return cur_stats[nf_id].dropped_pps;
}
double get_dropped_ratio_with_nf_id (int nf_id) {
    return cur_stats[nf_id].dropped_ratio;
}


void nf_need_output(int nf_id, int out_port, struct rte_ring *nf_tx_ring) {
    printf("NF %d needs outputting to port %d\n", nf_id, out_port);
    struct nf_tx_conf *add_conf = rte_calloc(NULL, 1, sizeof(struct nf_tx_conf *), 0);

    add_conf->nf_id = nf_id;
    add_conf->out_port = out_port;
    add_conf->nf_tx_ring = nf_tx_ring;

    nf_txconf_mapping[txconf_count] = add_conf;

    txconf_count++;
}


int flow_director_tx_thread(struct port_info *args) {

    uint32_t i, deq_nb;
    int ret;
    int start_flow, end_flow, flow_per_tx;
    struct rte_mbuf *pkts[MAX_PKTS_BURST_RX];

    uint8_t port_id = args->port_id;
    uint16_t queue_id = args->queue_id;
    int thread_id = args->thread_id;
    int nb_ports = args->nb_ports;
    flow_per_tx = txconf_count/nb_ports;
    printf(">>>tx thread %d read txconf_cnt=%d\n", thread_id, txconf_count);

    if(flow_per_tx == 0)
        flow_per_tx = 1;
    start_flow = 0+thread_id * flow_per_tx;
    end_flow = flow_per_tx * (thread_id+1);
    if(thread_id == nb_ports-1){
        end_flow = txconf_count;
    }

    RTE_LOG(INFO, ELASTICTHREAD, "%s() %d started on lcore %u and tx on port (%u, %u), serve for nf %d - nf %d \n", __func__, thread_id,
            rte_lcore_id(), port_id, queue_id, start_flow, end_flow);

    while (1) {

        // 轮询所有的nf_txring
        for (i = start_flow; i < end_flow; ++i) {  // 每个 tx director负责一部分
            if (nf_txconf_mapping[i] == NULL)
                continue;
            if (nf_txconf_mapping[i]->out_port != port_id)
                continue;
            deq_nb = rte_ring_sc_dequeue_bulk(nf_txconf_mapping[i]->nf_tx_ring,
                                              (void *)pkts, MAX_PKTS_BURST_TX, NULL);

            if (unlikely(deq_nb == 0))
                continue;

//            printf("tx %d recv nf %d with %d pkts\n",thread_id, nf_txconf_mapping[i]->nf_id, deq_nb);
            ret = rte_eth_tx_burst(nf_txconf_mapping[i]->out_port, queue_id, pkts, deq_nb);
            if (unlikely(ret < deq_nb)) {
                pktmbuf_free_bulk(&pkts[ret], deq_nb - ret);
            }

        }

    }
    RTE_LOG(DEBUG, ELASTICTHREAD, "Flow directer Tx exited\n");

    return 0;
}