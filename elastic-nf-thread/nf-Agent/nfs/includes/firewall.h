#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_hash.h>
#include <rte_lpm.h>

#include "pkt_help.h"
#define PASS 1
#define DROP 0

static uint16_t shift_8 = 1UL << 8;
static uint32_t shift_16 = 1UL << 16;
static uint64_t shift_32 = 1UL << 32;

struct ipv4_5tuple {
    uint32_t ip_dst;
    uint32_t ip_src;
    uint16_t port_dst;
    uint16_t port_src;
    uint8_t  proto;
} __attribute__((__packed__));

struct ipv4_firewall_hash_entry {
    struct ipv4_5tuple key;
    uint8_t action;
};
static uint32_t rule_number = 4;
static struct ipv4_firewall_hash_entry ipv4_firewall_hash_entry_array[] = {
        {{50463234,        16885952,         9,9,IPPROTO_UDP}, PASS},
        {{16885952, 16820416, 5678, 1234, IPPROTO_TCP}, DROP},
        {{IPv4(111,0,0,0), IPv4(100,30,0,1),  101, 11, IPPROTO_TCP}, PASS},
        {{IPv4(211,0,0,0), IPv4(200,40,0,1),  102, 12, IPPROTO_TCP}, PASS},
};
static inline void
firewall_fill_ipv4_5tuple_key(struct ipv4_5tuple *key, void *ipv4_hdr);
/* calc hash value */
static uint32_t
nf_hash_val(struct ipv4_5tuple* tmp_turple);
/* hash set insert */
static void
nf_hash_insert(struct nf_statistics* stats, struct ipv4_firewall_hash_entry* entry);
/* hash set find */
static int
nf_hash_lookup(struct nf_statistics* stats, struct ipv4_5tuple* key);
/* init tenant state */
void
nf_firewall_init(struct nf_statistics *stats);
/* handle tenant packets */
uint16_t
nf_firewall_handler(struct rte_mbuf *pkt[], uint16_t num, struct nf_statistics* stats);

int lthread_firewall(void *dumy);
int pthread_firewall(void *dumy);
