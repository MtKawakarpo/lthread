#ifdef AES_DECRYPT_H
#define AES_DECRYPT_H

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include <rte_common.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_ether.h>

#include "pkt_help.h"
#include "aes.h"


static BYTE de_key[1][32] = {
        {0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4}
};
static BYTE de_iv[1][16] = {
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f}
};

/* init tenant state */
void nf_aes_decrypt_init(struct nf_statistics *stats);
/* handle tenant packets */
uint16_t
nf_aes_decrypt_handler(struct rte_mbuf *pkt[], uint16_t num, struct nf_statistics *stats);

int lthread_aes_decryt(void *dumy);
int pthread_aes_decryt(void *dumy);

#endif