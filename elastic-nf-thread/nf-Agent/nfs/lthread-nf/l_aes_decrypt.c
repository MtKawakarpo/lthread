#include "../includes/aes_decrypt.h"
#include "../includes/nf_common.h"


/* init tenant state */
void nf_aes_decrypt_init(struct nf_statistics *stats){


    /* Initialise decryption engine. Key should be configurable. */
    aes_key_setup(de_key[0], stats->key_schedule, 256);
    return 0;
}

/* handle tenant packets */
uint16_t
nf_aes_decrypt_handler(struct rte_mbuf *pkt[], uint16_t num, struct nf_statistics *stats) {

    struct udp_hdr *udp;
    uint16_t i, num_out, plen, hlen;
    uint8_t *pkt_data, *eth;

    num_out = num;
    for (i = 0; i < num; i++) {
        /* Check if we have a valid UDP packet */
        udp = nf_pkt_udp_hdr(pkt[i]);
        if (udp != NULL) {

            /* Get at the payload */
            pkt_data = ((uint8_t *) udp) + sizeof(struct udp_hdr);
            /* Calculate length */
            eth = rte_pktmbuf_mtod(pkt[i], uint8_t *);
            hlen = pkt_data - eth;
            plen = pkt[i]->pkt_len - hlen;

            /* Decrypt. */
            /* IV should change with every packet, but we don't have any
            * way to send it to the other side. */
            aes_decrypt_ctr(pkt_data, plen, pkt_data, stats->key_schedule, 256, de_iv[0]);
            num_out ++;
        }
    }

    return num_out;
}

///* Set *hi and *lo to the high and low order bits of the cycle counter.
// *    Implementation requires assembly code to use the rdtsc instruction. */
//static uint64_t rdtsc_2(void)
//{
//    uint64_t var;
//    uint32_t hi, lo;
//
//    __asm volatile
//    ("rdtsc" : "=a" (lo), "=d" (hi));
//
//    var = ((uint64_t)hi << 32) | lo;
//    return (var);
//}
int
lthread_aes_decryt(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx, cnt;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;
    struct nf_statistics *statistics = nf_info_local->state;

    lthread_set_data((void *)nf_info_local);


    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
    nf_aes_decrypt_init(statistics);
    printf("finish init aes decryt\n");
    uint64_t start, end, cycle, cycle_poll;

    while (1){
        //FIXME:这里拆解了收发包接口，为了统计CPU利用率
//        nb_rx = nf_ring_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
        start = rdtsc_2();
        nb_rx = rte_ring_sc_dequeue_bulk(rq, pkts, BURST_SIZE, NULL);
        end = rdtsc_2();
        cycle_poll = end - start;
        if(unlikely(nb_rx == 0)){
            lthread_yield_with_cycle(0,cycle_poll );
        }
        if (unlikely(nb_rx > 0)) {
            start = rdtsc_2();
            nf_aes_decrypt_handler(pkts, nb_rx, statistics);
//            nb_tx = nf_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
            nb_tx = rte_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
            if (unlikely(nb_tx < nb_rx)) {
                uint32_t k;

                for (k = nb_tx; k < nb_rx; k++) {
                    struct rte_mbuf *m = pkts[k];

                    rte_pktmbuf_free(m);
                }
            }
            end = rdtsc_2();
            cycle = end - start;
            lthread_yield_with_cycle(cycle, cycle_poll);
        }else {
            continue;
        }
    }

}
