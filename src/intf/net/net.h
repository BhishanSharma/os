#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Network configuration. Until DHCP exists these are QEMU's user-mode
 * (slirp) defaults: we are 10.0.2.15, the router is 10.0.2.2, DNS is 10.0.2.3. */
typedef struct {
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
} net_config_t;

/* Ethertypes (host byte order) */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

void net_init(void);                        // call once after rtl8139_probe_init() == 0
int  net_is_up(void);                       // 1 if a NIC was found and net_init ran
const net_config_t *net_get_config(void);

void net_print_ifconfig(void);              // the `ifconfig` shell command
int  net_selftest(void);                    // the `nettest` shell command; 0 = got a reply
void net_set_debug(int on);                 // print one line per received frame

/* Formatting helpers (buffers: mac >= 18 bytes, ip >= 16 bytes) */
void net_fmt_mac(char *out, const uint8_t mac[6]);
void net_fmt_ip(char *out, const uint8_t ip[4]);

#endif
