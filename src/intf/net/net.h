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

/* ping() results */
#define PING_OK           0   /* at least one reply received */
#define PING_NO_REPLY    -1   /* every request timed out */
#define PING_NO_ROUTE    -2   /* could not resolve the next hop's MAC (ARP failed) */
#define PING_BAD_ARG     -3
#define PING_NET_DOWN    -4

void net_init(void);                        // call once after rtl8139_probe_init() == 0
int  net_is_up(void);                       // 1 if a NIC was found and net_init ran
const net_config_t *net_get_config(void);

void net_print_ifconfig(void);              // the `ifconfig` shell command
int  net_selftest(void);                    // the `nettest` shell command; 0 = got a reply
void net_set_debug(int on);                 // print one line per received frame

/* Parse dotted-quad text ("10.0.2.2") into 4 bytes. Returns 0 on success, -1 if malformed. */
int  net_parse_ip(const char *s, uint8_t out[4]);

/* Send `count` ICMP echo requests to `ip`, one per second, printing a line per
 * reply (like ping(8)) and a summary at the end. Returns a PING_* code. */
int  net_ping(const uint8_t ip[4], uint32_t count);

/* Fetch a plain HTTP URL and save the response body to the current FAT32 directory.
 * HTTPS is intentionally unsupported. `filename` may be NULL to derive one from the URL. */
int  net_download_http(const char *url, const char *filename);

/* Formatting helpers (buffers: mac >= 18 bytes, ip >= 16 bytes) */
void net_fmt_mac(char *out, const uint8_t mac[6]);
void net_fmt_ip(char *out, const uint8_t ip[4]);

#endif
