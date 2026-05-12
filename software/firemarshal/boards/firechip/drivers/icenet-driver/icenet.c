// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (C) 2017-2019 Regents of the University of California
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/circ_buf.h>

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>

#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

/* Can't add new CONFIG parameters in an external module, so define them here */
#define CONFIG_ICENET_MTU 1500
#define CONFIG_ICENET_RING_SIZE 64
/*
 * Keep IceNet checksum offload disabled until the FireSim TX rewrite path is
 * proven not to corrupt target-to-host payloads under F2.
 */
#undef CONFIG_ICENET_CHECKSUM
#define CONFIG_ICENET_TX_THRESHOLD 16

#define ICENET_NAME "icenet"
#define ICENET_SEND_REQ 0
#define ICENET_RECV_REQ 8
#define ICENET_SEND_COMP 16
#define ICENET_RECV_COMP 18
#define ICENET_COUNTS 20
#define ICENET_MACADDR 24
#define ICENET_INTMASK 32
#define ICENET_TXCSUM_REQ 40
#define ICENET_RXCSUM_RES 48
#define ICENET_CSUM_ENABLE 49

#define ICENET_INTMASK_TX 1
#define ICENET_INTMASK_RX 2
#define ICENET_INTMASK_BOTH 3
#define ICENET_TX_REQS_PER_PACKET 1

#define ETH_HEADER_BYTES 14
#define ALIGN_BYTES 8
#define ALIGN_MASK 0x7
#define ALIGN_SHIFT 3
#define MAX_FRAME_SIZE (CONFIG_ICENET_MTU + ETH_HEADER_BYTES + NET_IP_ALIGN)
#define DMA_PTR_ALIGN(p) ((typeof(p)) (__ALIGN_KERNEL((uintptr_t) (p), ALIGN_BYTES)))
#define DMA_LEN_ALIGN(n) (((((n) - 1) >> ALIGN_SHIFT) + 1) << ALIGN_SHIFT)
#define MACADDR_BYTES 6
#define ICENET_MAX_SEGS (MAX_SKB_FRAGS + 1)

struct sk_buff_cq_entry {
	struct sk_buff *skb;
	dma_addr_t dma_addrs[ICENET_MAX_SEGS];
	unsigned int dma_lens[ICENET_MAX_SEGS];
	bool dma_is_page[ICENET_MAX_SEGS];
	bool dma_is_coherent[ICENET_MAX_SEGS];
	int nsegs;
	void *tx_bounce;
	dma_addr_t tx_bounce_dma;
};

struct sk_buff_cq {
	struct sk_buff_cq_entry entries[CONFIG_ICENET_RING_SIZE];
	int head;
	int tail;
};

#define SK_BUFF_CQ_COUNT(cq) CIRC_CNT(cq.head, cq.tail, CONFIG_ICENET_RING_SIZE)
#define SK_BUFF_CQ_SPACE(cq) CIRC_SPACE(cq.head, cq.tail, CONFIG_ICENET_RING_SIZE)

static inline void sk_buff_cq_init(struct sk_buff_cq *cq)
{
	cq->head = 0;
	cq->tail = 0;
}

static inline struct sk_buff_cq_entry *sk_buff_cq_head_entry(
		struct sk_buff_cq *cq)
{
	return &cq->entries[cq->head];
}

static inline struct sk_buff_cq_entry *sk_buff_cq_tail_entry(
		struct sk_buff_cq *cq)
{
	return &cq->entries[cq->tail];
}

static inline void sk_buff_cq_push(struct sk_buff_cq *cq)
{
	cq->head = (cq->head + 1) & (CONFIG_ICENET_RING_SIZE - 1);
}

static inline struct sk_buff_cq_entry *sk_buff_cq_pop(struct sk_buff_cq *cq)
{
	struct sk_buff_cq_entry *entry;

	entry = &cq->entries[cq->tail];
	cq->tail = (cq->tail + 1) & (CONFIG_ICENET_RING_SIZE - 1);

	return entry;
}

static inline int sk_buff_cq_tail_nsegments(struct sk_buff_cq *cq)
{
	return cq->entries[cq->tail].nsegs;
}

struct icenet_device {
	struct device *dev;
	void __iomem *iomem;
	struct napi_struct napi;
	struct sk_buff_cq send_cq;
	struct sk_buff_cq recv_cq;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	int tx_irq;
	int rx_irq;
};

static inline int send_req_avail(struct icenet_device *nic)
{
	return ioread32(nic->iomem + ICENET_COUNTS) & 0xff;
}

static inline int recv_req_avail(struct icenet_device *nic)
{
	return (ioread32(nic->iomem + ICENET_COUNTS) >> 8) & 0xff;
}

static inline int send_comp_avail(struct icenet_device *nic)
{
	return (ioread32(nic->iomem + ICENET_COUNTS) >> 16) & 0xff;
}

static inline int recv_comp_avail(struct icenet_device *nic)
{
	return (ioread32(nic->iomem + ICENET_COUNTS) >> 24) & 0xff;
}

static inline void set_intmask(struct icenet_device *nic, uint32_t mask)
{
	atomic_t *mem = nic->iomem + ICENET_INTMASK;
	atomic_fetch_or(mask, mem);
}

static inline void clear_intmask(struct icenet_device *nic, uint32_t mask)
{
	atomic_t *mem = nic->iomem + ICENET_INTMASK;
	atomic_fetch_and(~mask, mem);
}

static inline void icenet_unmap_entry(struct icenet_device *nic,
		struct sk_buff_cq_entry *entry, enum dma_data_direction dir)
{
	int i;

	for (i = 0; i < entry->nsegs; i++) {
		if (entry->dma_is_coherent[i])
			continue;
		else if (entry->dma_is_page[i])
			dma_unmap_page(nic->dev, entry->dma_addrs[i],
					entry->dma_lens[i], dir);
		else
			dma_unmap_single(nic->dev, entry->dma_addrs[i],
					entry->dma_lens[i], dir);
	}
	entry->nsegs = 0;
}

static inline void submit_send_req(struct icenet_device *nic,
		dma_addr_t dma_addr, uint64_t len, bool partial)
{
	uint64_t packet;

	packet = ((uint64_t)partial << 63) | (len << 48) |
			(dma_addr & 0xffffffffffffL);
	iowrite64(packet, nic->iomem + ICENET_SEND_REQ);
}

static inline int prepare_send_bounce(struct icenet_device *nic,
		struct sk_buff_cq_entry *entry, struct sk_buff *skb)
{
	u8 *bounce = entry->tx_bounce;
	uint64_t len = skb->len + NET_IP_ALIGN;
	int ret;

	if (!bounce)
		return -ENOMEM;
	if (len > MAX_FRAME_SIZE)
		return -EMSGSIZE;

	if (skb_headroom(skb) >= NET_IP_ALIGN) {
		ret = skb_copy_bits(skb, -NET_IP_ALIGN, bounce, len);
	} else {
		memset(bounce, 0, NET_IP_ALIGN);
		ret = skb_copy_bits(skb, 0, bounce + NET_IP_ALIGN, skb->len);
	}
	if (ret)
		return ret;

	entry->dma_addrs[0] = entry->tx_bounce_dma;
	entry->dma_lens[0] = len;
	entry->dma_is_page[0] = false;
	entry->dma_is_coherent[0] = true;
	entry->nsegs = 1;
	return 0;
}

static inline int post_send(
		struct icenet_device *nic, struct sk_buff *skb)
{
	struct sk_buff_cq_entry *entry;
	int ret;
	int i;

	entry = sk_buff_cq_head_entry(&nic->send_cq);
	entry->skb = skb;
	entry->nsegs = 0;
	ret = prepare_send_bounce(nic, entry, skb);
	if (ret)
		return ret;

#ifdef CONFIG_ICENET_CHECKSUM
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		uint64_t start, offset, csum_req;
		start = skb_checksum_start_offset(skb) + NET_IP_ALIGN;
		offset = start + skb->csum_offset;
		csum_req = (1L << 48) | (offset << 32) | (start << 16);
		iowrite64(csum_req, nic->iomem + ICENET_TXCSUM_REQ);
		skb->ip_summed = CHECKSUM_NONE;
	} else {
		iowrite64(0, nic->iomem + ICENET_TXCSUM_REQ);
	}
#endif

	dma_wmb();
	for (i = 0; i < entry->nsegs; i++) {
		bool seg_partial = i != entry->nsegs - 1;
		submit_send_req(nic, entry->dma_addrs[i], entry->dma_lens[i],
				seg_partial);
	}

	sk_buff_cq_push(&nic->send_cq);

//	printk(KERN_DEBUG "IceNet: tx addr=%lx len=%llu\n", addr, len);
	return 0;
}

static inline int post_recv(
		struct icenet_device *nic, struct sk_buff *skb)
{
	int align = DMA_PTR_ALIGN(skb->data) - skb->data;
	dma_addr_t dma_addr;
	struct sk_buff_cq_entry *entry;

	skb_reserve(skb, align);
	dma_addr = dma_map_single(nic->dev, skb->data, MAX_FRAME_SIZE,
			DMA_FROM_DEVICE);
	if (dma_mapping_error(nic->dev, dma_addr)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	entry = sk_buff_cq_head_entry(&nic->recv_cq);
	entry->skb = skb;
	entry->dma_addrs[0] = dma_addr;
	entry->dma_lens[0] = MAX_FRAME_SIZE;
	entry->dma_is_page[0] = false;
	entry->dma_is_coherent[0] = false;
	entry->nsegs = 1;

	iowrite64(dma_addr, nic->iomem + ICENET_RECV_REQ);
	sk_buff_cq_push(&nic->recv_cq);
	return 0;
}

static inline int send_space(struct icenet_device *nic, int nfrags)
{
	return (send_req_avail(nic) >= nfrags) &&
		(SK_BUFF_CQ_SPACE(nic->send_cq) > 0);
}

static void complete_send(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct sk_buff *skb;
	struct sk_buff_cq_entry *entry;
	int i, n, nsegs;

	for (n = send_comp_avail(nic); n > 0; n -= nsegs) {
		BUG_ON(SK_BUFF_CQ_COUNT(nic->send_cq) == 0);
		nsegs = sk_buff_cq_tail_nsegments(&nic->send_cq);

		if (nsegs > n)
			break;

		for (i = 0; i < nsegs; i++)
			ioread16(nic->iomem + ICENET_SEND_COMP);

		entry = sk_buff_cq_pop(&nic->send_cq);
		icenet_unmap_entry(nic, entry, DMA_TO_DEVICE);
		skb = entry->skb;
		dev_consume_skb_irq(skb);
	}

	if (send_space(nic, MAX_SKB_FRAGS) && netif_queue_stopped(ndev)) {
//		netdev_info(ndev, "starting queue\n");
		netif_wake_queue(ndev);
	}
}

static int complete_recv(struct net_device *ndev, int budget)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct sk_buff *skb;
	struct sk_buff_cq_entry *entry;
	int len, n, i;
#ifdef CONFIG_ICENET_CHECKSUM
	int csum_res;
#endif

	n = recv_comp_avail(nic);
	if (budget < n) {
		n = budget;
	}

	for (i = 0; i < n; i++) {
		len = ioread16(nic->iomem + ICENET_RECV_COMP);
		entry = sk_buff_cq_pop(&nic->recv_cq);
		icenet_unmap_entry(nic, entry, DMA_FROM_DEVICE);
		skb = entry->skb;
		skb_put(skb, len);
		skb_pull(skb, NET_IP_ALIGN);

#ifdef CONFIG_ICENET_CHECKSUM
		csum_res = ioread8(nic->iomem + ICENET_RXCSUM_RES);
		if (csum_res == 3)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else if (csum_res == 1)
			printk(KERN_ERR "IceNet: Checksum offload detected incorrect checksum\n");
#endif
		skb->dev = ndev;
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		netif_receive_skb(skb);

//		printk(KERN_DEBUG "IceNet: rx addr=%p, len=%d\n",
//				skb->data, len);
	}

	return n;
}

static void alloc_recv(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	int hw_recv_cnt = recv_req_avail(nic);
	int sw_recv_cnt = SK_BUFF_CQ_SPACE(nic->recv_cq);
	int recv_cnt = (hw_recv_cnt < sw_recv_cnt) ? hw_recv_cnt : sw_recv_cnt;

	for ( ; recv_cnt > 0; recv_cnt--) {
		struct sk_buff *skb;
		skb = netdev_alloc_skb(ndev, MAX_FRAME_SIZE);
		if (!skb || post_recv(nic, skb))
			break;
	}
}

static inline void icenet_schedule(struct icenet_device *nic)
{
	struct napi_struct *napi = &nic->napi;
	if (likely(napi_schedule_prep(napi))) {
		__napi_schedule(napi);
	}
}


static irqreturn_t icenet_tx_isr(int irq, void *data)
{
	struct net_device *ndev = data;
	struct icenet_device *nic = netdev_priv(ndev);

	if (irq != nic->tx_irq)
		return IRQ_NONE;

	spin_lock(&nic->tx_lock);
	clear_intmask(nic, ICENET_INTMASK_TX);
	spin_unlock(&nic->tx_lock);

	icenet_schedule(nic);

	return IRQ_HANDLED;
}

static irqreturn_t icenet_rx_isr(int irq, void *data)
{
	struct net_device *ndev = data;
	struct icenet_device *nic = netdev_priv(ndev);

	if (irq != nic->rx_irq)
		return IRQ_NONE;

	spin_lock(&nic->rx_lock);
	clear_intmask(nic, ICENET_INTMASK_RX);
	spin_unlock(&nic->rx_lock);

	icenet_schedule(nic);

	return IRQ_HANDLED;
}

static int icenet_poll(struct napi_struct *napi, int budget)
{
	struct icenet_device *nic;
	struct net_device *ndev;
	int work_done;
	unsigned long flags;

	nic = container_of(napi, struct icenet_device, napi);
	ndev = dev_get_drvdata(nic->dev);

	spin_lock_irqsave(&nic->tx_lock, flags);
	complete_send(ndev);
	spin_unlock_irqrestore(&nic->tx_lock, flags);

	work_done = complete_recv(ndev, budget);
	alloc_recv(ndev);

	if (work_done < budget) {
		napi_complete_done(napi, work_done);
		set_intmask(nic, ICENET_INTMASK_RX);
	}

	return work_done;
}

static int icenet_parse_addr(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct device *dev = nic->dev;
	struct device_node *node = dev->of_node;
	struct resource regs;
	int err;

	err = of_address_to_resource(node, 0, &regs);
	if (err) {
		dev_err(dev, "missing \"reg\" property\n");
		return err;
	}

	nic->iomem = devm_ioremap_resource(dev, &regs);
	if (IS_ERR(nic->iomem)) {
		dev_err(dev, "could not remap io address %llx", regs.start);
		return PTR_ERR(nic->iomem);
	}

	return 0;
}

static int icenet_parse_irq(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct device *dev = nic->dev;
	struct device_node *node = dev->of_node;
	int err;

	nic->tx_irq = irq_of_parse_and_map(node, 0);
	err = devm_request_irq(dev, nic->tx_irq, icenet_tx_isr,
			IRQF_SHARED | IRQF_NO_THREAD,
			ICENET_NAME, ndev);
	if (err) {
		dev_err(dev, "could not obtain TX irq %d\n", nic->tx_irq);
		return err;
	}

	nic->rx_irq = irq_of_parse_and_map(node, 1);
	err = devm_request_irq(dev, nic->rx_irq, icenet_rx_isr,
			IRQF_SHARED | IRQF_NO_THREAD,
			ICENET_NAME, ndev);
	if (err) {
		dev_err(dev, "could not obtain RX irq %d\n", nic->rx_irq);
		return err;
	}

	return 0;
}

static int icenet_open(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	unsigned long flags;

#ifdef CONFIG_ICENET_CHECKSUM
	iowrite8(1, nic->iomem + ICENET_CSUM_ENABLE);
	mb();
#endif

	napi_enable(&nic->napi);

	alloc_recv(ndev);

	netif_start_queue(ndev);

	spin_lock_irqsave(&nic->rx_lock, flags);
	set_intmask(nic, ICENET_INTMASK_RX);
	spin_unlock_irqrestore(&nic->rx_lock, flags);

	printk(KERN_DEBUG "IceNet: opened device\n");

	return 0;
}

static int icenet_stop(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);

	napi_disable(&nic->napi);

	clear_intmask(nic, ICENET_INTMASK_BOTH);
	netif_stop_queue(ndev);

	printk(KERN_DEBUG "IceNet: stopped device\n");
	return 0;
}

static int icenet_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&nic->tx_lock, flags);

	if (unlikely(!send_space(nic, ICENET_TX_REQS_PER_PACKET))) {
		netif_stop_queue(ndev);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		spin_unlock_irqrestore(&nic->tx_lock, flags);
		netdev_err(ndev, "insufficient space in Tx ring\n");
		return NETDEV_TX_BUSY;
	}

	skb_tx_timestamp(skb);
	ret = post_send(nic, skb);
	if (unlikely(ret)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		spin_unlock_irqrestore(&nic->tx_lock, flags);
		netdev_err(ndev, "failed to map Tx packet for DMA\n");
		return NETDEV_TX_OK;
	}
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	if (send_comp_avail(nic) > CONFIG_ICENET_TX_THRESHOLD) {
		icenet_schedule(nic);
	}

	if (unlikely(!send_space(nic, ICENET_TX_REQS_PER_PACKET))) {
		netif_stop_queue(ndev);
		icenet_schedule(nic);
	}

	spin_unlock_irqrestore(&nic->tx_lock, flags);

	return NETDEV_TX_OK;
}

static void icenet_init_mac_address(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	uint64_t macaddr = ioread64(nic->iomem + ICENET_MACADDR);

	ndev->addr_assign_type = NET_ADDR_PERM;
	ndev->addr_len = MACADDR_BYTES;
	dev_addr_set(ndev, (void*)&macaddr);

	if (!is_valid_ether_addr(ndev->dev_addr))
		printk(KERN_WARNING "Invalid MAC address\n");
}

static const struct net_device_ops icenet_ops = {
	.ndo_open = icenet_open,
	.ndo_stop = icenet_stop,
	.ndo_start_xmit = icenet_start_xmit
};

static int icenet_alloc_tx_bounce(struct icenet_device *nic)
{
	int i;

	for (i = 0; i < CONFIG_ICENET_RING_SIZE; i++) {
		struct sk_buff_cq_entry *entry = &nic->send_cq.entries[i];

		entry->tx_bounce = dmam_alloc_coherent(nic->dev, MAX_FRAME_SIZE,
				&entry->tx_bounce_dma, GFP_KERNEL);
		if (!entry->tx_bounce)
			return -ENOMEM;
	}

	return 0;
}

static int icenet_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct icenet_device *nic;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	ndev = devm_alloc_etherdev(dev, sizeof(struct icenet_device));
	if (!ndev)
		return -ENOMEM;

	dev_set_drvdata(dev, ndev);
	nic = netdev_priv(ndev);
	nic->dev = dev;

	netif_napi_add(ndev, &nic->napi, icenet_poll);

	ether_setup(ndev);
	ndev->flags &= ~IFF_MULTICAST;
	ndev->netdev_ops = &icenet_ops;
#ifdef CONFIG_ICENET_CHECKSUM
	ndev->hw_features = NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_RXCSUM;
#else
	ndev->hw_features = 0;
	dev_info(dev, "IceNet TX coherent bounce enabled; checksum offload and SG disabled\n");
#endif

	ndev->features = ndev->hw_features;
	ndev->vlan_features = ndev->hw_features;
	ndev->max_mtu = CONFIG_ICENET_MTU;

	spin_lock_init(&nic->tx_lock);
	spin_lock_init(&nic->rx_lock);
	sk_buff_cq_init(&nic->send_cq);
	sk_buff_cq_init(&nic->recv_cq);
	if ((ret = icenet_alloc_tx_bounce(nic)) < 0) {
		dev_err(dev, "failed to allocate TX coherent bounce buffers\n");
		return ret;
	}

	if ((ret = icenet_parse_addr(ndev)) < 0)
		return ret;

	icenet_init_mac_address(ndev);
	if ((ret = register_netdev(ndev)) < 0) {
		dev_err(dev, "Failed to register netdev\n");
		return ret;
	}

	if ((ret = icenet_parse_irq(ndev)) < 0)
		return ret;

	printk(KERN_INFO "Registered IceNet NIC %02x:%02x:%02x:%02x:%02x:%02x\n",
			ndev->dev_addr[0],
			ndev->dev_addr[1],
			ndev->dev_addr[2],
			ndev->dev_addr[3],
			ndev->dev_addr[4],
			ndev->dev_addr[5]);

	return 0;
}

static int icenet_remove(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct icenet_device *nic;

	ndev = platform_get_drvdata(pdev);
	nic = netdev_priv(ndev);
	netif_napi_del(&nic->napi);
	unregister_netdev(ndev);
	return 0;
}

static struct of_device_id icenet_of_match[] = {
	{ .compatible = "ucb-bar,ice-nic" },
	{ .compatible = "ucbbar,ice-nic" },
	{}
};

static struct platform_driver icenet_driver = {
	.driver = {
		.name = ICENET_NAME,
		.of_match_table = icenet_of_match,
		.suppress_bind_attrs = true
	},
	.probe = icenet_probe,
	.remove = icenet_remove
};

module_platform_driver(icenet_driver);
MODULE_DESCRIPTION("Drives the FireChip IceNIC ethernet device (used in firesim)");
MODULE_LICENSE("GPL");
