#ifndef USBAUDIO_H_
#define USBAUDIO_H_

#define QUIRK_ANY_INTERFACE		-1

enum quirk_type {
	QUIRK_IGNORE_INTERFACE,
	QUIRK_COMPOSITE,
	QUIRK_AUTODETECT,
	QUIRK_MIDI_STANDARD_INTERFACE,
	QUIRK_MIDI_FIXED_ENDPOINT,
	QUIRK_MIDI_YAMAHA,
	QUIRK_MIDI_ROLAND,
	QUIRK_MIDI_MIDIMAN,
	QUIRK_MIDI_NOVATION,
	QUIRK_MIDI_RAW_BYTES,
	QUIRK_MIDI_EMAGIC,
	QUIRK_MIDI_CME,
	QUIRK_MIDI_AKAI,
	QUIRK_MIDI_US122L,
	QUIRK_MIDI_FTDI,
	QUIRK_MIDI_CH345,
	QUIRK_AUDIO_STANDARD_INTERFACE,
	QUIRK_AUDIO_FIXED_ENDPOINT,
	QUIRK_AUDIO_EDIROL_UAXX,
	QUIRK_AUDIO_STANDARD_MIXER,

	QUIRK_TYPE_COUNT
};

struct snd_usb_audio_quirk {
	const char *vendor_name;
	const char *product_name;
	int16_t ifnum;
	uint16_t type;
	const void *data;
};

struct snd_usb_midi_endpoint_info {
	int8_t   out_ep;	/* ep number, 0 autodetect */
	uint8_t  out_interval;	/* interval for interrupt endpoints */
	int8_t   in_ep;
	uint8_t  in_interval;
	uint16_t out_cables;	/* bitmask */
	uint16_t in_cables;	/* bitmask */
	int16_t  assoc_in_jacks[16];
	int16_t  assoc_out_jacks[16];
};

int __snd_usbmidi_create(struct snd_card *card,
			 struct usb_interface *iface,
			 struct list_head *midi_list,
			 const struct snd_usb_audio_quirk *quirk,
			 unsigned int usb_id);

static inline int snd_usbmidi_create(struct snd_card *card,
		       struct usb_interface *iface,
		       struct list_head *midi_list,
		       const struct snd_usb_audio_quirk *quirk)
{
	return __snd_usbmidi_create(card, iface, midi_list, quirk, 0);
}

void snd_usbmidi_input_stop(struct list_head *p);
void snd_usbmidi_input_start(struct list_head *p);
void snd_usbmidi_disconnect(struct list_head *p);
void snd_usbmidi_suspend(struct list_head *p);
void snd_usbmidi_resume(struct list_head *p);

#endif // USBAUDIO_H_
