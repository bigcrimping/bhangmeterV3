#pragma once
/* lwIP options for optical_bhang.
 * Tuned for Pico W HTTPS client (GitHub Contents API). */

/* Core */
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

/* Memory — use lwIP's built-in pool allocator (MEM_LIBC_MALLOC is incompatible
 * with PICO_CYW43_ARCH_THREADSAFE_BACKGROUND because callbacks run from IRQ).
 * MEM_ALIGNMENT MUST be 4 on the Cortex-M33 (RP2350): lwIP's opt.h defaults it
 * to 1 when undefined, which makes mem_malloc return misaligned pointers and
 * the first heap allocation (DHCP's pbuf) hard-faults core0. */
#define MEM_ALIGNMENT               4
/* mbedTLS routes ALL its allocations through this lwIP heap (because
 * MBEDTLS_PLATFORM_MEMORY is set with no CALLOC/FREE macros, the SDK's
 * altcp_tls_mbedtls_mem.c installs tls_malloc->mem_malloc). One TLS 1.2
 * handshake needs ~18-24 KB live (in+out record buffers ~8.5 KB at
 * MBEDTLS_SSL_MAX_CONTENT_LEN=4096, plus peer-cert parse + ECDHE + TX pbufs),
 * so the old 16 KB heap made altcp_tls_new()/ssl_setup() fail with OOM.
 * 48 KB gives comfortable fragmentation margin (plenty of SRAM freed after
 * shrinking the waveform buffers). */
#define MEM_SIZE                    (48 * 1024)
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     1
#define MEMP_NUM_TCP_SEG            32   /* must be >= TCP_SND_QUEUELEN = 4*TCP_SND_BUF/TCP_MSS */
#define MEMP_NUM_PBUF               24
#define PBUF_POOL_SIZE              24
/* lwIP auto-sizes MEMP_NUM_SYS_TIMEOUT from TCP+ARP+DHCP+DNS only (=5 here) and
 * does NOT count app timers. LWIP_SNTP needs one more; without this the pool
 * runs dry once TCP+DHCP-renew+DNS+SNTP timers are all live -> "MEMP_SYS_TIMEOUT
 * is empty" panic mid-upload. 8 gives headroom. */
#define MEMP_NUM_SYS_TIMEOUT        8

/* TCP */
#define LWIP_TCP                    1
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_QUEUE_OOSEQ             0

/* Protocols */
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define LWIP_ICMP                   0
#define LWIP_IGMP                   0
#define LWIP_RAW                    0

/* SNTP */
#define LWIP_SNTP                   1
#define SNTP_SERVER_DNS             1
#define SNTP_UPDATE_DELAY           3600000   /* 1 hour in ms */
/* Forward declaration so sntp.c can see the function when it expands the macro */
#include <stdint.h>
void sntp_set_time_callback(uint32_t sec, uint32_t us);
#define SNTP_SET_SYSTEM_TIME_US(sec, us) sntp_set_time_callback((sec), (us))

/* altcp / TLS */
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1

/* Debug (disable for production) */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0
