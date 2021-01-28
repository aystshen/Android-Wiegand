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
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>


#define WIEGANDINDRV_LIB_VERSION    "1.0.0"

#define WIEGAND_DEIVCE_NAME         "wiegand_in"

#define WIEGAND_MODE_26     26 //bit
#define WIEGAND_MODE_34     34 //bit

#define DEF_PULSE_WIDTH     100 //us
#define DEF_PULSE_INTERVAL  1000 //us
#define DEF_DATA_LENGTH     WIEGAND_MODE_26
#define DEVIATION           100 //us

/* ioctl command */
#define WIEGAND_IOC_MAGIC  'w'

#define WIEGAND_PULSE_WIDTH     _IOW(WIEGAND_IOC_MAGIC, 1, int)
#define WIEGAND_PULSE_INTERVAL  _IOW(WIEGAND_IOC_MAGIC, 2, int)
#define WIEGAND_FORMAT          _IOW(WIEGAND_IOC_MAGIC, 3, int)
#define WIEGAND_READ            _IOR(WIEGAND_IOC_MAGIC, 4, unsigned int)
#define WIEGAND_WRITE           _IOW(WIEGAND_IOC_MAGIC, 5, unsigned char *)
#define WIEGAND_STATUS          _IOR(WIEGAND_IOC_MAGIC, 6, int)

#define WIEGAND_IOC_MAXNR 6

struct wiegand_in_dev {
    struct platform_device  *platform_dev;
    struct device           *dev;
    struct miscdevice       mdev;
    int                     irq0;
    int                     irq1;
    unsigned int            data0_pin;
    unsigned int            data1_pin;
    unsigned int            current_data[2];
    unsigned int            wiegand_in_data[2];
    int                     data_length;
    int                     recvd_length;
    int                     pulse_width;
    int                     pulse_intval;
    int                     error;
    spinlock_t              lock;
    int                     use_count;
    struct hrtimer          timer;
    wait_queue_head_t       wq;
    struct timeval          latest;
    struct timeval          now;
};

static void wiegand_in_data_reset(struct wiegand_in_dev *wiegand_in)
{
    wiegand_in->recvd_length = -1;
    wiegand_in->current_data[0] = 0;
    wiegand_in->current_data[1] = 0;
    wiegand_in->wiegand_in_data[0] = 0;
    wiegand_in->wiegand_in_data[1] = 0;
    wiegand_in->error = 0;
}

static int wiegand_in_open(struct inode *inode, struct file *filp)
{
    struct miscdevice *dev = filp->private_data;
    struct wiegand_in_dev *wiegand_in = container_of(dev, struct wiegand_in_dev, mdev);

    spin_lock(&wiegand_in->lock);
    if (wiegand_in->use_count > 0) {
        spin_unlock(&wiegand_in->lock);
        return -EBUSY;
    }
    wiegand_in->use_count++;
    spin_unlock(&wiegand_in->lock);

    wiegand_in_data_reset(wiegand_in);
    enable_irq(gpio_to_irq(wiegand_in->data0_pin));
    enable_irq(gpio_to_irq(wiegand_in->data1_pin));
    return 0;
}

static int wiegand_in_release(struct inode *inode, struct file *filp)
{
    struct miscdevice *dev = filp->private_data;
    struct wiegand_in_dev *wiegand_in = container_of(dev, struct wiegand_in_dev, mdev);

    disable_irq(gpio_to_irq(wiegand_in->data0_pin));
    disable_irq(gpio_to_irq(wiegand_in->data1_pin));
    spin_lock(&wiegand_in->lock);
    wiegand_in->use_count--;
    spin_unlock(&wiegand_in->lock);

    return 0;
}

static ssize_t wiegand_in_read(struct file *filp, char *buf, size_t size, loff_t *l)
{
    struct miscdevice *dev = filp->private_data;
    struct wiegand_in_dev *wiegand_in = container_of(dev, struct wiegand_in_dev, mdev);

    if (filp->f_flags & O_NONBLOCK) {
        return -EAGAIN;
    }

    if (wait_event_interruptible(wiegand_in->wq,
                                 (wiegand_in->wiegand_in_data[0] > 0) || (wiegand_in->error))) {
        return -ERESTARTSYS;
    }

    if (wiegand_in->wiegand_in_data[0] > 0) {
        printk("wiegand_in_read: %d, %d\n", wiegand_in->wiegand_in_data[0], wiegand_in->wiegand_in_data[1]);
        if (copy_to_user(buf, wiegand_in->wiegand_in_data,
                         sizeof(wiegand_in->wiegand_in_data))) {
            return -EFAULT;
        }
        wiegand_in->wiegand_in_data[0] = 0;
        wiegand_in->wiegand_in_data[1] = 0;

        return sizeof(wiegand_in->wiegand_in_data);
    }
    wiegand_in->error = 0;

    return 0;
}

static unsigned int wiegand_in_poll(struct file *filp, poll_table *wait)
{
    unsigned int mask = 0;

    struct miscdevice *dev = filp->private_data;
    struct wiegand_in_dev *wiegand_in = container_of(dev, struct wiegand_in_dev, mdev);
    poll_wait(filp, &wiegand_in->wq, wait);

    if (wiegand_in->wiegand_in_data[0]) {
        mask |= POLLIN | POLLRDNORM;
    }

    return mask;
}

static unsigned int wiegand_in_rm_parity_bits(struct wiegand_in_dev *wiegand_in) {
    unsigned int data = 0;
    if (wiegand_in->data_length == WIEGAND_MODE_26) {
        data = (wiegand_in->wiegand_in_data[0] >> 1) & 0xffffff;
    } else if (wiegand_in->data_length == WIEGAND_MODE_34) {
        data = wiegand_in->wiegand_in_data[1] & 0x00000001;
        data = (data << 31) | ((wiegand_in->wiegand_in_data[0] >> 1) & 0x7fffffff);
    }
    return data;
}

static long wiegand_in_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct miscdevice *dev = filp->private_data;
    struct wiegand_in_dev *wiegand_in = container_of(dev, struct wiegand_in_dev, mdev);
    int cs, ret = 0;
    unsigned int data = 0;

    if (_IOC_TYPE(cmd) != WIEGAND_IOC_MAGIC) {
        dev_err(wiegand_in->dev, "%s, cmd magic unmatched.", __func__);
        return -EINVAL;
    }
    if (_IOC_NR(cmd) > WIEGAND_IOC_MAXNR) {
        dev_err(wiegand_in->dev, "%s, cmd out of range.", __func__);
        return -EINVAL;
    }

    if (_IOC_DIR(cmd) & _IOC_READ) {
        ret = !access_ok(VERIFY_WRITE, (void *)arg, _IOC_SIZE(cmd));
    } else if (_IOC_DIR(cmd) & _IOC_WRITE) {
        ret = !access_ok(VERIFY_READ, (void *)arg, _IOC_SIZE(cmd));
    }
    if (ret) {
        dev_err(wiegand_in->dev, "%s, verify r/w failed.", __func__);
        return -EFAULT;
    }

    switch (cmd) {
        case WIEGAND_PULSE_WIDTH: {
                if (get_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                wiegand_in->pulse_width = cs;
                dev_info(wiegand_in->dev, "%s: WIEGAND_PULSE_WIDTH pulse_width=%d\n", 
                    __func__, wiegand_in->pulse_width);
                break;
            }

        case WIEGAND_PULSE_INTERVAL: {
                if (get_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                wiegand_in->pulse_intval = cs;
                dev_info(wiegand_in->dev, "%s: WIEGAND_PULSE_INTERVAL pulse_intval=%d\n", 
                    __func__, wiegand_in->pulse_intval);
                break;
            }

        case WIEGAND_FORMAT: {
                if (get_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                wiegand_in->data_length = cs;
                dev_info(wiegand_in->dev, "%s: WIEGAND_FORMAT data_length=%d\n", 
                    __func__, wiegand_in->data_length);
                break;
            }

        case WIEGAND_READ: {
                data = wiegand_in_rm_parity_bits(wiegand_in);
                if (copy_to_user((int *)arg, &data, sizeof(int))) {
                    return -EFAULT;
                }
                
                dev_info(wiegand_in->dev, "%s: WIEGAND_READ[%d] buf:%08x%08x data:%08x\n", __func__,
                            wiegand_in->data_length,
                            wiegand_in->wiegand_in_data[0],
                            wiegand_in->wiegand_in_data[1],
                            data);
                
                wiegand_in->wiegand_in_data[0] = 0;
                wiegand_in->wiegand_in_data[1] = 0;
                break;
            }

        case WIEGAND_STATUS: {
                if (wiegand_in->wiegand_in_data[0] == 0) {
                    cs = 0;
                } else {
                    cs = 1;
                }
                if (put_user(cs, (unsigned int *)arg)) {
                    return -EINVAL;
                }
                dev_info(wiegand_in->dev, "%s: WIEGAND_STATUS status=%d\n", 
                    __func__, cs);
                break;
            }

        default:
            return -EINVAL;
    }

    return 0;
}

static struct file_operations wiegand_in_misc_fops = {
    .open       = wiegand_in_open,
    .release    = wiegand_in_release,
    .read       = wiegand_in_read,
    .unlocked_ioctl = wiegand_in_ioctl,
    .poll       = wiegand_in_poll,
};

static void wiegand_in_check_data(struct wiegand_in_dev *wiegand_in)
{
    if (wiegand_in->recvd_length == wiegand_in->data_length - 1) {
        memcpy(wiegand_in->wiegand_in_data, wiegand_in->current_data, sizeof(wiegand_in->current_data));
        wiegand_in->recvd_length = -1;
        wiegand_in->current_data[0] = 0;
        wiegand_in->current_data[1] = 0;
        wake_up_interruptible(&wiegand_in->wq);
    } else {
        printk("recvd data error: received length = %d, required length = %d\n",
               wiegand_in->recvd_length, wiegand_in->data_length);
        wiegand_in_data_reset(wiegand_in);
        wiegand_in->error = 1;
        wake_up_interruptible(&wiegand_in->wq);
    }
}

static enum hrtimer_restart wiegand_in_timeout(struct hrtimer * timer)
{
    struct wiegand_in_dev *wiegand_in = container_of(timer, struct wiegand_in_dev, timer);
    wiegand_in_check_data(wiegand_in);
    return HRTIMER_NORESTART;
}

static void wiegand_in_reset_timer(struct wiegand_in_dev *wiegand_in)
{
    int us = (wiegand_in->pulse_width + wiegand_in->pulse_intval)
             * wiegand_in->data_length;
    int s = us / 1000000;
    ktime_t time = ktime_set(s, (us % 1000000) * 1000);
    hrtimer_start(&wiegand_in->timer, time, HRTIMER_MODE_REL);
}

static int wiegand_in_check_irq(struct wiegand_in_dev *wiegand_in)
{
    int diff;

    if (wiegand_in->recvd_length < 0) {
        do_gettimeofday(&wiegand_in->latest);
        return 0;
    }

    /* Check how much time we have used already */
    do_gettimeofday(&wiegand_in->now);
    diff = wiegand_in->now.tv_usec - wiegand_in->latest.tv_usec;

    /* check fake interrupt */
    if (diff < wiegand_in->pulse_width) { //+ wiegand_in->pulse_intval - DEVIATION)
        return -1;
    }
    /*
     * if intarval is greater than DEVIATION,
     * then cheet it as beginning of another scan
     * and discard current data
     */
    else if (diff > wiegand_in->pulse_width + wiegand_in->pulse_intval
             + ((wiegand_in->pulse_width + wiegand_in->pulse_intval) << 1)) {
        hrtimer_cancel(&wiegand_in->timer);
        wiegand_in_data_reset(wiegand_in);
        return -1;
    }

    wiegand_in->latest.tv_sec = wiegand_in->now.tv_sec;
    wiegand_in->latest.tv_usec = wiegand_in->now.tv_usec;
    return 0;
}

static irqreturn_t wiegand_in_interrupt(int irq, void *dev_id)
{
    struct wiegand_in_dev *wiegand_in = (struct wiegand_in_dev *)dev_id;

    if (wiegand_in_check_irq(wiegand_in)) {
        return IRQ_HANDLED;
    }

    if (wiegand_in->recvd_length < 0) {
        wiegand_in_reset_timer(wiegand_in);
    }

    wiegand_in->recvd_length++;
    wiegand_in->current_data[1] <<= 1;
    wiegand_in->current_data[1] |= ((wiegand_in->current_data[0] >> 31) & 0x01);
    wiegand_in->current_data[0] <<= 1;
    if (irq == wiegand_in->irq1) {
        wiegand_in->current_data[0] |= 1;
    }
    return IRQ_HANDLED;
}

static int wiegand_in_parse_dt(struct device *dev,
                               struct wiegand_in_dev *wiegand)
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

static int wiegand_in_request_io_port(struct wiegand_in_dev *wiegand_in)
{
    int ret = 0;

    if (gpio_is_valid(wiegand_in->data0_pin)) {
        ret = gpio_request(wiegand_in->data0_pin, "WIEGAND_IN_DATA0");

        if (ret < 0) {
            dev_err(&wiegand_in->platform_dev->dev,
                    "Failed to request GPIO:%d, ERRNO:%d\n",
                    (s32)wiegand_in->data0_pin, ret);
            return -ENODEV;
        }

        gpio_direction_input(wiegand_in->data0_pin);
        dev_info(&wiegand_in->platform_dev->dev, "Success request data0 gpio\n");
    }

    if (gpio_is_valid(wiegand_in->data1_pin)) {
        ret = gpio_request(wiegand_in->data1_pin, "WIEGAND_IN_DATA1");

        if (ret < 0) {
            dev_err(&wiegand_in->platform_dev->dev,
                    "Failed to request GPIO:%d, ERRNO:%d\n",
                    (s32)wiegand_in->data1_pin, ret);
            return -ENODEV;
        }

        gpio_direction_input(wiegand_in->data1_pin);
        dev_info(&wiegand_in->platform_dev->dev, "Success request data1 gpio\n");
    }

    return ret;
}

static int wiegand_in_request_irq(struct wiegand_in_dev *wiegand_in)
{
    int ret = 0;

    /* use irq */
    if (gpio_is_valid(wiegand_in->data0_pin) || wiegand_in->irq0 > 0) {
        if (gpio_is_valid(wiegand_in->data0_pin)) {
            wiegand_in->irq0 = gpio_to_irq(wiegand_in->data0_pin);
        }

        dev_info(&wiegand_in->platform_dev->dev, "INT num %d, trigger type:%d\n",
                 wiegand_in->irq0, IRQF_TRIGGER_FALLING);
        ret = request_irq(wiegand_in->irq0,
                          wiegand_in_interrupt,
                          IRQF_TRIGGER_FALLING,
                          "wiegand_in_data0", wiegand_in);

        if (ret < 0) {
            dev_err(&wiegand_in->platform_dev->dev,
                    "Failed to request irq %d\n", wiegand_in->irq0);
        } else {
            disable_irq(wiegand_in->irq0);
        }
    }

    if (gpio_is_valid(wiegand_in->data1_pin) || wiegand_in->irq1 > 0) {
        if (gpio_is_valid(wiegand_in->data1_pin)) {
            wiegand_in->irq1 = gpio_to_irq(wiegand_in->data1_pin);
        }

        dev_info(&wiegand_in->platform_dev->dev, "INT num %d, trigger type:%d\n",
                 wiegand_in->irq1, IRQF_TRIGGER_FALLING);
        ret = request_irq(wiegand_in->irq1,
                          wiegand_in_interrupt,
                          IRQF_TRIGGER_FALLING,
                          "wiegand_in_data1", wiegand_in);

        if (ret < 0) {
            dev_err(&wiegand_in->platform_dev->dev,
                    "Failed to request irq %d\n", wiegand_in->irq1);
        } else {
            disable_irq(wiegand_in->irq1);
        }
    }

    return ret;
}

static int wiegand_in_probe(struct platform_device *pdev)
{
    int ret;
    struct wiegand_in_dev *wiegand_in;

    dev_info(&pdev->dev, "%s: WIEGAND IN VERSION = %s\n", __func__, WIEGANDINDRV_LIB_VERSION);

    wiegand_in = kzalloc(sizeof(struct wiegand_in_dev), GFP_KERNEL);
    if (!wiegand_in) {
        dev_info(&pdev->dev, "%s: alloc mem failed.\n", __func__);
        return -ENOMEM;
    }

    if (pdev->dev.of_node) {
        ret = wiegand_in_parse_dt(&pdev->dev, wiegand_in);
        if (ret) {
            dev_err(&pdev->dev, "%s: Failed parse dts.\n", __func__);
            goto exit_free_data;
        }
    }

    wiegand_in->platform_dev = pdev;

    ret = wiegand_in_request_io_port(wiegand_in);
    if (ret < 0) {
        dev_err(&pdev->dev, "%s: Failed request IO port.\n", __func__);
        goto exit_free_data;
    }

    ret = wiegand_in_request_irq(wiegand_in);
    if (ret < 0) {
        dev_err(&pdev->dev, "%s: Failed request irq.\n", __func__);
        goto exit_free_io_port;
    }

    wiegand_in->dev = &pdev->dev;
    wiegand_in->mdev.minor = MISC_DYNAMIC_MINOR;
    wiegand_in->mdev.name =  WIEGAND_DEIVCE_NAME;
    wiegand_in->mdev.fops = &wiegand_in_misc_fops;

    wiegand_in_data_reset(wiegand_in);

    spin_lock_init(&wiegand_in->lock);
    init_waitqueue_head(&wiegand_in->wq);
    hrtimer_init(&wiegand_in->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    wiegand_in->timer.function = wiegand_in_timeout;

    ret = misc_register(&wiegand_in->mdev);
    if (ret < 0) {
        dev_err(&pdev->dev, "%s: misc register failed.\n", __func__);
        goto exit_free_irq;
    }

    platform_set_drvdata(pdev, wiegand_in);
    dev_info(&pdev->dev, "%s: Weigand in driver register success.\n", __func__);

    return 0;

exit_free_irq:
    free_irq(wiegand_in->irq0, wiegand_in);
    free_irq(wiegand_in->irq1, wiegand_in);

exit_free_io_port:
    if (gpio_is_valid(wiegand_in->data0_pin)) {
        gpio_free(wiegand_in->data0_pin);
    }
    if (gpio_is_valid(wiegand_in->data1_pin)) {
        gpio_free(wiegand_in->data1_pin);
    }

exit_free_data:
    kfree(wiegand_in);
    return ret;
}

static int wiegand_in_remove(struct platform_device *dev)
{
    struct wiegand_in_dev *wiegand_in = platform_get_drvdata(dev);
    misc_deregister(&wiegand_in->mdev);
    free_irq(gpio_to_irq(wiegand_in->data0_pin), wiegand_in);
    free_irq(gpio_to_irq(wiegand_in->data1_pin), wiegand_in);
    gpio_free(wiegand_in->data0_pin);
    gpio_free(wiegand_in->data1_pin);
    kfree(wiegand_in);

    return 0;
}

static const struct of_device_id wiegand_of_match[] = {
    { .compatible =  "wiegandin"},
    {},
};

MODULE_DEVICE_TABLE(of, wiegand_of_match);

static struct platform_driver wiegand_in_driver = {
    .probe = wiegand_in_probe,
    .remove = wiegand_in_remove,
    .driver = {
        .name = WIEGAND_DEIVCE_NAME,
        .owner  = THIS_MODULE,
        .of_match_table = wiegand_of_match,
    },
};

module_platform_driver(wiegand_in_driver);

MODULE_AUTHOR("ayst.shen@foxmail.com");
MODULE_DESCRIPTION("Wiegand In");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");

