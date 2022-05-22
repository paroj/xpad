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