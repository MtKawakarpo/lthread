//
// Created by zzl on 2018/10/21.
//


#ifndef THREAD_MANAGER_H
#define THREAD_MANAGER_H

#endif //PERFORMANCE_THREAD_THREAD_MANAGER_H

/*
 * launch NF threads
 */

/*
 * Retrieve a burst of input packets from a receive queue of an Ethernet device
 * like rte_eth_rx_burst
 */
unsigned nf_eth_rx_burst(uint16_t port_id, uint16_t queue_id,
                                                    struct rte_mbuf **rx_bufs, const uint16_t burst_size);

/*
 * like rte_eth_tx_burst
 */
uint16_t nf_eth_tx_burst(uint16_t 	port_id, uint16_t queue_id,
                                         struct rte_mbuf **tx_pkts, uint16_t nb_pkts);
/*
 * like rte_ring_dequeue
 */
int nf_ring_dequeue(struct rte_ring *r, void **pkt);

/*
 * like rte_ring_dequeue_bulk
 */
unsigned int nf_ring_dequeue_bulk	(struct rte_ring *rx_ring, void **rx_bufs,
                                                                      unsigned int burst_size, unsigned int *available);
/*
 * like rte_ring_dequeue_burst
 */
unsigned nf_ring_dequeue_burst(struct rte_ring *rx_ring, void **rx_bufs,
                                                                   unsigned int burst_size, unsigned int *available);
/*
 * FIXME: if a thread hold a lock and would not unlock until finish enqueue operation,then it will sleep with a lock
 * otherwise we let thread don't unlock depend on enqueue ret
 */
int nf_ring_enqueue(struct rte_ring *tx_ring, void *tx_pkt);
/*
 * like rte_ring_enqueue_burst
 */
unsigned nf_ring_enqueue_burst(struct rte_ring *tx_ring, void *const *obj_table,
                                                                   unsigned int 	burst_size, unsigned int *free_space);

/*
 * like rte_ring_enqueue_bulk
 */
unsigned int nf_ring_enqueue_bulk	(struct rte_ring *tx_ring, void *const * tx_bufs,
                                                                      unsigned int burst_size, unsigned int *free_space);