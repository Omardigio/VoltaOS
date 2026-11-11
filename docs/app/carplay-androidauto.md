# CarPlay / Android Auto

The phone is shown inside the UI (video, audio and touch) through a **Carlinkit dongle** plugged into
a USB port. No keys, no Python service: the main program does the work.

**Requirements:** Carlinkit dongle, `libusb-1.0-0`. More details
in [Hardware](../hardware.md#carplay--android-auto-dongle).

## How to connect

1. Plug the dongle into a USB port of the PC.
2. Turn on **Bluetooth** on the phone and connect it to the dongle (it appears as a Bluetooth device on the
   phone). CarPlay or Android Auto start by themselves.
3. Open the **CarPlay / Android Auto** app from the drawer.

The states you see: *Starting the dongle…* → *Connect your phone* → *Connecting to …* → the phone's video.
If the dongle is not connected, *Dongle not available · Plug in the USB dongle* appears.

## Small panel and full screen

The app opens in the right-hand panel and can go **full screen**. The phone's video is always
16:9: in the small panel and in full screen you can choose how to fit it (see below).

## Settings

In [Settings › CarPlay / Android Auto](../settings/carplay-android-auto.md): theme, fit
(*Automatic* or *Fill*), resolution (480p / 720p / 1080p), density (DPI) and **Reconnect the phone**.
Resolution and DPI apply on reconnection, which starts by itself when you change them.

## Android Auto without a dongle

Volta OS also speaks Android Auto by itself, so an **Android** phone can connect with no dongle at all
(CarPlay still needs the Carlinkit dongle). Choose it in
[Settings › CarPlay / Android Auto › Connection](../settings/carplay-android-auto.md). The video, audio,
microphone and touch use the same screen and settings as the dongle.

### With a USB cable

1. Install the **Android Auto** app on the phone (it is built in on recent Android versions).
2. Install the USB permission rule once, so Volta OS may talk to the phone:
   ```bash
   sudo cp packaging/51-volta-android-auto.rules /etc/udev/rules.d/
   sudo udevadm control --reload && sudo udevadm trigger
   ```
   (with the AppImage, take the file from the repository). Then unplug and plug the phone again. If it
   still says *Cannot reach the phone*, use a wider rule: `SUBSYSTEM=="usb", TAG+="uaccess"`.
3. Plug the phone into a USB port with a **data** cable and open the app. The phone switches itself to
   accessory mode and the video appears after a few seconds.

**USB tethering and this mode use the same cable**: when the phone switches to Android Auto the USB
tethering (internet through the phone) drops. If your computer gets its internet that way, expect to
lose it while Android Auto is running.

### Over Wi-Fi

The protocol side is ready: Volta OS waits for the phone on TCP port 5288. What is **not done yet** is
the step that makes the phone connect by itself (the Bluetooth handshake that hands it the Wi-Fi name and
password, and the Wi-Fi network created by the computer), so for now this mode does not connect on its
own. It will need Bluetooth and a Wi-Fi adapter that can act as an access point.

### The certificate

Android Auto requires a head unit certificate. Volta OS includes the same public one used by the
open source head units (see `third_party/aap_cert/README.md`); if Google revokes it, put a replacement
`headunit.crt` and `headunit.key` in `~/.local/share/volta_os/`.

### If something does not work

Start Volta OS from a terminal: lines starting with `[AA` tell where it stops (`VOLTA_AA_TRACE=1` prints every
message). Attach them to your [issue](https://github.com/Omardigio/VoltaOS/issues).
