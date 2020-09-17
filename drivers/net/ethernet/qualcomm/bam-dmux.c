#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/qcom/smem_state.h>

#define BAM_DMUX_AUTOSUSPEND_DELAY	1000
#define BAM_DMUX_UL_WAKEUP_TIMEOUT	msecs_to_jiffies(2000)
#define BAM_DMUX_BUFFER_SIZE		SZ_2K
#define BAM_DMUX_NUM_DESC		32

struct bam_mux_hdr {
	uint16_t magic_num;
	uint8_t signal;
	uint8_t cmd;
	uint8_t pad_len;
	uint8_t ch_id;
	uint16_t pkt_len;
};

struct bam_dmux_dma_desc {
	struct bam_dmux *dmux;
	dma_addr_t dma_addr;
	struct bam_mux_hdr *hdr;
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

static void bam_dmux_rx_callback(void *data)
{
	struct bam_dmux_dma_desc *desc = data;
	struct bam_mux_hdr *hdr = desc->hdr;

	dev_err(desc->dmux->dev, "callback: magic: %#x, signal: %#x, cmd: %d, pad: %d, ch: %d, len: %d\n",
		hdr->magic_num, hdr->signal, hdr->cmd, hdr->pad_len, hdr->ch_id, hdr->pkt_len);
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
