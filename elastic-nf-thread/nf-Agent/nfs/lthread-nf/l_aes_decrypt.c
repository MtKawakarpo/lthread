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

    while (1){
        nb_rx = nf_ring_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx > 0)) {

            nf_aes_decrypt_handler(pkts, nb_rx, statistics);
            nb_tx = nf_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
//            printf("aes decryt %d suc transfer %d pkts\n", nf_id, nb_tx);

        }else {
            continue;
        }
    }

}
