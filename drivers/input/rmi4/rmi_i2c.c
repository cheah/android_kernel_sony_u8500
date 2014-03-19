/*
 * Copyright (c) 2011 Synaptics Incorporated
 * Copyright (c) 2011 Unixphere
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

#define COMMS_DEBUG 0

#define IRQ_DEBUG 0

#if COMMS_DEBUG || IRQ_DEBUG
#define DEBUG
#endif

#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/rmi.h>
#include "rmi_driver.h"
#include <linux/regulator/consumer.h>

#define RMI_PAGE_SELECT_REGISTER 0xff
#define RMI_I2C_PAGE(addr) (((addr) >> 8) & 0xff)

static int i2c_err_count;

static char *phys_proto_name = "i2c";

struct rmi_i2c_data {
	struct mutex page_mutex;
	int page;
	int enabled;
	int irq;
	int irq_flags;
	struct rmi_phys_device *phys;
};

static irqreturn_t rmi_i2c_irq_thread(int irq, void *p)
{
	struct rmi_phys_device *phys = p;
	struct rmi_device *rmi_dev = phys->rmi_dev;
	struct rmi_driver *driver = rmi_dev->driver;
	struct rmi_device_platform_data *pdata = phys->dev->platform_data;

#if IRQ_DEBUG
	dev_dbg(phys->dev, "ATTN gpio, value: %d.\n",
			gpio_get_value(pdata->attn_gpio));
#endif
	if (gpio_get_value(pdata->attn_gpio) == pdata->attn_polarity) {
		phys->info.attn_count++;
		if (driver && driver->irq_handler && rmi_dev)
			driver->irq_handler(rmi_dev, irq);
	}

	return IRQ_HANDLED;
}

/*
 * rmi_set_page - Set RMI page
 * @phys: The pointer to the rmi_phys_device struct
 * @page: The new page address.
 *
 * RMI devices have 16-bit addressing, but some of the physical
 * implementations (like SMBus) only have 8-bit addressing. So RMI implements
 * a page address at 0xff of every page so we can reliable page addresses
 * every 256 registers.
 *
 * The page_mutex lock must be held when this function is entered.
 *
 * Returns zero on success, non-zero on failure.
 */
static int rmi_set_page(struct rmi_phys_device *phys, unsigned int page)
{
	struct i2c_client *client = to_i2c_client(phys->dev);
	struct rmi_i2c_data *data = phys->data;
	struct rmi_device_platform_data *pdata = phys->dev->platform_data;
	char txbuf[2] = {RMI_PAGE_SELECT_REGISTER, page};
	int retval;

#if COMMS_DEBUG
	dev_dbg(&client->dev, "RMI4 I2C writes 3 bytes: %02x %02x\n",
		txbuf[0], txbuf[1]);
#endif
	phys->info.tx_count++;
	phys->info.tx_bytes += sizeof(txbuf);
	retval = i2c_master_send(client, txbuf, sizeof(txbuf));
	if (retval != sizeof(txbuf)) {
		i2c_err_count++;
		phys->info.tx_errs++;
		dev_err(&client->dev,
			"%s: set page:%d failed: %d. i2c_err_count:%d.",
			__func__, page,retval,i2c_err_count);
		if (pdata->gpio_reset && i2c_err_count < 6) {
			pdata->gpio_reset(pdata->gpio_data);
		}
		else
			dev_crit(&client->dev,"\n@@@@@@@@!!!!!%s: RMI GPIO RESET INVALID !!!!!@@@@@@@\n",__func__);
		return (retval < 0) ? retval : -EIO;
	}
	data->page = page;
	i2c_err_count = 0;
	return 0;
}

static int rmi_i2c_write_block(struct rmi_phys_device *phys, u16 addr, u8 *buf,
			       int len)
{
	struct i2c_client *client = to_i2c_client(phys->dev);
	struct rmi_i2c_data *data = phys->data;
	struct rmi_device_platform_data *pdata = phys->dev->platform_data;
	u8 txbuf[len + 1];
	int retval;
#if	COMMS_DEBUG
	int i;
#endif

	txbuf[0] = addr & 0xff;
	memcpy(txbuf + 1, buf, len);

	mutex_lock(&data->page_mutex);

	if (RMI_I2C_PAGE(addr) != data->page) {
		retval = rmi_set_page(phys, RMI_I2C_PAGE(addr));
		if (retval < 0)
			goto exit;
	}

#if COMMS_DEBUG
	dev_dbg(&client->dev, "RMI4 I2C writes %d bytes: ", sizeof(txbuf));
	for (i = 0; i < sizeof(txbuf); i++)
		dev_dbg(&client->dev, "%02x ", txbuf[i]);
	dev_dbg(&client->dev, "\n");
#endif

	phys->info.tx_count++;
	phys->info.tx_bytes += sizeof(txbuf);
	retval = i2c_master_send(client, txbuf, sizeof(txbuf));
	if (retval < 0) {
		i2c_err_count++;
		phys->info.tx_errs++;
		dev_err(&client->dev,
			"%s: write page:%d addr:0x%x data:0x%x failed: %d. i2c_err_count:%d.",
			__func__, RMI_I2C_PAGE(addr), addr, *buf, retval, i2c_err_count);
		if (pdata->gpio_reset && i2c_err_count < 6) {
			pdata->gpio_reset(pdata->gpio_data);
		}
		else
			dev_crit(&client->dev,"\n@@@@@@@@!!!!!%s: RMI GPIO RESET INVALID !!!!!@@@@@@@\n",__func__);
	}
	else {
		retval--; /* don't count the address byte */
		i2c_err_count = 0;
	}

exit:
	mutex_unlock(&data->page_mutex);
	return retval;
}

static int rmi_i2c_write(struct rmi_phys_device *phys, u16 addr, u8 data)
{
	int retval = rmi_i2c_write_block(phys, addr, &data, 1);
	return (retval < 0) ? retval : 0;
}

static int rmi_i2c_read_block(struct rmi_phys_device *phys, u16 addr, u8 *buf,
			      int len)
{
	struct i2c_client *client = to_i2c_client(phys->dev);
	struct rmi_i2c_data *data = phys->data;
	struct rmi_device_platform_data *pdata = phys->dev->platform_data;
	u8 txbuf[1] = {addr & 0xff};
	int retval;
#if	COMMS_DEBUG
	int i;
#endif

	mutex_lock(&data->page_mutex);

	if (RMI_I2C_PAGE(addr) != data->page) {
		retval = rmi_set_page(phys, RMI_I2C_PAGE(addr));
		if (retval < 0)
			goto exit;
	}

#if COMMS_DEBUG
	dev_dbg(&client->dev, "RMI4 I2C writes 1 bytes: %02x\n", txbuf[0]);
#endif
	phys->info.tx_count++;
	phys->info.tx_bytes += sizeof(txbuf);
	retval = i2c_master_send(client, txbuf, sizeof(txbuf));
	if (retval != sizeof(txbuf)) {
		i2c_err_count++;
		phys->info.tx_errs++;
		dev_err(&client->dev,
			"%s: read page:%d addr:0x%x failed: %d. i2c_err_count:%d.",
			__func__, RMI_I2C_PAGE(addr), addr, retval, i2c_err_count);
		if (pdata->gpio_reset && i2c_err_count < 6) {
			pdata->gpio_reset(pdata->gpio_data);
		}
		else
			dev_crit(&client->dev,"\n@@@@@@@@!!!!!%s: 1.RMI GPIO RESET INVALID !!!!!@@@@@@@\n",__func__);
		retval = (retval < 0) ? retval : -EIO;
		goto exit;
	}
	i2c_err_count = 0;

	retval = i2c_master_recv(client, buf, len);

	phys->info.rx_count++;
	phys->info.rx_bytes += len;
	if (retval < 0) {
		i2c_err_count++;
		phys->info.rx_errs++;
		dev_err(&client->dev,
			"%s: read page:%d addr:0x%x failed: %d. i2c_err_count:%d.",
			__func__, RMI_I2C_PAGE(addr), addr, retval, i2c_err_count);
		if (pdata->gpio_reset && i2c_err_count < 6) {
			pdata->gpio_reset(pdata->gpio_data);
		}
		else
			dev_crit(&client->dev,"\n@@@@@@@@!!!!!%s: 2.RMI GPIO RESET INVALID !!!!!@@@@@@@\n",__func__);
	}
#if COMMS_DEBUG
	else {
		i2c_err_count = 0;
		dev_dbg(&client->dev, "RMI4 I2C received %d bytes: ", len);
		for (i = 0; i < len; i++)
			dev_dbg(&client->dev, "%02x ", buf[i]);
		dev_dbg(&client->dev, "\n");
	}
#endif

exit:
	mutex_unlock(&data->page_mutex);
	return retval;
}

static int rmi_i2c_read(struct rmi_phys_device *phys, u16 addr, u8 *buf)
{
	int retval = rmi_i2c_read_block(phys, addr, buf, 1);
	return (retval < 0) ? retval : 0;
}

static int irq_no = -1;
static int acquire_attn_irq(struct rmi_i2c_data *data)
{
    irq_no = data->irq;
    pr_alert("%s: acquire irq %d\n", __func__, irq_no);
	return request_threaded_irq(data->irq, NULL, rmi_i2c_irq_thread,
			data->irq_flags, dev_name(data->phys->dev), data->phys);
}

static int enable_device(struct rmi_phys_device *phys)
{
	int retval = 0;

	struct rmi_i2c_data *data = phys->data;

	if (data->enabled)
		return 0;

	retval = acquire_attn_irq(data);
	if (retval)
		goto error_exit;

	data->enabled = true;
	dev_info(phys->dev, "Physical device enabled.\n");
	return 0;

error_exit:
	dev_err(phys->dev, "Failed to enable physical device. Code=%d.\n",
		retval);
	return retval;
}

static void disable_device(struct rmi_phys_device *phys)
{
	struct rmi_i2c_data *data = phys->data;

	if (!data->enabled)
		return;

	disable_irq(data->irq);
	free_irq(data->irq, data->phys);

	dev_info(phys->dev, "Physical device disabled.\n");
	data->enabled = false;
}

static ssize_t rmi_i2c_sysfs_enabled_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rmi_phys_device *phys = platform_get_drvdata(pdev);
	struct rmi_i2c_data *data = phys->data;

	return snprintf(buf, PAGE_SIZE, "%d\n", data->enabled);
}

static ssize_t rmi_i2c_sysfs_attn_gpio_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rmi_phys_device *phys = platform_get_drvdata(pdev);
	struct rmi_device_platform_data *pdata = phys->dev->platform_data;
	int gpio;

	gpio = gpio_get_value(pdata->attn_gpio);
	return snprintf(buf, PAGE_SIZE, "%d\n", gpio);
}

static ssize_t rmi_i2c_sysfs_attn_count_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rmi_phys_device *phys = platform_get_drvdata(pdev);

	return snprintf(buf, PAGE_SIZE, "%ld\n", phys->info.attn_count);
}

static ssize_t rmi_i2c_sysfs_force_attn_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rmi_phys_device *phys = platform_get_drvdata(pdev);
	struct rmi_device *rmi_dev = phys->rmi_dev;
	struct rmi_driver *driver = rmi_dev->driver;
	struct rmi_device_platform_data *pdata = phys->dev->platform_data;
	long val;
	int error;

	error = strict_strtol(buf, 0, &val);
	if (error)
		return error;

	dev_info(phys->dev, "ATTN gpio, value: %d.\n",
			gpio_get_value(pdata->attn_gpio));
	if (val) {
		dev_info(phys->dev, "force to handle attn.\n");
		if (driver && driver->irq_handler && rmi_dev)
			driver->irq_handler(rmi_dev, -1);
	}
	return count;
}

static struct device_attribute rmi_i2c_attrs[] = {
	__ATTR(enabled, RMI_RO_ATTR,
	       rmi_i2c_sysfs_enabled_show, rmi_store_error),
	__ATTR(attn_gpio, RMI_RO_ATTR,
	       rmi_i2c_sysfs_attn_gpio_show, rmi_store_error),
	__ATTR(attn_count, RMI_RO_ATTR,
	       rmi_i2c_sysfs_attn_count_show, rmi_store_error),
	__ATTR(force_attn, RMI_WO_ATTR,
	       rmi_show_error, rmi_i2c_sysfs_force_attn_store),
};

static int rmi_i2c_create_sysfs(struct i2c_client *client)
{
	int attr_count = 0;
	int retval = 0;

	dev_dbg(&client->dev, "Creating sysfs files.");
	for (attr_count = 0; attr_count < ARRAY_SIZE(rmi_i2c_attrs);
			attr_count++) {
		retval = sysfs_create_file(&client->dev.kobj,
				      &rmi_i2c_attrs[attr_count].attr);
		if (retval < 0) {
			dev_err(&client->dev, "Failed to create sysfs file for %s.",
			       rmi_i2c_attrs[attr_count].attr.name);
			return retval;
		}
	}

	return 0;
}

static int __devinit rmi_i2c_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct rmi_phys_device *rmi_phys;
	struct rmi_i2c_data *data;
	struct rmi_device_platform_data *pdata = client->dev.platform_data;
	int error;

	if (!pdata) {
		dev_err(&client->dev, "no platform data\n");
		return -EINVAL;
	}
	pr_info("%s: Probing %s at %#02x (IRQ %d).\n", __func__,
		pdata->sensor_name ? pdata->sensor_name : "-no name-",
		client->addr, pdata->attn_gpio);

	error = i2c_check_functionality(client->adapter, I2C_FUNC_I2C);
	if (!error) {
		dev_err(&client->dev, "i2c_check_functionality error %d.\n",
			error);
		return error;
	}

	rmi_phys = kzalloc(sizeof(struct rmi_phys_device), GFP_KERNEL);
	if (!rmi_phys)
		return -ENOMEM;

	data = kzalloc(sizeof(struct rmi_i2c_data), GFP_KERNEL);
	if (!data) {
		error = -ENOMEM;
		goto err_phys;
	}

	data->enabled = true;	/* We plan to come up enabled. */
	data->irq = gpio_to_irq(pdata->attn_gpio);
	if (pdata->level_triggered) {
		data->irq_flags = IRQF_ONESHOT |
			((pdata->attn_polarity == RMI_ATTN_ACTIVE_HIGH) ?
			IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW);
	} else {
		data->irq_flags =
			(pdata->attn_polarity == RMI_ATTN_ACTIVE_HIGH) ?
			IRQF_TRIGGER_RISING : IRQF_TRIGGER_FALLING;
	}
	data->phys = rmi_phys;

	rmi_phys->data = data;
	rmi_phys->dev = &client->dev;

	rmi_phys->write = rmi_i2c_write;
	rmi_phys->write_block = rmi_i2c_write_block;
	rmi_phys->read = rmi_i2c_read;
	rmi_phys->read_block = rmi_i2c_read_block;
	rmi_phys->enable_device = enable_device;
	rmi_phys->disable_device = disable_device;

	rmi_phys->info.proto = phys_proto_name;

	mutex_init(&data->page_mutex);

	i2c_err_count = 0;
	/* Setting the page to zero will (a) make sure the PSR is in a
	 * known state, and (b) make sure we can talk to the device.
	 */
	pr_info("%s: rmi_set_page", __func__);
	error = rmi_set_page(rmi_phys, 0);
	if (error) {
		dev_err(&client->dev, "Failed to set page select to 0.\n");
		goto err_data;
	}

	if (pdata->gpio_config) {
		error = pdata->gpio_config(pdata->gpio_data, true);
		if (error < 0) {
			dev_err(&client->dev, "failed to setup irq %d\n",
				pdata->attn_gpio);
			goto err_data;
		}
	}

	pr_info("%s: rmi_register_phys_device", __func__);
	error = rmi_register_phys_device(rmi_phys);
	if (error) {
		dev_err(&client->dev,
			"failed to register physical driver at 0x%.2X.\n",
			client->addr);
		goto err_gpio;
	}
	i2c_set_clientdata(client, rmi_phys);

	if (pdata->attn_gpio > 0) {
		error = acquire_attn_irq(data);
		if (error < 0) {
			dev_err(&client->dev,
				"request_threaded_irq failed %d\n",
				pdata->attn_gpio);
			goto err_unregister;
		}
	}

#if defined(CONFIG_RMI4_DEV)
	error = gpio_export(pdata->attn_gpio, false);
	if (error) {
		dev_warn(&client->dev, "%s: WARNING: Failed to "
				 "export ATTN gpio!\n", __func__);
		error = 0;
	} else {
		error = gpio_export_link(&(rmi_phys->rmi_dev->dev), "attn",
					pdata->attn_gpio);
		if (error) {
			dev_warn(&(rmi_phys->rmi_dev->dev),
				 "%s: WARNING: Failed to symlink ATTN gpio!\n",
				 __func__);
			error = 0;
		} else {
			dev_info(&(rmi_phys->rmi_dev->dev),
				"%s: Exported GPIO %d.", __func__,
				pdata->attn_gpio);
		}
	}
#endif /* CONFIG_RMI4_DEV */

	rmi_i2c_create_sysfs(client);

	pr_info("%s: registered rmi i2c driver at 0x%.2X.\n",
			client->addr);
	//dev_info(&client->dev, "registered rmi i2c driver at 0x%.2X.\n",
			//client->addr);
     regulator_enable(regulator_get(&client->dev, "vdd"));
	return 0;

err_unregister:
	rmi_unregister_phys_device(rmi_phys);
err_gpio:
	if (pdata->gpio_config)
		pdata->gpio_config(pdata->gpio_data, false);
err_data:
	kfree(data);
err_phys:
	kfree(rmi_phys);
	return error;
}

static int __devexit rmi_i2c_remove(struct i2c_client *client)
{
	struct rmi_phys_device *phys = i2c_get_clientdata(client);
	struct rmi_device_platform_data *pd = client->dev.platform_data;

	disable_device(phys);
	rmi_unregister_phys_device(phys);
	kfree(phys->data);
	kfree(phys);

	if (pd->gpio_config)
		pd->gpio_config(&pd->gpio_data, false);

	return 0;
}

static const struct i2c_device_id rmi_id[] = {
	{ "rmi", 0 },
	{ "rmi_i2c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rmi_id);

static struct i2c_driver rmi_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "rmi_i2c"
	},
	.id_table	= rmi_id,
	.probe		= rmi_i2c_probe,
	.remove		= __devexit_p(rmi_i2c_remove),
};

static int __init rmi_i2c_init(void)
{
	return i2c_add_driver(&rmi_i2c_driver);
}

static void __exit rmi_i2c_exit(void)
{
	i2c_del_driver(&rmi_i2c_driver);
}

module_init(rmi_i2c_init);
module_exit(rmi_i2c_exit);


MODULE_AUTHOR("Christopher Heiny <cheiny@synaptics.com>");
MODULE_DESCRIPTION("RMI I2C driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(RMI_DRIVER_VERSION);