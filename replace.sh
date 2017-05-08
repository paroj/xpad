#!/bin/bash
sudo cp xpad.c /usr/src/xpad-0.4/
sudo dkms remove -m xpad -v 0.4 --all
sudo dkms install -m xpad -v 0.4 --force
sudo modprobe -r xpad
sudo modprobe xpad

