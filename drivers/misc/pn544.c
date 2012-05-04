/*
 * Copyright (C) 2010 Trusted Logic S.A.
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/pn544.h>

#include <plat/i2c-omap-gpio.h>
static OMAP_GPIO_I2C_CLIENT *pn544_i2c_client;

#define MAX_BUFFER_SIZE	512
#define MAX_LLC_FRAME_SIZE 0x20
#define MAX_TRY_I2C_READ 10
#define NFC_DEBUG 1

#define I2C_SPEED_KHZ	300

struct pn544_dev	{
	wait_queue_head_t	read_wq;
	struct mutex		read_mutex;
	struct i2c_client	*client;
	struct miscdevice	pn544_device;
	unsigned int 		ven_gpio;
	unsigned int 		firm_gpio;
	unsigned int		irq_gpio;
	bool			irq_enabled;
	spinlock_t		irq_enabled_lock;
	unsigned int 	irq;
};

static void pn544_disable_irq(struct pn544_dev *pn544_dev)
{
	unsigned long flags;
	
	spin_lock_irqsave(&pn544_dev->irq_enabled_lock, flags);
	if (pn544_dev->irq_enabled) {
		disable_irq_nosync(pn544_dev->irq);
		pn544_dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&pn544_dev->irq_enabled_lock, flags);
}

static irqreturn_t pn544_dev_irq_handler(int irq, void *dev_id)
{
	struct pn544_dev *pn544_dev = dev_id;

	if (!gpio_get_value(pn544_dev->irq_gpio)) {
		pr_info("%s : irq error\n", __func__);
		return IRQ_HANDLED;
	}

	pn544_disable_irq(pn544_dev);
	/* Wake up waiting readers */

	wake_up(&pn544_dev->read_wq);
	
	return IRQ_HANDLED;
}


static int pn544_i2c_read(unsigned char reg_addr, uint8_t *buf, int len)
{
	OMAP_GPIO_I2C_RD_DATA i2c_rd_param;
	int ret = 0;

	i2c_rd_param.reg_len = 0;
	i2c_rd_param.reg_addr = NULL;
	i2c_rd_param.rdata_len = len;
	i2c_rd_param.rdata = buf;
	
	ret = omap_gpio_i2c_read(pn544_i2c_client, &i2c_rd_param);
	
	pr_info("%s : read len = %d, read data[0]=0x%x,  ret %d",
		__func__, i2c_rd_param.rdata_len, i2c_rd_param.rdata[0], ret);


	return (ret == 0) ? i2c_rd_param.rdata_len : ret;
}

static int pn544_i2c_write(unsigned char reg_addr, uint8_t *buf, int len)
{
	OMAP_GPIO_I2C_WR_DATA i2c_wr_param;
	int ret = 0;

	i2c_wr_param.reg_len = 0;
	i2c_wr_param.reg_addr =  NULL;
	i2c_wr_param.wdata_len = len;
	i2c_wr_param.wdata =  buf;
	ret = omap_gpio_i2c_write(pn544_i2c_client, &i2c_wr_param);
	
	return (ret == 0) ? i2c_wr_param.wdata_len : ret;
}

static ssize_t pn544_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn544_dev *pn544_dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret;
	char datasizelength = 1;
	char datasize = 0;
	char readingWatchdog = 0;

	pr_info("pn544_dev_read\n");

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	mutex_lock(&pn544_dev->read_mutex);
	
wait_irq :

	if(!gpio_get_value(pn544_dev->irq_gpio)) {

		if (filp->f_flags & O_NONBLOCK) {
			pr_info("%s : O_NONBLOCK\n", __func__);
			ret = -EAGAIN;
			goto fail;
		}

		pn544_dev->irq_enabled = true;
		
		enable_irq(pn544_dev->irq);
		
		//pr_info("pn544 : +h %d \n",gpio_get_value(pn544_dev->irq_gpio));
			
		ret = wait_event_interruptible(pn544_dev->read_wq,
				gpio_get_value(pn544_dev->irq_gpio));
		//gpio_get_value(pn544_dev->irq_gpio));
		
		//pr_info("pn544 : - h, %d\n",ret); 
		 
		pn544_disable_irq(pn544_dev);

		if (ret) 
			goto fail;
	}
				
	/* Read data */

	if (count == 1)/*Normal mode*/
	{
//		ret = i2c_master_recv(pn544_dev->client, tmp, datasizelength);
		ret = pn544_i2c_read(pn544_i2c_client->addr, tmp, datasizelength);

		if (copy_to_user(buf, tmp, datasizelength)) {
				pr_warning("%s : failed to copy to user space\n", __func__);
				return -EFAULT;
		}

		/*If bad frame is received from pn544,
		 * we need to read again after waiting that IRQ is down
		 */
		if (MAX_LLC_FRAME_SIZE<tmp[0] && readingWatchdog<MAX_TRY_I2C_READ)
		{
			pr_info("%s : size>MAX_LLC_FRAME_SIZE received!",
				__func__);
			msleep(2);//sleep 2ms to wait IRQ fall down
			readingWatchdog++;
			goto wait_irq;
		}

		datasize = tmp[0];
		ret = pn544_i2c_read(pn544_i2c_client->addr, tmp, datasize);
//		ret = i2c_master_recv(pn544_dev->client, tmp, datasize);

		mutex_unlock(&pn544_dev->read_mutex);

		if (ret < 0) {
			pr_info("%s: i2c_master_recv returned %d\n", __func__, ret);
			return ret;
		}
		if (ret > datasize+1) {
			pr_info("%s: received too many bytes from i2c (%d)\n",
				__func__, ret);
			return -EIO;
		}

		if (copy_to_user(buf+1, tmp, ret)) {
			pr_info("%s : failed to copy to user space\n", __func__);
				return -EFAULT;
		}
		//add one more byte for size byte (read on first time)
		ret++;
	}
	else /*download mode*/
	{
//		ret = i2c_master_recv(pn544_dev->client, tmp, count);
		ret = pn544_i2c_read(pn544_i2c_client->addr, tmp, count);

		/*If bad frame is received from pn544,
		 * we need to read again after waiting that IRQ is down
		 */
		if (MAX_LLC_FRAME_SIZE<tmp[0] && readingWatchdog<MAX_TRY_I2C_READ)
		{
			pr_info("%s : size>MAX_LLC_FRAME_SIZE received!", __func__);
			msleep(2);//sleep 2ms to wait IRQ fall down
			readingWatchdog++;
			goto wait_irq;
		}

		mutex_unlock(&pn544_dev->read_mutex);

		if (ret < 0) {
			pr_info("%s: i2c_master_recv returned %d\n", __func__, ret);
			return ret;
		}

		if (ret > count) {
			pr_info("%s: received too many bytes from i2c (%d)\n",
			__func__, ret);
			return -EIO;
		}

		if (copy_to_user(buf, tmp, ret)) {
				pr_info("%s : failed to copy to user space\n", __func__);
			return -EFAULT;
		}
	}

	return ret;
	
fail:
	pr_info("%s : --------> fail\n", __func__);
	mutex_unlock(&pn544_dev->read_mutex);
	return ret;
}


static ssize_t pn544_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn544_dev  *pn544_dev;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	pn544_dev = filp->private_data;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (copy_from_user(tmp, buf, count)) {
		pr_info("%s : failed to copy from user space\n", __func__);
		return -EFAULT;
	}

	pr_debug("%s : writing %zu bytes.\n", __func__, count);
	/* Write data */
	ret = pn544_i2c_write(pn544_i2c_client->addr, tmp, count);

	pr_info("%s :  ret : %d \n" , __func__, ret);
		
	if (ret != count) {
		pr_info("%s : i2c_master_send returned %d\n", __func__, ret);
		ret = -EIO;
	}

	return ret;
}

static int pn544_dev_open(struct inode *inode, struct file *filp)
{
	struct pn544_dev *pn544_dev = container_of(filp->private_data,
						struct pn544_dev,
						pn544_device);
	pr_info("pn544_dev_open");
	filp->private_data = pn544_dev;

	pr_debug("%s : %d,%d\n", __func__, imajor(inode), iminor(inode));

	return 0;
}

static int pn544_dev_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	struct pn544_dev *pn544_dev = filp->private_data;

	switch (cmd) {
	case PN544_SET_PWR:

		if (arg == 2) {
			/* power on with firmware download (requires hw reset)
			 */
			pr_info("%s power on with firmware\n", __func__);
			gpio_set_value(pn544_dev->ven_gpio, 1);
			gpio_set_value(pn544_dev->firm_gpio, 1);
			msleep(10);
			gpio_set_value(pn544_dev->ven_gpio, 0);
			msleep(10);
			gpio_set_value(pn544_dev->ven_gpio, 1);
			msleep(10);
		} else if (arg == 1) {
			/* power on */
			pr_info("%s power on\n", __func__);
			gpio_set_value(pn544_dev->firm_gpio, 0);
			gpio_set_value(pn544_dev->ven_gpio, 1);
			msleep(10);
		} else  if (arg == 0) {
			/* power off */
			pr_info("%s power off\n", __func__);
			gpio_set_value(pn544_dev->firm_gpio, 0);
			gpio_set_value(pn544_dev->ven_gpio, 0);
			msleep(10);
		} else {
			pr_info("%s bad arg %lu\n", __func__, arg);
			return -EINVAL;
		}
		break;
	default:
		pr_info("%s bad ioctl %u\n", __func__, cmd);
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations pn544_dev_fops = {
	.owner	= THIS_MODULE,
	.llseek	= no_llseek,
	.read	= pn544_dev_read,
	.write	= pn544_dev_write,
	.open	= pn544_dev_open,
	.ioctl  	= pn544_dev_ioctl,
};

static int pn544_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int ret;
	struct pn544_i2c_platform_data *platform_data;
	struct pn544_dev *pn544_dev;

	platform_data = client->dev.platform_data;
	if (platform_data == NULL) {
		pr_info("%s : nfc probe fail\n", __func__);
		return  -ENODEV;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_info("%s : need I2C_FUNC_I2C\n", __func__);
		return  -ENODEV;
	}

	ret = gpio_request(platform_data->irq_gpio, "nfc_int");
	if (ret)
		return  -ENODEV;
	ret = gpio_request(platform_data->ven_gpio, "nfc_ven");
	if (ret)
		goto err_ven;
	ret = gpio_request(platform_data->firm_gpio, "nfc_firm");
	if (ret)
		goto err_firm;

	pn544_dev = kzalloc(sizeof(*pn544_dev), GFP_KERNEL);

	if (pn544_dev == NULL) {
		dev_err(&client->dev,
				"failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	pn544_dev->irq_gpio = platform_data->irq_gpio;
	pn544_dev->ven_gpio  = platform_data->ven_gpio;
	pn544_dev->firm_gpio  = platform_data->firm_gpio;
 
//#if !defined(CONFIG_USE_GPIO_I2C)
	pn544_dev->client   = client;
//#endif
	/* init mutex and queues */
	init_waitqueue_head(&pn544_dev->read_wq);
	mutex_init(&pn544_dev->read_mutex);
	spin_lock_init(&pn544_dev->irq_enabled_lock);
 
	pn544_dev->pn544_device.minor = MISC_DYNAMIC_MINOR;
	pn544_dev->pn544_device.name = "pn544";
	pn544_dev->pn544_device.fops = &pn544_dev_fops;

	ret = misc_register(&pn544_dev->pn544_device);
	if (ret) {
		pr_info("%s : misc_register failed\n", __FILE__);
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	pn544_dev->irq_enabled = true;
	gpio_direction_input(pn544_dev->irq_gpio);
	gpio_direction_output(pn544_dev->ven_gpio,0);
	gpio_direction_output(pn544_dev->firm_gpio,0);

	pn544_dev->irq = OMAP_GPIO_IRQ(pn544_dev->irq_gpio);
	
	pr_info("%s : IRQ = %d GPIO = %d\n", __func__,
		pn544_dev->irq, OMAP_GPIO_NFC_IRQ );
	ret = request_irq(pn544_dev->irq, pn544_dev_irq_handler,
			  IRQF_TRIGGER_RISING, "pn544", pn544_dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	pn544_disable_irq(pn544_dev);
	i2c_set_clientdata(client, pn544_dev);

	
	return 0;

err_request_irq_failed:
	misc_deregister(&pn544_dev->pn544_device);
err_misc_register:
	mutex_destroy(&pn544_dev->read_mutex);
	kfree(pn544_dev);
err_exit:
	gpio_free(platform_data->firm_gpio);
err_firm:
	gpio_free(platform_data->ven_gpio);
err_ven:
	gpio_free(platform_data->irq_gpio);
	return ret;
}

static int pn544_remove(struct i2c_client *client)
{
	struct pn544_dev *pn544_dev;
	pr_info("pn544_remove \n");

	pn544_dev = i2c_get_clientdata(client);
	free_irq(client->irq, pn544_dev);
	misc_deregister(&pn544_dev->pn544_device);
	mutex_destroy(&pn544_dev->read_mutex);
	gpio_free(pn544_dev->irq_gpio);
	gpio_free(pn544_dev->ven_gpio);
	gpio_free(pn544_dev->firm_gpio);
	kfree(pn544_dev);

	return 0;
}

static const struct i2c_device_id pn544_id[] = {
	{ "pn544", 0 },
	{ }
};

static struct i2c_driver pn544_driver = {
	.id_table	= pn544_id,
	.probe		= pn544_probe,
	.remove		= pn544_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "pn544",
	},
};

/*
 * module load/unload record keeping
 */

static int __init pn544_dev_init(void)
{
	pr_info("pn544_dev_init \n");
	
	pn544_i2c_client=
		omap_gpio_i2c_init(OMAP_GPIO_NFC_SDA, OMAP_GPIO_NFC_SCL,
				0x2b, I2C_SPEED_KHZ);
	if (pn544_i2c_client == NULL) {
		pr_info( "pn544 omap_gpio_i2c_init failed!\n");
	}

	return i2c_add_driver(&pn544_driver);
}
module_init(pn544_dev_init);

static void __exit pn544_dev_exit(void)
{
	pr_info("pn544_dev_exit \n");
#if defined(CONFIG_USE_GPIO_I2C)
	omap_gpio_i2c_deinit(pn544_i2c_client);
#endif
	i2c_del_driver(&pn544_driver);
}
module_exit(pn544_dev_exit);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION("NFC PN544 driver");
MODULE_LICENSE("GPL");