// SPDX-License-Identifier: GPL-2.0-only
/*
 * RSInput Gamepad Driver
 *
 * Copyright (C) 2024 Teguh Sobirin <teguh@sobir.in>
 *
 */
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <uapi/linux/sched/types.h>

#define FRAME_HEAD_1              0xA5
#define FRAME_HEAD_2              0xD3
#define FRAME_HEAD_3              0x5A
#define FRAME_HEAD_4              0x3D

#define CMD_COMMOD                0x01
#define CMD_STATUS                0x02

#define DATA_COMMOD_VERSION       0x02
#define DATA_COMMOD_SET_PAR       0x05

#define FRAME_POS_SEQ             4
#define FRAME_POS_CMD             5
#define FRAME_POS_LEN_L           6
#define FRAME_POS_LEN_H           7
#define FRAME_POS_DATA_1          8
#define FRAME_POS_DATA_2          9
#define FRAME_POS_DATA_3          10
#define FRAME_POS_DATA_4          11
#define FRAME_POS_DATA_5          12
#define FRAME_POS_DATA_6          13
#define FRAME_POS_DATA_7          14
#define FRAME_POS_DATA_8          15
#define FRAME_POS_DATA_9          16
#define FRAME_POS_DATA_10         17
#define FRAME_POS_DATA_11         18
#define FRAME_POS_DATA_12         19
#define FRAME_POS_DATA_13         20
#define FRAME_POS_DATA_14         21

#define MCU_PKT_SIZE_MIN          9

#define MCU_VERSION_MAX_LEN       64
#define REPORT_RESUME_TIME        500

#define GAMEPAD_RAW_AXIS_MIN     -0x580
#define GAMEPAD_RAW_AXIS_MAX      0x580

#define GAMEPAD_RAW_TRIGGER_MIN   0x162
#define GAMEPAD_RAW_TRIGGER_MAX   0x755
#define GAMEPAD_OFFSET_TRIGGER_MAX (GAMEPAD_RAW_TRIGGER_MAX - GAMEPAD_RAW_TRIGGER_MIN)

struct rsinput_driver {
	struct serdev_device *serdev;
	struct input_dev *input;
	struct regulator *vdd;
	struct gpio_desc *boot_gpio;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	uint8_t rx_buf[1024];
	uint8_t sequence_number;
	bool resuming;
	ktime_t resume_time;
	const uint16_t *keymap;
	uint8_t num_keymaps;
	struct mutex mutex;
	bool invert_left_stick;
};

static const uint16_t keymap_standard[] = {
	BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
	BTN_WEST,    BTN_NORTH,     BTN_EAST,      BTN_SOUTH,
	BTN_TL,      BTN_TR,        BTN_SELECT,    BTN_START,
	BTN_THUMBL,  BTN_THUMBR,    KEY_HOME,      KEY_BACK
};

static const uint16_t keymap_face_swapped[] = {
	BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
	BTN_NORTH,   BTN_WEST,      BTN_SOUTH,     BTN_EAST,
	BTN_TL,      BTN_TR,        BTN_SELECT,    BTN_START,
	BTN_THUMBL,  BTN_THUMBR,    KEY_HOME,      KEY_BACK
};

static uint8_t compute_checksum(const uint8_t *data, size_t len)
{
	uint8_t checksum = 0;

	for (size_t i = FRAME_POS_SEQ; i < len - 1; i++)
		checksum ^= data[i];

	return checksum;
}

static int rsinput_send_command(struct rsinput_driver *drv, uint8_t cmd, const uint8_t *data,
				size_t len)
{
	uint8_t frame[256];
	uint8_t checksum = 0;
	size_t frame_len = 0;

	frame[frame_len++] = FRAME_HEAD_1;
	frame[frame_len++] = FRAME_HEAD_2;
	frame[frame_len++] = FRAME_HEAD_3;
	frame[frame_len++] = FRAME_HEAD_4;

	frame[frame_len++] = drv->sequence_number;
	drv->sequence_number++;

	frame[frame_len++] = cmd;

	frame[frame_len++] = len & 0xFF;
	frame[frame_len++] = (len >> 8) & 0xFF;

	if (data && len) {
		memcpy(&frame[frame_len], data, len);
		frame_len += len;
	}

	checksum = compute_checksum(frame, frame_len + 1);
	frame[frame_len++] = checksum;

	return serdev_device_write_buf(drv->serdev, frame, frame_len);
}

static int rsinput_init_commands(struct rsinput_driver *drv)
{
	int error;

	msleep(50);
	uint8_t version_request[] = {DATA_COMMOD_VERSION};

	error = rsinput_send_command(drv, CMD_COMMOD, version_request, sizeof(version_request));
	if (error < 0) {
		dev_err(&drv->serdev->dev, "Failed to request MCU version: %d\n", error);
		return error;
	}

	msleep(50);
	uint8_t mcu_params[] = {
		DATA_COMMOD_SET_PAR, 0x01,
		0x00, 0x00, 0x00, 0x28,
		0x00, 0x00, 0x00, 0x07
	};
	error = rsinput_send_command(drv, CMD_COMMOD, mcu_params, sizeof(mcu_params));
	if (error < 0) {
		dev_err(&drv->serdev->dev, "Failed to set MCU parameters: %d\n", error);
		return error;
	}

	return 0;
}

static void handle_cmd_commod(struct rsinput_driver *drv, const uint8_t *data,
			      size_t payload_length)
{
	switch (data[FRAME_POS_DATA_1]) {
	case DATA_COMMOD_VERSION:
		if (payload_length >= 1) {
			char mcu_version[MCU_VERSION_MAX_LEN] = {0};
			size_t version_length = payload_length;

			if (version_length > MCU_VERSION_MAX_LEN - 1)
				version_length = MCU_VERSION_MAX_LEN - 1;
			memcpy(mcu_version, &data[FRAME_POS_DATA_1], version_length);
			mcu_version[version_length] = '\0';
			dev_info(&drv->serdev->dev, "MCU Version: %s\n", mcu_version);
		} else {
			dev_err(&drv->serdev->dev, "Invalid MCU version response length\n");
		}
		break;
	case DATA_COMMOD_SET_PAR:
		dev_dbg(&drv->serdev->dev, "MCU parameters set successfully\n");
		break;
	default:
		dev_dbg(&drv->serdev->dev, "Unhandled CMD_COMMOD sub-command: 0x%02x\n",
			data[FRAME_POS_DATA_1]);
		break;
	}
}

static void handle_cmd_status(struct rsinput_driver *drv, const uint8_t *data,
			      size_t payload_length)
{
	if (drv->resuming) {
		if (ktime_ms_delta(ktime_get(), drv->resume_time) < REPORT_RESUME_TIME)
			return;

		drv->resuming = false;
	}

	if (payload_length >= 6) {
		static unsigned long prev_states;
		unsigned long keys = data[FRAME_POS_DATA_1] | (data[FRAME_POS_DATA_2] << 8);
		unsigned long current_states = keys, changes;
		int i;
		int8_t flip;

		mutex_lock(&drv->mutex);
		bitmap_xor(&changes, &current_states, &prev_states, drv->num_keymaps);

		for_each_set_bit(i, &changes, drv->num_keymaps) {
			input_report_key(drv->input, drv->keymap[i], (current_states & BIT(i)));
		}

		flip = drv->invert_left_stick ? 1 : -1;
		mutex_unlock(&drv->mutex);

		input_report_abs(drv->input, ABS_BRAKE,
			GAMEPAD_OFFSET_TRIGGER_MAX -
			(data[FRAME_POS_DATA_3] | (data[FRAME_POS_DATA_4] << 8)));
		input_report_abs(drv->input, ABS_GAS,
			GAMEPAD_OFFSET_TRIGGER_MAX -
			(data[FRAME_POS_DATA_5] | (data[FRAME_POS_DATA_6] << 8)));
		input_report_abs(drv->input, ABS_X,
			(int16_t)(data[FRAME_POS_DATA_7] | (data[FRAME_POS_DATA_8] << 8)) * flip);
		input_report_abs(drv->input, ABS_Y,
			(int16_t)(data[FRAME_POS_DATA_9] | (data[FRAME_POS_DATA_10] << 8)) * flip);
		input_report_abs(drv->input, ABS_RX,
			-(int16_t)(data[FRAME_POS_DATA_11] | (data[FRAME_POS_DATA_12] << 8)));
		input_report_abs(drv->input, ABS_RY,
			-(int16_t)(data[FRAME_POS_DATA_13] | (data[FRAME_POS_DATA_14] << 8)));

		input_sync(drv->input);
		prev_states = keys;
	} else {
		dev_warn(&drv->serdev->dev, "Invalid CMD_STATUS response length\n");
	}
}

static void rsinput_process_data(struct rsinput_driver *drv, const uint8_t *data, size_t len)
{
	while (len >= MCU_PKT_SIZE_MIN) {
		if (data[0] != FRAME_HEAD_1) {
			data++;
			len--;
			continue;
		}

		uint16_t payload_length = data[FRAME_POS_LEN_L] | (data[FRAME_POS_LEN_H] << 8);
		size_t frame_length = MCU_PKT_SIZE_MIN + payload_length;

		if (len < frame_length)
			return;

		uint8_t received_checksum = data[frame_length - 1];
		uint8_t computed_checksum = compute_checksum(data, frame_length);

		if (computed_checksum != received_checksum) {
			data += frame_length;
			len -= frame_length;
			continue;
		}

		switch (data[FRAME_POS_CMD]) {
		case CMD_COMMOD:
			handle_cmd_commod(drv, data, payload_length);
			break;
		case CMD_STATUS:
			handle_cmd_status(drv, data, payload_length);
			break;
		default:
			dev_warn(&drv->serdev->dev, "Unhandled command: 0x%02X\n",
				 data[FRAME_POS_CMD]);
			break;
		}

		data += frame_length;
		len -= frame_length;
	}

	if (len > 0)
		dev_warn(&drv->serdev->dev, "Trailing bytes after processing: %zu\n", len);
}

static size_t rsinput_rx(struct serdev_device *serdev, const u8 *data, size_t count)
{
	struct rsinput_driver *drv = serdev_device_get_drvdata(serdev);

	if (!drv || !data || count == 0) {
		dev_dbg_ratelimited(&serdev->dev, "Invalid RX data\n");
		goto error;
	}

	if (count > sizeof(drv->rx_buf)) {
		dev_dbg_ratelimited(&serdev->dev, "RX buffer overflow\n");
		goto error;
	}

	if (count < MCU_PKT_SIZE_MIN) {
		dev_dbg_ratelimited(&serdev->dev, "Frame too short for checksum validation\n");
		goto error;
	}

	memcpy(drv->rx_buf, data, count);

	rsinput_process_data(drv, drv->rx_buf, count);

error:
	return count;
}

static const struct serdev_device_ops rsinput_rx_ops = {
	.receive_buf = rsinput_rx,
};

static ssize_t invert_left_stick_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rsinput_driver *drv = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", !!drv->invert_left_stick);
}

static ssize_t invert_left_stick_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct rsinput_driver *drv = dev_get_drvdata(dev);
	unsigned long value;

	if (kstrtoul(buf, 0, &value))
		return -EINVAL;

	if (!!value && drv->invert_left_stick)
		goto out;

	mutex_lock(&drv->mutex);

	drv->invert_left_stick = !!value;

	mutex_unlock(&drv->mutex);

out:
	return len;
}

static DEVICE_ATTR_RW(invert_left_stick);

static ssize_t swap_face_buttons_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rsinput_driver *drv = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", (drv->keymap == keymap_face_swapped));
}

static ssize_t swap_face_buttons_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct rsinput_driver *drv = dev_get_drvdata(dev);
	unsigned long value;

	if (kstrtoul(buf, 0, &value))
		return -EINVAL;

	if (!!value && (drv->keymap == keymap_face_swapped))
		goto out;

	mutex_lock(&drv->mutex);

	if (value) {
		drv->keymap = keymap_face_swapped;
		drv->num_keymaps = ARRAY_SIZE(keymap_face_swapped);
	} else {
		drv->keymap = keymap_standard;
		drv->num_keymaps = ARRAY_SIZE(keymap_standard);
	}

	mutex_unlock(&drv->mutex);

out:
	return len;
}

static DEVICE_ATTR_RW(swap_face_buttons);

static struct attribute *rsinput_sysfs_attrs[] = {
	&dev_attr_invert_left_stick.attr,
	&dev_attr_swap_face_buttons.attr,
	NULL
};
ATTRIBUTE_GROUPS(rsinput_sysfs);

static int rsinput_probe(struct serdev_device *serdev)
{
	struct rsinput_driver *drv;
	int error;

	drv = devm_kzalloc(&serdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->vdd = devm_regulator_get(&serdev->dev, "vdd");
	if (IS_ERR(drv->vdd)) {
		error = PTR_ERR(drv->vdd);
		return error;
	}

	drv->boot_gpio =
		devm_gpiod_get_optional(&serdev->dev, "boot", GPIOD_OUT_HIGH);
	if (IS_ERR(drv->boot_gpio)) {
		error = PTR_ERR(drv->boot_gpio);
		dev_warn(&serdev->dev, "Unable to get boot gpio: %d\n", error);
	}

	drv->enable_gpio =
		devm_gpiod_get_optional(&serdev->dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(drv->enable_gpio)) {
		error = PTR_ERR(drv->enable_gpio);
		dev_warn(&serdev->dev, "Unable to get enable gpio: %d\n", error);
	}

	drv->reset_gpio =
		devm_gpiod_get_optional(&serdev->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(drv->reset_gpio)) {
		error = PTR_ERR(drv->reset_gpio);
		dev_warn(&serdev->dev, "Unable to get reset gpio: %d\n", error);
	}

	error = regulator_enable(drv->vdd);
	if (error < 0)
		return error;

	if (drv->boot_gpio)
		gpiod_set_value_cansleep(drv->boot_gpio, 0);

	if (drv->reset_gpio)
		gpiod_set_value_cansleep(drv->reset_gpio, 0);

	msleep(20);

	if (drv->enable_gpio)
		gpiod_set_value_cansleep(drv->enable_gpio, 1);

	if (drv->reset_gpio)
		gpiod_set_value_cansleep(drv->reset_gpio, 1);

	msleep(50);

	error = serdev_device_open(serdev);
	if (error)
		return dev_err_probe(&serdev->dev, error, "Unable to open UART device\n");

	drv->serdev = serdev;
	drv->sequence_number = 0;

	serdev_device_set_drvdata(serdev, drv);

	error = serdev_device_set_baudrate(serdev, 115200);
	if (error < 0)
		return dev_err_probe(&serdev->dev, error, "Failed to set up host baud rate\n");

	serdev_device_set_flow_control(serdev, false);

	drv->input = devm_input_allocate_device(&serdev->dev);
	if (!drv->input)
		return -ENOMEM;

	drv->input->phys = "rsinput-gamepad/input0";

	error = device_property_read_string(&serdev->dev, "label", &drv->input->name);
	if (error)
		drv->input->name = "RSInput Gamepad";

	drv->input->id.bustype = BUS_RS232;

	mutex_init(&drv->mutex);

	if (device_property_read_bool(&serdev->dev, "ayntec,face-swapped")) {
		drv->keymap = keymap_face_swapped;
		drv->num_keymaps = ARRAY_SIZE(keymap_face_swapped);
	} else {
		drv->keymap = keymap_standard;
		drv->num_keymaps = ARRAY_SIZE(keymap_standard);
	}

	if (device_property_read_bool(&serdev->dev, "ayntec,invert-left-stick"))
		drv->invert_left_stick = true;

	__set_bit(EV_KEY, drv->input->evbit);
	for (int i = 0; i < drv->num_keymaps; i++)
		input_set_capability(drv->input, EV_KEY, drv->keymap[i]);

	__set_bit(EV_ABS, drv->input->evbit);
	input_set_abs_params(drv->input, ABS_X,  GAMEPAD_RAW_AXIS_MIN, GAMEPAD_RAW_AXIS_MAX, 0, 0);
	input_set_abs_params(drv->input, ABS_Y,  GAMEPAD_RAW_AXIS_MIN, GAMEPAD_RAW_AXIS_MAX, 0, 0);
	input_set_abs_params(drv->input, ABS_RX, GAMEPAD_RAW_AXIS_MIN, GAMEPAD_RAW_AXIS_MAX, 0, 0);
	input_set_abs_params(drv->input, ABS_RY, GAMEPAD_RAW_AXIS_MIN, GAMEPAD_RAW_AXIS_MAX, 0, 0);

	input_set_abs_params(drv->input, ABS_BRAKE, 0, GAMEPAD_OFFSET_TRIGGER_MAX, 0, 0);
	input_set_abs_params(drv->input, ABS_GAS,   0, GAMEPAD_OFFSET_TRIGGER_MAX, 0, 0);

	error = input_register_device(drv->input);
	if (error)
		return error;

	serdev_device_set_client_ops(serdev, &rsinput_rx_ops);

	error = rsinput_init_commands(drv);
	if (error < 0) {
		serdev_device_close(serdev);
		return error;
	}

	return 0;
}

static int rsinput_suspend(struct device *dev)
{
	struct serdev_device *serdev = to_serdev_device(dev);
	struct rsinput_driver *drv = serdev_device_get_drvdata(serdev);

	serdev_device_close(serdev);

	if (drv->enable_gpio)
		gpiod_set_value_cansleep(drv->enable_gpio, 0);

	if (drv->reset_gpio)
		gpiod_set_value_cansleep(drv->reset_gpio, 0);

	regulator_disable(drv->vdd);

	return 0;
}

static int rsinput_resume(struct device *dev)
{
	struct serdev_device *serdev = to_serdev_device(dev);
	struct rsinput_driver *drv = serdev_device_get_drvdata(serdev);
	int error;

	error = regulator_enable(drv->vdd);
	if (error < 0)
		return error;

	if (drv->reset_gpio)
		gpiod_set_value_cansleep(drv->reset_gpio, 0);

	if (drv->enable_gpio)
		gpiod_set_value_cansleep(drv->enable_gpio, 1);

	if (drv->reset_gpio)
		gpiod_set_value_cansleep(drv->reset_gpio, 1);

	msleep(50);

	error = serdev_device_open(serdev);
	if (error)
		return dev_err_probe(dev, error, "Failed to reopen UART on resume\n");

	error = serdev_device_set_baudrate(serdev, 115200);
	if (error < 0)
		return dev_err_probe(dev, error, "Failed to restore baud rate on resume\n");

	serdev_device_set_flow_control(serdev, false);

	drv->resume_time = ktime_get();
	drv->resuming = true;
	drv->sequence_number = 0;

	error = rsinput_init_commands(drv);
	if (error < 0) {
		serdev_device_close(serdev);
		return error;
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rsinput_pm_ops, rsinput_suspend, rsinput_resume);

static void rsinput_remove(struct serdev_device *serdev)
{
	struct rsinput_driver *drv = serdev_device_get_drvdata(serdev);

	serdev_device_close(serdev);
	input_unregister_device(drv->input);
	if (drv->enable_gpio)
		gpiod_set_value_cansleep(drv->enable_gpio, 0);

	if (drv->reset_gpio)
		gpiod_set_value_cansleep(drv->reset_gpio, 0);

	regulator_disable(drv->vdd);
}

static const struct of_device_id rsinput_of_match[] = {
	{ .compatible = "ayntec,rsinput" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rsinput_of_match);

static struct serdev_device_driver rsinput_driver = {
	.probe = rsinput_probe,
	.remove = rsinput_remove,
	.driver = {
		.name = "rsinput",
		.dev_groups = rsinput_sysfs_groups,
		.of_match_table = rsinput_of_match,
		.pm = pm_sleep_ptr(&rsinput_pm_ops),
	},
};

module_serdev_device_driver(rsinput_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RSInput Gamepad Driver");
MODULE_AUTHOR("Teguh Sobirin <teguh@sobir.in>");
