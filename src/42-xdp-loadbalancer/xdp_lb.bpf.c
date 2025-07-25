// xdp_lb.bpf.c
#include <bpf/bpf_endian.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include "xx_hash.h"

#define MAX_TCP_CHECK_WORDS 750 // max 1500 bytes to check in TCP checksum. This is MTU dependent

struct backend_config {
    __u32 ip;
    unsigned char mac[ETH_ALEN];
};

// Backend IP and MAC address map
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 2);  // Two backends
    __type(key, __u32);
    __type(value, struct backend_config);
} backends SEC(".maps");

int client_ip = bpf_htonl(0xa000001);  
unsigned char client_mac[ETH_ALEN] = {0xDE, 0xAD, 0xBE, 0xEF, 0x0, 0x1};
int load_balancer_ip = bpf_htonl(0xa00000a);
unsigned char load_balancer_mac[ETH_ALEN] = {0xDE, 0xAD, 0xBE, 0xEF, 0x0, 0x10};

static __always_inline __u16
csum_fold_helper(__u64 csum)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        if (csum >> 16)
            csum = (csum & 0xffff) + (csum >> 16);
    }
    return ~csum;
}

static __always_inline __u16
tcph_csum(struct tcphdr *tcph, struct iphdr *iph, void *data_end)
{
    // Clear checksum
    tcph->check = 0;

    // Pseudo header checksum calculation
    __u32 sum = 0;
    sum += (__u16)(iph->saddr >> 16) + (__u16)(iph->saddr & 0xFFFF);
    sum += (__u16)(iph->daddr >> 16) + (__u16)(iph->daddr & 0xFFFF);
    sum += __constant_htons(IPPROTO_TCP);
    sum += __constant_htons((__u16)(data_end - (void *)tcph));

    // TCP header and payload checksum
    #pragma clang loop unroll_count(MAX_TCP_CHECK_WORDS)
    for (int i = 0; i <= MAX_TCP_CHECK_WORDS; i++) {
        __u16 *ptr = (__u16 *)tcph + i;
        if ((void *)ptr + 2 > data_end)
            break;
        sum += *(__u16 *)ptr;
    }

    // fold into 16 bit
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

static __always_inline __u16
iph_csum(struct iphdr *iph)
{
    iph->check = 0;
    unsigned long long csum = bpf_csum_diff(0, 0, (unsigned int *)iph, sizeof(struct iphdr), 0);
    return csum_fold_helper(csum);
}

SEC("xdp")
int xdp_load_balancer(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    bpf_printk("xdp_load_balancer received packet");

    // Ethernet header
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    // Check if the packet is IP (IPv4)
    if (eth->h_proto != __constant_htons(ETH_P_IP))
        return XDP_PASS;

    // IP header
    struct iphdr *iph = (struct iphdr *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;

    // Check if the protocol is TCP or UDP
    if (iph->protocol != IPPROTO_TCP)
        return XDP_PASS;

    // TCP header
    struct tcphdr *tcph = (void *)iph + iph->ihl * 4;
    if ((void *)tcph + sizeof(*tcph) > data_end)
        return XDP_PASS;
    
    bpf_printk("Received Source IP: 0x%x", bpf_ntohl(iph->saddr));
    bpf_printk("Received Destination IP: 0x%x", bpf_ntohl(iph->daddr));
    bpf_printk("Received Source MAC: %x:%x:%x:%x:%x:%x", eth->h_source[0], eth->h_source[1], eth->h_source[2], eth->h_source[3], eth->h_source[4], eth->h_source[5]);
    bpf_printk("Received Destination MAC: %x:%x:%x:%x:%x:%x", eth->h_dest[0], eth->h_dest[1], eth->h_dest[2], eth->h_dest[3], eth->h_dest[4], eth->h_dest[5]);

    if (iph->saddr == client_ip)
    {
        bpf_printk("Packet from client");

        struct {
            __u32 src_ip;
            __u32 dst_ip;
            __u16 src_port;
            __u16 dst_port;
        } four_tuple = {iph->saddr,
                        iph->daddr,
                        bpf_ntohs(tcph->source),
                        bpf_ntohs(tcph->dest)
                    };

        // Hash the 4-tuple for flow based backend decision
        __u32 key = xxhash32((const char *)&four_tuple, sizeof(four_tuple), 0) % 2;

        struct backend_config *backend = bpf_map_lookup_elem(&backends, &key);
        if (!backend)
            return XDP_PASS;
        
        iph->daddr = backend->ip;
        __builtin_memcpy(eth->h_dest, backend->mac, ETH_ALEN);
    }
    else
    {
        bpf_printk("Packet from backend");
        iph->daddr = client_ip;
        __builtin_memcpy(eth->h_dest, client_mac, ETH_ALEN);
    }

    // Update IP source address to the load balancer's IP
    iph->saddr = load_balancer_ip;
    // Update Ethernet source MAC address to the current lb's MAC
    __builtin_memcpy(eth->h_source, load_balancer_mac, ETH_ALEN);

    // Recalculate IP checksum
    iph->check = iph_csum(iph);

    // Recalculate TCP checksum
    tcph->check = tcph_csum(tcph, iph, data_end);

    bpf_printk("Redirecting packet to new IP 0x%x from IP 0x%x", 
                bpf_ntohl(iph->daddr), 
                bpf_ntohl(iph->saddr)
            );
    bpf_printk("New Dest MAC: %x:%x:%x:%x:%x:%x", eth->h_dest[0], eth->h_dest[1], eth->h_dest[2], eth->h_dest[3], eth->h_dest[4], eth->h_dest[5]);
    bpf_printk("New Source MAC: %x:%x:%x:%x:%x:%x\n", eth->h_source[0], eth->h_source[1], eth->h_source[2], eth->h_source[3], eth->h_source[4], eth->h_source[5]);
    // Return XDP_TX to transmit the modified packet back to the network
    return XDP_TX;
}

char _license[] SEC("license") = "GPL";
