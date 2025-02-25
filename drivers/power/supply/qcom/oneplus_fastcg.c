/****************************************************
 **Description:fastchg update firmware and driver
 *****************************************************/
#define pr_fmt(fmt) "FASTCHG: %s: " fmt, __func__

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/project_info.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/power_supply.h>
#include <linux/interrupt.h>
#include <linux/power/oem_external_fg.h>
#include <linux/pm_qos.h>
#include <linux/clk.h>

#define BYTE_OFFSET			2
#define BYTES_TO_WRITE		16
#define ERASE_COUNT		384	/* 0x8800-0x9FFF */
#define READ_COUNT			192
#define	FW_CHECK_FAIL		0
#define	FW_CHECK_SUCCESS	1

#define SHOW_FW_VERSION_DELAY_MS 18000

struct fastchg_device_info {
	struct i2c_client		*client;
	struct miscdevice   dash_device;
	struct mutex        read_mutex;
	wait_queue_head_t   read_wq;

	struct pinctrl_state *pinctrl_state_active;
	struct pinctrl_state *pinctrl_state_suspended;
	struct pinctrl_state *pinctrl_mcu_data_state_active;
	struct pinctrl_state *pinctrl_mcu_data_state_suspended;
	struct pinctrl *pinctrl;
	bool fast_chg_started;
	bool fast_low_temp_full;
	bool fast_chg_ing;
	bool fast_switch_to_normal;
	bool fast_normal_to_warm;
	bool irq_enabled;
	bool fast_chg_allow;
	bool firmware_already_updated;
	int adapter_update_report;
	int adapter_update_real;
	int battery_type;
	int irq;
	int mcu_en_gpio;
	int usb_sw_1_gpio;
	int usb_sw_2_gpio;
	int ap_clk;
	int ap_data;
	int dashchg_fw_ver_count;

	struct power_supply		*batt_psy;
	struct work_struct fastcg_work;
	struct work_struct mcu_wdg_work;
	struct timer_list watchdog;
	struct wakeup_source fastchg_wake_lock;
	struct wakeup_source fastchg_update_fireware_lock;

	struct delayed_work		update_firmware;
	struct delayed_work update_fireware_version_work;
	struct delayed_work adapter_update_work;
	char fw_id[12];
	char manu_name[12];
};

struct fastchg_device_info *fastchg_di;

static unsigned char *dashchg_firmware_data;
static struct clk *snoc_clk, *cnoc_clk;
static struct i2c_client *mcu_client;

void opchg_set_data_active(struct fastchg_device_info *chip)
{
	gpio_direction_input(chip->ap_data);
	pinctrl_select_state(chip->pinctrl,
		chip->pinctrl_mcu_data_state_active);
}

void set_mcu_en_gpio_value(int value)
{
	if (gpio_is_valid(fastchg_di->mcu_en_gpio))
		gpio_direction_output(fastchg_di->mcu_en_gpio, value);
}

void mcu_en_reset(void)
{
	if (gpio_is_valid(fastchg_di->mcu_en_gpio))
		gpio_direction_output(fastchg_di->mcu_en_gpio, 1);
}

void mcu_en_gpio_set(int value)
{
	if (value) {
		if (gpio_is_valid(fastchg_di->mcu_en_gpio))
			gpio_direction_output(fastchg_di->mcu_en_gpio, 0);
	} else {
		if (gpio_is_valid(fastchg_di->mcu_en_gpio)) {
			gpio_direction_output(fastchg_di->mcu_en_gpio, 1);
			usleep_range(10000, 10001);
			gpio_direction_output(fastchg_di->mcu_en_gpio, 0);
		}
	}
}
#define ADAPTER_UPDATE_DELAY              1400

void usb_sw_gpio_set(int value)
{
	pr_info("set usb_sw_gpio=%d\n", value);
	if (!gpio_is_valid(fastchg_di->usb_sw_1_gpio)
		&& !gpio_is_valid(fastchg_di->usb_sw_2_gpio)) {
		pr_err("gpio is invalid\n");
		return;
	}

	if (value) {
		gpio_direction_output(fastchg_di->usb_sw_1_gpio, 1);
		gpio_direction_output(fastchg_di->usb_sw_2_gpio, 1);
	} else {
		gpio_direction_output(fastchg_di->usb_sw_1_gpio, 0);
		gpio_direction_output(fastchg_di->usb_sw_2_gpio, 0);
	}
	fastchg_di->fast_chg_allow = value;
	/* david@bsp add log */
	pr_info("get usb_sw_gpio=%d&%d\n"
		, gpio_get_value(fastchg_di->usb_sw_1_gpio)
		, gpio_get_value(fastchg_di->usb_sw_2_gpio));
}

static int set_property_on_smbcharger(
	enum power_supply_property prop, bool data)
{
	static struct power_supply *psy;
	union power_supply_propval value = {data, };
	int ret;

	if (!psy) {
		psy = power_supply_get_by_name("battery");
		if (!psy) {
			pr_err("failed to get ps battery\n");
			return -EINVAL;
		}
	}
	ret = power_supply_set_property(psy, prop, &value);
	/* david@bsp modified */
	if (ret)
		return -EINVAL;

	return 0;
}


static int oneplus_dash_i2c_read(
	struct i2c_client *client, u8 addr, s32 len, u8 *rxbuf)
{
	return i2c_smbus_read_i2c_block_data(client, addr, len, rxbuf);
}

static int oneplus_dash_i2c_write(
	struct i2c_client *client, u8 addr, s32 len, u8 *txbuf)
{
	return i2c_smbus_write_i2c_block_data(client, addr, len, txbuf);
}

static bool dashchg_fw_check(void)
{
	unsigned char addr_buf[2] = {0x88, 0x00};
	unsigned char data_buf[32] = {0x0};
	int rc, i, j, addr;
	int fw_line = 0;

	rc = oneplus_dash_i2c_write(mcu_client, 0x01, 2, &addr_buf[0]);
	if (rc < 0) {
		pr_err("%s i2c_write 0x01 error\n", __func__);
		goto i2c_err;
	}

	usleep_range(2000, 2001);
	for (i = 0; i < READ_COUNT; i++) {
		oneplus_dash_i2c_read(mcu_client, 0x03, 16, &data_buf[0]);
		usleep_range(2000, 2001);
		oneplus_dash_i2c_read(mcu_client, 0x03, 16, &data_buf[16]);
		addr = 0x8800 + i * 32;

		/* compare recv_buf with dashchg_firmware_data[] begin */
		if (addr == ((dashchg_firmware_data[fw_line * 34 + 1] << 8)
			| dashchg_firmware_data[fw_line * 34])) {
			for (j = 0; j < 32; j++) {
			if (data_buf[j] != dashchg_firmware_data[fw_line * 34 + 2 + j]) {
				pr_info("%s fail,data_buf[%d]:0x%x!=dashchg_firmware_data[%d]:0x%x\n",
				__func__, j, data_buf[j],
				(fw_line * 34 + 2 + j),
				dashchg_firmware_data[fw_line * 34 + 2 + j]);
				pr_info("%s addr = 0x%x", __func__, addr);
				for (j = 0; j <= 31; j++)
					pr_info("%x\n", data_buf[i]);
				return FW_CHECK_FAIL;
				}
			}
			fw_line++;
		} else {
	/*pr_err("%s addr dismatch,addr:0x%x,stm_data:0x%x\n",__func__,*/
	/*addr,(dashchg_firmware_data[fw_line * 34 + 1] << 8) | */
	/*dashchg_firmware_data[fw_line * 34]);*/
		}
		/* compare recv_buf with dashchg_firmware_data[] end */
	}
	pr_info("result=success\n");
	return FW_CHECK_SUCCESS;
i2c_err:
	pr_err("result=fail\n");
	return FW_CHECK_FAIL;
}

static int dashchg_fw_write(
	unsigned char *data_buf,
	unsigned int offset, unsigned int length)
{
	unsigned int count = 0;
	unsigned char zero_buf[1] = {0};
	unsigned char temp_buf[1] = {0};
	unsigned char addr_buf[2] = {0x88, 0x00};
	int rc;

	count = offset;
	/* write data begin */
	while (count < (offset + length)) {
		addr_buf[0] = data_buf[count + 1];
		addr_buf[1] = data_buf[count];

		rc = oneplus_dash_i2c_write(mcu_client, 0x01, 2, &addr_buf[0]);
		if (rc < 0) {
			pr_err("i2c_write 0x01 error\n");
			return -EFAULT;
		}

		/* write 16 bytes data to dashchg */
		oneplus_dash_i2c_write(mcu_client,
		0x02, BYTES_TO_WRITE, &data_buf[count+BYTE_OFFSET]);
		oneplus_dash_i2c_write(mcu_client, 0x05, 1, &zero_buf[0]);
		oneplus_dash_i2c_read(mcu_client, 0x05, 1, &temp_buf[0]);

		/* write 16 bytes data to dashchg again */
		oneplus_dash_i2c_write(mcu_client,
		0x02, BYTES_TO_WRITE,
		&data_buf[count+BYTE_OFFSET+BYTES_TO_WRITE]);
		oneplus_dash_i2c_write(mcu_client, 0x05, 1, &zero_buf[0]);
		oneplus_dash_i2c_read(mcu_client, 0x05, 1, &temp_buf[0]);

		count = count + BYTE_OFFSET + 2 * BYTES_TO_WRITE;

		usleep_range(2000, 2001);
		if (count > (offset + length - 1))
			break;
	}
	return 0;
}

static irqreturn_t irq_rx_handler(int irq, void *dev_id);
static void reset_mcu_and_request_irq(struct fastchg_device_info *di)
{
	int ret;

	pr_info("\n");
	gpio_direction_output(di->ap_clk, 1);
	usleep_range(10000, 10001);
	gpio_direction_output(di->mcu_en_gpio, 1);
	usleep_range(10000, 10001);
	gpio_direction_output(di->mcu_en_gpio, 0);
	usleep_range(5000, 5001);
	opchg_set_data_active(di);
	di->irq = gpio_to_irq(di->ap_data);

	/* 0x01:rising edge, 0x02:falling edge */
	irq_set_status_flags(di->irq, IRQ_DISABLE_UNLAZY);
	ret = request_irq(di->irq, irq_rx_handler,
			IRQF_TRIGGER_RISING, "mcu_data", di);
	if (ret < 0)
		pr_err("request ap rx irq failed.\n");
	else
		di->irq_enabled = true;
}


static void dashchg_fw_update(struct work_struct *work)
{
	unsigned char zero_buf[1] = {0};
	unsigned char addr_buf[2] = {0x88, 0x00};
	unsigned char temp_buf[1] = {0};
	int i, rc = 0;
	unsigned int addr = 0x8800;
	int download_again = 0;
	struct fastchg_device_info *di = container_of(work,
			struct fastchg_device_info,
			update_firmware.work);

	__pm_stay_awake(&di->fastchg_update_fireware_lock);
	rc = dashchg_fw_check();
	if (rc == FW_CHECK_SUCCESS) {
		di->firmware_already_updated = true;
		reset_mcu_and_request_irq(di);
		__pm_relax(&di->fastchg_update_fireware_lock);
		set_property_on_smbcharger(POWER_SUPPLY_PROP_SWITCH_DASH, true);
		pr_info("FW check success\n"); /* david@bsp add log */
		return;
	}
	pr_info("start erasing data.......\n");

update_fw:
	/* erase address 0x200-0x7FF */
	for (i = 0; i < ERASE_COUNT; i++) {
		/* first:set address */
		rc = oneplus_dash_i2c_write(mcu_client, 0x01, 2, &addr_buf[0]);
		if (rc < 0) {
			pr_err("dashchg_update_fw, i2c_write 0x01 error\n");
			goto update_fw_err;
		}

		/* erase data:0x10 words once */
		oneplus_dash_i2c_write(mcu_client, 0x04, 1, &zero_buf[0]);
		usleep_range(1000, 1001);
		oneplus_dash_i2c_read(mcu_client, 0x04, 1, &temp_buf[0]);

		/* erase data:0x10 words once */
		addr = addr + 0x10;
		addr_buf[0] = addr >> 8;
		addr_buf[1] = addr & 0xFF;
	}
	usleep_range(10000, 10001);
	dashchg_fw_write(dashchg_firmware_data, 0, di->dashchg_fw_ver_count);

	/* fw check begin:read data from mcu and compare*/
	/*it with dashchg_firmware_data[] */
	rc = dashchg_fw_check();
	if (rc == FW_CHECK_FAIL) {
		download_again++;
		if (download_again > 3)
			goto update_fw_err;
		mcu_en_gpio_set(0);
		msleep(1000);
		pr_err("fw check fail, download fw again\n");
		goto update_fw;
	}
	/* fw check end */

	usleep_range(2000, 2001);
	/* jump to app code begin */
	oneplus_dash_i2c_write(mcu_client, 0x06, 1, &zero_buf[0]);
	oneplus_dash_i2c_read(mcu_client, 0x06, 1, &temp_buf[0]);
	/* jump to app code end */
	di->firmware_already_updated = true;
	reset_mcu_and_request_irq(di);
	__pm_relax(&di->fastchg_update_fireware_lock);
	set_property_on_smbcharger(POWER_SUPPLY_PROP_SWITCH_DASH, true);
	pr_info("result=success\n");
	return;

update_fw_err:
	di->firmware_already_updated = true;
	reset_mcu_and_request_irq(di);
	__pm_relax(&di->fastchg_update_fireware_lock);
	set_property_on_smbcharger(POWER_SUPPLY_PROP_SWITCH_DASH, true);
	pr_err("result=fail\n");
}

static struct external_battery_gauge *bq27541_data;
void bq27541_information_register(
	struct external_battery_gauge *fast_chg)
{
	if (bq27541_data) {
		bq27541_data = fast_chg;
		pr_err("multiple battery gauge called\n");
	} else {
		bq27541_data = fast_chg;
	}
}
EXPORT_SYMBOL(bq27541_information_register);

void bq27541_information_unregister(struct external_battery_gauge *batt_gauge)
{
	bq27541_data = NULL;
}

static bool bq27541_fast_chg_started(void)
{
	if (fastchg_di)
		return fastchg_di->fast_chg_started;

	return false;
}

static bool bq27541_get_fast_low_temp_full(void)
{
	if (fastchg_di)
		return fastchg_di->fast_low_temp_full;

	return false;
}

static int bq27541_set_fast_chg_allow(bool enable)
{
	if (fastchg_di)
		fastchg_di->fast_chg_allow = enable;

	return 0;
}

static bool bq27541_get_fast_chg_allow(void)
{
	if (fastchg_di)
		return fastchg_di->fast_chg_allow;

	return false;
}

static bool bq27541_fast_switch_to_normal(void)
{
	if (fastchg_di)
		return fastchg_di->fast_switch_to_normal;

	return false;
}

static bool bq27541_get_fast_chg_ing(void)
{
	if (fastchg_di)
		return fastchg_di->fast_chg_ing;

	return false;
}


static int bq27541_set_switch_to_noraml_false(void)
{
	if (fastchg_di)
		fastchg_di->fast_switch_to_normal = false;

	return 0;
}

static bool get_fastchg_firmware_already_updated(void)
{
	if (fastchg_di)
		return fastchg_di->firmware_already_updated;

	return false;
}

static bool fastchg_is_usb_switch_on(void)
{
	if (fastchg_di)
		return gpio_get_value(fastchg_di->usb_sw_1_gpio);

	return false;
}

int dash_get_adapter_update_status(void)
{
	if (!fastchg_di)
		return ADAPTER_FW_UPDATE_NONE;
	else
		return fastchg_di->adapter_update_report;
}

static struct external_battery_gauge fastcharge_information  = {
	.fast_chg_started =
		bq27541_fast_chg_started,
	.get_fast_low_temp_full =
		bq27541_get_fast_low_temp_full,
	.fast_switch_to_normal =
		bq27541_fast_switch_to_normal,
	.get_fast_chg_ing =
		bq27541_get_fast_chg_ing,
	.set_fast_chg_allow =
		bq27541_set_fast_chg_allow,
	.get_fast_chg_allow =
		bq27541_get_fast_chg_allow,
	.set_switch_to_noraml_false =
		bq27541_set_switch_to_noraml_false,
	.get_fastchg_firmware_already_updated =
		get_fastchg_firmware_already_updated,
	.is_usb_switch_on = fastchg_is_usb_switch_on,
	.get_adapter_update = dash_get_adapter_update_status,
};

static struct notify_dash_event *notify_event;

void notify_dash_unplug_register(struct notify_dash_event *event)
{
	if (notify_event) {
		notify_event = event;
		pr_err("multiple battery gauge called\n");
	} else {
		notify_event = event;
	}
}
EXPORT_SYMBOL(notify_dash_unplug_register);

void notify_dash_unplug_unregister(struct notify_dash_event *notify_event)
{
	notify_event = NULL;
}
EXPORT_SYMBOL(notify_dash_unplug_unregister);

static void mcu_init(struct fastchg_device_info *di)
{
	gpio_direction_output(di->ap_clk, 0);
	usleep_range(1000, 1001);
	gpio_direction_output(di->mcu_en_gpio, 1);
	usleep_range(1000, 1001);
	gpio_direction_output(di->mcu_en_gpio, 0);
}

static irqreturn_t irq_rx_handler(int irq, void *dev_id)
{
	struct fastchg_device_info *di = dev_id;

	pr_debug("triggered\n");
	schedule_work(&di->fastcg_work);
	return IRQ_HANDLED;
}

static void oneplus_notify_dash_charger_present(bool status)
{
	if (notify_event && notify_event->notify_dash_charger_present)
		notify_event->notify_dash_charger_present(status);
}

static void oneplus_notify_pmic_check_charger_present(void)
{
	if (notify_event && notify_event->notify_event)
		notify_event->notify_event();
}

static void notify_dash_charger_unplug(void)
{
	oneplus_notify_dash_charger_present(false);
	oneplus_notify_pmic_check_charger_present();
}

static void notify_check_usb_suspend(bool status, bool check_power_ok)
{
	if (notify_event && notify_event->op_contrl)
		notify_event->op_contrl(status, check_power_ok);
}

static int op_get_device_type(void)
{
	if (bq27541_data && bq27541_data->get_device_type)
		return bq27541_data->get_device_type();
	else
		return 0;
}

static int onplus_get_battery_mvolts(void)
{
	if (bq27541_data && bq27541_data->get_battery_mvolts)
		return bq27541_data->get_battery_mvolts();
	else
		return 4010 * 1000; /* retrun 4.01v for default */
}

static int onplus_get_battery_temperature(void)
{
	if (bq27541_data && bq27541_data->get_battery_temperature)
		return bq27541_data->get_battery_temperature();
	else
		return 255; /* retrun 25.5 for default temp */
}

static int onplus_get_batt_remaining_capacity(void)
{
	if (bq27541_data && bq27541_data->get_batt_remaining_capacity)
		return bq27541_data->get_batt_remaining_capacity();
	else
		return 5; /* retrun 5 for default remaining_capacity */
}

static int onplus_get_battery_soc(void)
{
	if (bq27541_data && bq27541_data->get_battery_soc)
		return bq27541_data->get_battery_soc();
	else
		return 50; /* retrun 50 for default soc */
}

static int onplus_get_average_current(void)
{
	if (bq27541_data && bq27541_data->get_average_current)
		return bq27541_data->get_average_current();
	else
		return 666 * 1000; /* retrun 666ma for default current */
}

void switch_mode_to_normal(void)
{
	usb_sw_gpio_set(0);
	mcu_en_gpio_set(1);
}

static void update_fast_chg_started(void)
{
	if (bq27541_data && bq27541_data->fast_chg_started_status)
		bq27541_data->fast_chg_started_status(
		fastchg_di->fast_chg_started);
}

static void request_mcu_irq(struct fastchg_device_info *di)
{
	int retval;

	opchg_set_data_active(di);
	gpio_set_value(di->ap_clk, 0);
	usleep_range(10000, 10001);
	gpio_set_value(di->ap_clk, 1);
	if (di->adapter_update_real
		!= ADAPTER_FW_NEED_UPDATE) {
		pr_info("%s\n", __func__);
		if (!di->irq_enabled) {
			di->irq = gpio_to_irq(di->ap_data);
			irq_set_status_flags(di->irq, IRQ_DISABLE_UNLAZY);
			retval = request_irq(di->irq, irq_rx_handler,
					IRQF_TRIGGER_RISING, "mcu_data", di);
			if (retval < 0)
				pr_err("request ap rx irq failed.\n");
			else
				di->irq_enabled = true;
		}
	} else {
		di->irq_enabled = true;
	}
}

static void fastcg_work_func(struct work_struct *work)
{
	struct fastchg_device_info *di = container_of(work,
			struct fastchg_device_info,
			fastcg_work);
	pr_info("\n");
	if (di->irq_enabled) {
		free_irq(di->irq, di);
		msleep(25);
		di->irq_enabled = false;
		wake_up(&di->read_wq);
	}
}

static void update_fireware_version_func(struct work_struct *work)
{
	struct fastchg_device_info *di = container_of(work,
			struct fastchg_device_info,
			update_fireware_version_work.work);

	if (!dashchg_firmware_data || di->dashchg_fw_ver_count == 0)
		return;

	snprintf(di->fw_id, 255, "0x%x",
	dashchg_firmware_data[di->dashchg_fw_ver_count - 4]);
	snprintf(di->manu_name, 255, "%s", "ONEPLUS");
	//push_component_info(FAST_CHARGE, di->fw_id, di->manu_name);
}

/* 15 secs */
#define WATCHDOG_INTERVAL 15000

#define WATCHDOG_MAX_CNT 10
static unsigned int watchdog_cnt = 0;

static void di_mcu_wdg(struct work_struct *work)
{
	struct fastchg_device_info *di = container_of(work,
			struct fastchg_device_info,
			mcu_wdg_work);

	pr_info("resetting GPIOs, continue charging normally\n");
	bq27541_data->set_allow_reading(true);
	di->fast_chg_started = false;
	di->fast_switch_to_normal = false;
	di->fast_low_temp_full = false;
	di->fast_chg_allow = false;
	di->fast_normal_to_warm = false;
	di->fast_chg_ing = false;

	switch_mode_to_normal();
	msleep(500);

	if (watchdog_cnt >= WATCHDOG_MAX_CNT) {
		pr_warn("max dash watchdog retries reached! "
			"retain normal charging\n");
		watchdog_cnt = 0;
		return;
	}
	watchdog_cnt++;

	/* 
	 * Reset MCU GPIO and request dash IRQ again after 
	 * switching to normal.
	 */
	mcu_en_reset();
	usleep_range(10000, 10002);
	usb_sw_gpio_set(1);
	usleep_range(10000, 10002);
	mcu_en_gpio_set(0);

	pr_info("re-attempting dash IRQ\n");

	oneplus_notify_dash_charger_present(true);
	request_mcu_irq(di);

	if (!timer_pending(&di->watchdog))
		mod_timer(&di->watchdog, jiffies + 
				msecs_to_jiffies(WATCHDOG_INTERVAL));
}

void di_watchdog(unsigned long data)
{
	struct fastchg_device_info *di = (struct fastchg_device_info *)data;

	pr_err("di_watchdog can't receive mcu data\n");

	schedule_work(&di->mcu_wdg_work);
}

#define MAX_BUFFER_SIZE 1024
#define ALLOW_DATA 0x2
#define REJECT_DATA 0x11
static void dash_write(struct fastchg_device_info *di, int data)
{
	int i;
	int device_type = op_get_device_type();

	usleep_range(2000, 2001);
	gpio_direction_output(di->ap_data, 0);
	pinctrl_select_state(di->pinctrl,
		di->pinctrl_mcu_data_state_suspended);
	for (i = 0; i < 3; i++) {
		if (i == 0)
			gpio_set_value(di->ap_data, data >> 1);
		else if (i == 1)
			gpio_set_value(di->ap_data, data & 0x1);
		else
			gpio_set_value(di->ap_data, device_type);
		gpio_set_value(di->ap_clk, 0);
		usleep_range(1000, 1001);
		gpio_set_value(di->ap_clk, 1);
		usleep_range(19000, 19001);
	}
}

static int dash_read(struct fastchg_device_info *di)
{
	int i;
	int bit = 0;
	int data = 0;

	for (i = 0; i < 7; i++) {
		gpio_set_value(di->ap_clk, 0);
		usleep_range(1000, 1001);
		gpio_set_value(di->ap_clk, 1);
		usleep_range(19000, 19001);
		bit = gpio_get_value(di->ap_data);
		data |= bit<<(6-i);
	}
	pr_err("recv data:0x%x\n", data);
	return data;
}

static int dash_dev_open(struct inode *inode, struct file *filp)
{
	struct fastchg_device_info *dash_dev = container_of(filp->private_data,
			struct fastchg_device_info, dash_device);

	filp->private_data = dash_dev;
	pr_debug("%d,%d\n", imajor(inode), iminor(inode));
	return 0;
}

static ssize_t dash_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct fastchg_device_info *di = filp->private_data;

	int data;
	int ret = 0;

	mutex_lock(&di->read_mutex);
	ret = wait_event_interruptible(di->read_wq,
			(!di->irq_enabled));
	if (ret) {
		mutex_unlock(&di->read_mutex);
		goto fail;
	}
	data = dash_read(di);
	mutex_unlock(&di->read_mutex);

	if (copy_to_user(buf, &data, 1)) {
		pr_err("failed to copy to user space\n");
		ret = -EFAULT;
	}

fail:
	return ret;
}
static struct op_adapter_chip *g_adapter_chip;

static void adapter_update_work_func(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct fastchg_device_info *chip =
		container_of(dwork,
		struct fastchg_device_info, adapter_update_work);
	bool update_result = false;
	int i = 0;

	if (!g_adapter_chip) {
		pr_info("%s g_adapter_chip NULL\n", __func__);
		return;
	}
	pr_info("%s begin\n", __func__);
	opchg_set_data_active(chip);
	if (!IS_ERR(snoc_clk)) {
		clk_set_rate(snoc_clk, 200000000);
		clk_prepare_enable(snoc_clk);
	}
	if (!IS_ERR(cnoc_clk)) {
		clk_set_rate(cnoc_clk, 75000000);
		clk_prepare_enable(cnoc_clk);
	}
	msleep(1000);
	for (i = 0; i < 3; i++) {
		update_result =
			g_adapter_chip->vops->adapter_update(g_adapter_chip,
			chip->ap_clk, chip->ap_data);
		if (update_result == true)
			break;
		if (i < 1)
			msleep(1650);
	}
	msleep(5000);
	if (update_result) {
		chip->adapter_update_real = ADAPTER_FW_UPDATE_SUCCESS;
	} else {
		chip->adapter_update_real = ADAPTER_FW_UPDATE_FAIL;
		chip->adapter_update_report = chip->adapter_update_real;
	}
	msleep(20);
	mcu_en_gpio_set(1);
	chip->fast_chg_started = false;
	chip->fast_chg_allow = false;
	chip->fast_chg_ing = false;
	msleep(1000);
	if (update_result) {
		msleep(2000);
		chip->adapter_update_report = ADAPTER_FW_UPDATE_SUCCESS;
	}
	notify_check_usb_suspend(true, false);
	notify_dash_charger_unplug();
	reset_mcu_and_request_irq(chip);
	clk_disable_unprepare(snoc_clk);
	clk_disable_unprepare(cnoc_clk);

	pr_info("%s end update_result:%d\n",
		__func__, update_result);
	__pm_relax(&chip->fastchg_wake_lock);

}

static void dash_adapter_update(struct fastchg_device_info *chip)
{
	pr_err("%s\n", __func__);
	/*schedule_delayed_work_on(5,*/
	/*&chip->adapter_update_work,*/
	/*round_jiffies_relative(*/
	/*msecs_to_jiffies(ADAPTER_UPDATE_DELAY)));*/
	schedule_delayed_work(&chip->adapter_update_work,
			msecs_to_jiffies(ADAPTER_UPDATE_DELAY));
}
void op_adapter_init(struct op_adapter_chip *chip)
{
	g_adapter_chip = chip;
}

#define DASH_IOC_MAGIC					0xff
#define DASH_NOTIFY_FIRMWARE_UPDATE		_IO(DASH_IOC_MAGIC, 1)
#define DASH_NOTIFY_FAST_PRESENT		_IOW(DASH_IOC_MAGIC, 2, int)
#define DASH_NOTIFY_FAST_ABSENT			_IOW(DASH_IOC_MAGIC, 3, int)
#define DASH_NOTIFY_NORMAL_TEMP_FULL	_IOW(DASH_IOC_MAGIC, 4, int)
#define DASH_NOTIFY_LOW_TEMP_FULL		_IOW(DASH_IOC_MAGIC, 5, int)
#define DASH_NOTIFY_BAD_CONNECTED		_IOW(DASH_IOC_MAGIC, 6, int)
#define DASH_NOTIFY_TEMP_OVER			_IOW(DASH_IOC_MAGIC, 7, int)
#define DASH_NOTIFY_ADAPTER_FW_UPDATE	_IOW(DASH_IOC_MAGIC, 8, int)
#define DASH_NOTIFY_BTB_TEMP_OVER		_IOW(DASH_IOC_MAGIC, 9, int)
#define DASH_NOTIFY_ALLOW_READING_IIC	_IOW(DASH_IOC_MAGIC, 10, int)
#define DASH_NOTIFY_UNDEFINED_CMD		_IO(DASH_IOC_MAGIC, 11)
#define DASH_NOTIFY_INVALID_DATA_CMD	_IO(DASH_IOC_MAGIC, 12)
#define DASH_NOTIFY_REQUEST_IRQ			_IO(DASH_IOC_MAGIC, 13)
#define DASH_NOTIFY_UPDATE_DASH_PRESENT	_IOW(DASH_IOC_MAGIC, 14, int)

static long  dash_dev_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct fastchg_device_info *di = filp->private_data;
	int volt = 0;
	int temp = 0;
	int soc = 0;
	int current_now = 0;
	int remain_cap = 0;

	switch (cmd) {
		case DASH_NOTIFY_FIRMWARE_UPDATE:
			schedule_delayed_work(&di->update_firmware,
					msecs_to_jiffies(2200));
			break;
		case DASH_NOTIFY_FAST_PRESENT:
			oneplus_notify_dash_charger_present(true);
			if (arg == DASH_NOTIFY_FAST_PRESENT + 1) {
				__pm_stay_awake(&di->fastchg_wake_lock);
				bq27541_data->set_allow_reading(false);
				di->fast_chg_allow = false;
				di->fast_normal_to_warm = false;
				mod_timer(&di->watchdog, jiffies + 
						msecs_to_jiffies(WATCHDOG_INTERVAL));
			} else if (arg == DASH_NOTIFY_FAST_PRESENT + 2) {
				pr_err("REJECT_DATA\n");
				dash_write(di, REJECT_DATA);
			} else if (arg == DASH_NOTIFY_FAST_PRESENT + 3) {
				notify_check_usb_suspend(false, false);
				dash_write(di, ALLOW_DATA);
				di->fast_chg_started = true;
			}
			break;
		case DASH_NOTIFY_FAST_ABSENT:
			if (arg == DASH_NOTIFY_FAST_ABSENT + 1) {
				bq27541_data->set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_switch_to_normal = false;
				di->fast_normal_to_warm = false;
				di->fast_chg_ing = false;
				pr_err("fastchg stop unexpectly, switch off fastchg\n");
				switch_mode_to_normal();
				del_timer(&di->watchdog);
				watchdog_cnt = 0;
				dash_write(di, REJECT_DATA);
			} else if (arg == DASH_NOTIFY_FAST_ABSENT + 2) {
				notify_check_usb_suspend(true, true);
				notify_dash_charger_unplug();
				__pm_relax(&di->fastchg_wake_lock);
			}
			break;
		case DASH_NOTIFY_ALLOW_READING_IIC:
			if (arg == DASH_NOTIFY_ALLOW_READING_IIC + 1) {
				bq27541_data->set_allow_reading(true);
				di->fast_chg_started = true;
				di->fast_chg_ing = true;
				volt = onplus_get_battery_mvolts();
				temp = onplus_get_battery_temperature();
				remain_cap =
				onplus_get_batt_remaining_capacity();
				soc = onplus_get_battery_soc();
				current_now = onplus_get_average_current();
				pr_err("volt:%d,temp:%d,remain_cap:%d,soc:%d,current:%d\n",
				volt, temp, remain_cap, soc, current_now);
				if (!di->batt_psy)
					di->batt_psy =
					power_supply_get_by_name("battery");
				if (di->batt_psy)
					power_supply_changed(di->batt_psy);
				bq27541_data->set_allow_reading(false);
				mod_timer(&di->watchdog, jiffies + 
						msecs_to_jiffies(WATCHDOG_INTERVAL));
				dash_write(di, ALLOW_DATA);
			}
			break;
		case DASH_NOTIFY_BTB_TEMP_OVER:
			mod_timer(&di->watchdog, jiffies + 
					msecs_to_jiffies(WATCHDOG_INTERVAL));
			dash_write(di, ALLOW_DATA);
			break;
		case DASH_NOTIFY_BAD_CONNECTED:
		case DASH_NOTIFY_NORMAL_TEMP_FULL:
			if (arg == DASH_NOTIFY_NORMAL_TEMP_FULL + 1) {
				pr_err("fastchg full, switch off fastchg, set usb_sw_gpio 0\n");
				switch_mode_to_normal();
				del_timer(&di->watchdog);
				watchdog_cnt = 0;
			} else if (arg == DASH_NOTIFY_NORMAL_TEMP_FULL + 2) {
				di->fast_switch_to_normal = true;
				bq27541_data->set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_chg_ing = false;
				notify_check_usb_suspend(true, false);
				oneplus_notify_pmic_check_charger_present();
				__pm_relax(&di->fastchg_wake_lock);
			}
			break;
		case DASH_NOTIFY_TEMP_OVER:
			if (arg == DASH_NOTIFY_TEMP_OVER + 1) {
				pr_err("fastchg temp over\n");
				switch_mode_to_normal();
				del_timer(&di->watchdog);
				watchdog_cnt = 0;
			} else if (arg == DASH_NOTIFY_TEMP_OVER + 2) {
				di->fast_normal_to_warm = true;
				bq27541_data->set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_chg_ing = false;
				notify_check_usb_suspend(true, false);
				notify_dash_charger_unplug();
				__pm_relax(&di->fastchg_wake_lock);
			}
			break;
		case DASH_NOTIFY_ADAPTER_FW_UPDATE:
			if (arg == DASH_NOTIFY_ADAPTER_FW_UPDATE + 1) {
				di->adapter_update_real
					= ADAPTER_FW_NEED_UPDATE;
				di->adapter_update_report
					= di->adapter_update_real;
			} else if (arg == DASH_NOTIFY_ADAPTER_FW_UPDATE + 2) {
				bq27541_data->set_allow_reading(true);
				di->fast_chg_started = false;
				oneplus_notify_dash_charger_present(true);
				dash_write(di, ALLOW_DATA);
				__pm_stay_awake(&di->fastchg_wake_lock);
				dash_adapter_update(di);
			}
			break;
		case DASH_NOTIFY_UNDEFINED_CMD:
			if (di->fast_chg_started) {
				pr_err("UNDEFINED_CMD, switch off fastchg\n");
				switch_mode_to_normal();
				msleep(500); /* avoid i2c conflict */
				/* data err */
				bq27541_data->set_allow_reading(true);
				di->fast_chg_started = false;
				__pm_relax(&di->fastchg_wake_lock);
				di->fast_chg_allow = false;
				di->fast_switch_to_normal = false;
				di->fast_normal_to_warm = false;
				di->fast_chg_ing = false;
				notify_check_usb_suspend(true, false);
			}
			break;
		case DASH_NOTIFY_INVALID_DATA_CMD:
			if (di->fast_chg_started) {
				bq27541_data->set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_switch_to_normal = false;
				di->fast_normal_to_warm = false;
				di->fast_chg_ing = false;
				pr_err("DASH_NOTIFY_INVALID_DATA_CMD, switch off fastchg\n");
				switch_mode_to_normal();
				del_timer(&di->watchdog);
				watchdog_cnt = 0;
				__pm_relax(&di->fastchg_wake_lock);
				notify_check_usb_suspend(true, true);
				oneplus_notify_pmic_check_charger_present();
			}
			break;
		case DASH_NOTIFY_REQUEST_IRQ:
			request_mcu_irq(di);
			break;
		case DASH_NOTIFY_UPDATE_DASH_PRESENT:
			if (arg == DASH_NOTIFY_UPDATE_DASH_PRESENT+1)
				update_fast_chg_started();
			break;
		default:
			pr_err("bad ioctl %u\n", cmd);
	}
	return 0;
}

static ssize_t dash_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct fastchg_device_info *di = filp->private_data;

	dashchg_firmware_data = kmalloc(count, GFP_ATOMIC);
	/*malloc for firmware, do not free*/
	if (di->firmware_already_updated)
		return 0;
	di->dashchg_fw_ver_count = count;
	if (copy_from_user(dashchg_firmware_data, buf, count)) {
		pr_err("failed to copy from user space\n");
		kfree(dashchg_firmware_data);
		return -EFAULT;
	}
	pr_info("fw_ver_count=%d\n", di->dashchg_fw_ver_count);
	return count;
}

static const struct file_operations dash_dev_fops = {
	.owner			= THIS_MODULE,
	.llseek			= no_llseek,
	.write			= dash_dev_write,
	.read			= dash_dev_read,
	.open			= dash_dev_open,
	.unlocked_ioctl	= dash_dev_ioctl,
};

static int dash_parse_dt(struct fastchg_device_info *di)
{
	u32 flags;
	struct device_node *dev_node = di->client->dev.of_node;

	if (!dev_node) {
		pr_err("device tree info. missing\n");
		return -EINVAL;
	}

	di->usb_sw_1_gpio = of_get_named_gpio_flags(dev_node,
			"microchip,usb-sw-1-gpio", 0, &flags);
	di->usb_sw_2_gpio = of_get_named_gpio_flags(dev_node,
			"microchip,usb-sw-2-gpio", 0, &flags);
	di->ap_clk = of_get_named_gpio_flags(dev_node,
			"microchip,ap-clk", 0, &flags);
	di->ap_data = of_get_named_gpio_flags(dev_node,
			"microchip,ap-data", 0, &flags);
	di->mcu_en_gpio = of_get_named_gpio_flags(dev_node,
			"microchip,mcu-en-gpio", 0, &flags);
	return 0;
}

static int request_dash_gpios(struct fastchg_device_info *di)
{
	int ret;

	if (gpio_is_valid(di->usb_sw_1_gpio)
		&& gpio_is_valid(di->usb_sw_2_gpio)) {
		ret = gpio_request(di->usb_sw_1_gpio, "usb_sw_1_gpio");
		if (ret)
			pr_err("gpio_request failed for %d ret=%d\n",
			di->usb_sw_1_gpio, ret);
		else
			gpio_direction_output(di->usb_sw_1_gpio, 0);

		ret = gpio_request(di->usb_sw_2_gpio, "usb_sw_2_gpio");
		if (ret)
			pr_err("gpio_request failed for %d ret=%d\n",
			di->usb_sw_2_gpio, ret);
		else
			gpio_direction_output(di->usb_sw_2_gpio, 0);

	}

	if (gpio_is_valid(di->ap_clk)) {
		ret = gpio_request(di->ap_clk, "ap_clk");
		if (ret)
			pr_err("gpio_request failed for %d ret=%d\n",
			di->ap_clk, ret);
	}

	if (gpio_is_valid(di->mcu_en_gpio)) {
		ret = gpio_request(di->mcu_en_gpio, "mcu_en_gpio");
		if (ret)
			pr_err("gpio_request failed for %d ret=%d\n",
			di->mcu_en_gpio, ret);
		else
			gpio_direction_output(di->mcu_en_gpio, 0);
	}

	if (gpio_is_valid(di->ap_data)) {
		ret = gpio_request(di->ap_data, "mcu_data");
		if (ret)
			pr_err("gpio_request failed for %d ret=%d\n",
			di->ap_data, ret);

	}

	return 0;
}

static int dash_pinctrl_init(struct fastchg_device_info *di)
{
	di->pinctrl = devm_pinctrl_get(&di->client->dev);
	if (IS_ERR_OR_NULL(di->pinctrl)) {
		dev_err(&di->client->dev,
				"Unable to acquire pinctrl\n");
		di->pinctrl = NULL;
		return 0;
	}
	di->pinctrl_state_active =
		pinctrl_lookup_state(di->pinctrl, "mux_fastchg_active");
	if (IS_ERR_OR_NULL(di->pinctrl_state_active)) {
		dev_err(&di->client->dev,
				"Can not fastchg_active state\n");
		devm_pinctrl_put(di->pinctrl);
		di->pinctrl = NULL;
		return PTR_ERR(di->pinctrl_state_active);
	}
	di->pinctrl_state_suspended =
		pinctrl_lookup_state(di->pinctrl,
				"mux_fastchg_suspend");
	if (IS_ERR_OR_NULL(di->pinctrl_state_suspended)) {
		dev_err(&di->client->dev,
				"Can not fastchg_suspend state\n");
		devm_pinctrl_put(di->pinctrl);
		di->pinctrl = NULL;
		return PTR_ERR(di->pinctrl_state_suspended);
	}

	di->pinctrl_mcu_data_state_active =
		pinctrl_lookup_state(di->pinctrl,
				"mcu_data_active");
	if (IS_ERR_OR_NULL(di->pinctrl_mcu_data_state_active)) {
		dev_err(&di->client->dev,
				"Can not mcu_data_active state\n");
		devm_pinctrl_put(di->pinctrl);
		di->pinctrl = NULL;
		return PTR_ERR(di->pinctrl_mcu_data_state_active);
	}
	di->pinctrl_mcu_data_state_suspended =
				pinctrl_lookup_state(di->pinctrl,
						"mcu_data_suspend");
	if (IS_ERR_OR_NULL(di->pinctrl_mcu_data_state_suspended)) {
		dev_err(&di->client->dev,
				"Can not fastchg_suspend state\n");
		devm_pinctrl_put(di->pinctrl);
		di->pinctrl = NULL;
		return PTR_ERR(di->pinctrl_mcu_data_state_suspended);
	}

	if (pinctrl_select_state(di->pinctrl,
				di->pinctrl_state_active) < 0)
		pr_err("pinctrl set active fail\n");

	if (pinctrl_select_state(di->pinctrl,
				di->pinctrl_mcu_data_state_active) < 0)
		pr_err("pinctrl set pinctrl_mcu_data_state_active fail\n");

	return 0;

}

static int dash_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct fastchg_device_info *di;
	int ret;

	pr_info("dash_probe\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("i2c_func error\n");
		goto err_check_functionality_failed;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		ret = -ENOMEM;
		goto err_check_functionality_failed;
	}

	di->client = mcu_client = client;
	di->firmware_already_updated = false;
	di->irq_enabled = true;
	di->fast_chg_ing = false;
	di->fast_low_temp_full = false;
	di->fast_chg_started = false;

	fastchg_di = di;

	ret = dash_parse_dt(di);
	if (ret == -EINVAL)
		goto err_read_dt;

	request_dash_gpios(di);
	dash_pinctrl_init(di);
	mutex_init(&di->read_mutex);

	init_waitqueue_head(&di->read_wq);
	wakeup_source_init(&di->fastchg_wake_lock, "fastcg_wake_lock");
	wakeup_source_init(&di->fastchg_update_fireware_lock, "fastchg_fireware_lock");

	INIT_WORK(&di->fastcg_work, fastcg_work_func);
	INIT_WORK(&di->mcu_wdg_work, di_mcu_wdg);
	INIT_DELAYED_WORK(&di->update_fireware_version_work,
		update_fireware_version_func);
	INIT_DELAYED_WORK(&di->update_firmware, dashchg_fw_update);
	INIT_DELAYED_WORK(&di->adapter_update_work, adapter_update_work_func);

	init_timer(&di->watchdog);
	di->watchdog.data = (unsigned long)di;
	di->watchdog.function = di_watchdog;

	di->dash_device.minor = MISC_DYNAMIC_MINOR;
	di->dash_device.name = "dash";
	di->dash_device.fops = &dash_dev_fops;
	ret = misc_register(&di->dash_device);
	/*when everything is ok, regist /dev/dash*/
	if (ret) {
		pr_err("%s : misc_register failed\n", __FILE__);
		goto err_misc_register_failed;
	}

	mcu_init(di);

	fastcharge_information_register(&fastcharge_information);
	schedule_delayed_work(&di->update_fireware_version_work,
			msecs_to_jiffies(SHOW_FW_VERSION_DELAY_MS));
	snoc_clk = clk_get(&client->dev, "snoc");
	cnoc_clk = clk_get(&client->dev, "cnoc");
	pr_info("dash_probe success\n");

	return 0;

err_misc_register_failed:
err_read_dt:
	kfree(di);
err_check_functionality_failed:
	pr_err("dash_probe fail\n");
	return 0;
}

static int dash_remove(struct i2c_client *client)
{
	struct fastchg_device_info *di = dev_get_drvdata(&client->dev);

	fastcharge_information_unregister(&fastcharge_information);
	if (gpio_is_valid(di->mcu_en_gpio))
		gpio_free(di->mcu_en_gpio);
	if (gpio_is_valid(di->usb_sw_1_gpio))
		gpio_free(di->usb_sw_1_gpio);
	if (gpio_is_valid(di->usb_sw_2_gpio))
		gpio_free(di->usb_sw_2_gpio);
	if (gpio_is_valid(di->ap_clk))
		gpio_free(di->ap_clk);
	if (gpio_is_valid(di->ap_data))
		gpio_free(di->ap_data);

	return 0;
}

static void dash_shutdown(struct i2c_client *client)
{
	usb_sw_gpio_set(0);
	mcu_en_reset();
}

static const struct of_device_id dash_match[] = {
	{ .compatible = "microchip,oneplus_fastcg" },
	{ },
};

static const struct i2c_device_id dash_id[] = {
	{ "dash_fastcg", 1 },
	{},
};
MODULE_DEVICE_TABLE(i2c, dash_id);

static struct i2c_driver dash_fastcg_driver = {
	.driver		= {
		.name = "dash_fastcg",
		.owner	= THIS_MODULE,
		.of_match_table = dash_match,
	},
	.probe		= dash_probe,
	.remove		= dash_remove,
	.shutdown	= dash_shutdown,
	.id_table	= dash_id,
};

static int __init dash_fastcg_init(void)
{
	return i2c_add_driver(&dash_fastcg_driver);
}
module_init(dash_fastcg_init);

static void __exit dash_fastcg_exit(void)
{
	i2c_del_driver(&dash_fastcg_driver);
}
module_exit(dash_fastcg_exit);
