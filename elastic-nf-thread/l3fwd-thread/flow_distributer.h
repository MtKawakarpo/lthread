/*
* Created by Zhilong Zheng
*/

#ifndef ELASTIC_FLOW_DISTRIBUTER_H
#define ELASTIC_FLOW_DISTRIBUTER_H

#include <stdio.h>
#include <stdint.h>


#define MAX_NF_NB  64
#define MAX_TABLE_ENTRIES (1417)

struct port_info{
    uint8_t port_id;
    uint8_t queue_id;
};

struct flow {
    uint32_t flow_id;
    uint32_t flow_hash;  // We use flow hashing value to identify a flow
    uint32_t nf_id;  // assign to an nf instance
};

struct table_entry {
    uint32_t packet_hash;
    struct flow *m_flow;
//    struct rte_ring* nf_rx_ring;
};

struct flow_table {
    struct table_entry *entries[MAX_TABLE_ENTRIES];
    uint32_t nb_entries;

};

struct nf_tx_conf {
    int nf_id;
    struct rte_ring *nf_tx_ring;
    int out_port;
};

struct nf_flow_stats {
    uint64_t total_pkts;

    uint64_t processed_pkts;

    uint64_t dropped_pkts;

    uint64_t processed_pps;

    uint64_t dropped_pps;

    double dropped_ratio;

};

extern struct flow *flows;
extern int flow_count;
extern struct flow_table *flow_table;
struct rte_ring *nf_rxring_mapping[MAX_NF_NB];
struct nf_tx_conf *nf_txconf_mapping[MAX_NF_NB];
extern int txconf_count;

void flows_init(uint32_t nb_flows);
struct flow* add_flow(int flow_hash, int nf_id);
void flow_table_init();
void flow_table_add_entry(uint32_t hash, int nf_id);
struct flow *flow_table_get_flow(uint32_t hash);

void bind_nf_to_rxring(int nf_id, struct rte_ring *rx_ring);

int flow_director_rx_thread(struct port_info *args);

void nf_need_output(int nf_id, int out_port, struct rte_ring *nf_tx_ring);
int flow_director_tx_thread(struct port_info *args);

// monitor统计所需数据以及接口
extern struct nf_flow_stats *last_stats;
extern struct nf_flow_stats *cur_stats;
extern int reset_stats;

void reset_nf_stats();

// monitor 给外部的接口
void monitor_update(int period);  // 统计的周期

uint64_t get_processed_pps_with_nf_id (int nf_id);
uint64_t get_dropped_pps_with_nf_id (int nf_id);
double get_dropped_ratio_with_nf_id (int nf_id);



#endif //ELASTIC_FLOW_DISTRIBUTER_H
