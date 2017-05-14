# Updated Xpad Linux Kernel Driver
Driver for the Xbox/ Xbox 360/ Xbox 360 Wireless/ Xbox One Controllers

This driver includes the latest changes in the upstream linux kernel and additionally carries the following staging changes:

* enable debug outputs to ease resolving issues
* some minor code refactoring improving readability 

# Installing
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
# Removing
```
sudo dkms remove -m xpad -v 0.4 --all
sudo rm -rf /usr/src/xpad-0.4
```
# Usage
This driver creates three devices for each attached gamepad

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
3. the generic event device
    * example `fftest /dev/input/by-id/usb-*360*event*`

# Debugging
As a regular unpriveledged user

Setup console to display kernel log.  
`dmesg --level=debug --follow`

Open a new console and access the device with jstest.  
`jstest /dev/input/jsX`

Interact with the device and observe that data packets recieved from device are printed to kernel log.
```
[ 3968.772128] xpad-dbg: 00000000: 20 00 b5 0e 00 00 00 00 00 00 0c 03 04 fd 6c 01 40 fe 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[ 3968.772135] xpad-dbg: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[ 3968.804137] xpad-dbg: 00000000: 20 00 b6 0e 00 00 00 00 00 00 0c 03 04 fd 6c 01 fc fd 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[ 3968.804145] xpad-dbg: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[ 3969.152120] xpad-dbg: 00000000: 20 00 b7 0e 00 00 00 00 00 00 0c 03 04 fd 6c 01 b8 fd 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[ 3969.152129] xpad-dbg: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

Save dmesg buffer and attach to bug report, don't forget to describe button sequences in bug report.  
`dmesg --level=debug > dmesg.txt`

Ctrl+C to close interactive console sessions when finished.

# Sending Upstream

1. `git format-patch --cover-letter upstream..master`
2. `git send-email --to xxx *.patch`
