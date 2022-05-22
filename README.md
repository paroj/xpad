
# Xpad    [![Badge License]][License]

*An updated **Xpad Linux Kernel Driver**.*

<kbd> Xbox </kbd> 
<kbd> Xbox 360 </kbd> 
<kbd> Xbox 360 Wireless </kbd> 
<kbd> Xbox One Controllers </kbd> 

<br>

<div align = center>

---

[![Button Change]][Change]  
[![Button Usage]][Usage]  
[![Button Debug]][Debug]

---

</div>

<br>

## Features

- Latest change from the upstream Linux kernel

- Enabled debug output for resolving issues

- Minor code refactoring for improved readability

<br>
<br>

## Connections

*This is a **USB** only driver.*

<br>

### Bluetooth

If you get past the pairing issues, the controller will <br>
only operate in the **[Generic - HID Bluetooth Profile]**.  

The **Xpad** driver will not be used.

<br>

### WiFi

The **Xbox One** controller can be used with it's wireless <br>
adapter, however it required a daemon in userspace. 

⤷ **[medusalix/xow]**

<br>Badge License]:

##### Opinion

Rather get a controller that supports bluetooth.

<br>
<br>

## Upstream

*Sending changes upstream.*

```sh
git format-patch \
    --cover-letter upstream..master
```
    
```sh
git send-email \
    --to xxx *.patch
```


<!----------------------------------------------------------------------------->

[Generic - HID Bluetooth Profile]: https://en.wikipedia.org/wiki/List_of_Bluetooth_profiles#Human_Interface_Device_Profile_(HID)
[medusalix/xow]: https://github.com/medusalix/xow

[Change]: Documentation/Install.md
[Usage]: Documentation/Usage.md
[Debug]: Documentation/Debug.md

[License]: LICENSE

[Badge License]: https://img.shields.io/badge/License-GPL_3-blue.svg?style=for-the-badge

<!---------------------------------{ Buttons }--------------------------------->

[Button Change]: https://img.shields.io/badge/Install_/_Update_/_Remove-cfab42?style=for-the-badge
[Button Usage]: https://img.shields.io/badge/Usage-62a0dd?style=for-the-badge
[Button Debug]: https://img.shields.io/badge/Debug-cf427c?style=for-the-badge