// SPDX-License-Identifier: GPL-2.0
/*
 * Input driver for joysticks connected over ADC.
 * Copyright (c) 2019-2020 Artur Rojek <contact@artur-rojek.eu>
 */
#include <linux/ctype.h>
#include <linux/input.h>
#include <linux/iio/iio.h>
#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sort.h>

#include <linux/unaligned.h>

struct adc_joystick_axis {
	u32 code;
	bool inverted;
};

struct adc_joystick {
	struct input_dev *input;
	struct iio_cb_buffer *buffer;
	struct iio_channel *chans;
	int *offsets;
	bool polled;
	unsigned int num_chans;
	struct adc_joystick_axis axes[] __counted_by(num_chans);
};

static int adc_joystick_invert(struct input_dev *dev,
			       unsigned int axis, int val)
{
	int min = input_abs_get_min(dev, axis);
	int max = input_abs_get_max(dev, axis);

	return (max + min) - val;
}

static void adc_joystick_poll(struct input_dev *input)
{
	struct adc_joystick *joy = input_get_drvdata(input);
	int i, val, ret;

	for (i = 0; i < joy->num_chans; i++) {
		ret = iio_read_channel_raw(&joy->chans[i], &val);
		if (ret < 0)
			return;
		if (joy->axes[i].inverted)
			val = adc_joystick_invert(input, i, val);
		input_report_abs(input, joy->axes[i].code, val);
	}
	input_sync(input);
}

static int adc_joystick_handle(const void *data, void *private)
{
	struct adc_joystick *joy = private;
	enum iio_endian endianness;
	int bytes, msb, val, off, i;
	const u8 *chan_data;
	bool sign;

	bytes = joy->chans[0].channel->scan_type.storagebits >> 3;

	for (i = 0; i < joy->num_chans; ++i) {
		endianness = joy->chans[i].channel->scan_type.endianness;
		msb = joy->chans[i].channel->scan_type.realbits - 1;
		sign = tolower(joy->chans[i].channel->scan_type.sign) == 's';
		off = joy->offsets[i];

		if (off < 0)
			return -EINVAL;

		chan_data = (const u8 *)data + off;

		switch (bytes) {
		case 1:
			val = *chan_data;
			break;
		case 2:
			/*
			 * Data is aligned to the sample size by IIO core.
			 * Call `get_unaligned_xe16` to hide type casting.
			 */
			if (endianness == IIO_BE)
				val = get_unaligned_be16(chan_data);
			else if (endianness == IIO_LE)
				val = get_unaligned_le16(chan_data);
			else /* IIO_CPU */
				val = *(const u16 *)chan_data;
			break;
		default:
			return -EINVAL;
		}

		val >>= joy->chans[i].channel->scan_type.shift;
		if (sign)
			val = sign_extend32(val, msb);
		else
			val &= GENMASK(msb, 0);
		if (joy->axes[i].inverted)
			val = adc_joystick_invert(joy->input, i, val);
		input_report_abs(joy->input, joy->axes[i].code, val);
	}

	input_sync(joy->input);

	return 0;
}

static int adc_joystick_si_cmp(const void *a, const void *b, const void *priv)
{
	const struct iio_channel *chans = priv;

	return chans[*(int *)a].channel->scan_index -
	       chans[*(int *)b].channel->scan_index;
}

static int *adc_joystick_get_chan_offsets(struct iio_channel *chans, int count)
{
	struct iio_dev *indio_dev = chans[0].indio_dev;
	struct device *dev = &indio_dev->dev;
	const struct iio_chan_spec *ch;
	int *offsets, *si_order;
	int idx, i, si, length, offset = 0;

	offsets = kmalloc_array(count, sizeof(int), GFP_KERNEL);
	if (!offsets)
		return ERR_PTR(-ENOMEM);

	si_order = kmalloc_array(count, sizeof(int), GFP_KERNEL);
	if (!si_order) {
		kfree(offsets);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < count; ++i)
		si_order[i] = i;
	/* Channels in buffer are ordered by scan index. Sort to match that. */
	sort_r(si_order, count, sizeof(int), adc_joystick_si_cmp, NULL, chans);

	for (i = 0; i < count; ++i) {
		idx = si_order[i];
		ch = chans[idx].channel;
		si = ch->scan_index;

		if (si < 0 || !test_bit(si, indio_dev->active_scan_mask)) {
			offsets[idx] = -1;
			continue;
		}

		/* Channels sharing scan indices also share the samples. */
		if (idx > 0 && si == chans[idx - 1].channel->scan_index) {
			offsets[idx] = offsets[idx - 1];
			continue;
		}

		offsets[idx] = offset;

		length = ch->scan_type.storagebits / 8;
		if (ch->scan_type.repeat > 1)
			length *= ch->scan_type.repeat;

		/* Account for channel alignment. */
		if (offset % length)
			offset += length - (offset % length);
		offset += length;
	}

	kfree(si_order);

	return offsets;
}

static int adc_joystick_open(struct input_dev *dev)
{
	struct adc_joystick *joy = input_get_drvdata(dev);
	struct device *devp = &dev->dev;
	int ret;

	joy->offsets = adc_joystick_get_chan_offsets(joy->chans,
						     joy->num_chans);
	if (IS_ERR(joy->offsets)) {
		dev_err(devp, "Unable to allocate channel offsets\n");
		return PTR_ERR(joy->offsets);
	}

	ret = iio_channel_start_all_cb(joy->buffer);
	if (ret) {
		dev_err(devp, "Unable to start callback buffer: %d\n", ret);
		kfree(joy->offsets);
		return ret;
	}

	return 0;
}

static void adc_joystick_close(struct input_dev *dev)
{
	struct adc_joystick *joy = input_get_drvdata(dev);

	iio_channel_stop_all_cb(joy->buffer);
	kfree(joy->offsets);
}

static void adc_joystick_cleanup(void *data)
{
	iio_channel_release_all_cb(data);
}

static int adc_joystick_set_axes(struct device *dev, struct adc_joystick *joy)
{
	struct adc_joystick_axis *axes = joy->axes;
	s32 range[2], fuzz, flat;
	unsigned int num_axes;
	int error, i;

	num_axes = device_get_child_node_count(dev);
	if (!num_axes) {
		dev_err(dev, "Unable to find child nodes\n");
		return -EINVAL;
	}

	if (num_axes != joy->num_chans) {
		dev_err(dev, "Got %d child nodes for %d channels\n",
			num_axes, joy->num_chans);
		return -EINVAL;
	}

	device_for_each_child_node_scoped(dev, child) {
		error = fwnode_property_read_u32(child, "reg", &i);
		if (error) {
			dev_err(dev, "reg invalid or missing\n");
			return error;
		}

		if (i >= num_axes) {
			dev_err(dev, "No matching axis for reg %d\n", i);
			return -EINVAL;
		}

		error = fwnode_property_read_u32(child, "linux,code",
						 &axes[i].code);
		if (error) {
			dev_err(dev, "linux,code invalid or missing\n");
			return error;
		}

		error = fwnode_property_read_u32_array(child, "abs-range",
						       range, 2);
		if (error) {
			dev_err(dev, "abs-range invalid or missing\n");
			return error;
		}

		if (range[0] > range[1]) {
			dev_dbg(dev, "abs-axis %d inverted\n", i);
			axes[i].inverted = true;
			swap(range[0], range[1]);
		}

		if (fwnode_property_read_u32(child, "abs-fuzz", &fuzz))
			fuzz = 0;

		if (fwnode_property_read_u32(child, "abs-flat", &flat))
			flat = 0;

		input_set_abs_params(joy->input, axes[i].code,
				     range[0], range[1], fuzz, flat);
	}

	return 0;
}


static int adc_joystick_count_channels(struct device *dev,
				       const struct iio_channel *chans,
				       bool polled,
				       unsigned int *num_chans)
{
	int bits;
	int i;

	/*
	 * Count how many channels we got. NULL terminated.
	 * Do not check the storage size if using polling.
	 */
	for (i = 0; chans[i].indio_dev; i++) {
		if (polled)
			continue;
		bits = chans[i].channel->scan_type.storagebits;
		if (!bits || bits > 16) {
			dev_err(dev, "Unsupported channel storage size\n");
			return -EINVAL;
		}
		if (bits != chans[0].channel->scan_type.storagebits) {
			dev_err(dev, "Channels must have equal storage size\n");
			return -EINVAL;
		}
	}

	*num_chans = i;
	return 0;
}

static int adc_joystick_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_channel *chans;
	struct adc_joystick *joy;
	struct input_dev *input;
	unsigned int poll_interval = 0;
	unsigned int num_chans;
	int error;

	chans = devm_iio_channel_get_all(dev);
	error = PTR_ERR_OR_ZERO(chans);
	if (error) {
		if (error != -EPROBE_DEFER)
			dev_err(dev, "Unable to get IIO channels");
		return error;
	}

	error = device_property_read_u32(dev, "poll-interval", &poll_interval);
	if (error) {
		/* -EINVAL means the property is absent. */
		if (error != -EINVAL)
			return error;
	} else if (poll_interval == 0) {
		dev_err(dev, "Unable to get poll-interval\n");
		return -EINVAL;
	}

	error = adc_joystick_count_channels(dev, chans, poll_interval != 0,
					    &num_chans);
	if (error)
		return error;

	joy = devm_kzalloc(dev, struct_size(joy, axes, num_chans), GFP_KERNEL);
	if (!joy)
		return -ENOMEM;

	joy->chans = chans;
	joy->num_chans = num_chans;

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "Unable to allocate input device\n");
		return -ENOMEM;
	}

	joy->input = input;
	input->name = pdev->name;
	input->id.bustype = BUS_HOST;

	error = adc_joystick_set_axes(dev, joy);
	if (error)
		return error;

	if (poll_interval != 0) {
		input_setup_polling(input, adc_joystick_poll);
		input_set_poll_interval(input, poll_interval);
	} else {
		input->open = adc_joystick_open;
		input->close = adc_joystick_close;

		joy->buffer = iio_channel_get_all_cb(dev, adc_joystick_handle,
						     joy);
		if (IS_ERR(joy->buffer)) {
			dev_err(dev, "Unable to allocate callback buffer\n");
			return PTR_ERR(joy->buffer);
		}

		error = devm_add_action_or_reset(dev, adc_joystick_cleanup,
						 joy->buffer);
		if (error) {
			dev_err(dev, "Unable to add action\n");
			return error;
		}
	}

	input_set_drvdata(input, joy);

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "Unable to register input device\n");
		return error;
	}

	return 0;
}

static const struct of_device_id adc_joystick_of_match[] = {
	{ .compatible = "adc-joystick", },
	{ }
};
MODULE_DEVICE_TABLE(of, adc_joystick_of_match);

static struct platform_driver adc_joystick_driver = {
	.driver = {
		.name = "adc-joystick",
		.of_match_table = adc_joystick_of_match,
	},
	.probe = adc_joystick_probe,
};
module_platform_driver(adc_joystick_driver);

MODULE_DESCRIPTION("Input driver for joysticks connected over ADC");
MODULE_AUTHOR("Artur Rojek <contact@artur-rojek.eu>");
MODULE_LICENSE("GPL");
