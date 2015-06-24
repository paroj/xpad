# Updated Xpad Linux Kernel Driver
Driver for the Xbox/ Xbox 360/ Xbox 360 Wireless/ Xbox One Controllers

## Changes compared to Linux Staging
using the master branch:

* fixed blinking LED on Xbox 360 Wireless Controllers ([patch by  Pierre-Loup A. Griffais](http://www.spinics.net/lists/linux-input/msg29446.html))
* fixed kernel panics due to URB request races ([patch by Sarah Bessmer](https://www.marc.info/?l=linux-input&m=140023068527280&w=2))
* updated Xbox One controller force feedback (based on [SteamOS Version](https://github.com/ValveSoftware/steamos_kernel/commit/d92cfaac03183c01382bde7e817d22e4ea9078d5))

# Installation
```
sudo git clone https://github.com/paroj/xpad.git /usr/src/xpad-0.4
sudo dkms install -m xpad -v 0.4
```
# Updating
```
cd /usr/src/xpad-0.4
sudo git fetch
sudo git checkout origin/master
sudo dkms remove -m xpad -v 0.4 --all
sudo dkms install -m xpad -v 0.4
```
# Usage
This driver creates two devices for each attached gamepad

1. /dev/input/js**N**
    * example `jstest /dev/input/js0`
2. /sys/class/leds/xpad**N**/brightness
    * example `echo COMMAND > /sys/class/leds/xpad0/brightness` where COMMAND is one of
        *  0: off
        *  1: all blink, then previous setting
        *  2: 1/top-left blink, then on
        *  3: 2/top-right blink, then on
        *  4: 3/bottom-left blink, then on
        *  5: 4/bottom-right blink, then on
        *  6: 1/top-left on
        *  7: 2/top-right on
        *  8: 3/bottom-left on
        *  9: 4/bottom-right on
        * 10: rotate
        * 11: blink, based on previous setting
        * 12: slow blink, based on previous setting
        * 13: rotate with two lights
        * 14: persistent slow all blink
        * 15: blink once, then previous setting

# Outstanding issues
These are high priority issues that should be worked on next:

* [only expose connected devices](https://github.com/paroj/xpad/issues/1)
* [do not drop force feedback packets](https://github.com/paroj/xpad/issues/1)

# Sending Upstream

1. `git format-patch --cover-letter upstream..master`
2. `git send-email --to xxx *.patch`
