// SPDX-License-Identifier: GPL-2.0-only
/* 
 * aw8738.c -- AW8738 ALSA SoC Codec driver
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>

static const struct snd_kcontrol_new spkr_switch = 
	SOC_DAPM_SINGLE_VIRT("Switch", 1);

struct aw8738_priv {
	struct gpio_desc *enable_gpio;
	unsigned int sequence;
};

static int aw8738_spkmode_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *comp = snd_soc_dapm_to_component(w->dapm);
	struct aw8738_priv *aw8738 = snd_soc_component_get_drvdata(comp);
	int i;

	if (!aw8738->enable_gpio)
		return 0;

	if (event & SND_SOC_DAPM_POST_PMU)
		for (i = 0; i < aw8738->sequence; i++){
			gpiod_set_value_cansleep(aw8738->enable_gpio, 0);
			udelay(2);
			gpiod_set_value_cansleep(aw8738->enable_gpio, 1);
			udelay(2);
		}
	else if (event & SND_SOC_DAPM_PRE_PMD)
		gpiod_set_value_cansleep(aw8738->enable_gpio, 0);

	udelay(300);
	return 0;
}

static const struct snd_soc_dapm_widget aw8738_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("SIN"),
	SND_SOC_DAPM_SWITCH("Speaker Playback", SND_SOC_NOPM, 0, 0,
			    &spkr_switch),
	SND_SOC_DAPM_OUT_DRV_E("DRV", SND_SOC_NOPM, 0, 0, NULL, 0,
			       aw8738_spkmode_event,
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("SOUT"),
};

static const struct snd_soc_dapm_route aw8738_dapm_routes[] = {
	{ "Speaker Playback", "Switch", "SIN" },
	{ "DRV", NULL, "Speaker Playback" },
	{ "SOUT", NULL, "DRV" },
};

static const struct snd_soc_component_driver aw8738_component_driver = {
	.dapm_widgets = aw8738_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aw8738_dapm_widgets),
	.dapm_routes = aw8738_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(aw8738_dapm_routes),
};

static struct snd_soc_dai_driver aw8738_dai_driver = {
	.name = "HiFi",
	.playback = {
		.stream_name = "PDM Playback",
	},
};

static int aw8738_platform_probe(struct platform_device *pdev)
{
	struct aw8738_priv *aw8738;
	int ret;

	aw8738 = devm_kzalloc(&pdev->dev, sizeof(*aw8738), GFP_KERNEL);
	if (!aw8738)
		return -ENOMEM;

	aw8738->enable_gpio = devm_gpiod_get_optional(&pdev->dev, "enable",
						      GPIOD_OUT_LOW);
	if (IS_ERR(aw8738->enable_gpio))
		return PTR_ERR(aw8738->enable_gpio);

	ret = device_property_read_u32(&pdev->dev, "aw8738,enable-sequence",
				       &aw8738->sequence);
	if (ret) {
		aw8738->sequence = 5;
		dev_dbg(&pdev->dev, "no sequence found, default: MODE 5\n");
	}

	dev_set_drvdata(&pdev->dev, aw8738);

	return devm_snd_soc_register_component(&pdev->dev,
					       &aw8738_component_driver,
					       &aw8738_dai_driver, 1);
}

#ifdef CONFIG_OF
static const struct of_device_id aw8738_device_id[] = {
	{ .compatible = "awinic,aw8738" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8738_device_id);
#endif

static struct platform_driver aw8738_platform_driver = {
	.driver = {
		.name = "aw8738",
		.of_match_table = of_match_ptr(aw8738_device_id),
	},
	.probe	= aw8738_platform_probe,
};
module_platform_driver(aw8738_platform_driver);

MODULE_DESCRIPTION("Awinic AW8738 Codec Driver");
MODULE_LICENSE("GPL v2");
