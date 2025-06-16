// lwipopts.h
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// Configurações essenciais para o Pico W
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    16000
#define PBUF_POOL_SIZE              24
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_DEBUG                  1
#define TCP_DEBUG                   LWIP_DBG_OFF
#define ETHARP_DEBUG                LWIP_DBG_OFF

#endif // LWIPOPTS_H