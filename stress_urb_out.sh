#!/bin/sh
# stress test irq_urb_out. provokes URB request dropping.
# script by Laura Abbott
# see: http://www.spinics.net/lists/linux-input/msg40477.html

while true; do
    for i in $(seq 0 5); do
        echo $i > /sys/class/leds/xpad0/subsystem/xpad0/brightness
    done
done
