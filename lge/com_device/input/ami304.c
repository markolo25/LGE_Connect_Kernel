/* drivers/i2c/chips/ami304.c - AMI304 compass driver
 *
 * Copyright (C) 2009 AMIT Technology Inc.
 * Author: Kyle Chen <sw-support@amit-inc.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/input/ami304.h>
#include <linux/kobject.h>
#include <mach/board_lge.h>

//#define DEBUG_PRINT //2011.03.07 woochang.chun@lge.com
#define AMI304_TEST
//#define AMI304_LOOPDATA_TEST

#define AMI304_DRV_NAME			"ami304"
#define DRIVER_VERSION			"1.0.11.19"
static struct i2c_client *ami304_i2c_client = NULL;

#if 0 //2011.02.16 woochang.chun, for the compile error fix 
/* Addresses to scan */
static unsigned short normal_i2c[] = { AMI304_I2C_ADDRESS, I2C_CLIENT_END };
#endif

/* Insmod parameters */
//I2C_CLIENT_INSMOD;

struct _ami304_data {
	rwlock_t lock;
	int chipset;
	int mode;
	int rate;
	volatile int updated;
} ami304_data;

typedef struct {
	int x;
	int y;
	int z;
}ami304_vec_t;

typedef struct {
	unsigned long pedo_step;
	unsigned long pedo_time;
	int pedo_stat;
}ami304_pedo_t;

struct _ami304mid_data {
	rwlock_t datalock;
	rwlock_t ctrllock;	
	int controldata[AMI304_CB_LENGTH];	
	int pedometerparam[AMI304_PD_LENGTH];
	int yaw;
	int roll;
	int pitch;
	ami304_vec_t nm;
	ami304_vec_t na;
	ami304_vec_t gyro;
	ami304_pedo_t pedo;	
	int status;
} ami304mid_data;

struct ami304_i2c_data {
	struct input_dev *input_dev;
	struct i2c_client *client;
};

static atomic_t dev_open_count;
static atomic_t hal_open_count;
static atomic_t daemon_open_count;

static u8 i2c_read_addr, i2c_read_len;

static int AMI304_I2c_Read(u8 regaddr, u8 *buf, u8 buf_len)
{
	int res = 0;

	res = i2c_master_send(ami304_i2c_client, &regaddr, 1);
	if (res<=0) goto exit_AMI304_I2c_Read;
	res = i2c_master_recv(ami304_i2c_client, buf, buf_len);
	if (res<=0) goto exit_AMI304_I2c_Read;

#if defined(AMI304_LOOPDATA_TEST)
	printk(KERN_INFO "%s : reg_adr=0x%x, buf_len = %d\n",__FUNCTION__, regaddr, buf_len);
	printk(KERN_INFO "%s : buf[0]=0x%x, buf[1]=0x%x, buf[2]=0x%x, buf[3]=0x%x, buf[4]=0x%x, buf[5]=0x%x, buf[6]=0x%x, buf[7]=0x%x\n",
		__FUNCTION__, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
#endif

	return res;

exit_AMI304_I2c_Read:
#if defined(AMI304_TEST)
	printk(KERN_ERR "%s AMI304_I2c_Read error res=%d\n",__FUNCTION__, res);
#endif
	return res;
}

static int AMI304_I2c_Write(u8 reg_adr, u8 *buf, u8 buf_len)
{
	int res = 0;
//	u8 databuf[buf_len+2];
	u8 databuf[64];

	databuf[0] = reg_adr;
	memcpy(&databuf[1], buf, buf_len);
	databuf[buf_len+1] = 0x00;

#if defined(AMI304_LOOPDATA_TEST)
	printk(KERN_INFO "%s : reg_adr=0x%x, buf_len = %d\n",__FUNCTION__, databuf[0], buf_len);
	printk(KERN_INFO "%s : buf[0]=0x%x, buf[1]=0x%x, buf[2]=0x%x, buf[3]=0x%x, buf[4]=0x%x, buf[5]=0x%x, buf[6]=0x%x, buf[7]=0x%x\n",
		__FUNCTION__,databuf[1],databuf[2],databuf[3],databuf[4],databuf[5],databuf[6],databuf[7], databuf[8]);
#endif

	res = i2c_master_send(ami304_i2c_client, databuf, buf_len+1);	
	if (res<=0) goto exit_AMI304_I2c_Write;	
	return res;

exit_AMI304_I2c_Write:
#if defined(AMI304_TEST)
	printk(KERN_ERR "%s AMI304_I2c_Write error res=%d\n",__FUNCTION__, res);
#endif
	return res;
}

static int AMI304_Chipset_Init(int mode, int chipset)
{
	u8 databuf[10];
	u8 regaddr;
	u8 ctrl1, ctrl2, ctrl3;
	unsigned char ctrl4[2];
	
	regaddr = AMI304_REG_CTRL1;
	i2c_master_send(ami304_i2c_client, &regaddr, 1);
	i2c_master_recv(ami304_i2c_client, &ctrl1, 1);

	regaddr = AMI304_REG_CTRL2;
	i2c_master_send(ami304_i2c_client, &regaddr, 1);
	i2c_master_recv(ami304_i2c_client, &ctrl2, 1);
	
	regaddr = AMI304_REG_CTRL3;
	i2c_master_send(ami304_i2c_client, &regaddr, 1);
	i2c_master_recv(ami304_i2c_client, &ctrl3, 1);		

//	regaddr = AMI304_REG_CTRL4; //2 bytes
//	i2c_master_send(ami304_i2c_client, &regaddr, 1);
//	i2c_master_recv(ami304_i2c_client, &(ctrl4[0]), 2);
	
	databuf[0] = AMI304_REG_CTRL1;
	if( mode==AMI304_FORCE_MODE )
	{
		databuf[1] = ctrl1 | AMI304_CTRL1_PC1 | AMI304_CTRL1_FS1_FORCE;
		write_lock(&ami304_data.lock);
		ami304_data.mode = AMI304_FORCE_MODE;
		write_unlock(&ami304_data.lock);			
	}
	else	
	{
		databuf[1] = ctrl1 | AMI304_CTRL1_PC1 | AMI304_CTRL1_FS1_NORMAL | AMI304_CTRL1_ODR1;
		write_lock(&ami304_data.lock);
		ami304_data.mode = AMI304_NORMAL_MODE;
		write_unlock(&ami304_data.lock);			
	}
	i2c_master_send(ami304_i2c_client, databuf, 2);		
	
	databuf[0] = AMI304_REG_CTRL2;
	databuf[1] = ctrl2 | AMI304_CTRL2_DREN | AMI304_CTRL2_DRP;
	i2c_master_send(ami304_i2c_client, databuf, 2);		
	
	databuf[0] = AMI304_REG_CTRL3;
	databuf[1] = ctrl3 | AMI304_CTRL3_B0_LO_CLR;
	i2c_master_send(ami304_i2c_client, databuf, 2);
	
	databuf[0] = AMI304_REG_CTRL4;	
	if( chipset == AMI304_CHIPSET ) //AMI304
	{
//		ctrl4[1]   = ctrl4[1] & AMI304_CTRL4_COMPASS_MODE; 	 //0x5D
		ctrl4[0] = 0x00;
		ctrl4[1] = 0x00;
	}
	else	//AMI306	//AMI306_CHIPSET
	{
//		ctrl4[1]   = ctrl4[1] | AMI306_CTRL4_HIGHSPEED_MODE; //0x5D
		ctrl4[0] = 0x7e;
		ctrl4[1] = 0xa0;
	}	
	databuf[1] = ctrl4[0];
	databuf[2] = ctrl4[1];
	i2c_master_send(ami304_i2c_client, databuf, 3);				
	
	return 0;
}

static int AMI304_SetMode(int newmode)
{
	int mode = 0;
	int chipset = 0;
	
	read_lock(&ami304_data.lock);
	mode = ami304_data.mode;
	chipset = ami304_data.chipset;
	read_unlock(&ami304_data.lock);		
	
	if (mode == newmode) 
		return 0;	
			
	return AMI304_Chipset_Init(newmode, chipset);
}

static int AMI304_ReadChipInfo(char *buf, int bufsize)
{
	if ((!buf)||(bufsize<=30))
		return -1;
		
	if (!ami304_i2c_client)
	{
		*buf = 0;
		return -2;
	}

	if (ami304_data.chipset == AMI306_CHIPSET)	
	{
		sprintf(buf, "AMI306 Chip");
	}
	else
	{
		sprintf(buf, "AMI304 Chip");
	}

	return 0;
}

static int AMI304_WIA(char *wia, int bufsize)
{
	char cmd;
	unsigned char databuf[10];

	if ((!wia)||(bufsize<=30))
		return -1;	
		
	if (!ami304_i2c_client)
	{
		*wia = 0;
		return -2;
	}

	cmd = AMI304_REG_WIA;
	i2c_master_send(ami304_i2c_client, &cmd, 1);
	udelay(20);
	i2c_master_recv(ami304_i2c_client, &(databuf[0]), 1);	
	
	sprintf(wia, "%02x", databuf[0]);
	
	return 0;
}

static int Identify_AMI_Chipset(void)
{
	char strbuf[AMI304_BUFSIZE];
	int WIARet = 0;
	int ret;
	
	if( (ret=AMI304_WIA(strbuf, AMI304_BUFSIZE))!=0 )
		return ret;
		
	sscanf(strbuf, "%x", &WIARet);	
	
	if (WIARet == AMI306_WIA_VALUE)	
	{
		ami304_data.chipset = AMI306_CHIPSET;
	}
	else
	{
		ami304_data.chipset = AMI304_CHIPSET;
	}
	
	return 0;
}

static int AMI304_ReadSensorData(char *buf, int bufsize)
{
	char cmd;
	int mode = 0;	
	unsigned char databuf[10];
	int res = 0;

	if ((!buf)||(bufsize<=80))
		return -1;
	if (!ami304_i2c_client)
	{
		*buf = 0;
		return -2;
	}
	
	read_lock(&ami304_data.lock);	
	mode = ami304_data.mode;
	read_unlock(&ami304_data.lock);		

	databuf[0] = AMI304_REG_CTRL3;
	databuf[1] = AMI304_CTRL3_FORCE_BIT;
	res = i2c_master_send(ami304_i2c_client, databuf, 2);	
	if (res<=0) goto exit_AMI304_ReadSensorData;
	//udelay(700);
	msleep(1);
	// We can read all measured data in once
	cmd = AMI304_REG_DATAXH;
	res = i2c_master_send(ami304_i2c_client, &cmd, 1);	
	if (res<=0) goto exit_AMI304_ReadSensorData;
//	udelay(20);
	res = i2c_master_recv(ami304_i2c_client, &(databuf[0]), 6);
	if (res<=0) goto exit_AMI304_ReadSensorData;

	sprintf(buf, "%02x %02x %02x %02x %02x %02x", databuf[0], databuf[1], databuf[2], databuf[3], databuf[4], databuf[5]);
#if defined(AMI304_LOOPDATA_TEST)
	{
		int mx=0;
		int my=0;
		int mz=0;
		int tmp_x=databuf[1];
		int tmp_y=databuf[3];
		int tmp_z=databuf[5];
		
		mx = tmp_x << 8 | databuf[0];
		my = tmp_y << 8 | databuf[2];
		mz = tmp_z << 8 | databuf[4];
		if (mx>32768)  mx = mx-65536;
		if (my>32768)  my = my-65536;
		if (mz>32768)  mz = mz-65536;
//		mx += 2048;
//		my += 2048;
//		mz += 2048;
		printk(KERN_INFO "%s Magnetic Raw Data : mx=%d my=%d mz=%d\n",__FUNCTION__, mx, my, mz);
	}
#endif
exit_AMI304_ReadSensorData:
	if (res<=0) {
		printk(KERN_ERR "%s I2C error: res value=%d\n",__FUNCTION__, res);
		return -3;
	}
	return 0;
}

static int AMI304_ReadPostureData(char *buf, int bufsize)
{
	if ((!buf)||(bufsize<=80))
		return -1;

	read_lock(&ami304mid_data.datalock);
	sprintf(buf, "%d %d %d %d", ami304mid_data.yaw, ami304mid_data.pitch, ami304mid_data.roll, ami304mid_data.status);
	read_unlock(&ami304mid_data.datalock);
	return 0;
}

static int AMI304_ReadCaliData(char *buf, int bufsize)
{
	if ((!buf)||(bufsize<=80))
		return -1;

	read_lock(&ami304mid_data.datalock);
	sprintf(buf, "%d %d %d %d %d %d %d", ami304mid_data.nm.x, ami304mid_data.nm.y, ami304mid_data.nm.z,ami304mid_data.na.x,ami304mid_data.na.y,ami304mid_data.na.z,ami304mid_data.status);
	read_unlock(&ami304mid_data.datalock);
	return 0;
}

static int AMI304_ReadGyroData(char *buf, int bufsize)
{
	if ((!buf)||(bufsize<=80))
		return -1;

	read_lock(&ami304mid_data.datalock);
	sprintf(buf, "%d %d %d", ami304mid_data.gyro.x, ami304mid_data.gyro.y, ami304mid_data.gyro.z);
	read_unlock(&ami304mid_data.datalock);
	return 0;
}

static int AMI304_ReadPedoData(char *buf, int bufsize)
{
	if ((!buf)||(bufsize<=80))
		return -1;

	read_lock(&ami304mid_data.datalock);
	sprintf(buf, "%ld %ld %d", ami304mid_data.pedo.pedo_step, ami304mid_data.pedo.pedo_time, ami304mid_data.pedo.pedo_stat);
	read_unlock(&ami304mid_data.datalock);
	return 0;		
}

static int AMI304_ReadMiddleControl(char *buf, int bufsize)
{
	if ((!buf)||(bufsize<=80))
		return -1;

	read_lock(&ami304mid_data.ctrllock);
	sprintf(buf, "%d %d %d %d %d %d %d %d %d %d", 
		ami304mid_data.controldata[AMI304_CB_LOOPDELAY], ami304mid_data.controldata[AMI304_CB_RUN], ami304mid_data.controldata[AMI304_CB_ACCCALI], ami304mid_data.controldata[AMI304_CB_MAGCALI],
		ami304mid_data.controldata[AMI304_CB_ACTIVESENSORS], ami304mid_data.controldata[AMI304_CB_PD_RESET], ami304mid_data.controldata[AMI304_CB_PD_EN_PARAM], ami304mid_data.controldata[AMI304_CB_QWERTY],
		ami304mid_data.controldata[AMI304_CB_CHANGE_WINDOW], ami304mid_data.controldata[AMI304_CB_UNDEFINE_2] );
	read_unlock(&ami304mid_data.ctrllock);
	return 0;
}

static int AMI304_Report_Value(int iEnable)
{
	int controlbuf[AMI304_CB_LENGTH];
	struct ami304_i2c_data *data = i2c_get_clientdata(ami304_i2c_client);
	
	if( !iEnable )
		return -1;

	read_lock(&ami304mid_data.ctrllock);
	memcpy(controlbuf, &ami304mid_data.controldata[0], sizeof(controlbuf));
	read_unlock(&ami304mid_data.ctrllock);			

	if(controlbuf[AMI304_CB_ACTIVESENSORS] & AMIT_BIT_ACCELEROMETER)
	{
		input_report_abs(data->input_dev, ABS_X, ami304mid_data.na.x);/* x-axis raw acceleration */
		input_report_abs(data->input_dev, ABS_Y, ami304mid_data.na.y);/* y-axis raw acceleration */
		input_report_abs(data->input_dev, ABS_Z, ami304mid_data.na.z);/* z-axis raw acceleration */
#if defined(AMI304_LOOPDATA_TEST)
		printk(KERN_INFO "%s Acceleration : acc_x=%d acc_y=%d acc_z=%d\n",__FUNCTION__, ami304mid_data.na.x, ami304mid_data.na.y, ami304mid_data.na.z);
#endif
	}
	if(controlbuf[AMI304_CB_ACTIVESENSORS] & AMIT_BIT_MAGNETIC_FIELD)
	{
		input_report_abs(data->input_dev, ABS_HAT0X, ami304mid_data.nm.x); /* x-axis of raw magnetic vector */
		input_report_abs(data->input_dev, ABS_HAT0Y, ami304mid_data.nm.y); /* y-axis of raw magnetic vector */
		input_report_abs(data->input_dev, ABS_BRAKE, ami304mid_data.nm.z); /* z-axis of raw magnetic vector */
		input_report_abs(data->input_dev, ABS_WHEEL, ami304mid_data.status);/* status of magnetic sensor */
#if defined(AMI304_LOOPDATA_TEST)
		printk(KERN_INFO "%s Magnetic : m_x=%d m_y=%d m_z=%d status=%d\n",__FUNCTION__, ami304mid_data.nm.x, ami304mid_data.nm.y, ami304mid_data.nm.z, ami304mid_data.status);
#endif
	}
	if(controlbuf[AMI304_CB_ACTIVESENSORS] & AMIT_BIT_ORIENTATION)
	{
		input_report_abs(data->input_dev, ABS_RX, ami304mid_data.yaw);	/* yaw */
		input_report_abs(data->input_dev, ABS_RY, ami304mid_data.pitch);/* pitch */
		input_report_abs(data->input_dev, ABS_RZ, ami304mid_data.roll);/* roll */
		input_report_abs(data->input_dev, ABS_RUDDER, ami304mid_data.status);/* status of orientation sensor */
#if defined(AMI304_LOOPDATA_TEST)
		printk(KERN_INFO "%s Orientation : yaw=%d pitch=%d roll=%d status=%d\n",__FUNCTION__, ami304mid_data.yaw, ami304mid_data.pitch, ami304mid_data.roll, ami304mid_data.status);
#endif
	}
	if(controlbuf[AMI304_CB_ACTIVESENSORS] & AMIT_BIT_GYROSCOPE)
	{
		input_report_abs(data->input_dev, ABS_HAT1X, ami304mid_data.gyro.x);/* x-axis of gyro sensor */
		input_report_abs(data->input_dev, ABS_HAT1Y, ami304mid_data.gyro.y);/* y-axis of gyro sensor */
		input_report_abs(data->input_dev, ABS_THROTTLE, ami304mid_data.gyro.z);/* z-axis of gyro sensor */
		
#if defined(AMI304_LOOPDATA_TEST)
		printk(KERN_INFO "%s Gyro : x=%d y=%d z=%d\n",__FUNCTION__, ami304mid_data.gyro.x, ami304mid_data.gyro.y, ami304mid_data.gyro.z);
#endif
	}

	input_sync(data->input_dev);
	   
	return 0;
}

static ssize_t show_chipinfo_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadChipInfo(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);		
}

static ssize_t show_sensordata_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadSensorData(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);
}

static ssize_t show_posturedata_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadPostureData(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);			
}

static ssize_t show_calidata_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadCaliData(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);			
}

static ssize_t show_gyrodata_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadGyroData(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);			
}

static ssize_t show_midcontrol_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_ReadMiddleControl(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);			
}

static ssize_t store_midcontrol_value(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	write_lock(&ami304mid_data.ctrllock);
	memcpy(&ami304mid_data.controldata[0], buf, sizeof(int)*AMI304_CB_LENGTH);	
 	write_unlock(&ami304mid_data.ctrllock);		
	return count;			
}

static ssize_t show_mode_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	int mode=0;
	read_lock(&ami304_data.lock);
	mode = ami304_data.mode;
	read_unlock(&ami304_data.lock);		
	return sprintf(buf, "%d\n", mode);			
}

static ssize_t store_mode_value(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int mode = 0;
	sscanf(buf, "%d", &mode);	
 	AMI304_SetMode(mode);
	return count;			
}

static ssize_t show_wia_value(struct device *dev, struct device_attribute *attr, char *buf)
{
	char strbuf[AMI304_BUFSIZE];
	AMI304_WIA(strbuf, AMI304_BUFSIZE);
	return sprintf(buf, "%s\n", strbuf);			
}

static DEVICE_ATTR(chipinfo, S_IRUGO, show_chipinfo_value, NULL);
static DEVICE_ATTR(sensordata, S_IRUGO, show_sensordata_value, NULL);
static DEVICE_ATTR(posturedata, S_IRUGO, show_posturedata_value, NULL);
static DEVICE_ATTR(calidata, S_IRUGO, show_calidata_value, NULL);
static DEVICE_ATTR(gyrodata, S_IRUGO, show_gyrodata_value, NULL);
static DEVICE_ATTR(midcontrol, S_IRUGO | S_IWUSR, show_midcontrol_value, store_midcontrol_value );
static DEVICE_ATTR(mode, S_IRUGO | S_IWUSR, show_mode_value, store_mode_value );
static DEVICE_ATTR(wia, S_IRUGO, show_wia_value, NULL);

static struct attribute *ami304_attributes[] = {
	&dev_attr_chipinfo.attr,
	&dev_attr_sensordata.attr,
	&dev_attr_posturedata.attr,
	&dev_attr_calidata.attr,
	&dev_attr_gyrodata.attr,
	&dev_attr_midcontrol.attr,
	&dev_attr_mode.attr,
	&dev_attr_wia.attr,
	NULL
};

static struct attribute_group ami304_attribute_group = {
	.attrs = ami304_attributes
};

static int ami304_open(struct inode *inode, struct file *file)
{	
	int ret = -1;
	if( atomic_cmpxchg(&dev_open_count, 0, 1)==0 ) {
		printk(KERN_INFO "Open device node:ami304\n");
		ret = nonseekable_open(inode, file);
	}	
	return ret;
}

static int ami304_release(struct inode *inode, struct file *file)
{
	atomic_set(&dev_open_count, 0);
	printk(KERN_INFO "Release device node:ami304\n");		
	return 0;
}

static int ami304_ioctl(struct inode *inode, struct file *file, unsigned int cmd,unsigned long arg)
{
	char strbuf[AMI304_BUFSIZE];
	int controlbuf[AMI304_CB_LENGTH];
	int valuebuf[4];
	int calidata[7];
	int gyrodata[3];
	long pedodata[3];	
	int pedoparam[AMI304_PD_LENGTH];
	void __user *data;
	int retval=0;
	int mode=0,chipset=0;
	int iEnReport;

    //check the authority is root or not
//    if(!capable(CAP_SYS_ADMIN)) {
//        retval = -EPERM;
//        goto err_out;
//	}
		
	switch (cmd) {
		case AMI304_IOCTL_INIT:
			read_lock(&ami304_data.lock);
			mode = ami304_data.mode;
			chipset = ami304_data.chipset;
			read_unlock(&ami304_data.lock);
			AMI304_Chipset_Init(mode, chipset);			
			break;
		
		case AMI304_IOCTL_READ_CHIPINFO:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadChipInfo(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
			break;

		case AMI304_IOCTL_READ_SENSORDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadSensorData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
			break;				
						
		case AMI304_IOCTL_READ_POSTUREDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadPostureData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
			break;			
	 
	 	case AMI304_IOCTL_WRITE_POSTUREDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&valuebuf, data, sizeof(valuebuf))) {
				retval = -EFAULT;
				goto err_out;
			}				
			write_lock(&ami304mid_data.datalock);
			ami304mid_data.yaw   = valuebuf[0];
			ami304mid_data.pitch = valuebuf[1];
			ami304mid_data.roll  = valuebuf[2];
			ami304mid_data.status = valuebuf[3];
			write_unlock(&ami304mid_data.datalock);		 	
	 		break;
	 	 
	        case AMI304_IOCTL_READ_CALIDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadCaliData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
	        	break;
	        
	    case AMI304_IOCTL_WRITE_CALIDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&calidata, data, sizeof(calidata))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.datalock);			
			ami304mid_data.nm.x = calidata[0];
			ami304mid_data.nm.y = calidata[1];
			ami304mid_data.nm.z = calidata[2];
			ami304mid_data.na.x = calidata[3];
			ami304mid_data.na.y = calidata[4];
			ami304mid_data.na.z = calidata[5];
			ami304mid_data.status = calidata[6];
			write_unlock(&ami304mid_data.datalock);
	    	break;    

		case AMI304_IOCTL_READ_GYRODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadGyroData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
			break;
			
		case AMI304_IOCTL_WRITE_GYRODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&gyrodata, data, sizeof(gyrodata))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.datalock);			
			ami304mid_data.gyro.x = gyrodata[0];
			ami304mid_data.gyro.y = gyrodata[1];
			ami304mid_data.gyro.z = gyrodata[2];
			write_unlock(&ami304mid_data.datalock);		
			break;
			
		case AMI304_IOCTL_READ_PEDODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadPedoData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
			break;

		case AMI304_IOCTL_WRITE_PEDODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&pedodata, data, sizeof(pedodata))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.datalock);			
			ami304mid_data.pedo.pedo_step = pedodata[0];
			ami304mid_data.pedo.pedo_time = pedodata[1];
			ami304mid_data.pedo.pedo_stat = (int)pedodata[2];
			write_unlock(&ami304mid_data.datalock);  		
			break;

		case AMI304_IOCTL_READ_PEDOPARAM:
            read_lock(&ami304mid_data.ctrllock);
			memcpy(pedoparam, &ami304mid_data.pedometerparam[0], sizeof(pedoparam));
			read_unlock(&ami304mid_data.ctrllock);			
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_to_user(data, pedoparam, sizeof(pedoparam))) {
				retval = -EFAULT;
				goto err_out;
			}			
			break;
			
        case AMI304_IOCTL_WRITE_PEDOPARAM:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(pedoparam, data, sizeof(pedoparam))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.ctrllock);
			memcpy(&ami304mid_data.pedometerparam[0], pedoparam, sizeof(pedoparam));
			write_unlock(&ami304mid_data.ctrllock);
	        break;	
	        
	        case AMI304_IOCTL_READ_CONTROL:
			read_lock(&ami304mid_data.ctrllock);
			memcpy(controlbuf, &ami304mid_data.controldata[0], sizeof(controlbuf));
			read_unlock(&ami304mid_data.ctrllock);			
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_to_user(data, controlbuf, sizeof(controlbuf))) {
				retval = -EFAULT;
				goto err_out;
			}						        
	        	break;

		case AMI304_IOCTL_WRITE_CONTROL:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(controlbuf, data, sizeof(controlbuf))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.ctrllock);
			memcpy(&ami304mid_data.controldata[0], controlbuf, sizeof(controlbuf));
			write_unlock(&ami304mid_data.ctrllock);		
			break;
			
		case AMI304_IOCTL_WRITE_MODE:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&mode, data, sizeof(mode))) {
				retval = -EFAULT;
				goto err_out;
			}		
			AMI304_SetMode(mode);				
			break;
					        				
		case AMI304_IOCTL_WRITE_REPORT:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&iEnReport, data, sizeof(iEnReport))) {
				retval = -EFAULT;
				goto err_out;
			}				
			AMI304_Report_Value(iEnReport);		
			break;
		
		case AMI304_IOCTL_READ_WIA:
			data = (void __user *) arg;
			if (data == NULL)
				break;		
			AMI304_WIA(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}								
			break;
					        				
		default:
			printk(KERN_ERR "%s not supported = 0x%04x", __FUNCTION__, cmd);
			retval = -ENOIOCTLCMD;
			break;
	}
	
err_out:
	return retval;	
}

static int ami304daemon_open(struct inode *inode, struct file *file)
{
	int ret = -1;
	if( atomic_cmpxchg(&daemon_open_count, 0, 1)==0 ) {
		printk(KERN_INFO "Open device node:ami304daemon\n");
		ret = 0;
	}
	return ret;	
}

static int ami304daemon_release(struct inode *inode, struct file *file)
{
	atomic_set(&daemon_open_count, 0);
	printk(KERN_INFO "Release device node:ami304daemon\n");	
	return 0;
}

static int ami304daemon_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	   unsigned long arg)
{
	int valuebuf[4];
	int calidata[7];
	int gyrodata[3];
	long pedodata[3];
	int controlbuf[AMI304_CB_LENGTH];
	char strbuf[AMI304_BUFSIZE];
	int pedoparam[AMI304_PD_LENGTH];	
	char i2creaddata[3];
	void __user *data;
	int retval=0;
	int mode;
	int iEnReport;

    //check the authority is root or not
//    if(!capable(CAP_SYS_ADMIN)) {
//        retval = -EPERM;
//        goto err_out;
//    }
		
	switch (cmd) {
			
		case AMI304DAE_IOCTL_GET_SENSORDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadSensorData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}
			break;
				
		case AMI304DAE_IOCTL_SET_POSTURE:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&valuebuf, data, sizeof(valuebuf))) {
				retval = -EFAULT;
				goto err_out;
			}				
			write_lock(&ami304mid_data.datalock);
			ami304mid_data.yaw   = valuebuf[0];
			ami304mid_data.pitch = valuebuf[1];
			ami304mid_data.roll  = valuebuf[2];
			ami304mid_data.status = valuebuf[3];
			write_unlock(&ami304mid_data.datalock);	
			break;		
			
		case AMI304DAE_IOCTL_SET_CALIDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&calidata, data, sizeof(calidata))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.datalock);			
			ami304mid_data.nm.x = calidata[0];
			ami304mid_data.nm.y = calidata[1];
			ami304mid_data.nm.z = calidata[2];
			ami304mid_data.na.x = calidata[3];
			ami304mid_data.na.y = calidata[4];
			ami304mid_data.na.z = calidata[5];
			ami304mid_data.status = calidata[6];
			write_unlock(&ami304mid_data.datalock);				
			break;								

        case AMI304DAE_IOCTL_SET_GYRODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&gyrodata, data, sizeof(gyrodata))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.datalock);			
			ami304mid_data.gyro.x = gyrodata[0];
			ami304mid_data.gyro.y = gyrodata[1];
			ami304mid_data.gyro.z = gyrodata[2];
			write_unlock(&ami304mid_data.datalock);
        	break;
        
        case AMI304DAE_IOCTL_SET_PEDODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&pedodata, data, sizeof(pedodata))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.datalock);			
			ami304mid_data.pedo.pedo_step = pedodata[0];
			ami304mid_data.pedo.pedo_time = pedodata[1];
			ami304mid_data.pedo.pedo_stat = (int)pedodata[2];
			write_unlock(&ami304mid_data.datalock);				
			break;								

		case AMI304DAE_IOCTL_GET_PEDOPARAM:
			read_lock(&ami304mid_data.ctrllock);
			memcpy(pedoparam, &ami304mid_data.pedometerparam[0], sizeof(pedoparam));
			read_unlock(&ami304mid_data.ctrllock);			
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_to_user(data, pedoparam, sizeof(pedoparam))) {
				retval = -EFAULT;
				goto err_out;
			}					
			break;

		case AMI304DAE_IOCTL_SET_PEDOPARAM:
		    data = (void __user *) arg;			
			if (data == NULL)
				break;	
			if (copy_from_user(pedoparam, data, sizeof(pedoparam))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.ctrllock);
			memcpy(&ami304mid_data.pedometerparam[0], pedoparam, sizeof(pedoparam));
			write_unlock(&ami304mid_data.ctrllock);					
			break;	

		case AMI304DAE_IOCTL_GET_CONTROL:
			read_lock(&ami304mid_data.ctrllock);
			memcpy(controlbuf, &ami304mid_data.controldata[0], sizeof(controlbuf));
			read_unlock(&ami304mid_data.ctrllock);			
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_to_user(data, controlbuf, sizeof(controlbuf))) {
				retval = -EFAULT;
				goto err_out;
			}					
			break;		
			
		case AMI304DAE_IOCTL_SET_CONTROL:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(controlbuf, data, sizeof(controlbuf))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.ctrllock);
			memcpy(&ami304mid_data.controldata[0], controlbuf, sizeof(controlbuf));
			write_unlock(&ami304mid_data.ctrllock);
			break;	
	
		case AMI304DAE_IOCTL_SET_MODE:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&mode, data, sizeof(mode))) {
				retval = -EFAULT;
				goto err_out;
			}		
			AMI304_SetMode(mode);				
			break;
								
		//Add for input_device sync			
		case AMI304DAE_IOCTL_SET_REPORT:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(&iEnReport, data, sizeof(iEnReport))) {
				retval = -EFAULT;
				goto err_out;
			}				
			AMI304_Report_Value(iEnReport);
			break;
		
		case AMI304DAE_IOCTL_GET_WIA:
			data = (void __user *) arg;
			if (data == NULL)
				break;		
			AMI304_WIA(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}
			break;

		case AMI304DAE_IOCTL_SET_I2CDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(strbuf, data, sizeof(strbuf))) {
				retval = -EFAULT;
				goto err_out;
			}	
			AMI304_I2c_Write(strbuf[0], &strbuf[2], strbuf[1]);
			break;	

		case AMI304DAE_IOCTL_SET_I2CADDR:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(i2creaddata, data, 2)) {
				retval = -EFAULT;
				goto err_out;
			}	
			i2c_read_addr = i2creaddata[0];
			i2c_read_len = i2creaddata[1];
			break;	

		case AMI304DAE_IOCTL_GET_I2CDATA:
			AMI304_I2c_Read(i2c_read_addr, &strbuf[0], i2c_read_len);
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_to_user(data, strbuf, i2c_read_len)) {
				retval = -EFAULT;
				goto err_out;
			}					
			break;		

		default:
			printk(KERN_ERR "%s not supported = 0x%04x", __FUNCTION__, cmd);
			retval = -ENOIOCTLCMD;
			break;
	}
	
err_out:
	return retval;	
}

static int ami304hal_open(struct inode *inode, struct file *file)
{
#if 0 //2011.02.16 woochang.chun, for the compile error fix 
	atomic_inc_and_test(&hal_open_count);
	printk(KERN_INFO "Open device node:ami304hal %d times.\n", atomic_read(&hal_open_count));
#endif	
	return 0;
}

static int ami304hal_release(struct inode *inode, struct file *file)
{
#if 0 //2011.02.16 woochang.chun, for the compile error fix 
	atomic_dec_and_test(&hal_open_count);
	printk(KERN_INFO "Release ami304hal, remainder is %d times.\n", atomic_read(&hal_open_count));	
#endif
	return 0;
}

static int ami304hal_ioctl(struct inode *inode, struct file *file, unsigned int cmd,unsigned long arg)
{
    int controlbuf[AMI304_CB_LENGTH];
	char strbuf[AMI304_BUFSIZE];
	int pedoparam[AMI304_PD_LENGTH];		
	void __user *data;
	int retval=0;
		
	switch (cmd) {
		
		case AMI304HAL_IOCTL_GET_SENSORDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadSensorData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}		
			break;
									
		case AMI304HAL_IOCTL_GET_POSTURE:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadPostureData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
			break;			
	 
		case AMI304HAL_IOCTL_GET_CALIDATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadCaliData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
	        	break;

		case AMI304HAL_IOCTL_GET_GYRODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadGyroData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}				
			break;
			
		case AMI304HAL_IOCTL_GET_PEDODATA:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			AMI304_ReadPedoData(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}						
			break;

		case AMI304HAL_IOCTL_GET_PEDOPARAM:
            read_lock(&ami304mid_data.ctrllock);
			memcpy(pedoparam, &ami304mid_data.pedometerparam[0], sizeof(pedoparam));
			read_unlock(&ami304mid_data.ctrllock);			
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_to_user(data, pedoparam, sizeof(pedoparam))) {
				retval = -EFAULT;
				goto err_out;
			}			
			break;
			
       case AMI304HAL_IOCTL_SET_PEDOPARAM:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(pedoparam, data, sizeof(pedoparam))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.ctrllock);
			memcpy(&ami304mid_data.pedometerparam[0], pedoparam, sizeof(pedoparam));
			write_unlock(&ami304mid_data.ctrllock);
	        break;	

		case AMI304HAL_IOCTL_GET_CONTROL:
			read_lock(&ami304mid_data.ctrllock);
			memcpy(controlbuf, &ami304mid_data.controldata[0], sizeof(controlbuf));
			read_unlock(&ami304mid_data.ctrllock);			
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_to_user(data, controlbuf, sizeof(controlbuf))) {
				retval = -EFAULT;
				goto err_out;
			}					
			break;		
			
		case AMI304HAL_IOCTL_SET_CONTROL:
			data = (void __user *) arg;
			if (data == NULL)
				break;	
			if (copy_from_user(controlbuf, data, sizeof(controlbuf))) {
				retval = -EFAULT;
				goto err_out;
			}	
			write_lock(&ami304mid_data.ctrllock);
			memcpy(&ami304mid_data.controldata[0], controlbuf, sizeof(controlbuf));
			write_unlock(&ami304mid_data.ctrllock);
			break;	

		case AMI304HAL_IOCTL_GET_WIA:
			data = (void __user *) arg;
			if (data == NULL)
				break;		
			AMI304_WIA(strbuf, AMI304_BUFSIZE);
			if (copy_to_user(data, strbuf, strlen(strbuf)+1)) {
				retval = -EFAULT;
				goto err_out;
			}
			break;

		default:
			printk(KERN_ERR "%s not supported = 0x%04x", __FUNCTION__, cmd);
			retval = -ENOIOCTLCMD;
			break;
	}
	
err_out:
	return retval;	
}

static struct file_operations ami304_fops = {
	.owner = THIS_MODULE,
	.open = ami304_open,
	.release = ami304_release,
	.ioctl = ami304_ioctl,
};

static struct miscdevice ami304_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ami304",
	.fops = &ami304_fops,
};


static struct file_operations ami304daemon_fops = {
	.owner = THIS_MODULE,
	.open = ami304daemon_open,
	.release = ami304daemon_release,
	.ioctl = ami304daemon_ioctl,
};

static struct miscdevice ami304daemon_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ami304daemon",
	.fops = &ami304daemon_fops,
};

static struct file_operations ami304hal_fops = {
	.owner = THIS_MODULE,
	.open = ami304hal_open,
	.release = ami304hal_release,
	.ioctl = ami304hal_ioctl,
};

static struct miscdevice ami304hal_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ami304hal",
	.fops = &ami304hal_fops,
};

static int ami304_input_init(struct ami304_i2c_data *data)
{
	int err=0;
	
	data->input_dev = input_allocate_device();
	if (!data->input_dev) {
		err = -ENOMEM;
		printk(KERN_ERR
		       "ami304_i2c_detect: Failed to allocate input device\n");
		goto exit_input_dev_alloc_failed;
	}
	set_bit(EV_ABS, data->input_dev->evbit);
	/* yaw */
	input_set_abs_params(data->input_dev, ABS_RX, 0, (360*10), 0, 0);
	/* pitch */
	input_set_abs_params(data->input_dev, ABS_RY, -(180*10), (180*10), 0, 0);
	/* roll */
	input_set_abs_params(data->input_dev, ABS_RZ, -(90*10), (90*10), 0, 0);
	/* status of orientation sensor */	
	input_set_abs_params(data->input_dev, ABS_RUDDER, 0, 5, 0, 0);
	
	/* x-axis of raw acceleration and the range is -2g to +2g */
	input_set_abs_params(data->input_dev, ABS_X, -(1000*2), (1000*2), 0, 0);
	/* y-axis of raw acceleration and the range is -2g to +2g */
	input_set_abs_params(data->input_dev, ABS_Y, -(1000*2), (1000*2), 0, 0);
	/* z-axis of raw acceleration and the range is -2g to +2g */
	input_set_abs_params(data->input_dev, ABS_Z, -(1000*2), (1000*2), 0, 0);
	
	/* x-axis of raw magnetic vector and the range is -3g to +3g */
	input_set_abs_params(data->input_dev, ABS_HAT0X, -(4000*3), (4000*3), 0, 0);
	/* y-axis of raw magnetic vector and the range is -3g to +3g */
	input_set_abs_params(data->input_dev, ABS_HAT0Y, -(4000*3), (4000*3), 0, 0);
	/* z-axis of raw magnetic vector and the range is -3g to +3g */
	input_set_abs_params(data->input_dev, ABS_BRAKE, -(4000*3), (4000*3), 0, 0);
	/* status of magnetic sensor */
	input_set_abs_params(data->input_dev, ABS_WHEEL, 0, 5, 0, 0);	

	/* x-axis of gyro sensor */
	input_set_abs_params(data->input_dev, ABS_HAT1X, -10000, 10000, 0, 0);
	/* y-axis of gyro sensor */
	input_set_abs_params(data->input_dev, ABS_HAT1Y, -10000, 10000, 0, 0);
	/* z-axis of gyro sensor */
	input_set_abs_params(data->input_dev, ABS_THROTTLE, -10000, 10000, 0, 0);

	data->input_dev->name = "Acompass";
	//register input device
	err = input_register_device(data->input_dev);
	if (err) {
		printk(KERN_ERR
		       "ami304_i2c_detect: Unable to register input device: %s\n",
		       data->input_dev->name);
		goto exit_input_register_device_failed;
	}
		
	return 0;
exit_input_register_device_failed:
	input_free_device(data->input_dev);	
exit_input_dev_alloc_failed:
	return err;	
}

static int __devinit ami304_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct ami304_i2c_data *data;
#if 1 //2011.03.07 woochang.chun@lge.com
	struct ami306_platform_data *pdata;
#endif	
	int err = 0;

#if defined(AMI304_TEST)
	printk(KERN_INFO "\nAMI304 MI sensor driver: ami304_probe\n");
#endif
	if(!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		printk(KERN_ERR "AMI304 : adapter can NOT support I2C_FUNC_I2C.\n");
		return -ENODEV;
	}
#if defined (DEBUG_PRINT)	
	printk(KERN_INFO "%s: line: %d\n", __func__, __LINE__);
#endif
	
#if 1 //2011.03.07 woochang.chun@lge.com
	/* device data setting */
	//---------------------------------------
	pdata = client->dev.platform_data;
	if(pdata == NULL) {
		dev_err(&client->dev,
			"failed to read platform data\n");
		err = -ENOMEM;
		goto exit;
	}
	//---------------------------------------
#endif

	if (!(data = kmalloc(sizeof(struct ami304_i2c_data), GFP_KERNEL))) {
		printk(KERN_INFO "AMI304 MI sensor driver: kmalloc failed\n");
		err = -ENOMEM;
		goto exit;
	}
	memset(data, 0, sizeof(struct ami304_i2c_data));

	data->client = client;
	i2c_set_clientdata(client, data);
	ami304_i2c_client = data->client;	

#if 1 //2011.03.07 woochang.chun@lge.com
	if(pdata->power_on){
#if defined (DEBUG_PRINT)	
		printk(KERN_INFO "%s: line: %d\n", __func__, __LINE__);
#endif
		pdata->power_on();
	}
#endif

	if( (err=Identify_AMI_Chipset())!=0 )  //get ami304_data.chipset
	{
		printk(KERN_INFO "Failed to identify AMI_Chipset!\n");	
		return err;
	}

	AMI304_Chipset_Init(AMI304_FORCE_MODE, ami304_data.chipset); // default is Force State	
	dev_info(&client->dev, "%s operating mode\n", ami304_data.mode? "force" : "normal");

	printk(KERN_INFO "Register input device!\n");	
	err = ami304_input_init(data);
	if(err)
		goto exit_kfree;

	//register misc device:ami304	       
	err = misc_register(&ami304_device);
	if (err) {
		printk(KERN_ERR
		       "ami304_device register failed\n");
		goto exit_misc_device_register_failed;
	}
	//register misc device:ami304daemon	
	err = misc_register(&ami304daemon_device);
	if (err) {
		printk(KERN_ERR
		       "ami304daemon_device register failed\n");
		goto exit_misc_device_register_failed;
	}
	//register misc device:ami304hal
	err = misc_register(&ami304hal_device);
	if (err) {
		printk(KERN_ERR
		       "ami304hal_device register failed\n");
		goto exit_misc_device_register_failed;
	}

	/* Register sysfs hooks */
	err = sysfs_create_group(&client->dev.kobj, &ami304_attribute_group);
	if (err)
		goto exit_sysfs_create_group_failed;

	return 0;
exit_sysfs_create_group_failed:	
exit_misc_device_register_failed:
	input_free_device(data->input_dev);
exit_kfree:	
	kfree(data);
exit:
	return err;
}

static int __devexit ami304_i2c_remove(struct i2c_client *client)
{
	struct ami304_i2c_data *data = i2c_get_clientdata(client);

    	sysfs_remove_group(&client->dev.kobj, &ami304_attribute_group);
	input_unregister_device(data->input_dev);
	kfree(i2c_get_clientdata(client));
	ami304_i2c_client = NULL;
	misc_deregister(&ami304hal_device);
	misc_deregister(&ami304daemon_device);
	misc_deregister(&ami304_device);	
	return 0;
}
struct i2c_device_id ami304_idtable[] = {
	{ "ami304", 0 },
	{}
};

MODULE_DEVICE_TABLE(i2c, ami304_idtable);

static struct i2c_driver ami304_i2c_driver = {
	.driver = {
		.name	= AMI304_DRV_NAME,
	},
	.probe			= ami304_i2c_probe,
	.remove			= __devexit_p(ami304_i2c_remove),
	.id_table	= ami304_idtable,	
#if 0 //2011.02.16 woochang.chun, for the compile error fix 
	.address_list	=	normal_i2c;
#endif	
};

static int __init ami304_init(void)
{
	int ret;
//	struct i2c_board_info info;
//	struct i2c_client *client;
//	struct i2c_adapter *adapter;	
	
	printk(KERN_INFO "AMI304 MI sensor driver: init\n");
	printk(KERN_INFO "ami304: driver version:%s\n",DRIVER_VERSION);

	rwlock_init(&ami304mid_data.ctrllock);
	rwlock_init(&ami304mid_data.datalock);
	rwlock_init(&ami304_data.lock);
	memset(&ami304mid_data.controldata[0], 0, sizeof(int)*AMI304_CB_LENGTH);	
	ami304mid_data.controldata[AMI304_CB_LOOPDELAY] =    20;  // Loop Delay
	ami304mid_data.controldata[AMI304_CB_RUN] =     	  1;   // Run	
	ami304mid_data.controldata[AMI304_CB_ACCCALI] =       0;   // Start-AccCali
	ami304mid_data.controldata[AMI304_CB_MAGCALI] =    	  1;   // Start-MagCali
	ami304mid_data.controldata[AMI304_CB_ACTIVESENSORS] = 0;   // Active Sensors
	ami304mid_data.controldata[AMI304_CB_PD_RESET] = 	  0;   // Pedometer not reset    
	ami304mid_data.controldata[AMI304_CB_PD_EN_PARAM] =   0;   // Disable parameters of Pedometer
	ami304mid_data.controldata[AMI304_CB_QWERTY] =   0;   // Qwerty Keyboard : close -> 0, open -> 1.
	ami304mid_data.controldata[AMI304_CB_CHANGE_WINDOW] =   0;   //ADC_WINDOW_CONTROL: ADC_WINDOW_NORMAL->0 ADC_WINDOW_CHANGED->1 ADC_WINDOW_EXCEEDED->2
	memset(&ami304mid_data.pedometerparam[0], 0, sizeof(int)*AMI304_PD_LENGTH);	
	atomic_set(&dev_open_count, 0);	
	atomic_set(&hal_open_count, 0);
	atomic_set(&daemon_open_count, 0);
	
	ret = i2c_add_driver(&ami304_i2c_driver);
	if ( ret != 0 ) {
		printk(KERN_INFO "can not add i2c driver\n");
		return ret;
	}

//	memset( &info , 0 , sizeof(struct i2c_board_info));
//	info.addr = AMI304_I2C_ADDRESS;
//	strlcpy(info.type,"ami304",I2C_NAME_SIZE);
//	adapter = i2c_get_adapter(3);
//	client = i2c_new_device(adapter,&info);
//	i2c_put_adapter(adapter);

	return ret;
}

static void __exit ami304_exit(void)
{
	atomic_set(&dev_open_count, 0);
	atomic_set(&hal_open_count, 0);
	atomic_set(&daemon_open_count, 0);	
	i2c_del_driver(&ami304_i2c_driver);
}

MODULE_AUTHOR("Kyle K.Y. Chen");
MODULE_DESCRIPTION("AMI304 MI-Sensor driver without DRDY");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);

module_init(ami304_init);
module_exit(ami304_exit);