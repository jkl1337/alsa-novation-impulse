#include "asm-generic/errno-base.h"
#include "linux/gfp_types.h"
#include "sound/asound.h"
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/midi.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#define MODULE_NAME "snd-novimp"
#define PREFIX MODULE_NAME ": "

MODULE_LICENSE("GPL");

#define BUFSIZE 32

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x1235, 0x001b, 2) },
	{},
};

struct novimp_midi_input_endpoint {
	struct novimp *ndev;
	struct snd_rawmidi_substream *substream;

	unsigned char buf[BUFSIZE];
	struct urb *urb;
};

struct novimp_midi_output_endpoint {
	struct novimp *ndev;
	struct snd_rawmidi_substream *substream;

	unsigned char buf[BUFSIZE];
	struct urb *urb;
	bool active;

	int pack_state;
	unsigned char pack_data[2];
};

struct novimp {
	struct usb_device *dev;
	struct snd_card *card;
	struct usb_interface *intf;
	int card_index;

	struct snd_rawmidi *rmidi;
	struct novimp_midi_input_endpoint inputs[2];
	struct novimp_midi_output_endpoint outputs[2];
};

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "card index");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string");

static DEFINE_MUTEX(devices_mutex);
static DECLARE_BITMAP(devices_used, SNDRV_CARDS);

static struct usb_driver snd_novimp_usb_driver;

static inline void novimp_pack_packet(struct urb *urb, uint8_t p0, uint8_t p1,
				      uint8_t p2, uint8_t p3)
{
	uint8_t *buf =
		(uint8_t *)urb->transfer_buffer + urb->transfer_buffer_length;
	buf[0] = p0;
	buf[1] = p1;
	buf[2] = p2;
	buf[3] = p3;
	urb->transfer_buffer_length += 4;
}

#define STATE_UNKNOWN 0
#define STATE_1PARAM 1
#define STATE_2PARAM_1 2
#define STATE_2PARAM_2 3
#define STATE_SYSEX_0 4
#define STATE_SYSEX_1 5
#define STATE_SYSEX_2 6

static void novimp_pack_byte(uint8_t b, struct urb *urb,
			     struct novimp_midi_output_endpoint *ep)
{
	uint8_t p0 = 0;

	if (b >= 0xf8) {
		novimp_pack_packet(urb, 0x0f, b, 0, 0);
	} else if (b >= 0xf0) {
		switch (b) {
		case 0xf0:
			ep->pack_data[0] = b;
			ep->pack_state = STATE_SYSEX_1;
			break;
		case 0xf1:
		case 0xf3:
			ep->pack_data[0] = b;
			ep->pack_state = STATE_1PARAM;
			break;
		case 0xf2:
			ep->pack_data[0] = b;
			ep->pack_state = STATE_2PARAM_1;
			break;
		case 0xf4:
		case 0xf5:
			ep->pack_state = STATE_UNKNOWN;
			break;
		case 0xf6:
			novimp_pack_packet(urb, 0x05, 0xf6, 0, 0);
			break;
		case 0xf7:
			switch (ep->pack_state) {
			case STATE_SYSEX_0:
				novimp_pack_packet(urb, 0x05, 0xf7, 0, 0);
				break;
			case STATE_SYSEX_1:
				novimp_pack_packet(urb, 0x06, ep->pack_data[0],
						   0xf7, 0);
				break;
			case STATE_SYSEX_2:
				novimp_pack_packet(urb, 0x07, ep->pack_data[0],
						   ep->pack_data[1], 0xf7);
				break;
			}
			ep->pack_state = STATE_UNKNOWN;
			break;
		}
	} else if (b >= 0x80) {
		ep->pack_data[0] = b;
		if (b >= 0xc0 && b <= 0xdf)
			ep->pack_state = STATE_1PARAM;
		else
			ep->pack_state = STATE_2PARAM_1;
	} else {
		switch (ep->pack_state) {
		case STATE_1PARAM:
			if (ep->pack_data[0] < 0xf0) {
				p0 = ep->pack_data[0] >> 4;
			} else {
				p0 = 0x02;
				ep->pack_state = STATE_UNKNOWN;
			}
			novimp_pack_packet(urb, p0, ep->pack_data[0], b, 0);
			break;
		case STATE_2PARAM_1:
			ep->pack_data[1] = b;
			ep->pack_state = STATE_2PARAM_2;
			break;
		case STATE_2PARAM_2:
			if (ep->pack_data[0] < 0xf0) {
				p0 = ep->pack_data[0] >> 4;
				ep->pack_state = STATE_2PARAM_1;
			} else {
				p0 = 0x03;
				ep->pack_state = STATE_UNKNOWN;
			}
			novimp_pack_packet(urb, p0, ep->pack_data[0],
					   ep->pack_data[1], b);
			break;
		case STATE_SYSEX_0:
			ep->pack_data[0] = b;
			ep->pack_state = STATE_SYSEX_1;
			break;
		case STATE_SYSEX_1:
			ep->pack_data[1] = b;
			ep->pack_state = STATE_SYSEX_2;
			break;
		case STATE_SYSEX_2:
			novimp_pack_packet(urb, 0x04, ep->pack_data[0],
					   ep->pack_data[1], b);
			ep->pack_state = STATE_SYSEX_0;
			break;
		}
	}
}

static void novimp_midi_send(struct novimp_midi_output_endpoint *ndev_ep)
{
	struct snd_rawmidi_substream *substream = READ_ONCE(ndev_ep->substream);
	struct urb *urb = ndev_ep->urb;
	int ret;

	if (!substream)
		return;

	// FIXME: configure 32 from endpoint max transfer
	while (urb->transfer_buffer_length + 3 < 32) {
		uint8_t b;
		if (snd_rawmidi_transmit(substream, &b, 1) != 1) {
			break;
		}
		novimp_pack_byte(b, urb, ndev_ep);
	}

	if (!urb->transfer_buffer_length)
		return;

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0)
		dev_err(&ndev_ep->ndev->dev->dev,
			PREFIX
			"%s (%p): usb_submit_urb() failed, ret=%d, len=%d\n",
			__func__, substream, ret, urb->transfer_buffer_length);
	else
		ndev_ep->active = true;
}

static void novimp_output_complete(struct urb *urb)
{
	struct novimp_midi_output_endpoint *ndev_ep = urb->context;

        urb->transfer_buffer_length = 0;
	ndev_ep->active = false;

	if (urb->status)
		dev_warn(&urb->dev->dev, PREFIX "output urb->status: %d\n",
			 urb->status);

	if (urb->status == -ESHUTDOWN)
		return;

	novimp_midi_send(ndev_ep);
}

static int novimp_midi_output_open(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static int novimp_midi_output_close(struct snd_rawmidi_substream *substream)
{
	struct novimp *ndev = substream->rmidi->private_data;
	struct novimp_midi_output_endpoint *ndev_ep =
		&ndev->outputs[substream->number];

	if (ndev_ep->active) {
		usb_kill_urb(ndev_ep->urb);
		ndev_ep->active = false;
	}

	return 0;
}

static void novimp_midi_output_trigger(struct snd_rawmidi_substream *substream,
				       int up)
{
	struct novimp *ndev = substream->rmidi->private_data;
	struct novimp_midi_output_endpoint *ndev_ep =
		&ndev->outputs[substream->number];

	if (up) {
		ndev_ep->substream = substream;
		if (!ndev_ep->active)
			novimp_midi_send(ndev_ep);
	} else {
		ndev_ep->substream = NULL;
	}
}

static const struct snd_rawmidi_ops novimp_midi_output = {
	.open = novimp_midi_output_open,
	.close = novimp_midi_output_close,
	.trigger = novimp_midi_output_trigger
};

static const struct snd_rawmidi_ops novimp_midi_input = {

};

static int novimp_init_midi(struct novimp *ndev)
{
	int ret;
	struct snd_rawmidi *rmidi;

	ret = snd_rawmidi_new(ndev->card, ndev->card->shortname, 0, 1, 0,
			      &rmidi);
	if (ret < 0)
		return ret;

	strscpy(rmidi->name, ndev->card->shortname, sizeof(rmidi->name));
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = ndev;

	rmidi->info_flags |= SNDRV_RAWMIDI_INFO_OUTPUT;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT,
			    &novimp_midi_output);

	ndev->rmidi = rmidi;

	ndev->outputs[0].urb = usb_alloc_urb(0, GFP_KERNEL);
	ndev->outputs[0].ndev = ndev;

	usb_fill_int_urb(ndev->outputs[0].urb, ndev->dev,
			 usb_sndintpipe(ndev->dev, 0x04), ndev->outputs[0].buf,
                         0, novimp_output_complete, &ndev->outputs[0], 2);

	if (usb_urb_ep_type_check(ndev->outputs[0].urb)) {
		dev_err(&ndev->dev->dev, PREFIX "invalid MIDI EP%d\n", 0x04);
		return -EINVAL;
	}
        return 0;
}

static void novimp_free_usb_resources(struct novimp *ndev,
				      struct usb_interface *interface)
{
	struct usb_interface *alt_intf = usb_ifnum_to_if(ndev->dev, 3);
	/* TODO kill urbs and coherent buffers */

	usb_kill_urb(ndev->outputs[0].urb);

	if (ndev->intf) {
		usb_set_intfdata(ndev->intf, NULL);
		ndev->intf = NULL;
	}

	if (alt_intf) {
		usb_driver_release_interface(&snd_novimp_usb_driver, alt_intf);
	}
}

static int novimp_probe(struct usb_interface *interface,
			const struct usb_device_id *usb_id)
{
	/* extern int usb_driver_claim_interface(struct usb_driver *driver, */
	/* 			struct usb_interface *iface, void *data); */

	int card_index;
	int err;
	struct snd_card *card;
	struct novimp *ndev;
	char vname[32], pname[32], usbpath[32];
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct usb_interface *alt_intf = usb_ifnum_to_if(usb_dev, 3);

	mutex_lock(&devices_mutex);

	for (card_index = 0; card_index < SNDRV_CARDS; ++card_index) {
		if (!test_bit(card_index, devices_used))
			break;
	}
	if (card_index >= SNDRV_CARDS) {
		mutex_unlock(&devices_mutex);
		return -ENOENT;
	}

	err = snd_card_new(&interface->dev, index[card_index], id[card_index],
			   THIS_MODULE, sizeof(*ndev), &card);

	if (err < 0) {
		mutex_unlock(&devices_mutex);
		return err;
	}

	ndev = card->private_data;
	ndev->dev = usb_dev;
	ndev->card = card;
	ndev->card_index = card_index;
	ndev->intf = interface;

	if (alt_intf) {
		err = usb_driver_claim_interface(&snd_novimp_usb_driver,
						 alt_intf, NULL);
		if (err < 0) {
			err = -EBUSY;
			goto probe_error;
		}
	}

	strscpy(card->driver, MODULE_NAME, sizeof(card->driver));

	usb_string(usb_dev, usb_dev->descriptor.iProduct, pname, sizeof(pname));
	usb_string(usb_dev, usb_dev->descriptor.iManufacturer, vname,
		   sizeof(vname));

	strscpy(card->shortname, pname, sizeof card->shortname);

	usb_make_path(usb_dev, usbpath, sizeof(usbpath));
	snprintf(card->longname, sizeof card->longname, "%s %s (%s)", vname,
		 pname, usbpath);

	err = novimp_init_midi(ndev);
	if (err < 0)
		goto probe_error;

	err = snd_card_register(card);
	if (err < 0)
		goto probe_error;

	usb_set_intfdata(interface, ndev);
	set_bit(card_index, devices_used);

	mutex_unlock(&devices_mutex);
	return 0;

probe_error:
	dev_info(&ndev->dev->dev, MODULE_NAME ": error during probing");
	snd_card_free(card);
	mutex_unlock(&devices_mutex);
	return err;
}

static void novimp_disconnect(struct usb_interface *interface)
{
	struct novimp *nimp = usb_get_intfdata(interface);

	printk(KERN_INFO "Disconnect %p\n", interface);

	if (!nimp)
		return;

	mutex_lock(&devices_mutex);

	snd_card_disconnect(nimp->card);
	novimp_free_usb_resources(nimp, interface);

	clear_bit(nimp->card_index, devices_used);

	snd_card_free_when_closed(nimp->card);

	mutex_unlock(&devices_mutex);
}

static struct usb_driver snd_novimp_usb_driver = { .name = "snd-usb-novimp",
						   .probe = novimp_probe,
						   .disconnect =
							   novimp_disconnect,
						   .id_table = id_table };

module_usb_driver(snd_novimp_usb_driver);
