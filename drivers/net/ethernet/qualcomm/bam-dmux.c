#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/qcom/smem_state.h>

#define BAM_DMUX_BUFFER_SIZE		SZ_2K
#define BAM_DMUX_NUM_DESC		32

#define BAM_DMUX_AUTOSUSPEND_DELAY	1000
#define BAM_DMUX_UL_WAKEUP_TIMEOUT	msecs_to_jiffies(2000)

#define BAM_DMUX_HDR_MAGIC		0x33fc

enum {
	BAM_DMUX_HDR_CMD_DATA,
	BAM_DMUX_HDR_CMD_OPEN,
	BAM_DMUX_HDR_CMD_CLOSE,
};

enum {
	BAM_DMUX_CH_DATA_RMNET_0,
	BAM_DMUX_CH_DATA_RMNET_1,
	BAM_DMUX_CH_DATA_RMNET_2,
	BAM_DMUX_CH_DATA_RMNET_3,
	BAM_DMUX_CH_DATA_RMNET_4,
	BAM_DMUX_CH_DATA_RMNET_5,
	BAM_DMUX_CH_DATA_RMNET_6,
	BAM_DMUX_CH_DATA_RMNET_7,
	BAM_DMUX_CH_USB_RMNET_0,
	BAM_DMUX_NUM_CH
};

struct bam_dmux_hdr {
	u16 magic;	/* = BAM_DMUX_HDR_MAGIC */
	u8 signal;
	u8 cmd;
	u8 pad;		/* 0...3: len should be word-aligned */
	u8 ch;
	u16 len;
};

struct bam_dmux_dma_desc {
	struct bam_dmux *dmux;
	dma_addr_t dma_addr;
	struct bam_dmux_hdr *hdr;
	struct dma_async_tx_descriptor *dma;
};

struct bam_dmux {
	struct device *dev;

	struct qcom_smem_state *pc, *pc_ack;
	u32 pc_mask, pc_ack_mask;
	bool pc_state, pc_ack_state;
	struct completion pc_completion, pc_ack_completion;

	struct dma_chan *rx, *tx;
	struct bam_dmux_dma_desc rx_desc[BAM_DMUX_NUM_DESC];

	struct net_device *netdevs[BAM_DMUX_NUM_CH];
};

struct bam_dmux_netdev {
	struct bam_dmux *dmux;
	u8 ch;
};

static void bam_dmux_pc_vote(struct bam_dmux *dmux, bool enable)
{
	reinit_completion(&dmux->pc_ack_completion);
	qcom_smem_state_update_bits(dmux->pc, dmux->pc_mask,
				    enable ? dmux->pc_mask : 0);
}

static void bam_dmux_pc_ack(struct bam_dmux *dmux)
{
	qcom_smem_state_update_bits(dmux->pc_ack, dmux->pc_ack_mask,
				    dmux->pc_ack_state ? 0 : dmux->pc_ack_mask);
	dmux->pc_ack_state = !dmux->pc_ack_state;
}

static int bam_dmux_netdev_open(struct net_device *dev)
{
	netdev_err(dev, "open\n");
	return 0;
}

static int bam_dmux_netdev_stop(struct net_device *dev)
{
	netdev_err(dev, "stop\n");
	return 0;
}

static netdev_tx_t bam_dmux_netdev_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	netdev_err(dev, "start_xmit\n");
	return NETDEV_TX_OK;
}

static const struct net_device_ops bam_dmux_ops_ether = {
	.ndo_open		= bam_dmux_netdev_open,
	.ndo_stop		= bam_dmux_netdev_stop,
	.ndo_start_xmit		= bam_dmux_netdev_start_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static void bam_dmux_netdev_setup(struct net_device *dev)
{
	/* Hardcode ethernet mode for now */
	ether_setup(dev);
	random_ether_addr(dev->dev_addr);
	dev->netdev_ops = &bam_dmux_ops_ether;

	dev->needed_headroom = sizeof(struct bam_dmux_hdr);
	dev->needed_tailroom = sizeof(u32); /* word-aligned */
	dev->max_mtu = 2000; /* TODO: Dynamic MTU */
}

static void bam_dmux_cmd_open(struct bam_dmux *dmux, struct bam_dmux_hdr *hdr)
{
	struct net_device *netdev = dmux->netdevs[hdr->ch];
	struct bam_dmux_netdev *bndev;
	const char *name;
	int ret;

	if (netdev) {
		if (netif_device_present(netdev))
			dev_err(dmux->dev, "Channel already open: %d\n", hdr->ch);
		else
			netif_device_attach(netdev);
		return;
	}

	name = "rmnet%d";
	if (hdr->ch == BAM_DMUX_CH_USB_RMNET_0)
		name = "rmnet_usb%d";

	netdev = alloc_netdev(sizeof(*bndev), name, NET_NAME_ENUM,
			      bam_dmux_netdev_setup);
	if (!netdev)
		return; /* -ENOMEM */

	bndev = netdev_priv(netdev);
	bndev->dmux = dmux;
	bndev->ch = hdr->ch;

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(dmux->dev, "Failed to register netdev for channel %d: %d\n",
			hdr->ch, ret);
		free_netdev(netdev);
		return;
	}

	netdev_info(netdev, "open channel %d\n", hdr->ch);
	dmux->netdevs[hdr->ch] = netdev;
}

static void bam_dmux_cmd_close(struct bam_dmux *dmux, struct bam_dmux_hdr *hdr)
{
	struct net_device *netdev = dmux->netdevs[hdr->ch];

	if (!netdev || netif_device_present(netdev)) {
		dev_err(dmux->dev, "Channel not open: %d\n", hdr->ch);
		return;
	}

	netif_device_detach(netdev);
}

static void bam_dmux_rx_callback(void *data)
{
	struct bam_dmux_dma_desc *desc = data;
	struct bam_dmux *dmux = desc->dmux;
	struct bam_dmux_hdr *hdr = desc->hdr;

	if (hdr->magic != BAM_DMUX_HDR_MAGIC) {
		dev_err(dmux->dev, "Invalid magic in header: %#x\n", hdr->magic);
		goto out;
	}

	if (hdr->ch >= BAM_DMUX_NUM_CH) {
		dev_warn(dmux->dev, "Unsupported channel: %d\n", hdr->ch);
		goto out;
	}

	dev_err(desc->dmux->dev, "callback: magic: %#x, signal: %#x, cmd: %d, pad: %d, ch: %d, len: %d\n",
		hdr->magic, hdr->signal, hdr->cmd, hdr->pad, hdr->ch, hdr->len);

	switch (hdr->cmd) {
	case BAM_DMUX_HDR_CMD_DATA:
		/* TODO */
		break;
	case BAM_DMUX_HDR_CMD_OPEN:
		bam_dmux_cmd_open(dmux, hdr);
		break;
	case BAM_DMUX_HDR_CMD_CLOSE:
		bam_dmux_cmd_close(dmux, hdr);
		break;
	default:
		dev_warn(dmux->dev, "Unsupported command %d on channel %d\n",
			 hdr->cmd, hdr->ch);
		break;
	}

out:
	return;
}

static bool bam_dmux_power_on(struct bam_dmux *dmux)
{
	struct device *dev = dmux->dev;
	int i;

	dmux->rx = dma_request_chan(dev, "rx");
	if (IS_ERR(dmux->rx)) {
		dev_err(dev, "Failed to request RX DMA channel: %pe\n", dmux->rx);
		dmux->rx = NULL;
		return false;
	}

	/* Queue RX descriptors */
	for (i = 0; i < BAM_DMUX_NUM_DESC; ++i) {
		struct bam_dmux_dma_desc *desc = &dmux->rx_desc[i];

		desc->dma = dmaengine_prep_slave_single(dmux->rx, desc->dma_addr,
						        BAM_DMUX_BUFFER_SIZE,
							DMA_DEV_TO_MEM,
							DMA_PREP_INTERRUPT);
		if (!desc->dma) {
			dev_err(dev, "Failed to prepare RX channel, desc %d\n", i);
			return false;
		}

		desc->dma->callback = bam_dmux_rx_callback;
		desc->dma->callback_param = desc;
		desc->dma->cookie = dmaengine_submit(desc->dma);
	}

	dma_async_issue_pending(dmux->rx);
	return true;
}

static void bam_dmux_power_off(struct bam_dmux *dmux)
{
	if (dmux->tx) {
		dmaengine_terminate_sync(dmux->tx);
		dma_release_channel(dmux->tx);
		dmux->tx = NULL;
	}

	if (dmux->rx) {
		dmaengine_terminate_sync(dmux->rx);
		dma_release_channel(dmux->rx);
		dmux->rx = NULL;
	}
}

static irqreturn_t bam_dmux_pc_irq(int irq, void *data)
{
	struct bam_dmux *dmux = data;

	dmux->pc_state = !dmux->pc_state;
	dev_err(dmux->dev, "pc: %d\n", dmux->pc_state);

	if (dmux->pc_state) {
		if (bam_dmux_power_on(dmux)) {
			bam_dmux_pc_ack(dmux);
			complete_all(&dmux->pc_completion);
		} else {
			bam_dmux_power_off(dmux);
		}
	} else {
		reinit_completion(&dmux->pc_completion);
		WARN_ON(pm_runtime_active(dmux->dev));
		bam_dmux_power_off(dmux);
		bam_dmux_pc_ack(dmux);
	}

	return IRQ_HANDLED;
}

static irqreturn_t bam_dmux_pc_ack_irq(int irq, void *data)
{
	struct bam_dmux *dmux = data;

	dev_err(dmux->dev, "pc ack\n");
	complete_all(&dmux->pc_ack_completion);

	return IRQ_HANDLED;
}

static int __maybe_unused bam_dmux_runtime_suspend(struct device *dev)
{
	struct bam_dmux *dmux = dev_get_drvdata(dev);

	dev_err(dev, "runtime suspend\n");
	bam_dmux_pc_vote(dmux, false);

	return 0;
}

static int __maybe_unused bam_dmux_runtime_resume(struct device *dev)
{
	struct bam_dmux *dmux = dev_get_drvdata(dev);

	dev_err(dev, "runtime resume\n");

	/* Wait until previous power down was acked */
	if (!wait_for_completion_timeout(&dmux->pc_ack_completion,
					 BAM_DMUX_UL_WAKEUP_TIMEOUT))
		return -ETIMEDOUT;

	/* Vote for power state */
	bam_dmux_pc_vote(dmux, true);

	/* Wait for ack */
	if (!wait_for_completion_timeout(&dmux->pc_ack_completion,
					 BAM_DMUX_UL_WAKEUP_TIMEOUT)) {
		bam_dmux_pc_vote(dmux, false);
		return -ETIMEDOUT;
	}

	/* Wait until we're up */
	if (!wait_for_completion_timeout(&dmux->pc_completion,
					 BAM_DMUX_UL_WAKEUP_TIMEOUT)) {
		bam_dmux_pc_vote(dmux, false);
		return -ETIMEDOUT;
	}

	/* Request TX channel */
	dmux->tx = dma_request_chan(dev, "tx");
	if (IS_ERR(dmux->rx)) {
		dev_err(dev, "Failed to request TX DMA channel: %pe\n", dmux->tx);
		dmux->tx = NULL;
		bam_dmux_pc_vote(dmux, false);
		return -EIO;
	}

	return 0;
}

static bool bam_dmux_alloc_rx(struct bam_dmux *dmux)
{
	void *base;
	dma_addr_t dma_base;
	int i;

	base = dma_alloc_coherent(dmux->dev,
				  BAM_DMUX_NUM_DESC * BAM_DMUX_BUFFER_SIZE,
				  &dma_base, GFP_KERNEL);
	if (!base) {
		dev_err(dmux->dev, "dma_alloc_coherent failed\n");
		return false;
	}

	for (i = 0; i < BAM_DMUX_NUM_DESC; ++i) {
		struct bam_dmux_dma_desc *desc = &dmux->rx_desc[i];

		desc->dmux = dmux;
		desc->hdr = base + i * BAM_DMUX_BUFFER_SIZE;
		desc->dma_addr = dma_base + i * BAM_DMUX_BUFFER_SIZE;
	}

	return true;
}

static int bam_dmux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bam_dmux *dmux;
	int ret, pc_irq, pc_ack_irq;
	unsigned bit;

	dmux = devm_kzalloc(dev, sizeof(*dmux), GFP_KERNEL);
	if (!dmux)
		return -ENOMEM;

	dmux->dev = dev;
	platform_set_drvdata(pdev, dmux);

	pc_irq = platform_get_irq_byname(pdev, "pc");
	if (pc_irq < 0)
		return pc_irq;

	pc_ack_irq = platform_get_irq_byname(pdev, "pc-ack");
	if (pc_ack_irq < 0)
		return pc_ack_irq;

	dmux->pc = qcom_smem_state_get(dev, "pc", &bit);
	if (IS_ERR(dmux->pc))
		return dev_err_probe(dev, PTR_ERR(dmux->pc), "Failed to get pc state\n");
	dmux->pc_mask = BIT(bit);

	dmux->pc_ack = qcom_smem_state_get(dev, "pc-ack", &bit);
	if (IS_ERR(dmux->pc_ack))
		return dev_err_probe(dev, PTR_ERR(dmux->pc_ack), "Failed to get pc-ack state\n");
	dmux->pc_ack_mask = BIT(bit);

	init_completion(&dmux->pc_completion);
	init_completion(&dmux->pc_ack_completion);
	complete_all(&dmux->pc_ack_completion);

	if (!bam_dmux_alloc_rx(dmux))
		return -ENOMEM;

	/* Runtime PM manages our own power vote.
	 * Note that the RX path may be active even if we are runtime suspended,
	 * since it is controlled by the remote side.
	 */
	pm_runtime_set_autosuspend_delay(dev, BAM_DMUX_AUTOSUSPEND_DELAY);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	ret = devm_request_threaded_irq(dev, pc_irq, NULL, bam_dmux_pc_irq,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT, NULL, dmux);
	if (ret)
		return ret;

	/* Get initial state */
	ret = irq_get_irqchip_state(pc_irq, IRQCHIP_STATE_LINE_LEVEL, &dmux->pc_state);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, pc_ack_irq, NULL, bam_dmux_pc_ack_irq,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT, NULL, dmux);
	if (ret)
		return ret;

	/* Did remote finish initialization before us? */
	if (dmux->pc_state) {
		if (bam_dmux_power_on(dmux)) {
			bam_dmux_pc_ack(dmux);
			complete_all(&dmux->pc_completion);
		} else {
			bam_dmux_power_off(dmux);
		}
	}

	return 0;
}

static const struct dev_pm_ops bam_dmux_pm_ops = {
	SET_RUNTIME_PM_OPS(bam_dmux_runtime_suspend, bam_dmux_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static const struct of_device_id bam_dmux_of_match[] = {
	{ .compatible = "qcom,bam-dmux" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bam_dmux_of_match);

static struct platform_driver bam_dmux_driver = {
	.probe = bam_dmux_probe,
	.driver = {
		.name = "bam-dmux",
		.pm = &bam_dmux_pm_ops,
		.of_match_table = bam_dmux_of_match,
	},
};
module_platform_driver(bam_dmux_driver);

MODULE_DESCRIPTION("QCOM BAM DMUX ethernet/IP driver");
MODULE_LICENSE("GPL v2");
