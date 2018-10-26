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
#include "nfs/simple_forward.h"
#include "nfs/nf_common.h"

#define RX_RING_SIZE 512
#define TX_RING_SIZE 512
#define NUM_MBUFS 8192 * 4
#define MBUF_SIZE (1600 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MBUF_CACHE_SIZE 0
#define NO_FLAGS 0
#define RING_MAX 1  // 1个ring

#define NUM_PORTS 1
#define MAX_NUM_PORT 4
#define MAX_LCORE_NUM 24
#define MAX_NF_NUM 1000
#define MAX_AGENT_NUM 5
#define NF_QUEUE_RING_SIZE 256
#define MAX_THREAD 10
#define NF_NAME_RX_RING "Agent_NF_%u_rq"
#define NF_NAME_TX_RING "Agent_NF_%u_tq"

#define MONITOR_PERIOD 30  // 3秒钟更新 一次 monitor的信息

static uint8_t port_id_list[NUM_PORTS] = {  0};

static struct rte_mempool *pktmbuf_pool;

//now each rx/tx work for one flow
struct tx_rx_thread_info
{
    uint8_t port;
    uint8_t queue;
    uint16_t level;
    uint8_t thread_id;
    uint16_t fid;
};

struct Agent_info{
    uint64_t core_mask_count;
    int priority;
    int Agent_id;
    int nfs_num;
    int core_list[MAX_LCORE_NUM];
};

struct Agent_info *agents_info_data;
uint16_t nb_nfs= 2;
uint16_t nb_agents = 1;
uint16_t nb_lcores;
int rx_thread_num = 1;
int tx_thread_num = 1;
//根据不同机器来制定, 0预留给core manager
int rx_exclusive_lcore = 2;
int tx_exclusive_lcore = 4;
struct tx_rx_thread_info *tx[4 * MAX_NUM_PORT];//infor of port using by thread
struct tx_rx_thread_info *rx[4 * MAX_NUM_PORT];
struct nf_thread_info *nf[ MAX_NF_NUM];

uint64_t flow_ip_table[MAX_NF_NUM]={
        16820416, 33597632, 67152064, 83929280, 100706496, 117483712, 50374848, 134260928
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

/*
 *  send pkts through one port for flows identified from start_fid to end_fid,
 *  tx rate is set by the parameter tx_delay[tx_id]
 */
//static int lcore_tx_main(void *arg) {
//    uint16_t i, j, sent, nb_rx, nb_tx;
//    uint16_t thread_id;
//    uint16_t queue_id, port_id;
//    int nf_id = 0;
//    struct tx_rx_thread_info *tx = (struct tx_rx_thread_info *)arg;
//    struct rte_mbuf *pkts[BURST_SIZE];
//    struct rte_ring *ring;
//
//    signal(SIGINT, handle_signal);
//    signal(SIGTERM, handle_signal);
//
//    thread_id = tx->thread_id;
//    port_id = tx->port;
//    queue_id = tx->queue;
//
//    printf("Core %d: Running TX thread %d,\n", rte_lcore_id(), tx->thread_id);
//
//    for(;keep_running;){
//        ring = nfs_info_data[nf_id].tx_q;
//        nb_rx = rte_ring_dequeue_burst(ring, pkts, BURST_SIZE*2, NULL);
//        queue_id = nf_id % RING_MAX;
//
//        if(unlikely(nb_rx>0)){
//
//            nb_tx = rte_eth_tx_burst(port_id, queue_id, pkts, nb_rx);
//            if (unlikely(nb_tx < nb_rx)) {
//                do {
//                    rte_pktmbuf_free(pkts[nb_tx]);
//                } while (++nb_tx < nb_rx);
//            }//end free if
//        }//end process if
//        nf_id++;
//        nf_id%=nb_nfs;
//    }
//
//    return 0;
//}

/*
 * receive pkts from all queues of one port and update the stats of ports and flows
 */
//static int lcore_rx_main(void *arg) {
//
//    uint16_t i, queue_id, receive, p_id, thread_id, nb_tx, cnt = 0;
//    struct ipv4_hdr *ipv4_hdr;
//    struct tx_rx_thread_info *rx_local = (struct tx_rx_thread_info *)arg;
//    struct rte_mbuf *pkts[BURST_SIZE];
//    struct timespec *payload, now = {0, 0};
//
//    signal(SIGINT, handle_signal);
//    signal(SIGTERM, handle_signal);
//
//    p_id = rx_local->port;
//    queue_id = rx_local->queue;
//    thread_id = rx_local->thread_id;
//
//    printf("Core %d: Running RX thread %d for port %d queue %d\n", rte_lcore_id(), thread_id, p_id, queue_id);
//
//    for (; keep_running;) {
//
//        receive = rte_eth_rx_burst(p_id, queue_id, pkts, BURST_SIZE);
//        queue_id++;
//        queue_id%=RING_MAX;
//
//        if (likely(receive > 0)) {
////            if(thread_id == 1)
////                printf("rx %d recv %d pkts on queue %d\n", thread_id, receive, queue_id);
//
//            //TODO: classifier pkts should follow some rule
//            nb_tx = rte_ring_enqueue_burst(nfs_info_data[queue_id%nb_nfs].rx_q, pkts, receive, NULL);
//            if (unlikely(nb_tx < receive)) {
//                uint32_t k;
//                for (k = nb_tx; k < receive; k++) {
//                    struct rte_mbuf *m = pkts[k];
//                    rte_pktmbuf_free(m);
//                }
//            }
//
//        }else {
//            continue;
//        }
//    }
//    return 0;
//}

static void
lthread_null(__rte_unused void *args)
{
    int lcore_id = rte_lcore_id();
//    printf("Starting scheduler on lcore %d.\n", lcore_id);
    lthread_exit(NULL);
}
/*
 * notify thread manager to giveback the core of a Agent,
 * thread manager will migrate nfs to another core of the Agent
 * */
int notify_to_give_back(int victim_lcore_id, int agent_id){
    //TODO: 应该选择负载最小的core
    int dst_core = agents_info_data[agent_id].core_list[0];
    int index = 0, lc = 0;//lcore

    printf("CM notify sched %d give back, migrate existing nfs to core %d\n", victim_lcore_id, dst_core);
    set_give_back_flag(dst_core, victim_lcore_id);
    while(read_give_back_flag(victim_lcore_id)>0){
        rte_delay_ms(10);
    }
    uint8_t count = (agents_info_data[agent_id].core_mask_count&255);
    uint64_t bitmap = agents_info_data[agent_id].core_mask_count>>8;
    printf("update agent %d core list, old cnt=%d, mask=%d\n", agent_id, count, bitmap);
    count -=1;
    bitmap = (bitmap & ~(0x1<<victim_lcore_id));
    printf("new mask and count = %d, %d\n", count, bitmap);//11
    agents_info_data[agent_id].core_mask_count = ((bitmap<<8)|count);
    while(bitmap>0){
        if((bitmap&1UL)==1){
            agents_info_data[agent_id].core_list[index++] = lc;
            printf(">>>>hold core %d\n", agents_info_data[agent_id].core_list[index-1]);
        }
        lc++;
        bitmap = bitmap>>1;
    }
    return 0;
}
/* before give a new core to a Agent, update its core params */
void add_core_to_set(int agent_id, int lcore_id){

    uint64_t mask = agents_info_data[agent_id].core_mask_count;
    uint8_t count = ((mask&255) +1);
    uint64_t bitmap = mask|(1<<8<<lcore_id);
    agents_info_data[agent_id].core_mask_count = (bitmap|count);
    agents_info_data[agent_id].core_list[count-1]=lcore_id;
    printf("update agent %d core list, new cnt= %d, mask= %d\n", agent_id, count, bitmap>>8);

    return;
}
int reply_to_Agent_request(int agent_id){
    int ret = -1;//refuse
    //TODO: select a idle core or do preemption
    //if preemption, call notify_to_give_back() to take back the core
    return ret;
}
/*
 * main loop of core manager
 */
static int
lthread_master_spawner(__rte_unused void *arg) {
    struct lthread *lt[MAX_THREAD];
//    int lcore_id = rte_lcore_id();
    int lcore_id, agent_id;
    int nfs_num_per_core;
    int i, sched, add_flag, new_core_id;
    int index_of_agent[MAX_AGENT_NUM];
    int monitor_tick = 0;
    printf("entering core manager loop on core %d\n", rte_lcore_id());
    //TODO: dispatch nfs to scheds of each Agent
    for(i = 0;i<MAX_AGENT_NUM;i++){
        index_of_agent[i] = 0;
    }

    for(i = 0;i<nb_nfs; i++){

        agent_id = nfs_info_data[i].agent_id;
        lcore_id = agents_info_data[agent_id].core_list[index_of_agent[agent_id]];
        index_of_agent[agent_id]++;
        index_of_agent[agent_id] %= (agents_info_data[agent_id].core_mask_count & 255);
        launch_batch_nfs(lt, &lcore_id, 1, nfs_info_data[i].fun, (void *)nf[i]);
    }

    printf("finish launch nfs\n");

    while (1){
        rte_delay_ms(100);
        for(i = 0;i<nb_agents;i++){
            int sched_cnt = (agents_info_data[i].core_mask_count&255);
            for(sched=0; sched<sched_cnt; sched++){
                lcore_id = agents_info_data[i].core_list[sched];
                add_flag = check_add_flag(lcore_id);
                new_core_id = check_new_core_id(lcore_id);

                if(add_flag == 0 && new_core_id>0){
                    //已经重新分配过核了，并且scheduler scale完毕
//                    printf("CM find scheduler %d resume new core, reset new_core_id\n", lcore_id);
                    set_new_core(lcore_id, -1);
                }else if(add_flag == 1 && new_core_id == -1){
                    //新的请求，分配
                    new_core_id = reply_to_Agent_request(i);
                    if(new_core_id >0){
                        add_core_to_set(i, new_core_id);
//                    printf("CM find sched %d add_flag ==1, set new_core_id=%d\n", lcore_id, new_core_id);
                        set_new_core(lcore_id, new_core_id);//notify thread manager
                    }
                }else if(add_flag == 1 && new_core_id >0){
                    //已经分配过核，scheduler正在scale, do nothing
                }//end process request
            }//end of scanning sched
        }//end of scanning agent

        // 在这里算nf丢包信息
        monitor_tick++;
        if (monitor_tick == MONITOR_PERIOD) {
            monitor_update(3);
            monitor_tick = 0;
        }

//        if (monitor_tick % 25 == 0) {
//            // 示例: 获取nf的monitor信息
//            uint64_t processed_pps = get_processed_pps_with_nf_id(0);  // nf 0 的处理能力
//            uint64_t dropped_pps = get_dropped_pps_with_nf_id(0); // nf 0 每秒丢包数
//            double dropped_ratio = get_dropped_ratio_with_nf_id(0); // nf 0 这段时间的丢包率
//
//            for(i =0;i<nb_nfs; i++){
//                processed_pps = get_processed_pps_with_nf_id(i);
//                dropped_pps = get_dropped_pps_with_nf_id(i);
//                dropped_ratio = get_dropped_ratio_with_nf_id(i);
//                printf(">>> NF %d <<< processing pps: %ld, drop pps: %ld, drop ratio: %lf\n", i, processed_pps,
//                       dropped_pps, dropped_ratio);
//            }
//
//        }
    }

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
/*
 * at first give cores to each agent
 */
uint64_t coremask_set[MAX_AGENT_NUM]={
        0x54003, 0x500002, 0x1000001, 0xc14000002
};
int give_cores_to_Agent(int agent_id){
    //core 0 is exclusive to core manager
    //TODO: policy: p0: 6,8,10,(0x540)  p1:12,14(0x5000), p2:16(0x10000)  p3:18,20(0xc140000)
//    uint64_t coremaskcnt = 0x1E04;
    printf("get agent %d, coremask=%d\n", agent_id, coremask_set[agent_id]);
    return coremask_set[agent_id];
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

    /* init Agent info */
    uint64_t tmp_mask;
    int index = 0, lc=0;

    for(i = 0;i<nb_agents;i++){
        agents_info_data[i].Agent_id = 1000+i;
        agents_info_data[i].nfs_num = 0;
        agents_info_data[i].priority = i;
    }
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
        //TODO: load nf function
        nfs_info_data[i].fun = lthread_forwarder;
        nfs_info_data[i].priority = i % nb_agents;
        nfs_info_data[i].agent_id = i % nb_agents;
        agents_info_data[nfs_info_data[i].priority].nfs_num +=1;
        nf[i] = calloc(1, sizeof(struct nf_thread_info));
        nf[i]->nf_id = i;

    }

    for(i = 0;i<nb_agents ;i++){

        index = 0; lc=0;
        tmp_mask = agents_info_data[i].core_mask_count = give_cores_to_Agent(i);
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


    // 分配一个核给 flow distributor rx thread
    flow_table_init();
    struct port_info *port_info1 = calloc(1, sizeof(struct port_info));
    port_info1->port_id = 0;
    port_info1->queue_id = 0;
    cur_lcore = rx_exclusive_lcore;
    if (rte_eal_remote_launch(flow_director_rx_thread, (void *) port_info1, cur_lcore) == -EBUSY) {
        printf("Core %d is already busy, can't use for RX of flow director \n", cur_lcore);
        return -1;
    }
    for(i = 0;i<nb_nfs;i++){
        printf(">>>add flow entry %d-->nf %d\n", flow_ip_table[i], i);
        flow_table_add_entry(flow_ip_table[i], i); // flow hash (这里使用的是flow的源IP值), nf_id
        bind_nf_to_rxring(i, nfs_info_data[i].rx_q);
    }

    // 分配一个核给 flow distributor tx thread
    struct port_info *port_info2 = calloc(1, sizeof(struct port_info));
    port_info2->port_id = 0;
    port_info2->queue_id = 0;
    cur_lcore = tx_exclusive_lcore;
    if (rte_eal_remote_launch(flow_director_tx_thread, (void *) port_info2, cur_lcore) == -EBUSY) {
        printf("Core %d is already busy, can't use for TX of flow director \n", cur_lcore);
        return -1;
    }
    // attach NF的tx ring 到flow director上
    nf_need_output(0, 0, nfs_info_data[0].tx_q);  // 参数为 nf_id, 要往哪个port 上 发数据, nf_id 对应的tx_ring
    nf_need_output(1, 0, nfs_info_data[1].tx_q);


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
//    for(i = 0;i<tx_thread_num;i++){
//        cur_lcore = tx_exclusive_lcore;
//        if (rte_eal_remote_launch(lcore_tx_main, (void *) tx[i], cur_lcore) == -EBUSY) {
//            printf("Core %d is already busy, can't use for tx \n", cur_lcore);
//            return -1;
//        }
//    }
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