 /*Simple synchronous userspace interface to SPI devices
  *
  * Copyright (C) 2006 SWAPP
  *     Andrea Paterniani <a.paterniani@swapp-eng.it>
  * Copyright (C) 2007 David Brownell (simplification, cleanup)
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program; if not, write to the Free Software
  * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
  */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>

#include "gf_spi.h"

#if defined(USE_SPI_BUS)
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#elif defined(USE_PLATFORM_BUS)
#include <linux/platform_device.h>
#endif

#define GF_SPIDEV_NAME     "goodix,fingerprint"
/*device name after register in charater*/
#define GF_DEV_NAME            "goodix_fp"
#define	GF_INPUT_NAME	    "gf3208"	/*"goodix_fp" */

#define	CHRD_DRIVER_NAME	"goodix_fp_spi"
#define	CLASS_NAME		    "goodix_fp"
#define SPIDEV_MAJOR		225	/* assigned */
#define N_SPI_MINORS		32	/* ... up to 256 */


struct gf_key_map key_map[] =
{
      {  "POWER",  KEY_POWER  },
      {  "HOME" ,  KEY_HOME   },
      {  "MENU" ,  KEY_MENU   },
      {  "BACK" ,  KEY_BACK   },
      {  "UP"   ,  KEY_UP     },
      {  "DOWN" ,  KEY_DOWN   },
      {  "LEFT" ,  KEY_LEFT   },
      {  "RIGHT",  KEY_RIGHT  },
      {  "FORCE",  KEY_F9     },
      {  "CLICK",  KEY_F3     },
};

/**************************debug******************************/
#define GF_DEBUG
/*#undef  GF_DEBUG*/

#ifdef  GF_DEBUG
#define gf_dbg(fmt, args...) do { \
					pr_warn("goodix:" fmt, ##args);\
		} while (0)
#define FUNC_ENTRY()  pr_warn("goodix:%s, entry\n", __func__)
#define FUNC_EXIT()  pr_warn("goodix:%s, exit\n", __func__)
#else
#define gf_dbg(fmt, args...)
#define FUNC_ENTRY()
#define FUNC_EXIT()
#endif

/*Global variables*/
/*static MODE g_mode = GF_IMAGE_MODE;*/
static DECLARE_BITMAP(minors, N_SPI_MINORS);
static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);
static struct gf_dev gf;
static struct class *gf_class;

void gf_free_irq(struct gf_dev *gf_dev)
{
    devm_free_irq(&gf_dev->spi->dev,gf_dev->irq,gf_dev);
    gf_dev->irq_enabled = 1;
}

static void gf_enable_irq(struct gf_dev *gf_dev)
{
	if (!gf_dev->irq_enabled) {
		enable_irq(gf_dev->irq);
		gf_dev->irq_enabled = 1;
	}
}

void gf_disable_irq(struct gf_dev *gf_dev)
{
	if (gf_dev->irq_enabled) {
		gf_dev->irq_enabled = 0;
		disable_irq(gf_dev->irq);
	}
}

#ifdef AP_CONTROL_CLK
static long spi_clk_max_rate(struct clk *clk, unsigned long rate)
{
	long lowest_available, nearest_low, step_size, cur;
	long step_direction = -1;
	long guess = rate;
	int max_steps = 10;

	cur = clk_round_rate(clk, rate);
	if (cur == rate)
		return rate;

	/* if we got here then: cur > rate */
	lowest_available = clk_round_rate(clk, 0);
	if (lowest_available > rate)
		return -EINVAL;

	step_size = (rate - lowest_available) >> 1;
	nearest_low = lowest_available;

	while (max_steps-- && step_size) {
		guess += step_size * step_direction;
		cur = clk_round_rate(clk, guess);

		if ((cur < rate) && (cur > nearest_low))
			nearest_low = cur;
		/*
		 * if we stepped too far, then start stepping in the other
		 * direction with half the step size
		 */
		if (((cur > rate) && (step_direction > 0))
		    || ((cur < rate) && (step_direction < 0))) {
			step_direction = -step_direction;
			step_size >>= 1;
		}
	}
	return nearest_low;
}

static void spi_clock_set(struct gf_dev *gf_dev, int speed)
{
	long rate;
	int rc;

	rate = spi_clk_max_rate(gf_dev->core_clk, speed);
	if (rate < 0) {
		pr_info("goodix %s: no match found for requested clock frequency:%d",
			__func__, speed);
		return;
	}

	rc = clk_set_rate(gf_dev->core_clk, rate);
}

static int gfspi_ioctl_clk_init(struct gf_dev *data)
{
	pr_debug("goodix %s: enter\n", __func__);

	data->clk_enabled = 0;
	data->core_clk = clk_get(&data->spi->dev, "core_clk");
	if (IS_ERR_OR_NULL(data->core_clk)) {
		pr_err("goodix %s: fail to get core_clk\n", __func__);
		return -1;
	}
	data->iface_clk = clk_get(&data->spi->dev, "iface_clk");
	if (IS_ERR_OR_NULL(data->iface_clk)) {
		pr_err("goodix %s: fail to get iface_clk\n", __func__);
		clk_put(data->core_clk);
		data->core_clk = NULL;
		return -2;
	}
	return 0;
}

static int gfspi_ioctl_clk_enable(struct gf_dev *data)
{
	int err;

	pr_debug("goodix %s: enter\n", __func__);

	if (data->clk_enabled)
		return 0;

	err = clk_prepare_enable(data->core_clk);
	if (err) {
		pr_err("goodix %s: fail to enable core_clk\n", __func__);
		return -1;
	}

	err = clk_prepare_enable(data->iface_clk);
	if (err) {
		pr_err("goodix %s: fail to enable iface_clk\n", __func__);
		clk_disable_unprepare(data->core_clk);
		return -2;
	}

	data->clk_enabled = 1;

	return 0;
}

static int gfspi_ioctl_clk_disable(struct gf_dev *data)
{
	pr_debug("goodix %s: enter\n", __func__);

	if (!data->clk_enabled)
		return 0;

	clk_disable_unprepare(data->core_clk);
	clk_disable_unprepare(data->iface_clk);
	data->clk_enabled = 0;

	return 0;
}

static int gfspi_ioctl_clk_uninit(struct gf_dev *data)
{
	pr_debug("goodix %s: enter\n", __func__);

	if (data->clk_enabled)
		gfspi_ioctl_clk_disable(data);

	if (!IS_ERR_OR_NULL(data->core_clk)) {
		clk_put(data->core_clk);
		data->core_clk = NULL;
	}

	if (!IS_ERR_OR_NULL(data->iface_clk)) {
		clk_put(data->iface_clk);
		data->iface_clk = NULL;
	}

	return 0;
}
#endif

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_dev *gf_dev = &gf;
	struct gf_key gf_key = { 0 };
	int retval = 0;
        int i;
#ifdef AP_CONTROL_CLK
	unsigned int speed = 0;
#endif
	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENODEV;

	if (_IOC_DIR(cmd) & _IOC_READ)
		retval =
		    !access_ok(VERIFY_WRITE, (void __user *)arg,
			       _IOC_SIZE(cmd));
	if ((retval == 0) && (_IOC_DIR(cmd) & _IOC_WRITE))
		retval =
		    !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (retval)
		return -EFAULT;

    if(gf_dev->device_available == 0)
    {
        if((cmd == GF_IOC_POWER_ON) || (cmd == GF_IOC_POWER_OFF)||(cmd == GF_IOC_ENABLE_GPIO))
        {
            pr_info("goodix power cmd\n");
        }
        else
        {
            pr_info("goodix Sensor is power off currently. \n");
            return -ENODEV;
        }
    }
	switch (cmd) {
        case GF_IOC_ENABLE_GPIO:
            gf_dbg("ioctl cmd = GF_IOC_ENABLE_GPIO\n");
            break;
        case GF_IOC_RELEASE_GPIO:
            gf_cleanup(gf_dev);
            gf_dbg("ioctl cmd = GF_IOC_RELEASE_GPIO\n");
            break;
        case GF_IOC_DISABLE_IRQ:
            gf_disable_irq(gf_dev);
            gf_dbg("ioctl cmd = GF_IOC_DISABLE_IRQ\n");
            break;
        case GF_IOC_ENABLE_IRQ:
            gf_enable_irq(gf_dev);
            gf_dbg("ioctl cmd = GF_IOC_ENABLE_IRQ\n");
            break;
        case GF_IOC_SETSPEED:
        #ifdef AP_CONTROL_CLK
        retval = __get_user(speed, (u32 __user *) arg);
        if (retval == 0) {
            if (speed > 12 * 1000 * 1000) {
                pr_warn("goodix Set speed:%d is larger than 12Mbps.\n", speed);
                } else {
                spi_clock_set(gf_dev, speed);
                }
        } else {
        pr_warn("goodix Failed to get speed from user. retval = %d\n",	retval);
        }
        #endif
		break;
	case GF_IOC_RESET:
        gf_hw_reset(gf_dev, 3);
        gf_dbg("ioctl cmd = GF_IOC_RESET\n");
        break;
	case GF_IOC_COOLBOOT:
        gf_dbg("ioctl cmd = GF_IOC_COOLBOOT\n");
		gf_power_off(gf_dev);
		mdelay(5);
		gf_power_on(gf_dev);
		break;
	case GF_IOC_SENDKEY:
        gf_dbg("ioctl cmd = GF_IOC_SENDKEY\n");
		if (copy_from_user
		    (&gf_key, (struct gf_key *)arg, sizeof(struct gf_key))) {
			retval = -EFAULT;
			break;
		}

                for(i = 0; i< ARRAY_SIZE(key_map); i++) {
                    if(key_map[i].val == gf_key.key){
                        input_report_key(gf_dev->input, gf_key.key, gf_key.value);
                        input_sync(gf_dev->input);
                        break;
                    }
                }

                if(i == ARRAY_SIZE(key_map)) {
                    retval = -EFAULT;
                }

		break;
    case GF_IOC_CLK_READY:
        #ifdef AP_CONTROL_CLK
        gfspi_ioctl_clk_enable(gf_dev);
        #endif
		break;
    case GF_IOC_CLK_UNREADY:
        #ifdef AP_CONTROL_CLK
        gfspi_ioctl_clk_disable(gf_dev);
        #endif
        break;
	case GF_IOC_PM_FBCABCK:
        gf_dbg("ioctl cmd = GF_IOC_PM_FBCABCK\n");
		__put_user(gf_dev->fb_black, (u8 __user *) arg);
		break;
    case GF_IOC_POWER_ON:
        gf_dbg("ioctl cmd = GF_IOC_POWER_ON\n");
        if(gf_dev->device_available == 1)
            pr_info("goodix Sensor has already powered-on.\n");
        else
            gf_power_on(gf_dev);
        gf_dev->device_available = 1;
        break;
    case GF_IOC_POWER_OFF:
        gf_dbg("ioctl cmd = GF_IOC_POWER_OFF\n");
        if(gf_dev->device_available == 0)
            pr_info("goodix Sensor has already powered-off.\n");
        else
            gf_power_off(gf_dev);
        gf_dev->device_available = 0;
        break;
	default:
		gf_dbg("goodix Unsupport cmd:0x%x\n", cmd);
		break;
	}
	return retval;
}

#ifdef CONFIG_COMPAT
static long
gf_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif /*CONFIG_COMPAT*/

static irqreturn_t gf_irq(int irq, void *handle)
{

#if defined(GF_NETLINK_ENABLE)
	struct gf_dev *gf_dev = &gf;
	char temp = GF_NET_EVENT_IRQ;
	wake_lock_timeout(&gf_dev->ttw_wl, msecs_to_jiffies(1000));
	sendnlmsg(&temp);
    pr_warn("goodix:%s irq happen\n", __func__);
#elif defined (GF_FASYNC)

	struct gf_dev *gf_dev = &gf;
	if (gf_dev->async)
		kill_fasync(&gf_dev->async, SIGIO, POLL_IN);
#endif

	return IRQ_HANDLED;

}

static int driver_init_partial(struct gf_dev *gf_dev)
{
	int ret = 0;
	FUNC_ENTRY();
	gf_dev->device_available = 1;
	if (gf_parse_dts(gf_dev))
		goto error;
	gf_dev->irq = gf_irq_num(gf_dev);
	ret = devm_request_threaded_irq(&gf_dev->spi->dev,
					gf_dev->irq,
					NULL,
					gf_irq,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"gf",gf_dev);
	if(ret){
		pr_err("Could not request irq %d\n",gpio_to_irq(gf_dev->irq_gpio));
		goto error;
	}
	FUNC_EXIT();
	return 0;

error:

	gf_cleanup(gf_dev);
	gf_dev->device_available = 0;
	return -1;


}


static int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev;
	int status = -ENXIO;

	FUNC_ENTRY();
	mutex_lock(&device_list_lock);

	list_for_each_entry(gf_dev, &device_list, device_entry) {
		if (gf_dev->devt == inode->i_rdev) {
			gf_dbg("Found\n");
			status = 0;
			break;
		}
	}

	if (status == 0) {
            driver_init_partial(gf_dev);
			gf_dev->users++;
			filp->private_data = gf_dev;
			nonseekable_open(inode, filp);
			gf_dbg("Succeed to open device. irq = %d\n",
							gf_dev->irq);
			if (gf_dev->users == 1)
				gf_enable_irq(gf_dev);
			gf_power_on(gf_dev);
			gf_hw_reset(gf_dev, 360);
	} else {
		gf_dbg("goodix No device for minor %d\n", iminor(inode));
	}
	mutex_unlock(&device_list_lock);
	FUNC_EXIT();
	return status;
}

#ifdef GF_FASYNC
static int gf_fasync(int fd, struct file *filp, int mode)
{
	struct gf_dev *gf_dev = filp->private_data;
	int ret;

	FUNC_ENTRY();
	ret = fasync_helper(fd, filp, mode, &gf_dev->async);
	FUNC_EXIT();
	return ret;
}
#endif

static int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev;
	int status = 0;

	FUNC_ENTRY();
	mutex_lock(&device_list_lock);
	gf_dev = filp->private_data;
	filp->private_data = NULL;

	gf_dev->users--;
	if (!gf_dev->users) {
    /*power off the sensor*/
    gf_dev->device_available = 0;
        gf_power_off(gf_dev);
	}
	gf_cleanup(gf_dev);
	mutex_unlock(&device_list_lock);
	FUNC_EXIT();

	return status;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	/* REVISIT switch to aio primitives, so that userspace
	 * gets more complete API coverage.  It'll simplify things
	 * too, except for the locking.
	 */
	.unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gf_compat_ioctl,
#endif /*CONFIG_COMPAT*/
	.open = gf_open,
	.release = gf_release,
#ifdef GF_FASYNC
	.fasync = gf_fasync,
#endif
};

static int goodix_fb_state_chg_callback(struct notifier_block *nb,
					unsigned long val, void *data)
{
	struct gf_dev *gf_dev;
	struct fb_event *evdata = data;
	unsigned int blank;

#if defined(GF_NETLINK_ENABLE)
        char temp = 0;
#endif

	if (val != FB_EARLY_EVENT_BLANK)
		return 0;
	gf_dev = container_of(nb, struct gf_dev, notifier);
	if (evdata && evdata->data && val == FB_EARLY_EVENT_BLANK && gf_dev) {
		blank = *(int *)(evdata->data);
		switch (blank) {
		case FB_BLANK_POWERDOWN:
			if (gf_dev->device_available == 1) {
				gf_dev->fb_black = 1;
#if defined(GF_NETLINK_ENABLE)
                                temp = GF_NET_EVENT_FB_BLACK;
                                sendnlmsg(&temp);
#elif defined (GF_FASYNC)
                                if (gf_dev->async) {
                                    kill_fasync(&gf_dev->async, SIGIO,
                                            POLL_IN);
                                }
#endif
                        }
                        break;
                case FB_BLANK_UNBLANK:
                        if (gf_dev->device_available == 1) {
                            gf_dev->fb_black = 0;
#if defined(GF_NETLINK_ENABLE)
                            temp = GF_NET_EVENT_FB_UNBLACK;
                            sendnlmsg(&temp);
#elif defined (GF_FASYNC)
				if (gf_dev->async) {
					kill_fasync(&gf_dev->async, SIGIO,
						    POLL_IN);
				}
#endif
			}
			break;
		default:
			pr_info("goodix %s defalut\n", __func__);
			break;
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block goodix_noti_block = {
	.notifier_call = goodix_fb_state_chg_callback,
};

static void gf_reg_key_kernel(struct gf_dev *gf_dev)
{
    int i;

    set_bit(EV_KEY, gf_dev->input->evbit); //tell the kernel is key event
    for(i = 0; i< ARRAY_SIZE(key_map); i++) {
        set_bit(key_map[i].val, gf_dev->input->keybit);
    }

    gf_dev->input->name = GF_INPUT_NAME;
    if (input_register_device(gf_dev->input))
        pr_warn("goodix Failed to register GF as input device.\n");
}


#if defined(USE_SPI_BUS)
static int gf_probe(struct spi_device *spi)
#elif defined(USE_PLATFORM_BUS)
static int gf_probe(struct platform_device *pdev)
#endif
{
	struct gf_dev *gf_dev = &gf;
	int status = -EINVAL;
	unsigned long minor;
	FUNC_ENTRY();
	/* Initialize the driver data */
	INIT_LIST_HEAD(&gf_dev->device_entry);

#if defined(USE_SPI_BUS)
	gf_dev->spi = spi;
#elif defined(USE_PLATFORM_BUS)
	gf_dev->spi = pdev;
#endif

	gf_dev->irq_gpio 		= 	-EINVAL;
	gf_dev->reset_gpio 	= 	-EINVAL;
	gf_dev->pwr_gpio 	= 	-EINVAL;
	gf_dev->device_available =  0;
	gf_dev->fb_black  =  0;
	gf_dev->irq_enabled = 1;//modified from 0 to 1 for test lijun
	gf_dev->fingerprint_pinctrl = NULL;

	/* If we can allocate a minor number, hook up this device.
	 * Reusing minors is fine so long as udev or mdev is working.
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		struct device *dev;

		gf_dev->devt = MKDEV(SPIDEV_MAJOR, minor);
		dev = device_create(gf_class, &gf_dev->spi->dev, gf_dev->devt,
				    gf_dev, GF_DEV_NAME);
		status = IS_ERR(dev) ? PTR_ERR(dev) : 0;
	} else {
		dev_dbg(&gf_dev->spi->dev, "goodix no minor number available!\n");
		status = -ENODEV;
	}

	if (status == 0) {
		set_bit(minor, minors);
		list_add(&gf_dev->device_entry, &device_list);
	} else {
		gf_dev->devt = 0;
	}
	mutex_unlock(&device_list_lock);

	if (status == 0) {
		/*input device subsystem */
		gf_dev->input = input_allocate_device();
		if (gf_dev->input == NULL) {
			dev_dbg(&gf_dev->input->dev,
				"goodix Faile to allocate input device.\n");
			status = -ENOMEM;
			goto error;  //add for test at 20160310 by davy
		}
#ifdef AP_CONTROL_CLK
		pr_info("goodix Get the clk resource.\n");
		/* Enable spi clock */
		if (gfspi_ioctl_clk_init(gf_dev))
			goto gfspi_probe_clk_init_failed;


		if (gfspi_ioctl_clk_enable(gf_dev))
			goto gfspi_probe_clk_enable_failed;

		spi_clock_set(gf_dev, 4.8*1000*1000);
#endif

		gf_dev->notifier = goodix_noti_block;
		fb_register_client(&gf_dev->notifier);
		gf_reg_key_kernel(gf_dev);

		wake_lock_init(&gf_dev->ttw_wl, WAKE_LOCK_SUSPEND, "goodix_ttw_wl");//add by davy
	}

	pr_warn("goodix --------gf_probe end---OK.--------\n");
	return status;

error:
	gf_dev->device_available = 0;
	if (gf_dev->devt != 0) {
		pr_info("Err: status = %d\n", status);
		mutex_lock(&device_list_lock);
		list_del(&gf_dev->device_entry);
		device_destroy(gf_class, gf_dev->devt);
		clear_bit(MINOR(gf_dev->devt), minors);
		mutex_unlock(&device_list_lock);

#ifdef AP_CONTROL_CLK
gfspi_probe_clk_enable_failed:
		gfspi_ioctl_clk_uninit(gf_dev);
gfspi_probe_clk_init_failed:
#endif
		if (gf_dev->input != NULL)
			input_unregister_device(gf_dev->input);
	}

	FUNC_EXIT();
	return status;
}

/*static int __devexit gf_remove(struct spi_device *spi)*/
#if defined(USE_SPI_BUS)
static int gf_remove(struct spi_device *spi)
#elif defined(USE_PLATFORM_BUS)
static int gf_remove(struct platform_device *pdev)
#endif
{
	struct gf_dev *gf_dev = &gf;
	FUNC_ENTRY();

	/* make sure ops on existing fds can abort cleanly */
	if (gf_dev->irq)
		free_irq(gf_dev->irq, gf_dev);

	if (gf_dev->input != NULL)
		input_unregister_device(gf_dev->input);
		input_free_device(gf_dev->input);

	/* prevent new opens */
	mutex_lock(&device_list_lock);
	list_del(&gf_dev->device_entry);
	device_destroy(gf_class, gf_dev->devt);
	clear_bit(MINOR(gf_dev->devt), minors);
	if (gf_dev->users == 0)
		kfree(gf_dev);

        mutex_unlock(&device_list_lock);

	wake_lock_destroy(&gf_dev->ttw_wl);//add by davy

	FUNC_EXIT();
	return 0;
}

#if defined(USE_SPI_BUS)
static int gf_suspend(struct spi_device *spi, pm_message_t mesg)
#elif defined(USE_PLATFORM_BUS)
static int gf_suspend(struct platform_device *pdev, pm_message_t state)
#endif
{
    struct gf_dev *gf_dev = &gf;
    enable_irq_wake(gf_dev->irq);
    gf_dbg( "goodix FP suspend.\n");
	return 0;
}

#if defined(USE_SPI_BUS)
static int gf_resume(struct spi_device *spi)
#elif defined(USE_PLATFORM_BUS)
static int gf_resume(struct platform_device *pdev)
#endif
{
    struct gf_dev *gf_dev = &gf;
    disable_irq_wake(gf_dev->irq);
	gf_dbg( "goodix FP resume.\n");
	return 0;
}

static struct of_device_id gx_match_table[] = {
	{.compatible = GF_SPIDEV_NAME,},
	{},
};

#if defined(USE_SPI_BUS)
static struct spi_driver gf_driver = {
#elif defined(USE_PLATFORM_BUS)
static struct platform_driver gf_driver = {
#endif
	.driver = {
		   .name = GF_DEV_NAME,
		   .owner = THIS_MODULE,
#if defined(USE_SPI_BUS)
		  //.bus    = &spi_bus_type,
#endif
		   .of_match_table = gx_match_table,
		   },
	.probe = gf_probe,
	.remove = gf_remove,
	.suspend = gf_suspend,
	.resume = gf_resume,
};

static int __init gf_init(void)
{
	int status;
	FUNC_ENTRY();

	pr_warn("goodix ---------gf_init start.--------\n");
	/* Claim our 256 reserved device numbers.  Then register a class
	 * that will key udev/mdev to add/remove /dev nodes.  Last, register
	 * the driver which manages those device numbers.
	 */

	BUILD_BUG_ON(N_SPI_MINORS > 256);
	status = register_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME, &gf_fops);
	if (status < 0) {
		pr_warn("goodix Failed to register char device!\n");
		FUNC_EXIT();
		return status;
	}
	gf_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(gf_class)) {
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
		pr_warn("goodix Failed to create class.\n");
		FUNC_EXIT();
		return PTR_ERR(gf_class);
	}
#if defined(USE_PLATFORM_BUS)
	status = platform_driver_register(&gf_driver);
#elif defined(USE_SPI_BUS)
	status = spi_register_driver(&gf_driver);
#endif
	if (status < 0) {
		class_destroy(gf_class);
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
		pr_warn("goodix Failed to register SPI driver.\n");
	}

#ifdef GF_NETLINK_ENABLE
        netlink_init();
#endif
	pr_info("goodix  status = 0x%x\n", status);
	FUNC_EXIT();

	pr_warn("goodix --------gf_init end---OK.--------\n");
	return 0;		//status;
}

module_init(gf_init);

static void __exit gf_exit(void)
{
	FUNC_ENTRY();
#ifdef GF_NETLINK_ENABLE
        netlink_exit();
#endif
#if defined(USE_PLATFORM_BUS)
	platform_driver_unregister(&gf_driver);
#elif defined(USE_SPI_BUS)
	spi_unregister_driver(&gf_driver);
#endif
	class_destroy(gf_class);
	unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
	FUNC_EXIT();
}

module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_DESCRIPTION("User mode SPI device interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:gf-spi");