/*
 * Copyright (C) 2008 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/leds.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/slab.h>
#include <mach/msm_fb.h>
#include <mach/msm_panel.h>

#include "mddi_client_eid.h"

static DECLARE_WAIT_QUEUE_HEAD(s6d04d1_vsync_wait);

#define INTERVAL_ADJUSTING	300

struct panel_info {
	struct msm_mddi_client_data *client_data;
	struct platform_device pdev;
	struct msm_panel_data panel_data;
	struct msmfb_callback *fb_callback;
	struct work_struct panel_work;
	struct work_struct adjust_panel_work;
	struct workqueue_struct *fb_wq;
	struct wake_lock idle_lock;
	atomic_t frame_counter;
	int s6d04d1_got_int;
};

static struct cabc_config cabc_config;
static struct platform_device mddi_samsung_cabc = {
        .name = "samsung_cabc",
        .id = 0,
        .dev = {
                .platform_data = &cabc_config,
        }
};

static void s6d04d1_adjust_work(struct work_struct *work){
	struct panel_info * panel = container_of(work, struct panel_info, adjust_panel_work);
	struct msm_mddi_client_data *client_data = panel->client_data;
	struct msm_mddi_bridge_platform_data *bridge_data =
		client_data->private_client_data;

	if(bridge_data->adjust)
		bridge_data->adjust(client_data);

	atomic_set(&panel->frame_counter, INTERVAL_ADJUSTING);
}

static void s6d04d1_request_vsync(struct msm_panel_data *panel_data,
		      struct msmfb_callback *callback)
{
	struct panel_info *panel = container_of(panel_data, struct panel_info,
						panel_data);
	struct msm_mddi_client_data *client_data = panel->client_data;

	panel->fb_callback = callback;
	if (panel->s6d04d1_got_int) {
		panel->s6d04d1_got_int = 0;
		client_data->activate_link(client_data); /* clears interrupt */
	}
}

static void s6d04d1_wait_vsync(struct msm_panel_data *panel_data)
{
	struct panel_info *panel = container_of(panel_data, struct panel_info,
						panel_data);
	struct msm_mddi_client_data *client_data = panel->client_data;

	if (panel->s6d04d1_got_int) {
		panel->s6d04d1_got_int = 0;
		client_data->activate_link(client_data); /* clears interrupt */
	}

	if (wait_event_timeout(s6d04d1_vsync_wait, panel->s6d04d1_got_int,
				HZ/2) == 0)
		printk(KERN_ERR "timeout waiting for VSYNC\n");

	panel->s6d04d1_got_int = 0;
	/* interrupt clears when screen dma starts */
}

static int s6d04d1_suspend(struct msm_panel_data *panel_data)
{
	struct panel_info *panel = container_of(panel_data, struct panel_info,
						panel_data);
	struct msm_mddi_client_data *client_data = panel->client_data;
	struct msm_mddi_bridge_platform_data *bridge_data =
		client_data->private_client_data;
	int ret;

	ret = bridge_data->uninit(bridge_data, client_data);
	if (ret) {
		printk(KERN_INFO "mddi s6d04d1 client: non zero return from "
			"uninit\n");
		return ret;
	}
	client_data->suspend(client_data);
	return 0;
}

static int s6d04d1_resume(struct msm_panel_data *panel_data)
{
	struct panel_info *panel = container_of(panel_data, struct panel_info,
						panel_data);
	struct msm_mddi_client_data *client_data = panel->client_data;
	struct msm_mddi_bridge_platform_data *bridge_data =
		client_data->private_client_data;
	int ret;

	client_data->resume(client_data);
	ret = bridge_data->init(bridge_data, client_data);
	if (ret)
		return ret;
	return 0;
}

static int s6d04d1_blank(struct msm_panel_data *panel_data)
{
	struct panel_info *panel = container_of(panel_data, struct panel_info,
						panel_data);
	struct msm_mddi_client_data *client_data = panel->client_data;
	struct msm_mddi_bridge_platform_data *bridge_data =
		client_data->private_client_data;

        if (cabc_config.bl_handle)
                cabc_config.bl_handle(&mddi_samsung_cabc, LED_OFF);

	return bridge_data->blank(bridge_data, client_data);
}

static int s6d04d1_unblank(struct msm_panel_data *panel_data)
{
	struct panel_info *panel = container_of(panel_data, struct panel_info,
						panel_data);
	struct msm_mddi_client_data *client_data = panel->client_data;
	struct msm_mddi_bridge_platform_data *bridge_data =
		client_data->private_client_data;

        if (cabc_config.bl_handle) {
                mdelay(40);
                cabc_config.bl_handle(&mddi_samsung_cabc, LED_FULL);
        }

	return bridge_data->unblank(bridge_data, client_data);
}

static int s6d04d1_shutdown(struct msm_panel_data *panel_data)
{
	struct panel_info *panel = container_of(panel_data, struct panel_info,
						panel_data);
	struct msm_mddi_client_data *client_data = panel->client_data;
	struct msm_mddi_bridge_platform_data *bridge_data =
		client_data->private_client_data;

	pr_debug("%s\n", __func__);
	if(bridge_data->shutdown)
		return bridge_data->shutdown(bridge_data, client_data);

	return 0;
}

irqreturn_t s6d04d1_vsync_interrupt(int irq, void *data)
{
	struct panel_info *panel = data;
	panel->s6d04d1_got_int = 1;

	if (atomic_dec_and_test(&panel->frame_counter)) {
		schedule_work(&panel->adjust_panel_work);
		return IRQ_HANDLED;
	}

	if (panel->fb_callback && atomic_read(&panel->frame_counter) > 0) {
		panel->fb_callback->func(panel->fb_callback);
		panel->fb_callback = NULL;
	}
	wake_up(&s6d04d1_vsync_wait);

	return IRQ_HANDLED;
}

static int setup_vsync(struct panel_info *panel, int init)
{
	int ret;
	int gpio = 97;
	unsigned int irq;

	if (!init) {
		ret = 0;
		goto uninit;
	}
	ret = gpio_request(gpio, "vsync");
	if (ret)
		goto err_request_gpio_failed;

	ret = gpio_direction_input(gpio);
	if (ret)
		goto err_gpio_direction_input_failed;

	ret = irq = gpio_to_irq(gpio);
	if (ret < 0)
		goto err_get_irq_num_failed;

	ret = request_irq(irq, s6d04d1_vsync_interrupt, IRQF_TRIGGER_RISING,
			  "vsync", panel);
	if (ret)
		goto err_request_irq_failed;

	printk(KERN_INFO "vsync on gpio %d now %d\n",
	       gpio, gpio_get_value(gpio));
	return 0;

uninit:
	free_irq(gpio_to_irq(gpio), panel->client_data);
err_request_irq_failed:
err_get_irq_num_failed:
err_gpio_direction_input_failed:
	gpio_free(gpio);
err_request_gpio_failed:
	return ret;
}

static int mddi_s6d04d1_probe(struct platform_device *pdev)
{
	struct msm_mddi_client_data *client_data = pdev->dev.platform_data;
	struct msm_mddi_bridge_platform_data *bridge_data =
		client_data->private_client_data;
	struct panel_data *panel_data = &bridge_data->panel_conf;
	int ret;

	struct panel_info *panel = kzalloc(sizeof(struct panel_info),
					   GFP_KERNEL);

	printk(KERN_DEBUG "%s: enter.\n", __func__);

	if (!panel)
		return -ENOMEM;
	platform_set_drvdata(pdev, panel);

	ret = setup_vsync(panel, 1);
	if (ret) {
		dev_err(&pdev->dev, "mddi_s6d04d1_setup_vsync failed\n");
		return ret;
	}

	cabc_config.pwm_data = panel_data->pwm;
	cabc_config.min_level = panel_data->min_level;
	cabc_config.shrink = panel_data->shrink;
	cabc_config.shrink_br = panel_data->shrink_br;
	cabc_config.change_cabcmode = panel_data->change_cabcmode;
	if (panel_data->caps & MSMFB_CAP_CABC) {
		printk(KERN_INFO "CABC enabled\n");
		cabc_config.client = client_data;
		platform_device_register(&mddi_samsung_cabc);
	}

	panel->client_data = client_data;
	panel->panel_data.suspend = s6d04d1_suspend;
	panel->panel_data.resume = s6d04d1_resume;
	panel->panel_data.wait_vsync = s6d04d1_wait_vsync;
	panel->panel_data.request_vsync = s6d04d1_request_vsync;
	panel->panel_data.blank = s6d04d1_blank;
	panel->panel_data.unblank = s6d04d1_unblank;
	panel->panel_data.shutdown = s6d04d1_shutdown;
	panel->panel_data.fb_data = &bridge_data->fb_data;
	panel->panel_data.caps = 0;

	panel->pdev.name = "msm_panel";
	panel->pdev.id = pdev->id;
	panel->pdev.resource = client_data->fb_resource;
	panel->pdev.num_resources = 1;
	panel->pdev.dev.platform_data = &panel->panel_data;

	platform_device_register(&panel->pdev);
	wake_lock_init(&panel->idle_lock, WAKE_LOCK_IDLE, "s6d04d1_idle_lock");

	INIT_WORK(&panel->adjust_panel_work, s6d04d1_adjust_work);
	atomic_set(&panel->frame_counter, INTERVAL_ADJUSTING);

	return 0;
}

static int mddi_s6d04d1_remove(struct platform_device *pdev)
{
	struct panel_info *panel = platform_get_drvdata(pdev);

	setup_vsync(panel, 0);
	kfree(panel);
	return 0;
}

static struct platform_driver mddi_client_0101_04d1 = {
	.probe = mddi_s6d04d1_probe,
	.remove = mddi_s6d04d1_remove,
	.driver = { .name = "mddi_c_0101_04d1" },
};

static int __init mddi_client_s6d04d1_init(void)
{
	return platform_driver_register(&mddi_client_0101_04d1);
}

module_init(mddi_client_s6d04d1_init);
