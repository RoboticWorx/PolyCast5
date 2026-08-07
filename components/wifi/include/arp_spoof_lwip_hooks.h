#ifndef ARP_SPOOF_LWIP_HOOKS_H
#define ARP_SPOOF_LWIP_HOOKS_H

/*
 * Custom lwIP IPv4-input hook for the Wi-Fi ARP spoof feature.
 *
 * This header is pulled into lwIP's option processing via ESP_IDF_LWIP_HOOK_FILENAME
 * (wired for the lwip component in the top-level CMakeLists.txt). It must stay minimal and
 * self-contained: only forward-declare the lwIP types it names so it is safe to include very
 * early in the lwIP option chain. The implementation lives in arp_spoof.c.
 *
 * The hook is a no-op (returns 0, packet processed as normal) whenever ARP spoof is idle, so
 * it has no effect on normal networking.
 */

struct pbuf;
struct netif;

int arp_spoof_lwip_ip4_input_hook(struct pbuf *pbuf, struct netif *input_netif);

#define LWIP_HOOK_IP4_INPUT(pbuf, input_netif) arp_spoof_lwip_ip4_input_hook(pbuf, input_netif)

/*
 * The IPv6 input hook is NOT installed via a macro here. ESP-IDF already defines
 * LWIP_HOOK_IP6_INPUT (CONFIG_LWIP_HOOK_IP6_INPUT_DEFAULT=y) pointing at a __weak
 * lwip_hook_ip6_input(); defining our own macro would collide with it. Instead ndp_spoof.c
 * provides a strong lwip_hook_ip6_input() that overrides the weak default (and preserves its
 * behavior). Nothing to declare here.
 */

#endif // ARP_SPOOF_LWIP_HOOKS_H
