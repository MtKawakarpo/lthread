/*
 *  Core Manager
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
#include "flow_distributer.h"

#define RX_RING_SIZE 512
#define TX_RING_SIZE 512
#define NUM_MBUFS 8192 * 4
#define MBUF_SIZE (1600 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MBUF_CACHE_SIZE 0
#define BURST_SIZE 32
#define NO_FLAGS 0
#define RING_MAX 10

#define NUM_PORTS 1
#define MAX_NUM_PORT 4
#define MAX_LCORE_NUM 24
#define MAX_NF_NUM 1000
#define MAX_AGENT_NUM 5
#define NF_QUEUE_RING_SIZE 256
#define MAX_THREAD 10
#define NF_NAME_RX_RING "Agent_NF_%u_rq"
#define NF_NAME_TX_RING "Agent_NF_%u_tq"

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
    int agent_id;
};
struct nf_thread_info{
    int nf_id;
};
struct Agent_info{
    uint64_t core_mask_count;
    int priority;
    int Agent_id;
    int core_list[MAX_LCORE_NUM];
};
struct nf_info *nfs_info_data;
struct Agent_info *agents_info_data;
uint16_t nb_nfs= 2;
uint16_t nb_agents = 1;
uint16_t nb_lcores;
int rx_thread_num = 1;
int tx_thread_num = 1;
int rx_exclusive_lcore = 5;
int tx_exclusive_lcore = 6;
struct tx_rx_thread_info *tx[4 * MAX_NUM_PORT];//infor of port using by thread
struct tx_rx_thread_info *rx[4 * MAX_NUM_PORT];
struct nf_thread_info *nf[ MAX_NF_NUM];

uint64_t flow_ip_table[MAX_NF_NUM]={
        16820416, 33597632
};

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

    printf("Creating mbuf pool '%s' [%u mbufs] ...\n",
           "MBUF_POOL", num_mbufs);
    pktmbuf_pool = rte_mempool_create("MBUF_POOL", num_mbufs,
                                      MBUF_SIZE, MBUF_CACHE_SIZE,
                                      sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init,
                                      NULL, rte_pktmbuf_init, NULL, rte_socket_id(), NO_FLAGS);

    return (pktmbuf_pool == NULL); /* 0  on success */
}

static inline const char * get_nf_rq_name(int i){
    static char buffer[sizeof(NF_NAME_RX_RING) + 10];
    snprintf(buffer, sizeof(buffer)-1, NF_NAME_RX_RING, i);
    return buffer;
}
char *get_nf_tq_name(int i){
    static char buffer[sizeof(NF_NAME_TX_RING)+10];
    snprintf(buffer, sizeof(buffer)-1, NF_NAME_TX_RING, i);
    return buffer;

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
    int nf_id = 0;
    struct tx_rx_thread_info *tx = (struct tx_rx_thread_info *)arg;
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *ring;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    thread_id = tx->thread_id;
    port_id = tx->port;
    queue_id = tx->queue;

    printf("Core %d: Running TX thread %d,\n", rte_lcore_id(), tx->thread_id);

    for(;keep_running;){
        ring = nfs_info_data[nf_id].tx_q;
        nb_rx = rte_ring_dequeue_burst(ring, pkts, BURST_SIZE*2, NULL);
        queue_id = nf_id % RING_MAX;

        if(unlikely(nb_rx>0)){

            nb_tx = rte_eth_tx_burst(port_id, queue_id, pkts, nb_rx);
            if (unlikely(nb_tx < nb_rx)) {
                do {
                    rte_pktmbuf_free(pkts[nb_tx]);
                } while (++nb_tx < nb_rx);
            }//end free if
        }//end process if
        nf_id++;
        nf_id%=nb_nfs;
    }

    return 0;
}

/*
 * receive pkts from all queues of one port and update the stats of ports and flows
 */
static int lcore_rx_main(void *arg) {

    uint16_t i, queue_id, receive, p_id, thread_id, nb_tx, cnt = 0;
    struct ipv4_hdr *ipv4_hdr;
    struct tx_rx_thread_info *rx_local = (struct tx_rx_thread_info *)arg;
    struct rte_mbuf *pkts[BURST_SIZE];
    struct timespec *payload, now = {0, 0};

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    p_id = rx_local->port;
    queue_id = rx_local->queue;
    thread_id = rx_local->thread_id;

    printf("Core %d: Running RX thread %d for port %d queue %d\n", rte_lcore_id(), thread_id, p_id, queue_id);

    for (; keep_running;) {

        receive = rte_eth_rx_burst(p_id, queue_id, pkts, BURST_SIZE);
        queue_id++;
        queue_id%=RING_MAX;

        if (likely(receive > 0)) {
//            if(thread_id == 1)
//                printf("rx %d recv %d pkts on queue %d\n", thread_id, receive, queue_id);

            //TODO: classifier pkts should follow some rule
            nb_tx = rte_ring_enqueue_burst(nfs_info_data[queue_id%nb_nfs].rx_q, pkts, receive, NULL);
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


static int
lthread_nf(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx, cnt;
    struct ipv4_hdr *ipv4_hdr;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;

    lthread_set_data((void *)nf_info_local);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
//	rte_delay_ms(1000);
    for (; keep_running;) {

        nb_rx = nf_ring_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx > 0)) {
            //do somthing
            nb_tx = nf_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
//            printf("nf %d suc transfer %d pkts\n", nf_id, nb_tx);

        }else {
            continue;
        }
    }
    return 0;

}

static void
lthread_null(__rte_unused void *args)
{
    int lcore_id = rte_lcore_id();
    printf("Starting scheduler on lcore %d.\n", lcore_id);
    lthread_exit(NULL);
}

/*
 * main loop of core manager
 */
static int
lthread_master_spawner(__rte_unused void *arg) {
    struct lthread *lt[MAX_THREAD];
//    int lcore_id = rte_lcore_id();
    int lcore_id = 2;//for test
    long long thread_id;

    printf("entering core manager loop on core %d\n", rte_lcore_id());
    //TODO: dispatch nfs to scheds of each Agent
    launch_batch_nfs(lt, &lcore_id, nb_nfs, lthread_nf, (void *)nf[0],
                     (void *)nf[1]);
    printf("finish launch nfs\n");
    //call scheduler
    //TODO: CM

    while (1){
        rte_delay_ms(100);
    }

    return 0;
}

static int
sched_spawner(__rte_unused void *arg) {
    struct lthread *lt;
    int lcore_id = rte_lcore_id();

    printf(">launching scheduler on core %d\n", lcore_id);
    launch_batch_nfs(&lt, &lcore_id, 1, lthread_null, NULL);
    slave_scheduler_run();

    return 0;
}
int give_cores_to_Agent(int agent_id){
    //core 0 is exclusive to core manager
    //TODO: policy
    uint64_t coremaskcnt = 0x1E04;
    return coremaskcnt;
}

int main(int argc, char *argv[]) {

    int ret, i, j;
    uint8_t total_ports, cur_lcore;

    int socket_id = rte_socket_id();
    const char *rq_name;
    const char *tq_name;

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    total_ports = rte_eth_dev_count();
    nb_lcores = 0;

    printf("available ports: %d\n", total_ports);

    //configuration of each tx thread, each tx thread send one flow
    for(i = 0; i< tx_thread_num;i++){

        tx[i] = calloc(1, sizeof(struct tx_rx_thread_info));
        tx[i]->port = 0;
        tx[i]->queue = i ;
        tx[i]->thread_id = i;
        printf("tx[%d]->port %d\n", i, tx[i]->port);
    }
//
//	for (i = 0; i < rx_thread_num ; ++i) {
//		rx[i] = calloc(1, sizeof(struct tx_rx_thread_info));
//		rx[i]->port = 0;
//		rx[i]->queue = i;
//		printf("rx[%d]->port %d\n", i, rx[i]->port);
//		rx[i]->thread_id = i;
//	}

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
    nfs_info_data = rte_calloc("nfs info",
                               MAX_NF_NUM, sizeof(*nfs_info_data), 0);
    if (nfs_info_data == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for nf  details\n");
    agents_info_data = rte_calloc("agents info", MAX_AGENT_NUM, sizeof(*agents_info_data), 0);
    if (agents_info_data == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for agent = details\n");

    /* init nf info */
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
    /* init Agent info */
    uint64_t tmp_mask;
    int index = 0, lc=0;
    for(i = 0;i<nb_agents;i++){
        index = 0; lc=0;
        agents_info_data[i].Agent_id = 1000+i;
        tmp_mask = agents_info_data[i].core_mask_count = give_cores_to_Agent(agents_info_data[i].Agent_id);
        //TODO: modify init_Agent to multi Agents version
        agents_info_data[i].priority = i;
        init_Agent(agents_info_data[i].Agent_id, agents_info_data[i].core_mask_count);
        printf(">>init Agent %d with priotity %d, %d core\n", agents_info_data[i].Agent_id, agents_info_data[i].priority,
               (agents_info_data[i].core_mask_count)&255);
        tmp_mask = (tmp_mask>>8);
        while(tmp_mask>0){
            if((tmp_mask&1UL)==1){
                agents_info_data[i].core_list[index++] = lc;
                printf(">>>>get core %d\n", agents_info_data[i].core_list[index-1]);
            }
            lc++;
            tmp_mask = tmp_mask>>1;
        }
    }

    // 分配一个核给 flow distributor
    flow_table_init();
    struct port_info *port_info1 = calloc(1, sizeof(struct port_info));
    port_info1->port_id = 0;
    port_info1->queue_id = 0;
    cur_lcore = rx_exclusive_lcore;
    if (rte_eal_remote_launch(flow_director_thread, (void *) port_info1, cur_lcore) == -EBUSY) {
        printf("Core %d is already busy, can't use for rx \n", cur_lcore);
        return -1;
    }
    for(i = 0;i<nb_nfs;i++){
        printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[i], i);
        flow_table_add_entry(flow_ip_table[i], i); // flow hash (这里使用的是flow的源IP值), nf_id
        bind_nf_to_rxring(i, nfs_info_data[i].rx_q);
    }

    /* launch scheduler of each Agent */
    for(j = 0; j<nb_agents;j++){
        int core_cnt = (agents_info_data[j].core_mask_count&255);
        for(i = 0;i<core_cnt;i++){
            cur_lcore = agents_info_data[j].core_list[i];
            if (rte_eal_remote_launch(sched_spawner, NULL, cur_lcore) == -EBUSY) {
                printf("Core %d is already busy, can't use for sched\n", cur_lcore);
                return -1;
            }
        }
    }

    /* a core for EAL tx thread */
    for(i = 0;i<tx_thread_num;i++){
        cur_lcore = tx_exclusive_lcore;
        if (rte_eal_remote_launch(lcore_tx_main, (void *) tx[i], cur_lcore) == -EBUSY) {
            printf("Core %d is already busy, can't use for tx \n", cur_lcore);
            return -1;
        }
    }
    /* main loop of CM */
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

    return 0;
}