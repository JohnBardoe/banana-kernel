// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <linux/of_clk.h>
#include <linux/slab.h>

struct dumb {
        struct device *dev;
        struct clk **clks;
        int clk_count;

        struct delayed_work test_work;
        bool enabled;
};

static int dumb_clocks_get(struct dumb *par, struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct clk *clock;
	int i;

	if (dev_get_platdata(&pdev->dev) || !np)
		return 0;

	par->clk_count = of_clk_get_parent_count(np);
	if (!par->clk_count)
		return 0;

	par->clks = kcalloc(par->clk_count, sizeof(struct clk *), GFP_KERNEL);
	if (!par->clks)
		return -ENOMEM;

	for (i = 0; i < par->clk_count; i++) {
		clock = of_clk_get(np, i);
		if (IS_ERR(clock)) {
			if (PTR_ERR(clock) == -EPROBE_DEFER) {
				while (--i >= 0) {
					if (par->clks[i])
						clk_put(par->clks[i]);
				}
				kfree(par->clks);
				return -EPROBE_DEFER;
			}
			dev_err(&pdev->dev, "%s: clock %d not found: %ld\n",
				__func__, i, PTR_ERR(clock));
			continue;
		}
		par->clks[i] = clock;
	}

	return 0;
}

static void dumb_test_work(struct work_struct *work)
{
	struct dumb *d = container_of(work, struct dumb, test_work.work);
        int i;

        if (d->enabled) {
                dev_err(d->dev, "Disabling clocks! RIP\n");
        } else {
                dev_err(d->dev, "Enabling clocks again! :)\n");
        }

        for (i = 0; i < d->clk_count; ++i) {
                if (d->enabled) {
                        clk_disable_unprepare(d->clks[i]);
                } else {
                        clk_prepare_enable(d->clks[i]);
                }
        }

        d->enabled = !d->enabled;

        // Toggle in 10 seconds
	schedule_delayed_work(&d->test_work, msecs_to_jiffies(10 * 1000));
}

static int dumb_probe(struct platform_device *pdev)
{
        struct device *dev = &pdev->dev;
        struct dumb *d;
        int ret;

        dev_err(dev, "Hello, I'm dumb!\n");

        d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
        if (!d)
                return -ENOMEM;

        d->dev = dev;

        ret = dumb_clocks_get(d, pdev);
        if (ret) {
                dev_err(dev, "Failed to get clocks: %d\n", ret);
                return ret;
        }

        INIT_DELAYED_WORK(&d->test_work, dumb_test_work);
        dumb_test_work(&d->test_work.work); // Enable clocks

        return 0;
}

static const struct of_device_id dumb_match_table[] = {
	{ .compatible = "minecrell,dumb-mdss-clk", },
	{ }
};

static struct platform_driver dumb_driver = {
	.probe = dumb_probe,
	.driver = {
		.name = "dumb-mdss-clk",
		.of_match_table = dumb_match_table,
	},
};
builtin_platform_driver(dumb_driver);
