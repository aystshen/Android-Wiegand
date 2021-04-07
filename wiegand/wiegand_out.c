/*
 * Copyright 2021 Bob Shen.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/wakelock.h>
#include <linux/clk.h>
#include <linux/syscalls.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/gpio.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/unistd.h>
#include <linux/of_platform.h>

#define WIEGANDOUTDRV_LIB_VERSION    "1.0.0"

#define WIEGAND_DEIVCE_NAME         "wiegand_out"

#define WIEGAND_MODE_26     26 //bit
#define WIEGAND_MODE_34     34 //bit

#define DEF_PULSE_WIDTH     100 //us
#define DEF_PULSE_INTERVAL  1000 //us
#define DEF_DATA_LENGTH     WIEGAND_MODE_26
#define DEVIATION           100 //us

#define PLUSE_WIDTH_STATE   0
#define PLUSE_INTVAL_STATE  1

#define WIEGAND_OUT0_DATA   '0'
#define WIEGAND_OUT1_DATA   '1'

#define MAX_WIEGAND_DATA_LEN 2

/* ioctl command */
#define WIEGAND_IOC_MAGIC  'w'

#define WIEGAND_PULSE_WIDTH     _IOW(WIEGAND_IOC_MAGIC, 1, int)
#define WIEGAND_PULSE_INTERVAL  _IOW(WIEGAND_IOC_MAGIC, 2, int)
#define WIEGAND_FORMAT          _IOW(WIEGAND_IOC_MAGIC, 3, int)
#define WIEGAND_READ            _IOR(WIEGAND_IOC_MAGIC, 4, unsigned int)
#define WIEGAND_WRITE           _IOW(WIEGAND_IOC_MAGIC, 5, unsigned int)
#define WIEGAND_STATUS          _IOR(WIEGAND_IOC_MAGIC, 6, int)

#define WIEGAND_IOC_MAXNR 6


struct wiegand_out_dev {
    struct platform_device  *platform_dev;
    struct device           *dev;
    struct miscdevice       mdev;
    unsigned int            data0_pin;
    unsigned int            data1_pin;
    unsigned int            wiegand_data;
    unsigned int            wiegand_out_data[MAX_WIEGAND_DATA_LEN];
    int                     pos;
    int                     data_length;
    int                     pulse_width; //us
    int                     pulse_intval; //us
    int                     state;
    spinlock_t              lock;
    int                     use_count;
    struct hrtimer          timer;
    wait_queue_head_t       wq;
};

static unsigned char odd_parity_26(unsigned long wg_data)
{
    unsigned char i, even_val = 0;
    unsigned char ret;
    
    for (i = 12; i < 24; i++) {
        if (((wg_data >> i) & 0x01) == 0x01) {
            even_val++;
        }
    }
    if ((even_val % 2) == 0) {
        ret = 1;
    } else {
        ret = 0;
    }

    return ret;
}

static unsigned char even_parity_26(unsigned long wg_data)
{
    unsigned char i, odd_val = 0;
    unsigned char ret;

    for (i = 0; i < 12; i++) {
        if (((wg_data >> i) & 0x01) == 0x01) {
            odd_val++;
        }
    }
    if ((odd_val % 2) == 0) {
        ret = 0;
    } else {
        ret = 1;
    }

    return ret;
}

static unsigned char odd_parity_34(unsigned long long wg_data)
{
    unsigned char i, even_val = 0;
    unsigned char ret;

    for (i = 16; i < 32; i++) {
        if (((wg_data >> i) & 0x01) == 0x01) {
            even_val++;
        }
    }
    if ((even_val % 2) == 0) {
        ret = 1;
    } else {
        ret = 0;
    }

    return ret;
}

static unsigned char even_parity_34(unsigned long long wg_data)
{
    unsigned char i, odd_val = 0;
    unsigned char ret;
    for (i = 0; i < 16; i++) {
        if (((wg_data >> i) & 0x01) == 0x01) {
            odd_val++;
        }
    }
    if ((odd_val % 2) == 0) {
        ret = 0;
    } else {
        ret = 1;
    }

    return ret;
}

static void wiegand_out_data_reset(struct wiegand_out_dev *wiegand_out)
{
    gpio_direction_output(wiegand_out->data0_pin, 1);
    gpio_direction_output(wiegand_out->data1_pin, 1);
}

static void wiegand_out_set_start_state(struct wiegand_out_dev *wiegand_out)
{
    wiegand_out->state = PLUSE_INTVAL_STATE;
}

static void wiegand_out_set_current_state(struct wiegand_out_dev *wiegand_out)
{
    wiegand_out->state = (wiegand_out->state == PLUSE_WIDTH_STATE) ? PLUSE_INTVAL_STATE : PLUSE_WIDTH_STATE;
}

static void wiegand_out_start_pulse_width_timer(struct wiegand_out_dev *wiegand_out)
{
    int us = wiegand_out->pulse_width;
    int s = us / 1000000;
    ktime_t time = ktime_set(s, (us % 1000000) * 1000);
    hrtimer_start(&wiegand_out->timer, time, HRTIMER_MODE_REL);
}

static void wiegand_out_start_pulse_intval_timer(struct wiegand_out_dev *wiegand_out)
{
    int us = wiegand_out->pulse_intval;
    int s = us / 1000000;
    ktime_t time = ktime_set(s, (us % 1000000) * 1000);
    hrtimer_start(&wiegand_out->timer, time, HRTIMER_MODE_REL);
}

static void wiegand_out_start_write(struct wiegand_out_dev *wiegand_out)
{
    wiegand_out->pos = 0;
    wiegand_out_set_start_state(wiegand_out);
    wiegand_out_data_reset(wiegand_out);
    hrtimer_start(&wiegand_out->timer, ktime_set(0, 0), HRTIMER_MODE_REL);
}

static int wiegand_out_add_parity_bits(struct wiegand_out_dev *wiegand_out)
{
    unsigned long data = 0;
    unsigned int tmp[MAX_WIEGAND_DATA_LEN] = {0x00, 0x00};

    if (wiegand_out->data_length == WIEGAND_MODE_26) {
        data = wiegand_out->wiegand_data & 0xffffff;

        // First 12 bits even parity
        if (even_parity_26(data)) {
            data = 0x01000000 | data;
        } else {
            data = 0x00ffffff & data;
        }

        // After 12 bits odd parity
        if (odd_parity_26(data)) {
            data = (data << 1) | 0x00000001;
        } else {
            data = (data << 1) & 0x01fffffe;
        }
        
        tmp[0] = data << 6;
        tmp[1] = 0x00;

        // Use data with parity bits
        memcpy(wiegand_out->wiegand_out_data, tmp, sizeof(wiegand_out->wiegand_out_data));
    } else if (wiegand_out->data_length == WIEGAND_MODE_34) {
        data = wiegand_out->wiegand_data;

        // First 16 bits even parity
        if (even_parity_34(data)) {
            tmp[0] = (data >> 1) | 0x80000000;
        } else {
            tmp[0] = (data >> 1) & 0x7fffffff;
        }

        tmp[1] = (data & 0x00000001) << 31;

        // After 16 bits odd parity
        if (odd_parity_34(data)) {
            tmp[1] |= 0x40000000;
        } else {
            tmp[1] &= 0xbfffffff;
        }

        // Use data with parity bits
        memcpy(wiegand_out->wiegand_out_data, tmp, sizeof(wiegand_out->wiegand_out_data));
    }

    dev_info(wiegand_out->dev, "%s: parity: %08x%08x\n", __func__,
        wiegand_out->wiegand_out_data[0],
        wiegand_out->wiegand_out_data[1]);
    
    return 0;
}

static enum hrtimer_restart wiegand_out_timeout(struct hrtimer *timer)
{
    int index = 0, offset = 0;
    struct wiegand_out_dev *wiegand_out = container_of(timer, struct wiegand_out_dev, timer);

    wiegand_out_set_current_state(wiegand_out);

    if (wiegand_out->state == PLUSE_WIDTH_STATE) {
        if (wiegand_out->pos == wiegand_out->data_length) {
            if (waitqueue_active(&wiegand_out->wq)) {
                wake_up_interruptible(&wiegand_out->wq);
            }

            return HRTIMER_NORESTART;
        }

        index = wiegand_out->pos / 32;
        offset = wiegand_out->pos % 32;
        if (wiegand_out->wiegand_out_data[index] & (0x80000000 >> offset)) {
            gpio_direction_output(wiegand_out->data1_pin, 0);
        } else {
            gpio_direction_output(wiegand_out->data0_pin, 0);
        }

        wiegand_out->pos++;
        wiegand_out_start_pulse_width_timer(wiegand_out);
    } else {
        gpio_direction_output(wiegand_out->data0_pin, 1);
        gpio_direction_output(wiegand_out->data1_pin, 1);
        wiegand_out_start_pulse_intval_timer(wiegand_out);
    }

    return HRTIMER_NORESTART;
}

static int wiegand_out_open(struct inode *inode, struct file *filp)
{
    struct miscdevice *dev = filp->private_data;
    struct wiegand_out_dev *wiegand_out = container_of(dev, struct wiegand_out_dev, mdev);

    spin_lock(&wiegand_out->lock);
    if (wiegand_out->use_count > 0) {
        spin_unlock(&wiegand_out->lock);
        return -EBUSY;
    }
    wiegand_out->use_count++;
    spin_unlock(&wiegand_out->lock);

    wiegand_out_data_reset(wiegand_out);
    return 0;
}

static int wiegand_out_release(struct inode *inode, struct file *filp)
{
    struct miscdevice *dev = filp->private_data;
    struct wiegand_out_dev *wiegand_out = container_of(dev, struct wiegand_out_dev, mdev);

    spin_lock(&wiegand_out->lock);
    wiegand_out->use_count--;
    spin_unlock(&wiegand_out->lock);

    return 0;
}

static ssize_t wiegand_out_write(struct file *filp, const char __user *buf, size_t size, loff_t *l)
{
    int ret, us, s;
    struct miscdevice *dev = filp->private_data;
    struct wiegand_out_dev *wiegand_out = container_of(dev, struct wiegand_out_dev, mdev);

    if (filp->f_flags & O_NONBLOCK) {
        return -EAGAIN;
    }

    if (size > MAX_WIEGAND_DATA_LEN) {
        dev_err(wiegand_out->dev, "ERROR: wiegand out data length error, max is %d, please check.\n", MAX_WIEGAND_DATA_LEN);
        return -EFAULT;
    }

    if (copy_from_user(wiegand_out->wiegand_out_data, buf, size)) {
        return -EFAULT;
    }

    dev_info(wiegand_out->dev, "%s:[%d] %08x%08x\n", __func__,
                            wiegand_out->data_length,
                            wiegand_out->wiegand_out_data[0],
                            wiegand_out->wiegand_out_data[1]);

    wiegand_out_start_write(wiegand_out);

    us = (wiegand_out->pulse_width + wiegand_out->pulse_intval)
         * wiegand_out->data_length;
    s = us / 1000000;

    //ret = interruptible_sleep_on_timeout(&wiegand_out->wq, (s + 2) * HZ);
    ret = wait_event_interruptible_timeout(wiegand_out->wq, !waitqueue_active(&wiegand_out->wq), (s + 2) * HZ);
    if (!ret) {
        dev_err(wiegand_out->dev, "wiegand write timeout\n");
        return -EIO;
    }

    return 0;
}

static long wiegand_out_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    int cs;
    struct miscdevice *dev = filp->private_data;
    struct wiegand_out_dev *wiegand_out = container_of(dev, struct wiegand_out_dev, mdev);

    if (_IOC_TYPE(cmd) != WIEGAND_IOC_MAGIC) {
        dev_err(wiegand_out->dev, "%s, cmd magic unmatched.", __func__);
        return -EINVAL;
    }
    if (_IOC_NR(cmd) > WIEGAND_IOC_MAXNR) {
        dev_err(wiegand_out->dev, "%s, cmd out of range.", __func__);
        return -EINVAL;
    }

    if (_IOC_DIR(cmd) & _IOC_READ) {
        ret = !access_ok(VERIFY_WRITE, (void *)arg, _IOC_SIZE(cmd));
    } else if (_IOC_DIR(cmd) & _IOC_WRITE) {
        ret = !access_ok(VERIFY_READ, (void *)arg, _IOC_SIZE(cmd));
    }
    if (ret) {
        dev_err(wiegand_out->dev, "%s, verify r/w failed.", __func__);
        return -EFAULT;
    }

    switch (cmd) {
        case WIEGAND_PULSE_WIDTH: {
                if (get_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                wiegand_out->pulse_width = cs;
                dev_info(wiegand_out->dev, "%s: WIEGAND_PULSE_WIDTH pulse_width=%d\n", 
                    __func__, wiegand_out->pulse_width);
                break;
            }

        case WIEGAND_PULSE_INTERVAL: {
                if (get_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                wiegand_out->pulse_intval = cs;
                dev_info(wiegand_out->dev, "%s: WIEGAND_PULSE_INTERVAL pulse_intval=%d\n", 
                    __func__, wiegand_out->pulse_intval);
                break;
            }

        case WIEGAND_FORMAT: {
                if (get_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                wiegand_out->data_length = cs;
                dev_info(wiegand_out->dev, "%s: WIEGAND_FORMAT data_length=%d\n", 
                    __func__, wiegand_out->data_length);
                break;
            }

        case WIEGAND_WRITE: {
                if (get_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                wiegand_out->wiegand_data = cs;
                dev_info(wiegand_out->dev, "%s: WIEGAND_WRITE[%d] %08x\n", __func__,
                            wiegand_out->data_length,
                            wiegand_out->wiegand_data);
                                   
                wiegand_out_add_parity_bits(wiegand_out);
                wiegand_out_start_write(wiegand_out);
                break;
            }

        default:
            return -EINVAL;
    }

    return 0;
}

static int wiegand_out_parse_dt(struct device *dev, struct wiegand_out_dev *wiegand)
{
    int ret = 0;
    struct device_node *np = dev->of_node;

    wiegand->data0_pin = of_get_named_gpio(np, "wiegand,data0", 0);
    if (!gpio_is_valid(wiegand->data0_pin)) {
        dev_err(dev, "Invalid data0 gpio");
        return -1;
    }

    wiegand->data1_pin = of_get_named_gpio(np, "wiegand,data1", 0);
    if (!gpio_is_valid(wiegand->data1_pin)) {
        dev_err(dev, "Invalid data1 gpio");
        return -1;
    }

    ret = of_property_read_u32(np, "wiegand,data_length", &wiegand->data_length);
    if (ret) {
        wiegand->data_length = DEF_DATA_LENGTH;
    }

    ret = of_property_read_u32(np, "wiegand,pulse_width", &wiegand->pulse_width);
    if (ret) {
        wiegand->pulse_width = DEF_PULSE_WIDTH;
    }

    ret = of_property_read_u32(np, "wiegand,pulse_intval", &wiegand->pulse_intval);
    if (ret) {
        wiegand->pulse_intval = DEF_PULSE_INTERVAL;
    }

    dev_info(dev, "%s: data_length=%d pulse_width=%d pulse_intval=%d\n", __func__,
             wiegand->data_length, wiegand->pulse_width, wiegand->pulse_intval);

    return 0;
}

static int wiegand_in_request_io_port(struct wiegand_out_dev *wiegand_out)
{
    int ret = 0;

    if (gpio_is_valid(wiegand_out->data0_pin)) {
        ret = gpio_request(wiegand_out->data0_pin, "WIEGAND_IN_DATA0");

        if (ret < 0) {
            dev_err(&wiegand_out->platform_dev->dev, "Failed to request GPIO:%d, ERRNO:%d\n", (s32)wiegand_out->data0_pin, ret);
            return -ENODEV;
        }
        gpio_direction_input(wiegand_out->data0_pin);
        dev_info(&wiegand_out->platform_dev->dev, "Success request data0 gpio\n");
    }

    if (gpio_is_valid(wiegand_out->data1_pin)) {
        ret = gpio_request(wiegand_out->data1_pin, "WIEGAND_IN_DATA1");

        if (ret < 0) {
            dev_err(&wiegand_out->platform_dev->dev, "Failed to request GPIO:%d, ERRNO:%d\n", (s32)wiegand_out->data1_pin, ret);
            return -ENODEV;
        }
        gpio_direction_input(wiegand_out->data1_pin);
        dev_info(&wiegand_out->platform_dev->dev, "Success request data1 gpio\n");
    }

    return ret;
}

static struct file_operations wiegand_out_misc_fops = {
    .open       = wiegand_out_open,
    .release    = wiegand_out_release,
    .write      = wiegand_out_write,
    .unlocked_ioctl = wiegand_out_ioctl,
};

static int wiegand_out_probe(struct platform_device *pdev)
{
    int ret = -1;
    struct wiegand_out_dev *wiegand_out;

    dev_info(&pdev->dev, "%s: WIEGAND OUT VERSION = %s\n", __func__, WIEGANDOUTDRV_LIB_VERSION);

    wiegand_out = kzalloc(sizeof(struct wiegand_out_dev), GFP_KERNEL);
    if (!wiegand_out) {
        printk("%s: alloc mem failed.\n", __FUNCTION__);
        ret = -ENOMEM;
    }

    if (pdev->dev.of_node) {
        ret = wiegand_out_parse_dt(&pdev->dev, wiegand_out);
        if (ret) {
            dev_err(&pdev->dev, "%s: Failed parse dts.\n", __func__);
            goto exit_free_data;
        }
    }

    wiegand_out->platform_dev = pdev;

    ret = wiegand_in_request_io_port(wiegand_out);
    if (ret < 0) {
        dev_err(&pdev->dev, "%s: Failed request IO port.\n", __func__);
        goto exit_free_data;
    }

    wiegand_out->dev = &pdev->dev;
    wiegand_out->mdev.minor = MISC_DYNAMIC_MINOR;
    wiegand_out->mdev.name =  "wiegand_out";
    wiegand_out->mdev.fops = &wiegand_out_misc_fops;

    wiegand_out_data_reset(wiegand_out);
    wiegand_out->pulse_width = DEF_PULSE_WIDTH;
    wiegand_out->pulse_intval = DEF_PULSE_INTERVAL;
    wiegand_out->data_length = DEF_DATA_LENGTH;

    spin_lock_init(&wiegand_out->lock);
    init_waitqueue_head(&wiegand_out->wq);
    hrtimer_init(&wiegand_out->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    wiegand_out->timer.function = wiegand_out_timeout;

    ret = misc_register(&wiegand_out->mdev);
    if (ret < 0) {
        dev_err(&pdev->dev, "misc_register failed %s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
        goto exit_free_io_port;
    }

    platform_set_drvdata(pdev, wiegand_out);
    dev_info(&pdev->dev, "%s: Weigand out driver register success.\n", __func__);

    return 0;

exit_free_io_port:
    if (gpio_is_valid(wiegand_out->data0_pin)) {
        gpio_free(wiegand_out->data0_pin);
    }
    if (gpio_is_valid(wiegand_out->data1_pin)) {
        gpio_free(wiegand_out->data1_pin);
    }

exit_free_data:
    kfree(wiegand_out);
    return ret;
}

static int wiegand_out_remove(struct platform_device *dev)
{
    struct wiegand_out_dev *wiegand_out = platform_get_drvdata(dev);
    misc_deregister(&wiegand_out->mdev);
    gpio_free(wiegand_out->data0_pin);
    gpio_free(wiegand_out->data1_pin);
    kfree(wiegand_out);

    return 0;
}

static const struct of_device_id wiegand_of_match[] = {
    { .compatible =  "wiegandout"},
    {},
};

MODULE_DEVICE_TABLE(of, wiegand_of_match);

static struct platform_driver wiegand_out_driver = {
    .probe      = wiegand_out_probe,
    .remove     = wiegand_out_remove,
    .driver = {
        .name = WIEGAND_DEIVCE_NAME,
        .owner  = THIS_MODULE,
        .of_match_table = wiegand_of_match,
    },
};

module_platform_driver(wiegand_out_driver);

MODULE_AUTHOR("ayst.shen@foxmail.com");
MODULE_DESCRIPTION("Wiegand Out");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
