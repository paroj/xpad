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