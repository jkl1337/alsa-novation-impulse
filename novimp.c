#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/midi.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#include "usbaudio.h"

#define MODULE_NAME "snd-novimp"
#define VERSION	"0.0.1"
#define PREFIX MODULE_NAME ": "


MODULE_AUTHOR("John Luebs");
MODULE_DESCRIPTION("Support for auxiliary interfaces of Novation Impulse MIDI Controllers");
MODULE_LICENSE("GPL");
MODULE_VERSION(VERSION);

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(0x1235, 0x0019, 0xff) },
	{ USB_DEVICE_INTERFACE_CLASS(0x1235, 0x001a, 0xff) },
	{ USB_DEVICE_INTERFACE_CLASS(0x1235, 0x001b, 0xff) },
	{},
};

struct midi_interface {
	struct usb_interface *intf;
};

struct novimp {
	struct usb_device *dev;
	struct snd_card *card;

	struct list_head midi_list;

	int card_index;
};

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "card index");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string");

static DEFINE_MUTEX(devices_mutex);
static struct novimp *novimp_devices[SNDRV_CARDS];

static struct usb_driver snd_novimp_usb_driver;

static int novimp_init_midi(struct novimp *ndev,
			    struct usb_interface *interface)
{
	static const struct snd_usb_midi_endpoint_info quirk_data = {
		.out_cables = 0x0001, .in_cables = 0x0001
	};

	static const struct snd_usb_audio_quirk quirk = {
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = &quirk_data
	};
	return snd_usbmidi_create(ndev->card, interface, &ndev->midi_list,
				  &quirk);
}

static int novimp_create(struct usb_interface *intf, struct usb_device *dev,
			 int idx, struct novimp **rndev)
{
	struct snd_card *card;
	struct novimp *ndev;
	int err;
	char vname[32], pname[32], usbpath[32];

	*rndev = NULL;

	err = snd_card_new(&intf->dev, index[idx], id[idx], THIS_MODULE,
			   sizeof(*ndev), &card);
	if (err < 0) {
		dev_err(&dev->dev, "cannot create card instance %d\n", idx);
		return err;
	}

	ndev = card->private_data;
	ndev->dev = dev;
	ndev->card = card;
	INIT_LIST_HEAD(&ndev->midi_list);
	ndev->card_index = idx;

	strscpy(card->driver, MODULE_NAME, sizeof(card->driver));

	usb_string(dev, dev->descriptor.iProduct, pname, sizeof(pname));
	usb_string(dev, dev->descriptor.iManufacturer, vname, sizeof(vname));

	snprintf(card->shortname, sizeof(card->shortname), "%sA%d", pname,
		 intf->cur_altsetting->desc.bInterfaceNumber);

	usb_make_path(dev, usbpath, sizeof(usbpath));
	snprintf(card->longname, sizeof card->longname, "%s %s (%s)", vname,
		 pname, usbpath);

	*rndev = ndev;
	return 0;
}

static int novimp_probe(struct usb_interface *interface,
			const struct usb_device_id *usb_id)
{
	/* extern int usb_driver_claim_interface(struct usb_driver *driver, */
	/* 			struct usb_interface *iface, void *data); */

	int err, i;
	struct novimp *ndev = NULL;
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	mutex_lock(&devices_mutex);

	for (i = 0; i < SNDRV_CARDS; ++i) {
		if (!novimp_devices[i]) {
			err = novimp_create(interface, usb_dev, i, &ndev);
			if (err < 0) {
				dev_info(&usb_dev->dev,
					 PREFIX "create failed %d", err);
				goto probe_error;
			}
			break;
		}
	}

	if (!ndev) {
		dev_err(&usb_dev->dev, "no available audio device");
		err = -ENODEV;
		goto probe_error;
	}

	err = novimp_init_midi(ndev, interface);
	if (err < 0) {
		dev_err(&usb_dev->dev, PREFIX "init_midi failed");
		goto probe_error;
	}

	err = snd_card_register(ndev->card);
	if (err < 0) {
		snd_usbmidi_disconnect(ndev->midi_list.next);
		dev_err(&usb_dev->dev, PREFIX "snd_card_register failed");
		goto probe_error;
	}

	usb_set_intfdata(interface, ndev);
	novimp_devices[ndev->card_index] = ndev;

	mutex_unlock(&devices_mutex);
	return 0;

probe_error:
	if (ndev)
		snd_card_free(ndev->card);
	mutex_unlock(&devices_mutex);
	return err;
}

static void novimp_disconnect(struct usb_interface *interface)
{
	struct novimp *ndev = usb_get_intfdata(interface);

	if (!ndev)
		return;

	dev_info(&ndev->dev->dev, PREFIX "disconnect %p", interface);

	mutex_lock(&devices_mutex);

	if (!list_empty(&ndev->midi_list))
		snd_usbmidi_disconnect(ndev->midi_list.next);

	snd_card_disconnect(ndev->card);

	novimp_devices[ndev->card_index] = NULL;

	snd_card_free_when_closed(ndev->card);

	mutex_unlock(&devices_mutex);
}

static struct usb_driver snd_novimp_usb_driver = { .name = "snd-usb-novimp",
						   .probe = novimp_probe,
						   .disconnect =
							   novimp_disconnect,
						   .id_table = id_table };

module_usb_driver(snd_novimp_usb_driver);
