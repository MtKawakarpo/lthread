#ifndef LTHREAD_NF_COMMON_H
#define LTHREAD_NF_COMMON_H

#define BURST_SIZE 32
#define BIG_PRIME 10000019
#define MAX_STATE_LEN 2000
#define IDS_RULE_NUM 6
#define AES_ENCRYPT_LEN 60
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include "aes.h"
#include "pkt_help.h"

typedef void (*lthread_func_t) (void *);

struct hash_node {
    uint32_t ip_src, ip_dst;
    uint16_t port_src, port_dst;
    uint8_t proto, action;
    uint8_t is_valid;
};

struct nf_statistics {

    struct hash_node *hash_map;//firewall
    WORD key_schedule[AES_ENCRYPT_LEN]; //aes
};

struct nf_info{

    lthread_func_t fun;
    struct rte_ring *rx_q;
    struct rte_rinf *tx_q;
    int nf_id;
    int belong_to_sfc;
    int next_nf;
    int agent_id;
    int priority;
    int service_time;
    uint32_t lcore_id;
    struct nf_statistics *state;
};
struct nf_thread_info{
    int nf_id;
};
struct nf_info *nfs_info_data;

#endif //LTHREAD_NF_COMMON_H

