// SPDX-License-Identifier: GPL-2.0
/*
 * CST128A capacitive touchscreen driver
 *
 * Based on FocalTech FT5x06 register protocol.
 * Compatible with CST128A / CST128 / FT5x06 series touch controllers.
 *
 * This driver supports the D310 480x800 display panel with CST/FT series
 * touch controller connected via I2C.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>

/* CST128A register definitions (FocalTech-compatible protocol) */
#define CST128A_REG_DEVICE_MODE	0x00	/* Device mode register */
#define CST128A_REG_TD_STATUS	0x02	/* Touch data status register */
#define CST128A_REG_TOUCH_DATA	0x03	/* Touch data start register */
#define CST128A_REG_ID_G_MODE	0xA4	/* Interrupt mode register */

#define CST128A_MAX_TOUCH_POINTS	5

/* Touch event types */
#define TOUCH_EVENT_DOWN	0x00
#define TOUCH_EVENT_UP		0x01
#define TOUCH_EVENT_CONTACT	0x02
#define TOUCH_EVENT_RESERVED	0x03

struct cst128a_ts_data {
	struct i2c_client *client;
	struct input_dev *input;
	int reset_gpio;
	int irq_gpio;
	u32 x_max;
	u32 y_max;
};

static bool cst128a_debug;
module_param_named(debug, cst128a_debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable CST128A debug logs");

static int cst128a_i2c_write(struct cst128a_ts_data *ts,
			     u8 addr, u8 *buf, u16 len)
{
	struct i2c_client *client = ts->client;
	struct i2c_msg msg;
	u8 send_buf[6] = {0};
	int ret;

	if (len > sizeof(send_buf) - 1) {
		dev_err(&client->dev, "write len %d too large\n", len);
		return -EINVAL;
	}

	send_buf[0] = addr;
	if (len > 0)
		memcpy(&send_buf[1], buf, len);

	msg.flags = 0;
	msg.addr = client->addr;
	msg.buf = send_buf;
	msg.len = len + 1;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret == 1)
		return 0;

	dev_err(&client->dev, "%s: write error, addr=0x%02x len=%d ret=%d\n",
		__func__, addr, len, ret);
	return ret < 0 ? ret : -EIO;
}

static int cst128a_i2c_read(struct cst128a_ts_data *ts,
			    u8 addr, u8 *buf, u16 len)
{
	struct i2c_client *client = ts->client;
	struct i2c_msg msg[2];
	int ret;

	msg[0].flags = 0;
	msg[0].addr = client->addr;
	msg[0].buf = &addr;
	msg[0].len = 1;

	msg[1].flags = I2C_M_RD;
	msg[1].addr = client->addr;
	msg[1].buf = buf;
	msg[1].len = len;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret == 2)
		return 0;

	dev_err(&client->dev, "%s: read error, addr=0x%02x len=%d ret=%d\n",
		__func__, addr, len, ret);
	return ret < 0 ? ret : -EIO;
}

static int cst128a_ts_reset(struct cst128a_ts_data *ts)
{
	if (!gpio_is_valid(ts->reset_gpio))
		return 0;

	gpio_set_value_cansleep(ts->reset_gpio, 1);
	msleep(20);
	gpio_set_value_cansleep(ts->reset_gpio, 0);
	msleep(5);
	gpio_set_value_cansleep(ts->reset_gpio, 1);
	msleep(300);

	return 0;
}

static irqreturn_t cst128a_ts_isr(int irq, void *dev_id)
{
	struct cst128a_ts_data *ts = dev_id;
	u8 rdbuf[30] = {0};
	int i, type, x, y, id;
	bool down;
	int ret;
	int valid_points = 0;

	/* Read touch data: status register + up to 5 touch point data */
	ret = cst128a_i2c_read(ts, CST128A_REG_TD_STATUS, rdbuf, 29);
	if (ret) {
		if (cst128a_debug)
			dev_err(&ts->client->dev,
				"ISR read failed: irq=%d ret=%d irq_gpio=%d level=%d\n",
				irq, ret, ts->irq_gpio,
				gpio_is_valid(ts->irq_gpio) ?
				gpio_get_value_cansleep(ts->irq_gpio) : -1);
		goto out;
	}

	if (cst128a_debug)
		dev_info(&ts->client->dev,
			 "ISR: irq=%d status=0x%02x raw=%*ph\n",
			 irq, rdbuf[0], 29, rdbuf);

	for (i = 0; i < CST128A_MAX_TOUCH_POINTS; i++) {
		u8 *buf = &rdbuf[i * 6 + 1];

		/*
		 * Touch point data format (6 bytes per point):
		 * buf[0]: [7:6] event flag, [3:0] X high 4 bits
		 * buf[1]: X low 8 bits
		 * buf[2]: [7:4] touch ID, [3:0] Y high 4 bits
		 * buf[3]: Y low 8 bits
		 */
		type = buf[0] >> 6;
		if (type == TOUCH_EVENT_RESERVED)
			continue;
		valid_points++;

		x = ((buf[0] & 0x0f) << 8) | buf[1];
		y = ((buf[2] & 0x0f) << 8) | buf[3];
		id = (buf[2] >> 4) & 0x0f;
		down = (type != TOUCH_EVENT_UP);

		/* Clamp coordinates */
		if (x > ts->x_max)
			x = ts->x_max;
		if (y > ts->y_max)
			y = ts->y_max;

		input_mt_slot(ts->input, id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, down);

		if (!down)
			continue;

		input_report_abs(ts->input, ABS_MT_POSITION_X, x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, y);

		if (cst128a_debug)
			dev_info(&ts->client->dev,
				 "ISR point[%d]: id=%d type=%d x=%d y=%d\n",
				 i, id, type, x, y);
	}

	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input);

	if (cst128a_debug && valid_points == 0)
		dev_info(&ts->client->dev, "ISR: no valid touch points decoded\n");

out:
	return IRQ_HANDLED;
}

static int cst128a_ts_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct cst128a_ts_data *ts;
	struct input_dev *input;
	struct device_node *np = client->dev.of_node;
	u32 x_max = 480;
	u32 y_max = 800;
	u8 data;
	u8 status;
	int ret;

	dev_info(&client->dev, "CST128A touch probe, addr=0x%02x\n",
		 client->addr);

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;

	/* Parse optional resolution from device tree */
	if (np) {
		of_property_read_u32(np, "touchscreen-size-x", &x_max);
		of_property_read_u32(np, "touchscreen-size-y", &y_max);
	}
	ts->x_max = x_max;
	ts->y_max = y_max;

	/* Get reset GPIO */
	ts->reset_gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (gpio_is_valid(ts->reset_gpio)) {
		ret = devm_gpio_request_one(&client->dev, ts->reset_gpio,
					    GPIOF_OUT_INIT_HIGH,
					    "cst128a reset");
		if (ret) {
			dev_err(&client->dev, "Failed to request reset GPIO: %d\n", ret);
			return ret;
		}
	} else {
		dev_warn(&client->dev, "No reset GPIO specified\n");
	}

	/* Get IRQ GPIO */
	ts->irq_gpio = of_get_named_gpio(np, "irq-gpios", 0);
	if (!gpio_is_valid(ts->irq_gpio)) {
		dev_err(&client->dev, "Failed to get irq GPIO\n");
		return -EINVAL;
	}

	ret = devm_gpio_request_one(&client->dev, ts->irq_gpio,
				    GPIOF_IN, "cst128a irq");
	if (ret) {
		dev_err(&client->dev, "Failed to request irq GPIO: %d\n", ret);
		return ret;
	}

	/* Reset the touch controller */
	ret = cst128a_ts_reset(ts);
	if (ret)
		return ret;

	/*
	 * Refuse to bind if the controller does not answer basic I2C traffic.
	 * Registering an input device after repeated -EREMOTEIO errors leaves
	 * a broken touchscreen node behind and can trigger severe IRQ churn.
	 */
	ret = cst128a_i2c_read(ts, CST128A_REG_TD_STATUS, &status, 1);
	if (ret) {
		dev_err(&client->dev,
			"touch controller is not responding on I2C: %d\n", ret);
		return ret;
	}
	if (cst128a_debug)
		dev_info(&client->dev, "Initial TD_STATUS=0x%02x\n", status);

	/* Initialize CST128A: set to normal operating mode */
	data = 0;
	ret = cst128a_i2c_write(ts, CST128A_REG_DEVICE_MODE, &data, 1);
	if (ret) {
		dev_err(&client->dev, "failed to set device mode: %d\n", ret);
		return ret;
	}

	/* Set interrupt mode: trigger on touch */
	data = 1;
	ret = cst128a_i2c_write(ts, CST128A_REG_ID_G_MODE, &data, 1);
	if (ret) {
		dev_err(&client->dev, "failed to set interrupt mode: %d\n", ret);
		return ret;
	}

	/* Register interrupt */
	ret = devm_request_threaded_irq(&client->dev,
					gpio_to_irq(ts->irq_gpio),
					NULL, cst128a_ts_isr,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					client->name, ts);
	if (ret) {
		dev_err(&client->dev, "Failed to request IRQ: %d\n", ret);
		return ret;
	}

	/* Setup input device */
	input = devm_input_allocate_device(&client->dev);
	if (!input) {
		dev_err(&client->dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}

	ts->input = input;
	input->name = "CST128A Touchscreen";
	input->id.bustype = BUS_I2C;
	input->dev.parent = &client->dev;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->x_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->y_max, 0, 0);

	ret = input_mt_init_slots(input, CST128A_MAX_TOUCH_POINTS,
				  INPUT_MT_DIRECT);
	if (ret) {
		dev_err(&client->dev, "Failed to init MT slots: %d\n", ret);
		return ret;
	}

	ret = input_register_device(input);
	if (ret) {
		dev_err(&client->dev, "Failed to register input device: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(client, ts);

	dev_info(&client->dev,
		 "CST128A touchscreen ready (%dx%d, irq_gpio=%d, rst_gpio=%d)\n",
		 ts->x_max, ts->y_max, ts->irq_gpio, ts->reset_gpio);

	return 0;
}

static int cst128a_ts_remove(struct i2c_client *client)
{
	/* devm handles cleanup */
	return 0;
}

static const struct of_device_id cst128a_of_match[] = {
	{ .compatible = "hyn,cst128a", },
	{ .compatible = "hynitron,cst128a", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cst128a_of_match);

static const struct i2c_device_id cst128a_id[] = {
	{ "cst128a_ts", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, cst128a_id);

static struct i2c_driver cst128a_ts_driver = {
	.driver = {
		.name		= "cst128a_ts",
		.of_match_table	= of_match_ptr(cst128a_of_match),
	},
	.probe		= cst128a_ts_probe,
	.remove		= cst128a_ts_remove,
	.id_table	= cst128a_id,
};

module_i2c_driver(cst128a_ts_driver);

MODULE_DESCRIPTION("CST128A Capacitive Touchscreen Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("LinTx");
MODULE_INFO(intree, "Y");
