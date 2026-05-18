/*
 * PIUIO / LXIO interface driver
 *
 * Copyright (C) 2012-2014 Devin J. Pohly (djpohly+linux@gmail.com)
 * LXIO V1/V2 support additions
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This code is based on the Linux USB HID keyboard driver by Vojtech Pavlik.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/sysfs.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/leds.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/usb/input.h>


/*
 * Device and protocol definitions
 */
#define USB_VENDOR_ID_ANCHOR 0x0547
#define USB_PRODUCT_ID_PYTHON2 0x1002
#define USB_VENDOR_ID_BTNBOARD 0x0d2f
#define USB_PRODUCT_ID_BTNBOARD 0x1010

#define USB_VENDOR_ID_LXIO 0x0d2f
#define USB_PRODUCT_ID_LXIO_V1 0x1020
#define USB_PRODUCT_ID_LXIO_V2 0x1040

/* USB message used to communicate with the device */
#define PIUIO_MSG_REQ 0xae
#define PIUIO_MSG_VAL 0
#define PIUIO_MSG_IDX 0

#define PIUIO_MSG_SZ 8
#define LXIO_MSG_SZ 16
#define PIUIO_MAX_MSG_SZ 16
#define PIUIO_MSG_LONGS (PIUIO_MAX_MSG_SZ / sizeof(unsigned long))

/* Input keycode ranges */
#define PIUIO_BTN_REG BTN_JOYSTICK
#define PIUIO_NUM_REG (BTN_GAMEPAD - BTN_JOYSTICK)
#define PIUIO_BTN_EXTRA BTN_TRIGGER_HAPPY
#define PIUIO_NUM_EXTRA (KEY_MAX - BTN_TRIGGER_HAPPY)
#define PIUIO_NUM_BTNS (PIUIO_NUM_REG + PIUIO_NUM_EXTRA)


/**
 * struct piuio_led - auxiliary struct for led devices
 * @piu:	Pointer back to the enclosing structure
 * @dev:	Actual led device
 */
struct piuio_led {
	struct piuio *piu;
	struct led_classdev dev;
};

/**
 * struct piuio_devtype - parameters for different types of PIUIO devices
 * @led_names:	Array of LED names, of length @outputs, to use in sysfs
 * @inputs:	Number of input pins
 * @outputs:	Number of output pins
 * @mplex:	Number of sets of inputs
 * @mplex_bits:	Number of output bits reserved for multiplexing
 * @msg_sz:	Size of USB message/report in bytes
 */
struct piuio_devtype {
	const char **led_names;
	int inputs;
	int outputs;
	int mplex;
	int mplex_bits;
	int msg_sz;
};

/**
 * struct piuio - state of each attached PIUIO
 * @type:	Type of PIUIO device
 * @idev:	Input device associated with this PIUIO
 * @phys:	Physical path of the device. @idev's phys field points to this
 *		buffer
 * @udev:	USB device associated with this PIUIO
 * @in:		URB for requesting the current state of one set of inputs
 * @out:	URB for sending data to outputs and multiplexer
 * @cr_in:	Setup packet for @in URB (PIUIO mode)
 * @cr_out:	Setup packet for @out URB (PIUIO mode)
 * @old_inputs:	Previous state of input pins from the @in URB for each of the
 *		input sets (PIUIO mode).  For LXIO mode: previous button bitmask.
 * @inputs:	Buffer for the @in URB
 * @outputs:	Buffer for the @out URB
 * @new_outputs:
 * 		Staging for the @outputs buffer
 * @set:	Current set of inputs to read (0 .. @type->mplex - 1)
 * @is_lxio:	True if this is an LXIO (HID interrupt) device
 */
struct piuio {
	struct piuio_devtype *type;

	struct input_dev *idev;
	char phys[64];

	struct usb_device *udev;
	struct urb *in, *out;
	struct usb_ctrlrequest cr_in, cr_out;
	wait_queue_head_t shutdown_wait;

	unsigned long (*old_inputs)[PIUIO_MSG_LONGS];
	unsigned long inputs[PIUIO_MSG_LONGS];
	unsigned char outputs[PIUIO_MAX_MSG_SZ];
	unsigned char new_outputs[PIUIO_MAX_MSG_SZ];

	struct piuio_led *led;

	int set;
	bool is_lxio;
};

static const char *led_names[] = {
	"piuio::output0",
	"piuio::output1",
	"piuio::output2",
	"piuio::output3",
	"piuio::output4",
	"piuio::output5",
	"piuio::output6",
	"piuio::output7",
	"piuio::output8",
	"piuio::output9",
	"piuio::output10",
	"piuio::output11",
	"piuio::output12",
	"piuio::output13",
	"piuio::output14",
	"piuio::output15",
	"piuio::output16",
	"piuio::output17",
	"piuio::output18",
	"piuio::output19",
	"piuio::output20",
	"piuio::output21",
	"piuio::output22",
	"piuio::output23",
	"piuio::output24",
	"piuio::output25",
	"piuio::output26",
	"piuio::output27",
	"piuio::output28",
	"piuio::output29",
	"piuio::output30",
	"piuio::output31",
	"piuio::output32",
	"piuio::output33",
	"piuio::output34",
	"piuio::output35",
	"piuio::output36",
	"piuio::output37",
	"piuio::output38",
	"piuio::output39",
	"piuio::output40",
	"piuio::output41",
	"piuio::output42",
	"piuio::output43",
	"piuio::output44",
	"piuio::output45",
	"piuio::output46",
	"piuio::output47",
};

static const char *bbled_names[] = {
	"piuio::bboutput0",
	"piuio::bboutput1",
	"piuio::bboutput2",
	"piuio::bboutput3",
	"piuio::bboutput4",
	"piuio::bboutput5",
	"piuio::bboutput6",
	"piuio::bboutput7",
};

static const char *lxio_led_names[] = {
	"piuio::p1_lu",
	"piuio::p1_ru",
	"piuio::p1_cn",
	"piuio::p1_ld",
	"piuio::p1_rd",
	"piuio::p2_lu",
	"piuio::p2_ru",
	"piuio::p2_cn",
	"piuio::p2_ld",
	"piuio::p2_rd",
	"piuio::bass",
	"piuio::halo_r2",
	"piuio::halo_r1",
	"piuio::halo_l2",
	"piuio::halo_l1",
	"piuio::coin_counter",
	"piuio::coin_counter2",
	"piuio::p1_lu_menu",
	"piuio::p1_ru_menu",
	"piuio::p1_cn_menu",
	"piuio::p1_ld_menu",
	"piuio::p1_rd_menu",
	"piuio::p2_lu_menu",
	"piuio::p2_ru_menu",
	"piuio::p2_cn_menu",
	"piuio::p2_ld_menu",
	"piuio::p2_rd_menu",
};

/* Full device parameters */
static struct piuio_devtype piuio_dev_full = {
	.led_names = led_names,
	.inputs = (PIUIO_NUM_BTNS < 48) ? PIUIO_NUM_BTNS : 48,
	.outputs = 48,
	.mplex = 4,
	.mplex_bits = 2,
	.msg_sz = PIUIO_MSG_SZ,
};

/* Button board device parameters */
static struct piuio_devtype piuio_dev_bb = {
	.led_names = bbled_names,
	.inputs = (PIUIO_NUM_BTNS < 8) ? PIUIO_NUM_BTNS : 8,
	.outputs = 8,
	.mplex = 1,
	.mplex_bits = 0,
	.msg_sz = PIUIO_MSG_SZ,
};

/* LXIO V1/V2 device parameters */
static struct piuio_devtype piuio_dev_lxio = {
	.led_names = lxio_led_names,
	.inputs = 96,
	.outputs = ARRAY_SIZE(lxio_led_names),
	.mplex = 1,
	.mplex_bits = 0,
	.msg_sz = LXIO_MSG_SZ,
};


/*
 * Auxiliary functions for reporting input events
 */
static int keycode(unsigned int pin)
{
	/* Use joystick buttons first, then the extra "trigger happy" range. */
	if (pin < PIUIO_NUM_REG)
		return PIUIO_BTN_REG + pin;
	pin -= PIUIO_NUM_REG;
	return PIUIO_BTN_EXTRA + pin;
}

/*
 * LXIO keycode mapping
 *
 * The LXIO 16-byte HID report uses active-low signaling. Each panel
 * button position has 4 independent sensors (bytes 0-3 for P1, 4-7 for
 * P2).  lxio_in_completed ANDs each 4-sensor bank together so that a
 * button registers as pressed when ANY sensor triggers.  After merging,
 * bytes 1-3 hold the same value as byte 0 and bytes 5-7 hold the same
 * value as byte 4 -- we skip bits 8-31 and 40-63 here to avoid
 * reporting duplicate key events.
 *
 * Meaningful byte positions (after sensor merging):
 *   Byte 0  (bits 0-7):   Pad 1 panel sensors
 *   Byte 4  (bits 32-39):  Pad 2 panel sensors
 *   Byte 8  (bits 64-71):  Pad 1 coin/aux
 *   Byte 9  (bits 72-79):  Pad 2 coin/aux
 *   Bytes 10-11 (bits 80-95):  Front buttons
 *
 * Unused:
 *   Bytes 12-15 (bits 96+):     Unused/stale
 */
static int lxio_keycode(unsigned int pin)
{
	/* Skip redundant sensor bytes 1-3, 5-7 and unused bytes 12-15 */
	if ((pin >= 8 && pin < 32) ||
	    (pin >= 40 && pin < 64) ||
	    pin >= 96)
		return 0;

	/* Compact sparse pin ranges into continuous keycode space:
	 *   Raw pin:  0-7, 32-39, 64-71, 72-79, 80-95
	 *   Compact:  0-7,  8-15, 16-23, 24-31, 32-47
	 */
	if (pin >= 80)
		pin -= 48;	/* 80-95  → 32-47 */
	else if (pin >= 64)
		pin -= 48;	/* 64-79  → 16-31 */
	else if (pin >= 32)
		pin -= 24;	/* 32-39  → 8-15  */
	/* 0-7 stays as 0-7 */

	return keycode(pin);
}


/*
 * URB completion handlers
 */
static void piuio_in_completed(struct urb *urb)
{
	struct piuio *piu = urb->context;
	unsigned long changed[PIUIO_MSG_LONGS];
	unsigned long b;
	int i, s;
	int cur_set;
	int ret = urb->status;

	if (ret) {
		dev_warn(&piu->udev->dev, "piuio callback(in): error %d\n", ret);
		goto resubmit;
	}

	/* Get the index of the previous input set (always 0 if no multiplexer) */
	cur_set = (piu->set + piu->type->mplex - 1) % piu->type->mplex;

	/* Note what has changed in this input set, then store the inputs for
	 * next time */
	for (i = 0; i < PIUIO_MSG_LONGS; i++) {
		changed[i] = piu->inputs[i] ^ piu->old_inputs[cur_set][i];
		piu->old_inputs[cur_set][i] = piu->inputs[i];
	}

	/* If we are using a multiplexer, changes only count when none of the
	 * corresponding inputs in other sets are pressed.  Since "pressed"
	 * reads as 0, we can use & to knock those bits out of the changes. */
	for (s = 0; s < piu->type->mplex; s++) {
		if (s == cur_set)
			continue;
		for (i = 0; i < PIUIO_MSG_LONGS; i++)
			changed[i] &= piu->old_inputs[s][i];
	}

	/* For each input which has changed state, report whether it was pressed
	 * or released based on the current value. */
	for_each_set_bit(b, changed, piu->type->inputs) {
		input_event(piu->idev, EV_MSC, MSC_SCAN, b + 1);
		input_report_key(piu->idev, keycode(b), !test_bit(b, piu->inputs));
	}

	/* Done reporting input events */
	input_sync(piu->idev);

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret == -EPERM)
		dev_info(&piu->udev->dev, "piuio resubmit(in): shutdown\n");
	else if (ret)
		dev_err(&piu->udev->dev, "piuio resubmit(in): error %d\n", ret);

	/* Let any waiting threads know we're done here */
	wake_up(&piu->shutdown_wait);
}

static void piuio_out_completed(struct urb *urb)
{
	struct piuio *piu = urb->context;
	int ret = urb->status;

	if (ret) {
		dev_warn(&piu->udev->dev, "piuio callback(out): error %d\n", ret);
		goto resubmit;
	}

	/* Copy in the new outputs */
	memcpy(piu->outputs, piu->new_outputs, piu->type->msg_sz);

	/* If we have a multiplexer, switch to the next input set in rotation
	 * and set the appropriate output bits */
	piu->set = (piu->set + 1) % piu->type->mplex;

	/* Set multiplexer bits */
	piu->outputs[0] &= ~((1 << piu->type->mplex_bits) - 1);
	piu->outputs[0] |= piu->set;
	piu->outputs[2] &= ~((1 << piu->type->mplex_bits) - 1);
	piu->outputs[2] |= piu->set;

resubmit:
	ret = usb_submit_urb(piu->out, GFP_ATOMIC);
	if (ret == -EPERM)
		dev_info(&piu->udev->dev, "piuio resubmit(out): shutdown\n");
	else if (ret)
		dev_err(&piu->udev->dev, "piuio resubmit(out): error %d\n", ret);

	/* Let any waiting threads know we're done here */
	wake_up(&piu->shutdown_wait);
}

/*
 * LXIO IN interrupt completion
 *
 * Extract button states from the 16-byte HID input report (active-low:
 * 0 = pressed) and report changes since the last reading.
 *
 * The real LXIO hardware has 4 independent sensors per panel button,
 * sent as 4 identical byte positions (bytes 0-3 for P1, 4-7 for P2).
 * We AND the 4 bytes together: in active-low logic, a button is
 * pressed when ANY of the 4 sensors reads 0.
 */
static void lxio_in_completed(struct urb *urb)
{
	struct piuio *piu = urb->context;
	unsigned long changed[PIUIO_MSG_LONGS];
	unsigned long merged[PIUIO_MSG_LONGS];
	int i;
	int b;
	int ret = urb->status;

	if (ret) {
		dev_warn(&piu->udev->dev, "lxio callback(in): error %d\n", ret);
		goto resubmit;
	}

	/* Merge 4-sensor banks by ANDing bytes (active-low: 0=pressed,
	 * so AND means "pressed if any sensor triggers") */
	memcpy(merged, piu->inputs, sizeof(merged));
	{
		u8 *m = (u8 *)merged;
		m[0] = m[1] = m[2] = m[3] = m[0] & m[1] & m[2] & m[3];
		m[4] = m[5] = m[6] = m[7] = m[4] & m[5] & m[6] & m[7];
	}

	/* Note what has changed against the previous merged state */
	for (i = 0; i < PIUIO_MSG_LONGS; i++) {
		changed[i] = merged[i] ^ piu->old_inputs[0][i];
		piu->old_inputs[0][i] = merged[i];
	}

	/* Report changes for each mapped bit */
	for_each_set_bit(b, changed, piu->type->inputs) {
		int kc = lxio_keycode(b);

		if (!kc)
			continue;
		input_event(piu->idev, EV_MSC, MSC_SCAN, b + 1);
		input_report_key(piu->idev, kc,
				 !test_bit(b, merged));
	}
	input_sync(piu->idev);

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret == -EPERM)
		dev_info(&piu->udev->dev, "lxio resubmit(in): shutdown\n");
	else if (ret)
		dev_err(&piu->udev->dev, "lxio resubmit(in): error %d\n", ret);

	wake_up(&piu->shutdown_wait);
}

/*
 * LXIO OUT interrupt completion
 *
 * After sending the lamp output report, advance multiplexer (no-op for
 * LXIO since mplex=1) and resubmit.
 */
static void lxio_out_completed(struct urb *urb)
{
	struct piuio *piu = urb->context;
	int ret = urb->status;

	if (ret) {
		dev_warn(&piu->udev->dev, "lxio callback(out): error %d\n", ret);
		goto resubmit;
	}

	/* Copy staged outputs */
	memcpy(piu->outputs, piu->new_outputs, piu->type->msg_sz);

	/* Advance multiplexer (no-op for LXIO) */
	piu->set = (piu->set + 1) % piu->type->mplex;
	
resubmit:
	ret = usb_submit_urb(piu->out, GFP_ATOMIC);
	if (ret == -EPERM)
		dev_info(&piu->udev->dev, "lxio resubmit(out): shutdown\n");
	else if (ret)
		dev_err(&piu->udev->dev, "lxio resubmit(out): error %d\n", ret);

	wake_up(&piu->shutdown_wait);
}


/*
 * Input device events
 */
static int piuio_open(struct input_dev *idev)
{
	struct piuio *piu = input_get_drvdata(idev);
	int ret;

	/* Kick off the polling */
	ret = usb_submit_urb(piu->out, GFP_KERNEL);
	if (ret) {
		dev_err(&piu->udev->dev, "piuio submit(out): error %d\n", ret);
		return -EIO;
	}

	ret = usb_submit_urb(piu->in, GFP_KERNEL);
	if (ret) {
		dev_err(&piu->udev->dev, "piuio submit(in): error %d\n", ret);
		usb_kill_urb(piu->out);
		return -EIO;
	}

	return 0;
}

static void piuio_close(struct input_dev *idev)
{
	struct piuio *piu = input_get_drvdata(idev);
	long remaining;

	/* Stop polling, but wait for the last requests to complete */
	usb_block_urb(piu->in);
	usb_block_urb(piu->out);
	remaining = wait_event_timeout(piu->shutdown_wait,
			atomic_read(&piu->in->use_count) == 0 &&
			atomic_read(&piu->out->use_count) == 0,
			msecs_to_jiffies(5));
	usb_unblock_urb(piu->in);
	usb_unblock_urb(piu->out);

	if (!remaining) {
		// Timed out
		dev_warn(&piu->udev->dev, "piuio close: urb timeout\n");
		usb_kill_urb(piu->in);
		usb_kill_urb(piu->out);
	}

	/* XXX Reset the outputs? */
}


/*
 * Led device event
 */
static void piuio_led_set(struct led_classdev *dev, enum led_brightness b)
{
	struct piuio_led *led = container_of(dev, struct piuio_led, dev);
	struct piuio *piu = led->piu;
	int n;

	n = led - piu->led;
	if (n >= piu->type->outputs) {
		dev_err(&piu->udev->dev, "piuio led: bad number %d\n", n);
		return;
	}

	/* Meh, forget atomicity, these aren't super-important */
	if (b)
		__set_bit(n, (unsigned long *) piu->new_outputs);
	else
		__clear_bit(n, (unsigned long *) piu->new_outputs);
}


/*
 * Structure initialization and destruction
 */
static void piuio_input_init(struct piuio *piu, struct device *parent)
{
	struct input_dev *idev = piu->idev;
	int i;

	/* Fill in basic fields */
	idev->name = piu->is_lxio ? "LXIO input" : "PIUIO input";
	idev->phys = piu->phys;
	usb_to_input_id(piu->udev, &idev->id);
	idev->dev.parent = parent;

	/* HACK: Buttons are sufficient to trigger a /dev/input/js* device, but
	 * for systemd (and consequently udev and Xorg) to consider us a
	 * joystick, we have to have a set of XY absolute axes. */
	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_ABS, idev->evbit);

	/* Configure buttons */
	if (piu->is_lxio) {
		for (i = 0; i < piu->type->inputs; i++) {
			int kc = lxio_keycode(i);
			if (kc)
				set_bit(kc, idev->keybit);
		}
	} else {
		for (i = 0; i < piu->type->inputs; i++)
			set_bit(keycode(i), idev->keybit);
	}
	clear_bit(0, idev->keybit);

	/* Configure fake axes */
	set_bit(ABS_X, idev->absbit);
	set_bit(ABS_Y, idev->absbit);
	input_set_abs_params(idev, ABS_X, 0, 0, 0, 0);
	input_set_abs_params(idev, ABS_Y, 0, 0, 0, 0);

	/* Set device callbacks */
	idev->open = piuio_open;
	idev->close = piuio_close;

	/* Link input device back to PIUIO */
	input_set_drvdata(idev, piu);
}

static int piuio_leds_init(struct piuio *piu)
{
	int i;
	int ret;

	for (i = 0; i < piu->type->outputs; i++) {
		/* Initialize led device and point back to piuio struct */
		piu->led[i].dev.name = piu->type->led_names[i];
		piu->led[i].dev.brightness_set = piuio_led_set;
		piu->led[i].piu = piu;

		/* Register led device */
		ret = led_classdev_register(&piu->udev->dev, &piu->led[i].dev);
		if (ret)
			goto out_unregister;
	}

	return 0;

out_unregister:
	for (--i; i >= 0; i--)
		led_classdev_unregister(&piu->led[i].dev);
	return ret;
}

static void piuio_leds_destroy(struct piuio *piu)
{
	int i;
	for (i = 0; i < piu->type->outputs; i++)
		led_classdev_unregister(&piu->led[i].dev);
}

static int piuio_init(struct piuio *piu, struct input_dev *idev,
		struct usb_device *udev, struct usb_interface *intf)
{
	/* Note: if this function returns an error, piuio_destroy will still be
	 * called, so we don't need to clean up here */

	/* Allocate USB request blocks */
	piu->in = usb_alloc_urb(0, GFP_KERNEL);
	piu->out = usb_alloc_urb(0, GFP_KERNEL);
	if (!piu->in || !piu->out) {
		dev_err(&udev->dev, "piuio init: failed to allocate URBs\n");
		return -ENOMEM;
	}

	/* Create dynamically allocated arrays */
	piu->old_inputs = kzalloc(sizeof(*piu->old_inputs) * piu->type->mplex,
		GFP_KERNEL);
	if (!piu->old_inputs) {
		dev_err(&udev->dev, "piuio init: failed to allocate old_inputs\n");
		return -ENOMEM;
	}

	piu->led = kzalloc(sizeof(*piu->led) * piu->type->outputs, GFP_KERNEL);
	if (!piu->led) {
		dev_err(&udev->dev, "piuio init: failed to allocate led devices\n");
		return -ENOMEM;
	}

	init_waitqueue_head(&piu->shutdown_wait);

	piu->idev = idev;
	piu->udev = udev;
	usb_make_path(udev, piu->phys, sizeof(piu->phys));
	strlcat(piu->phys, "/input0", sizeof(piu->phys));

	/* Prepare URBs depending on device type */
	if (piu->is_lxio) {
		/* LXIO: discover HID interrupt endpoints dynamically */
		struct usb_host_interface *alt = intf->cur_altsetting;
		struct usb_endpoint_descriptor *ep_in = NULL, *ep_out = NULL;
		int i;

		for (i = 0; i < alt->desc.bNumEndpoints; i++) {
			struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
			if (!ep_in && usb_endpoint_is_int_in(ep))
				ep_in = ep;
			else if (!ep_out && usb_endpoint_is_int_out(ep))
				ep_out = ep;
		}

		if (!ep_in || !ep_out) {
			dev_err(&udev->dev, "lxio init: missing interrupt endpoints\n");
			return -ENODEV;
		}

		if (usb_endpoint_maxp(ep_in) > PIUIO_MAX_MSG_SZ ||
		    usb_endpoint_maxp(ep_out) > PIUIO_MAX_MSG_SZ) {
			dev_err(&udev->dev,
				"lxio init: endpoint size exceeds buffer (%d)\n",
				PIUIO_MAX_MSG_SZ);
			return -ENODEV;
		}

		usb_fill_int_urb(piu->out, udev,
				usb_sndintpipe(udev, usb_endpoint_num(ep_out)),
				piu->outputs, usb_endpoint_maxp(ep_out),
				lxio_out_completed, piu,
				ep_out->bInterval);

		usb_fill_int_urb(piu->in, udev,
				usb_rcvintpipe(udev, usb_endpoint_num(ep_in)),
				piu->inputs, usb_endpoint_maxp(ep_in),
				lxio_in_completed, piu,
				ep_in->bInterval);
	} else {
		/* PIUIO: use vendor control transfers */
		piu->cr_out.bRequestType = USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
		piu->cr_out.bRequest = cpu_to_le16(PIUIO_MSG_REQ);
		piu->cr_out.wValue = cpu_to_le16(PIUIO_MSG_VAL);
		piu->cr_out.wIndex = cpu_to_le16(PIUIO_MSG_IDX);
		piu->cr_out.wLength = cpu_to_le16(PIUIO_MSG_SZ);
		usb_fill_control_urb(piu->out, udev, usb_sndctrlpipe(udev, 0),
				(void *) &piu->cr_out, piu->outputs, PIUIO_MSG_SZ,
				piuio_out_completed, piu);

		piu->cr_in.bRequestType = USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
		piu->cr_in.bRequest = cpu_to_le16(PIUIO_MSG_REQ);
		piu->cr_in.wValue = cpu_to_le16(PIUIO_MSG_VAL);
		piu->cr_in.wIndex = cpu_to_le16(PIUIO_MSG_IDX);
		piu->cr_in.wLength = cpu_to_le16(PIUIO_MSG_SZ);
		usb_fill_control_urb(piu->in, udev, usb_rcvctrlpipe(udev, 0),
				(void *) &piu->cr_in, piu->inputs, PIUIO_MSG_SZ,
				piuio_in_completed, piu);
	}

	return 0;
}

static void piuio_destroy(struct piuio *piu)
{
	/* These handle NULL gracefully, so we can call this to clean up if init
	 * fails */
	kfree(piu->led);
	kfree(piu->old_inputs);
	usb_free_urb(piu->out);
	usb_free_urb(piu->in);
}


/*
 * USB connect and disconnect events
 */
static int piuio_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct piuio *piu;
	struct usb_device *udev = interface_to_usbdev(intf);
	struct input_dev *idev;
	int ret = -ENOMEM;

	/* Allocate PIUIO state and determine device type */
	piu = kzalloc(sizeof(struct piuio), GFP_KERNEL);
	if (!piu) {
		dev_err(&intf->dev, "piuio probe: failed to allocate state\n");
		return ret;
	}

	if (id->idVendor == USB_VENDOR_ID_LXIO &&
	    (id->idProduct == USB_PRODUCT_ID_LXIO_V1 ||
	     id->idProduct == USB_PRODUCT_ID_LXIO_V2)) {
		/* LXIO V1/V2 HID device */
		piu->type = &piuio_dev_lxio;
		piu->is_lxio = true;
	} else if (id->idVendor == USB_VENDOR_ID_BTNBOARD &&
		   id->idProduct == USB_PRODUCT_ID_BTNBOARD) {
		/* Button board card */
		piu->type = &piuio_dev_bb;
	} else {
		/* Full card */
		piu->type = &piuio_dev_full;
	}

	/* Allocate input device for generating buttonpresses */
	idev = input_allocate_device();
	if (!idev) {
		dev_err(&intf->dev, "piuio probe: failed to allocate input dev\n");
		kfree(piu->old_inputs);
		kfree(piu);
		return ret;
	}

	/* Initialize PIUIO state and input device */
	ret = piuio_init(piu, idev, udev, intf);
	if (ret)
		goto err;

	piuio_input_init(piu, &intf->dev);

	/* Initialize and register led devices */
	ret = piuio_leds_init(piu);
	if (ret)
		goto err;

	/* Register input device */
	ret = input_register_device(piu->idev);
	if (ret) {
		dev_err(&intf->dev, "piuio probe: failed to register input dev\n");
		piuio_leds_destroy(piu);
		goto err;
	}

	/* Final USB setup */
	usb_set_intfdata(intf, piu);
	return 0;

err:
	piuio_destroy(piu);
	input_free_device(idev);
	kfree(piu);
	return ret;
}

static void piuio_disconnect(struct usb_interface *intf)
{
	struct piuio *piu = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!piu) {
		dev_err(&intf->dev, "piuio disconnect: uninitialized device?\n");
		return;
	}

	usb_kill_urb(piu->in);
	usb_kill_urb(piu->out);
	piuio_leds_destroy(piu);
	input_unregister_device(piu->idev);
	piuio_destroy(piu);
	kfree(piu);
}


/*
 * USB driver and module definitions
 */
static struct usb_device_id piuio_id_table[] = {
	/* Python WDM2 Encoder used for PIUIO boards */
	{ USB_DEVICE(USB_VENDOR_ID_ANCHOR, USB_PRODUCT_ID_PYTHON2) },
	/* Special USB ID for button board devices */
	{ USB_DEVICE(USB_VENDOR_ID_BTNBOARD, USB_PRODUCT_ID_BTNBOARD) },
	/* LXIO V1 HID device */
	{ USB_DEVICE(USB_VENDOR_ID_LXIO, USB_PRODUCT_ID_LXIO_V1) },
	/* LXIO V2 HID device */
	{ USB_DEVICE(USB_VENDOR_ID_LXIO, USB_PRODUCT_ID_LXIO_V2) },
	{},
};

MODULE_DEVICE_TABLE(usb, piuio_id_table);

static struct usb_driver piuio_driver = {
	.name =		"piuio",
	.probe =	piuio_probe,
	.disconnect =	piuio_disconnect,
	.id_table =	piuio_id_table,
};

MODULE_AUTHOR("Devin J. Pohly");
MODULE_DESCRIPTION("PIUIO / LXIO input/output driver");
MODULE_VERSION("1.1");
MODULE_LICENSE("GPL");

module_usb_driver(piuio_driver);
