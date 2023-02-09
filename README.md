# ALSA Driver for Novation Impulse Control Interfaces

This is an auxiliary driver for Novation Impulse MIDI Controllers. These MIDI controllers were originally shipped with a program called Automap that would allow the custom binding of controls to DAW plugins and supported DAW mixer controls. The Automap software interfaced to the Controllers via 2 proprietary USB interfaces (interface numbers 2 and 3) with interrupt endpoints. In the Windows version of Automap a custom streaming media device driver was used to access these interfaces.

The Plugin control mode for the keyboard is available by sending SYSEX messages to the standard MIDI ports on the device without any special software. This had been reverse engineered and there are a number of openly available unofficial plugins for DAWs such as FLStudio and Bitwig that allow custom mapping of controls and writing to the display. However the behavior of the keyboard is slighly different than when the proprietary interfaces are used. 

The Keyboard has 3 "zones" of control: the mixers/faders, the knobs, and the pads. Each zone can be enabled individually. When all 3 zones are enabled via the standard MIDI port it does alter the behavior of the Mod wheel to only output on channel 3, regardless of how the keyboard template is setup. However, if the control mode is enabled through the non-standard MIDI interface/ports then it does not alter the Mod wheel output.

## MIDI Messages

All Sysex messages begin with the preamble F0 00 20 29 which is the Focusrite/Novation header and they end with the trailer F7. The preamble and trailer will be omitted in the following:

* 00 70: Get firmware version: Example response: 00 70 00 00 06 05 08 00 00 06 09 03 0D -> Boot version: 658, Main version: 693
* 67 06 0x 0y 0z: Device inquiry and enabling control mode for each zone. x = 1 to enable fader zone, y = 1 to enable knob zone, z = 1 to enable pad zone. Device responds with device ID: 67 0x xx, where device ID is xxx. 719 for Impulse 25, 71A for Impulse 49 and 71B for Impulse 61.
* 67 05 0x 00 00 [Up to 72 bytes of ASCII text]: Control label assignment for zone x (0 = faders, 1 = buttons, 2 = knobs). The 72 characters of text are 8 bytes for each control starting from left to right and top to bottom with the 9th byte being 20h (ASCII Space). This allows setting persistent text labels to each control when the controller is in Plugin mode. The controller also stores a value for each of the controls that defaults to 0 which can be set by sending the corresponding MIDI control message with the value desired. Sending the control value to a button controller only seems to set the LED. When pressing the button it will display the value 0. No way to change this displayed value has been found.
* 67 08 [Up to 64 bytes of ASCII text]: Show the text on the full text display. The width of this display is 8 characters, so anything more than 8 characters will cause a repeating scrolling display. The device will also trim whitespace from the string.
* 67 09 [3 bytes ASCII]: Display text in the 7/8-segment numeric display area. Note the first character is an 8 eight segment display (it has a center top vertical segment) and can display characters such as capital M, that the other two cannot.
* 43 00 00 [template data]: Write template data to RAM in the current template bank. 

## Character Table for 3 digit display
![3 character display map](char-table.png)
