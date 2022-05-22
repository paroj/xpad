
# Xpad

*An updated **Xpad Linux Kernel Driver**.*

<kbd> Xbox </kbd> 
<kbd> Xbox 360 </kbd> 
<kbd> Xbox 360 Wireless </kbd> 
<kbd> Xbox One Controllers </kbd> 

<br>

<div align = center>

---

[![Button Install]][Install]  
[![Button Usage]][Usage]  
[![Button Debug]][Debug]

---

</div>

<br>

This driver includes the latest changes in the upstream linux kernel and additionally carries the following staging changes:

* enable debug outputs to ease resolving issues
* some minor code refactoring improving readability 

## Xbox One Controllers
This driver is only used if you connect the controller via USB.

**Connecting via Bluetooth**  
If you get past the pairing issues, the controller will operate in the [generic-HID bluetooth profile](https://en.wikipedia.org/wiki/List_of_Bluetooth_profiles#Human_Interface_Device_Profile_(HID)).  
The xpad driver will not be used.

**Conencting via XBox One Wireless Adapter (WiFi)**  
The adapter needs daemon in userspace, see: [medusalix/xow](https://github.com/medusalix/xow)  
Opinion: rather get a controller that supports bluetooth.

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

[Install]: Documentation/Install.md
[Usage]: Documentation/Usage.md
[Debug]: Documentation/Debug.md


<!---------------------------------{ Buttons }--------------------------------->

[Button Install]: https://img.shields.io/badge/Install_/_Update_/_Remove-cfab42?style=for-the-badge
[Button Usage]: https://img.shields.io/badge/Usage-62a0dd?style=for-the-badge
[Button Debug]: https://img.shields.io/badge/Debug-cf427c?style=for-the-badge