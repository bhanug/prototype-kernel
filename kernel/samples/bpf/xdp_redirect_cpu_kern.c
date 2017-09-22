/*  XDP redirect to CPUs via cpu_map
 *
 * -=- WARNING -=- EXPERIMENTAL: featured under development!
 *
 *  GPLv2, Copyright(c) 2017 Jesper Dangaard Brouer, Red Hat, Inc.
 */
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/if_vlan.h>
#include <uapi/linux/ip.h>
#include <uapi/linux/ipv6.h>
#include <uapi/linux/in.h>
#include <uapi/linux/tcp.h>

#include <uapi/linux/bpf.h>
#include "bpf_helpers.h"

#define MAX_CPUS 12 /* WARNING - sync with _user.c */

/* Special map type that can XDP_REDIRECT frames to another CPU */
struct bpf_map_def SEC("maps") cpu_map = {
	.type		= BPF_MAP_TYPE_CPUMAP,
	.key_size	= sizeof(u32),
	.value_size	= sizeof(u32),
	.max_entries	= MAX_CPUS,
};

/* Common stats data record to keep userspace more simple */
struct datarec {
	__u64 processed;
	__u64 dropped;
};

/* Count RX packets, as XDP bpf_prog doesn't get direct TX-success
 * feedback.  Redirect TX errors can be caught via a tracepoint.
 */
struct bpf_map_def SEC("maps") rx_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(u32),
	.value_size	= sizeof(struct datarec),
	.max_entries	= 1,
};

/* Used by trace point */
struct bpf_map_def SEC("maps") redirect_err_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(u32),
	.value_size	= sizeof(struct datarec),
	.max_entries	= 2,
	/* TODO: have entries for all possible errno's */
};

/* Used by trace point */
struct bpf_map_def SEC("maps") cpumap_enqueue_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(u32),
	.value_size	= sizeof(struct datarec),
	.max_entries	= MAX_CPUS,
};

/* Used by trace point */
struct bpf_map_def SEC("maps") cpumap_kthread_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(u32),
	.value_size	= sizeof(struct datarec),
	.max_entries	= 1,
};

/* Helper parse functions */

/* Parse Ethernet layer 2, extract network layer 3 offset and protocol
 *
 * Returns false on error and non-supported ether-type
 */
struct vlan_hdr {
	__be16 h_vlan_TCI;
	__be16 h_vlan_encapsulated_proto;
};

static __always_inline
bool parse_eth(struct ethhdr *eth, void *data_end,
	       u16 *eth_proto, u64 *l3_offset)
{
	u16 eth_type;
	u64 offset;

	offset = sizeof(*eth);
	if ((void *)eth + offset > data_end)
		return false;

	eth_type = eth->h_proto;

	/* Skip non 802.3 Ethertypes */
	if (unlikely(ntohs(eth_type) < ETH_P_802_3_MIN))
		return false;

	/* Handle VLAN tagged packet */
	if (eth_type == htons(ETH_P_8021Q) || eth_type == htons(ETH_P_8021AD)) {
		struct vlan_hdr *vlan_hdr;

		vlan_hdr = (void *)eth + offset;
		offset += sizeof(*vlan_hdr);
		if ((void *)eth + offset > data_end)
			return false;
		eth_type = vlan_hdr->h_vlan_encapsulated_proto;
	}
	/* TODO: Handle double VLAN tagged packet */

	*eth_proto = ntohs(eth_type);
	*l3_offset = offset;
	return true;
}

static __always_inline
int get_proto_ipv4(struct xdp_md *ctx, u64 nh_off)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
        struct iphdr *iph = data + nh_off;

        if (iph + 1 > data_end)
                return 0;
        return iph->protocol;
}

static __always_inline
int get_proto_ipv6(struct xdp_md *ctx, u64 nh_off)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
        struct ipv6hdr *ip6h = data + nh_off;

        if (ip6h + 1 > data_end)
                return 0;
        return ip6h->nexthdr;
}

SEC("xdp_cpu_map0")
int  xdp_prognum0_no_touch(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
	struct datarec* rec;
	u32 cpu_dest = 0;
	u32 key = 0;

	/* Count RX packet in map */
	rec = bpf_map_lookup_elem(&rx_cnt, &key);
	if (rec)
		rec->processed++;

	return bpf_redirect_map(&cpu_map, cpu_dest, 0);
}

SEC("xdp_cpu_map1_touch_data")
int  xdp_prognum1_touch_data(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	volatile u16 eth_type;
	struct datarec* rec;
	u32 cpu_dest = 0;
	u32 key = 0;

	/* Validate packet length is minimum Eth header size */
	if (eth + 1 > data_end) {
		return XDP_ABORTED;
	}

	/* Count RX packet in map */
	rec = bpf_map_lookup_elem(&rx_cnt, &key);
	if (rec)
		rec->processed++;

	/* Read packet data, and use it (drop non 802.3 Ethertypes) */
	eth_type = eth->h_proto;
	if (ntohs(eth_type) < ETH_P_802_3_MIN) {
		if (rec)
			rec->dropped++;
		return XDP_DROP;
	}

	return bpf_redirect_map(&cpu_map, cpu_dest, 0);
}

SEC("xdp_cpu_map2_round_robin")
int  xdp_prognum2_round_robin(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	struct datarec* rec;
	u32 cpu_dest = 0;
	u32 *cpu_lookup;
	u32 key = 0;

	/* Count RX packet in map */
	rec = bpf_map_lookup_elem(&rx_cnt, &key);
	if (rec) {
		rec->processed++;
		cpu_dest = (u32)(rec->processed) % 4;
		cpu_dest += 1; // exclude 0, and use 1,2,3,4
	}

	/* Check cpu_dest is valid */
	cpu_lookup = bpf_map_lookup_elem(&cpu_map, &cpu_dest);
	if (!cpu_lookup)
		return XDP_ABORTED;

	if (cpu_dest >= MAX_CPUS )
		return XDP_ABORTED;

	return bpf_redirect_map(&cpu_map, cpu_dest, 0);
}

SEC("xdp_cpu_map_proto_separate")
int  xdp_prog_cpu_map_prognum3(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	u8 ip_proto = IPPROTO_UDP;
	struct datarec* rec;
	u16 eth_proto = 0;
	u64 l3_offset = 0;
	u32 cpu_dest = 0;
	u32 key = 0;

	/* Count RX packet in map */
	rec = bpf_map_lookup_elem(&rx_cnt, &key);
	if (rec)
		rec->processed++;

	if (!(parse_eth(eth, data_end, &eth_proto, &l3_offset))) {
		return XDP_PASS; /* Just skip */
	}

	/* Extract L4 protocol */
	switch (eth_proto) {
	case ETH_P_IP:
		ip_proto = get_proto_ipv4(ctx, l3_offset);
		break;
	case ETH_P_IPV6:
		ip_proto = get_proto_ipv6(ctx, l3_offset);
		break;
	case ETH_P_ARP:
		cpu_dest = 1; /* ARP packet handled on separate CPU */
		break;
	default:
		cpu_dest = 0;
	}

	/* Choose CPU based on L4 protocol */
	switch (ip_proto) {
	case IPPROTO_ICMP:
	case IPPROTO_ICMPV6:
		cpu_dest = 1;
		break;
	case IPPROTO_TCP:
		cpu_dest = 2;
		break;
	case IPPROTO_UDP:
		cpu_dest = 3;
		break;
	default:
		cpu_dest = 0;
	}

	if (cpu_dest >= MAX_CPUS )
		return XDP_ABORTED;

	return bpf_redirect_map(&cpu_map, cpu_dest, 0);
}

char _license[] SEC("license") = "GPL";

/*** Trace point code ***/

/* Tracepoint format: /sys/kernel/debug/tracing/events/xdp/xdp_redirect/format
 * Code in:                kernel/include/trace/events/xdp.h
 */
struct xdp_redirect_ctx {
	unsigned short common_type;	//	offset:0;  size:2; signed:0;
	unsigned char common_flags;	//	offset:2;  size:1; signed:0;
	unsigned char common_preempt_count;//	offset:3;  size:1; signed:0;
	int common_pid;			//	offset:4;  size:4; signed:1;

	int prog_id;			//	offset:8;  size:4; signed:1;
	u32 act;			//	offset:12  size:4; signed:0;
	int ifindex;			//	offset:16  size:4; signed:1;
	int err;			//	offset:20  size:4; signed:1;
	int to_ifindex;			//	offset:24  size:4; signed:1;
	u32 map_id;			//	offset:28  size:4; signed:0;
	int map_index;			//	offset:32  size:4; signed:1;
};					//	offset:36

enum {
	XDP_REDIRECT_SUCCESS = 0,
	XDP_REDIRECT_ERROR = 1
};

static __always_inline
int xdp_redirect_collect_stat(struct xdp_redirect_ctx *ctx)
{
	u32 key = XDP_REDIRECT_ERROR;
	struct datarec *rec;
	int err = ctx->err;

	if (!err)
		key = XDP_REDIRECT_SUCCESS;

	rec = bpf_map_lookup_elem(&redirect_err_cnt, &key);
	if (!rec)
		return 0;
	rec->dropped += 1;

	return 0; /* Indicate event was filtered (no further processing)*/
	/*
	 * Returning 1 here would allow e.g. a perf-record tracepoint
	 * to see and record these events, but it doesn't work well
	 * in-practice as stopping perf-record also unload this
	 * bpf_prog.  Plus, there is additional overhead of doing so.
	 */
}

SEC("tracepoint/xdp/xdp_redirect_err")
int trace_xdp_redirect_err(struct xdp_redirect_ctx *ctx)
{
	return xdp_redirect_collect_stat(ctx);
}


SEC("tracepoint/xdp/xdp_redirect_map_err")
int trace_xdp_redirect_map_err(struct xdp_redirect_ctx *ctx)
{
	return xdp_redirect_collect_stat(ctx);
}

/* Tracepoint: /sys/kernel/debug/tracing/events/xdp/xdp_cpumap_enqueue/format
 * Code in:         kernel/include/trace/events/xdp.h
 */
struct cpumap_enqueue_ctx {
	unsigned short common_type;	//	offset:0;  size:2; signed:0;
	unsigned char common_flags;	//	offset:2;  size:1; signed:0;
	unsigned char common_preempt_count;//	offset:3;  size:1; signed:0;
	int common_pid;			//	offset:4;  size:4; signed:1;

	int map_id;			//	offset:8;  size:4; signed:1;
	u32 act;			//	offset:12; size:4; signed:0;
	int cpu;			//	offset:16; size:4; signed:1;
	unsigned int drops;		//	offset:20; size:4; signed:0;
	unsigned int processed;		//	offset:24; size:4; signed:0;
	int to_cpu;			//	offset:28; size:4; signed:1;
};

SEC("tracepoint/xdp/xdp_cpumap_enqueue")
int trace_xdp_cpumap_enqueue(struct cpumap_enqueue_ctx *ctx)
{
	u32 to_cpu = ctx->to_cpu;
	struct datarec *rec;

	if (to_cpu >= MAX_CPUS)
		return 1;

	rec = bpf_map_lookup_elem(&cpumap_enqueue_cnt, &to_cpu);
	if (!rec)
		return 0;
	rec->processed += ctx->processed;
	rec->dropped   += ctx->drops;

	return 0;
}

/* Tracepoint: /sys/kernel/debug/tracing/events/xdp/xdp_cpumap_kthread/format
 * Code in:         kernel/include/trace/events/xdp.h
 */
struct cpumap_kthread_ctx {
	unsigned short common_type;	//	offset:0;  size:2; signed:0;
	unsigned char common_flags;	//	offset:2;  size:1; signed:0;
	unsigned char common_preempt_count;//	offset:3;  size:1; signed:0;
	int common_pid;			//	offset:4;  size:4; signed:1;

	int map_id;			//	offset:8;  size:4; signed:1;
	u32 act;			//	offset:12; size:4; signed:0;
	int cpu;			//	offset:16; size:4; signed:1;
	unsigned int drops;		//	offset:20; size:4; signed:0;
	unsigned int processed;		//	offset:24; size:4; signed:0;
	int time_limit;			//	offset:28; size:4; signed:1;
};

SEC("tracepoint/xdp/xdp_cpumap_kthread")
int trace_xdp_cpumap_kthread(struct cpumap_kthread_ctx *ctx)
{
	struct datarec *rec;
	u32 key = 0;

	rec = bpf_map_lookup_elem(&cpumap_kthread_cnt, &key);
	if (!rec)
		return 0;
	rec->processed += ctx->processed;
	rec->dropped   += ctx->drops;

	// TODO: Use time_limit
	return 0;
}
