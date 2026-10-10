// iwlwifi.h - Intel Wi-Fi 6 (AX101/AX201/AX211 "So" family), started from scratch
//
// The card has its own processors; the driver loads Intel's firmware into
// them (through a "context info" block in memory that tells the card where
// the firmware sections, the command queue and the receive queue are), and
// then talks to the firmware with commands. Modelled on OpenBSD's iwx(4)
// and Linux's iwlwifi.
//
// It scans, joins open and WPA2-Personal (AES) networks, and then serves the
// network stack as its network card (nic_attach), so DHCP, ping, DNS and
// downloads go over Wi-Fi. The joined network is saved in /WIFI.CFG (the
// network name and the key derived from the password, not the password) and
// joined again at boot and whenever the connection drops.
#ifndef IWLWIFI_H
#define IWLWIFI_H

/* `wifi start`: bring the card up to "firmware alive", printing each step.
 * Returns 0 if the firmware said it is alive. */
int iwl_start(void);

/* `wifi scan`: start the card if needed, send the setup commands, scan
 * every channel (listening only) and list the networks heard. */
int iwl_scan(void);

/* The name numbered `number` (1, 2, ...) in the list `wifi scan` printed
 * last; 0 if there is no such number. */
const char *iwl_scan_pick(int number);

/* `wifi connect`: join `ssid`. `password` may be 0 or "" for an open
 * network or one saved before. Then gets an address over DHCP. */
int iwl_connect(const char *ssid, const char *password);

/* 1 if `ssid` is the saved network (no password needed). */
int iwl_has_saved(const char *ssid);

void iwl_disconnect(void);        /* `wifi disconnect`: leave, stop reconnecting */
void iwl_forget(void);            /* `wifi forget`: delete the saved network */
void iwl_print_status(void);      /* `wifi status` */

/* While the shell is idle: reconnect after a drop, renew the DHCP lease. */
void iwl_service(void);

/* At boot: join the saved network. 1 = nothing saved (or no Intel card),
 * 0 = connected, -1 = failed (will keep trying); `how` describes it. */
int iwl_autoconnect(char *how, int size);

#endif
