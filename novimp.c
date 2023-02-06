#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/midi.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#include "usbaudio.h"

#define MODULE_NAME "snd-novimp"
#define PREFIX MODULE_NAME ": "

MODULE_LICENSE("GPL");

#define get_endpoint(alt, ep) (&(alt)->endpoint[ep].desc)

#define MAX_INTERFACE 4

#define BUFSIZE 32

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x1235, 0x001b, 0x02) },
	{},
};

struct midi_interface {
	struct usb_interface *intf;
	struct list_head midi;
};

struct novimp {
	struct usb_device *dev;
	struct snd_card *card;

	struct midi_interface interfaces[MAX_INTERFACE];
	int num_interfaces;

	atomic_t active;
	atomic_t shutdown;
	atomic_t usage_count;
	wait_queue_head_t shutdown_wait;

	int last_iface;
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
	return snd_usbmidi_create(ndev->card, interface, &ndev->interfaces[ndev->num_interfaces].midi,
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
	init_waitqueue_head(&ndev->shutdown_wait);
	ndev->num_interfaces = 0;
	ndev->card_index = idx;
	ndev->dev = dev;
	ndev->card = card;
	atomic_set(&ndev->active, 1);
	atomic_set(&ndev->usage_count, 0);
	atomic_set(&ndev->shutdown, 0);

	strscpy(card->driver, MODULE_NAME, sizeof(card->driver));

	usb_string(dev, dev->descriptor.iProduct, pname, sizeof(pname));
	usb_string(dev, dev->descriptor.iManufacturer, vname, sizeof(vname));

	strscpy(card->shortname, pname, sizeof card->shortname);

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
	struct novimp *ndev;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct snd_card *card;
	struct midi_interface *midi_if;

	mutex_lock(&devices_mutex);

	ndev = NULL;
	for (i = 0; i < SNDRV_CARDS; ++i) {
		if (novimp_devices[i] && novimp_devices[i]->dev == usb_dev) {
			/* FIXME: need a shutdown lock */
			ndev = novimp_devices[i];
			// TODO: handle pm
			// atomic_inc(&ndev->active)
			break;
		}
	}
	if (!ndev) {
		// no initial card structure exists - create it
		// first find a free card slot
		for (i = 0; i < SNDRV_CARDS; ++i) {
			if (!novimp_devices[i]) {
				err = novimp_create(interface, usb_dev, i, &ndev);
				if (err < 0) {
					dev_info(&usb_dev->dev, MODULE_NAME ": create failed %d", err);
					goto probe_error;
				}
				break;
			}
		}
	}

	if (!ndev || ndev->num_interfaces >= MAX_INTERFACE) {
		dev_err(&usb_dev->dev, "no available audio device\n");
		err = -ENODEV;
		goto probe_error;
	}


	card = ndev->card;
	midi_if = &ndev->interfaces[ndev->num_interfaces];
	INIT_LIST_HEAD(&midi_if->midi);
	midi_if->intf = interface;

	err = novimp_init_midi(ndev, interface);
	if (err < 0) {
		if (!card->registered)
			snd_card_free(card);
		goto probe_error;
	}

	++ndev->num_interfaces;

	if (!card->registered) {
		err = snd_card_register(card);
		if (err < 0) {
			snd_card_free(card);
			goto probe_error;
		}
	} else {
		err = snd_device_register_all(card);
		if (err < 0) {
			dev_err(&usb_dev->dev, "failed to register a secondary device\n");
			goto probe_error;
		}
	}

	usb_set_intfdata(interface, ndev);

	mutex_unlock(&devices_mutex);
	return 0;

probe_error:
	mutex_unlock(&devices_mutex);
	return err;
}

static void novimp_disconnect(struct usb_interface *interface)
{
	struct novimp *ndev = usb_get_intfdata(interface);
	int i;

	printk(KERN_INFO "Disconnect %p\n", interface);

	if (!ndev)
		return;

	mutex_lock(&devices_mutex);

	for (i = 0; i < ndev->num_interfaces; ++i) {

	}
	snd_card_disconnect(ndev->card);
	snd_card_free_when_closed(ndev->card);

	mutex_unlock(&devices_mutex);
}

static struct usb_driver snd_novimp_usb_driver = { .name = "snd-usb-novimp",
						   .probe = novimp_probe,
						   .disconnect =
							   novimp_disconnect,
						   .id_table = id_table };

module_usb_driver(snd_novimp_usb_driver);
