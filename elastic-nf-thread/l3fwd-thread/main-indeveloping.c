//
// Created by zzl on 2018/10/25.
//

/*
 * TODO: add portmask parse and flownum parse
 * A multiple flow generator, support different pktsize in one flow
 * and different rx rate among multiple flows
 *
 *   Created by Haiping Wang on 2018/4/5.
 */
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

#include <nf_lthread_api.h>
#include <thread_manager.h>

#define RX_RING_SIZE 512
#define TX_RING_SIZE 512
#define NUM_MBUFS 8192 * 4
#define MBUF_SIZE (1600 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MBUF_CACHE_SIZE 0
#define BURST_SIZE 32
#define NO_FLAGS 0
#define RING_MAX 10

//port and rx/tx
#define NUM_PORTS 1
#define MAX_NUM_PORT 4

#define MAX_NF_NUM 1000
#define NF_QUEUE_RING_SIZE 128
#define NF_NAME_RX_RING "Agent_%u_NF_%u_rq"
#define NF_NAME_TX_RING "Agent_%u_NF_%u_tq"
static int Agent_id = 1001;

static uint8_t port_id_list[NUM_PORTS] = {  0};

static struct rte_mempool *pktmbuf_pool;
static uint8_t keep_running = 1;

//now each rx/tx work for one flow
struct tx_rx_thread_info
{
	uint8_t port;
	uint8_t queue;
	uint16_t level;
	uint8_t thread_id;
	uint16_t fid;
};

struct nf_info{

	struct rte_ring *rx_q;
	struct rte_rinf *tx_q;
	int nf_id;
	uint16_t lcore_id;
};
struct nf_thread_info{
	int nf_id;
};

struct nf_info *nfs_info_data;
uint16_t nb_nfs= 3;
uint16_t nb_lcores;
int rx_thread_num;
int tx_thread_num;
struct tx_rx_thread_info *tx[2 * MAX_NUM_PORT];//infor of port using by thread
struct tx_rx_thread_info *rx[2 * MAX_NUM_PORT];
struct nf_thread_info *nf[ 2 * MAX_NUM_PORT];

static const struct rte_eth_conf port_conf_default = {
		.rxmode = {
				.mq_mode = ETH_MQ_RX_RSS,
				.max_rx_pkt_len = ETHER_MAX_LEN,
		},
		.rx_adv_conf = {
				.rss_conf = {
						.rss_key = NULL,
						.rss_hf = ETH_RSS_TCP,
				},
		},
		.txmode = {
				.mq_mode = ETH_MQ_TX_NONE,
		},
};

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static int port_init(uint8_t port) {
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = RING_MAX;
	const uint16_t tx_rings = RING_MAX;

	int retval;
	uint16_t q;

	if (port >= rte_eth_dev_count())
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
										rte_eth_dev_socket_id(port), NULL, pktmbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
										rte_eth_dev_socket_id(port), NULL);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
	" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	printf("try to enable port %d\n", port);
	rte_eth_promiscuous_enable(port);
	printf(">success\n");
	struct rte_eth_link  link;
	int cnt = 0;
	do{
		rte_eth_link_get(port, &link);
		cnt++;
		printf("try get link %d\n", cnt);
		if(cnt==5)
			break;
	}while(link.link_status != ETH_LINK_UP);
	if(link.link_status == ETH_LINK_DOWN){
//        rte_exit(EXIT_FAILURE, ":: error: link is still down\n");
		printf("link down with port %d\n", port);
		return 0;
	}
//    rte_delay_ms(1);
	printf("finish init\n");
	return 0;
}

static int
init_mbuf_pools(void) {

	const unsigned num_mbufs = NUM_MBUFS * NUM_PORTS;

	/* don't pass single-producer/single-consumer flags to mbuf create as it
     * seems faster to use a cache instead */
	printf("Creating mbuf pool '%s' [%u mbufs] ...\n",
		   "MBUF_POOL", num_mbufs);
	pktmbuf_pool = rte_mempool_create("MBUF_POOL", num_mbufs,
									  MBUF_SIZE, MBUF_CACHE_SIZE,
									  sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init,
									  NULL, rte_pktmbuf_init, NULL, rte_socket_id(), NO_FLAGS);

	return (pktmbuf_pool == NULL); /* 0  on success */
}



struct ipv4_hdr*
get_pkt_ipv4_hdr(struct rte_mbuf* pkt) {
	struct ipv4_hdr* ipv4 = (struct ipv4_hdr*)(rte_pktmbuf_mtod(pkt, uint8_t*) + sizeof(struct ether_hdr));

	/* In an IP packet, the first 4 bits determine the version.
     * The next 4 bits are called the Internet Header Length, or IHL.
     * DPDK's ipv4_hdr struct combines both the version and the IHL into one uint8_t.
     */
	uint8_t version = (ipv4->version_ihl >> 4) & 0b1111;
	if (unlikely(version != 4)) {
		return NULL;
	}
	return ipv4;
}

static void handle_signal(int sig)
{
	if (sig == SIGINT || sig == SIGTERM)
		keep_running = 0;
}

/*
 *  send pkts through one port for flows identified from start_fid to end_fid,
 *  tx rate is set by the parameter tx_delay[tx_id]
 */
static int lcore_tx_main(void *arg) {
	uint16_t i, j, sent, nb_rx, nb_tx;
	uint16_t thread_id;
	uint16_t queue_id, port_id;
	struct tx_rx_thread_info *tx = (struct tx_rx_thread_info *)arg;
	struct rte_mbuf *pkts[BURST_SIZE];
	struct rte_ring *ring;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	thread_id = tx->thread_id;
	port_id = tx->port;
	queue_id = tx->queue;

	ring = nfs_info_data[thread_id%nb_nfs].tx_q;

	printf("Core %d: Running TX thread %d,\n", rte_lcore_id(), tx->thread_id);

	for(;keep_running;){
//        printf("tx %d try dequeue pkt from nf ring %d", thread_id, thread_id%nb_nfs);
		nb_rx = rte_ring_dequeue_burst(ring, pkts, BURST_SIZE, NULL);
//        printf("suc\n");

		if(unlikely(nb_rx>0)){
			//do somthoie
//            printf("tx %d suc recv %d pkts from nf %d\n", thread_id, nb_rx, thread_id%nb_nfs);
			nb_tx = rte_eth_tx_burst(port_id, queue_id, pkts, nb_rx);
//            printf("tx %d suc send out %d pkts\n", thread_id, nb_tx);
			if (unlikely(nb_tx < nb_rx)) {
				do {
					rte_pktmbuf_free(pkts[nb_tx]);
				} while (++nb_tx < nb_rx);
			}//end free if
//            printf("finish a batch\n");
		}//end process if

	}

	return 0;
}

/*
 * receive pkts from all queues of one port and update the stats of ports and flows
 */
static int lcore_rx_main(void *arg) {

	uint16_t i, queue_id, receive, p_id, thread_id, nb_tx;
	struct ipv4_hdr *ipv4_hdr;
	struct tx_rx_thread_info *rx = (struct tx_rx_thread_info *)arg;
	struct rte_mbuf *pkts[BURST_SIZE];
	struct timespec *payload, now = {0, 0};
	FILE *fp;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	p_id = rx->port;
	queue_id = rx->queue;
	thread_id = rx->thread_id;

	printf("Core %d: Running RX thread %d for port %d queue %d\n", rte_lcore_id(), thread_id, p_id, queue_id);
	rte_delay_ms(1000);
	for (; keep_running;) {
//        printf("try to recv pkts\n");
		receive = rte_eth_rx_burst(p_id, queue_id, pkts, BURST_SIZE);
//        printf("suc\n");

		if (likely(receive > 0)) {
//            if(thread_id == 1)
//                printf("rx %d recv %d pkts\n", thread_id, receive);
			for (i = 0; i < receive; i++) {

//                ipv4_hdr = get_pkt_ipv4_hdr(pkts[i]);
			}
			//TODO: classifier pkts should follow some rule
			nb_tx = rte_ring_enqueue_burst(nfs_info_data[thread_id%nb_nfs].rx_q, pkts, receive, NULL);
			if (unlikely(nb_tx < receive)) {
				uint32_t k;
				for (k = nb_tx; k < receive; k++) {
					struct rte_mbuf *m = pkts[k];
					rte_pktmbuf_free(m);
				}
			}

		}else {
			continue;
		}
	}
	return 0;
}

int comp(const void*a, const void *b){

	return  *(int *)b - *(int *)a;
}

static inline const char * get_nf_rq_name(int i){
	static char buffer[sizeof(NF_NAME_RX_RING) + 10];
	snprintf(buffer, sizeof(buffer)-1, NF_NAME_RX_RING, Agent_id, i);
	return buffer;
}
char *get_nf_tq_name(int i){
	static char buffer[sizeof(NF_NAME_TX_RING)+10];
	snprintf(buffer, sizeof(buffer)-1, NF_NAME_TX_RING,Agent_id, i);
	return buffer;

}

static int
lthread_nf(void *dumy){

	uint16_t i, nf_id, nb_rx, nb_tx;
	struct ipv4_hdr *ipv4_hdr;
	struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
	struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
	struct rte_mbuf *pkts[BURST_SIZE];
	struct rte_ring *rq;
	struct rte_ring *tq;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	nf_id = nf_info_local->nf_id;
	rq = nf_info_local->rx_q;
	tq = nf_info_local->tx_q;

	printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
	rte_delay_ms(1000);
	for (; keep_running;) {
//        printf("nf %d try to dequeue pkts\n", nf_id);
		nb_rx = rte_ring_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
//        printf("suc\n");
		if (likely(nb_rx > 0)) {
			//do somthing
			nb_tx = rte_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
//            printf("nf %d suc transfer %d pkts\n", nf_id, nb_tx);

			if (unlikely(nb_tx < nb_rx)) {
				uint32_t k;
				for (k = nb_tx; k < nb_rx; k++) {
					struct rte_mbuf *m = pkts[k];
					rte_pktmbuf_free(m);
				}
			}

		}else {
			continue;
		}
	}
	return 0;

}

static void
lthread_spawner(__rte_unused void *arg) {
    struct lthread *lt[MAX_THREAD];
    int i;
    int n_thread = nb_nfs +rx_thread_num +tx_thread_num;
    int lcore_id = -1;

    printf("Entering lthread_spawner\n");

    /*
     * Create producers (rx threads) on default lcore
     */

    launch_batch_nfs(lt, &lcore_id, rx_thread_num, lcore_rx_main, (void *)&rx[0],
                     (void *)&rx[1], (void *)&rx[2]);
    //FIXME: now just support 3 rx thread
//	launch_sfc(lt, &lcore_id, n_rx_thread, lthread_rx, (void *)&rx_thread[0],
//			   lthread_rx, (void *)&rx_thread[1], lthread_rx, (void *)&rx_thread[2]);
//    rx_thread[0].conf.lcore_id = rx_thread[1].conf.lcore_id = rx_thread[2].conf.lcore_id = lcore_id;

    /*
     * Wait for all producers. Until some producers can be started on the same
     * scheduler as this lthread, yielding is required to let them to run and
     * prevent deadlock here.
     */
    while (rte_atomic16_read(&rx_counter) < n_rx_thread)
        lthread_sleep(100000);

    /*
     * Create consumers (tx threads) on default lcore_id
     */
    //FIXME: now just support 6 tx thread
    launch_batch_nfs(lt, &lcore_id, n_tx_thread, lthread_tx, (void *)&tx_thread[0], (void *)&tx_thread[1], (void *)&tx_thread[2],
                     (void *)&tx_thread[3], (void *)&tx_thread[4], (void *)&tx_thread[5]);
    tx_thread[0].conf.lcore_id = tx_thread[1].conf.lcore_id = tx_thread[2].conf.lcore_id = tx_thread[3].conf.lcore_id = lcore_id;

    /*
     * Wait for all threads finished
     */

    for (i = 0; i < n_thread; i++)
        lthread_join(lt[i], NULL);

}
static int
lthread_master_spawner(__rte_unused void *arg) {
    struct lthread *lt;
    int lcore_id = rte_lcore_id();
    long long thread_id;

    launch_batch_nfs(&lt, &lcore_id, 1, lthread_spawner, NULL);
    //call scheduler
    //TODO: call different scheduler on different cores
    slave_scheduler_run();

    return 0;
}
static void
lthread_null(__rte_unused void *args)
{
	int lcore_id = rte_lcore_id();
	printf("Starting scheduler on lcore %d.\n", lcore_id);
	lthread_exit(NULL);
}
static int
sched_spawner(__rte_unused void *arg) {
	struct lthread *lt;
	int lcore_id = rte_lcore_id();

	//TODO: launching nf should transfer to master scheduler to do
	launch_batch_nfs(&lt, &lcore_id, 1, lthread_null, NULL);
	slave_scheduler_run();

	return 0;
}
int main(int argc, char *argv[]) {

	int ret, i, j;
	uint8_t total_ports, cur_lcore;

	int socket_id = rte_socket_id();
	const char *rq_name;
	const char *tq_name;
	int delay_time = 1000;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	total_ports = rte_eth_dev_count();
	rx_thread_num = total_ports * 2;
	tx_thread_num = total_ports * 2;

	printf("available ports: %d\n", total_ports);

	//configuration of each tx thread, each tx thread send one flow
	for(i = 0; i< tx_thread_num;i++){

		tx[i] = calloc(1, sizeof(struct tx_rx_thread_info));
		//50-50 distribute to 2 ports
		tx[i]->port = 0;//1:1 allocate tx thread to 2 ports
		tx[i]->queue = i ;
		tx[i]->thread_id = i;
		printf("rx[%d]->port %d queue %d\n", i, tx[i]->port, tx[i]->queue);
	}

	printf("rx_thread_num:%d\n", rx_thread_num);
	for (i = 0; i < rx_thread_num ; ++i) {
		rx[i] = calloc(1, sizeof(struct tx_rx_thread_info));
		//50-50 distribute to 2 ports
		rx[i]->port = 0;
		rx[i]->queue = i;
		printf("rx[%d]->port %d queue %d\n", i, rx[i]->port, rx[i]->queue);
		rx[i]->thread_id = i;
	}

	cur_lcore = rte_lcore_id();
	if (total_ports < 1)
		rte_exit(EXIT_FAILURE, "ports is not enough\n");
	ret = init_mbuf_pools();
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "Cannot create needed mbuf pools\n");

	for(i = 0;i<NUM_PORTS;i++){
		ret = port_init(port_id_list[i]);
		if (ret != 0)
			rte_exit(EXIT_FAILURE, "Cannot init tx port %u\n", tx[i]->port);
	}
	nfs_info_data = rte_calloc("nfs info details",
							   10, sizeof(*nfs_info_data), 0);
	if (nfs_info_data == NULL)
		rte_exit(EXIT_FAILURE, "Cannot allocate memory for nf program details\n");
	for(i = 0;i<nb_nfs;i++){
		rq_name = get_nf_rq_name(i);
		tq_name = get_nf_tq_name(i);
		nfs_info_data[i].rx_q = rte_ring_create(rq_name, NF_QUEUE_RING_SIZE, socket_id, RING_F_SP_ENQ);
		nfs_info_data[i].tx_q = rte_ring_create(tq_name, NF_QUEUE_RING_SIZE, socket_id, RING_F_SP_ENQ);
		if(nfs_info_data[i].rx_q == NULL || nfs_info_data[i].tx_q == NULL){
			rte_exit(EXIT_FAILURE, "cannot create ring for nf %d\n", i);
		}
		nfs_info_data[i].nf_id = i;
		nf[i] = calloc(1, sizeof(struct nf_thread_info));
		nf[i]->nf_id = i;
	}
	printf(">>>suc create nf ring\n");

	//let us try lthread!
	nb_lcores = init_Agent(nb_lcores);
	printf("Agent get % core\n", nb_lcores);
//		launch_scheduler(nb_lcores);
//        init_cores(nb_lcores);
//		lthread_num_schedulers_set(nb_lcores);

	rte_eal_mp_remote_launch(sched_spawner, NULL, SKIP_MASTER);
	lthread_master_spawner(NULL);

	//pthread
//	for( i = 0;i< rx_thread_num ;i++){
//		cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
//		printf("in launching rx %d port %d\n", i, rx[i]->port);
//		if (rte_eal_remote_launch(lcore_rx_main, (void *)rx[i],  cur_lcore) == -EBUSY) {
//			printf("Core %d is already busy, can't use for rx %d \n", cur_lcore, i);
//			return -1;
//		}
//	}

//	for(i = 0;i<nb_nfs;i++){
//		cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
//		printf("launching nf thread %d\n", i);
//		if(rte_eal_remote_launch(lthread_nf, (void *)nf[i], cur_lcore) == -EBUSY){
//			printf("core %d cannot use for nf %d\n", cur_lcore, i);
//			return -1;
//		}
//	}
	//
//	for(i = 0;i<tx_thread_num;i++){
//		cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
//		if (rte_eal_remote_launch(lcore_tx_main, (void *) tx[i], cur_lcore) == -EBUSY) {
//			printf("Core %d is already busy, can't use for tx \n", cur_lcore);
//			return -1;
//		}
//	}

	int flow_id = 0;
	for (; keep_running;){

		rte_delay_ms(delay_time);
//        rte_delay_ms(500);

	}
	return 0;
}