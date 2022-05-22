# Debugging

*The following instructions can be done as* <br>
*a regular user without admin privileges.*

<br>

1. Display the kernel log in the console:

    ```sh
    dmesg --level=debug --follow
    ```
    
    <br>

2. In a new terminal access the device with `jstest` :

    ```sh
    jstest /dev/input/js<Id>
    ```
    
    ```sh
    jstest /dev/input/js0
    ```
    
    <br>

3. Interact with the device and observe how the <br>
   received packets are being printed to the log.

    ```
    [ 3968.772128] xpad-dbg: 00000000: 20 00 b5 0e 00 00 00 00 00 00 0c 03 04 fd 6c 01 40 fe 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    [ 3968.772135] xpad-dbg: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    [ 3968.804137] xpad-dbg: 00000000: 20 00 b6 0e 00 00 00 00 00 00 0c 03 04 fd 6c 01 fc fd 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    [ 3968.804145] xpad-dbg: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    [ 3969.152120] xpad-dbg: 00000000: 20 00 b7 0e 00 00 00 00 00 00 0c 03 04 fd 6c 01 b8 fd 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    [ 3969.152129] xpad-dbg: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    ```
    
    <br>
    
4. Save the `dmsg` buffer with:

    ```sh
    dmesg --level=debug > dmesg.txt
    ```
    
    <br>

5. Use  <kbd> Ctrl + C </kbd>  to close <br>
   the console when finished.
   
   <br>

6. Write a bug report describing the button <br>
   sequences and attach the saved buffer.
   
<br>
