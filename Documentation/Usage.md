
# Usage

*This driver creates three devices for each attached gamepad.*

<br>

## Test

`/dev/input/js<Id>`

#### Example

```sh
jstest /dev/input/js0
```

<br>
<br>

## Commanding

`/sys/class/leds/xpad<Id>/brightness`

#### Example

```sh
echo <Command> > /sys/class/leds/xpad0/brightness
```

```sh
echo 0 > /sys/class/leds/xpad0/brightness
```

| Command | Description
|:-------:|:-----------
|  `0` | Turn Off
|  `1` | Blink All 🠖 Previous Setting
|  `2` | Blink `1 / top-left` 🠖 On
|  `3` | Blink `2 / top-right` 🠖 On
|  `4` | Blink `3 / bottom-left` 🠖 On
|  `5` | Blink `4 / bottom-right` 🠖 On
|  `6` | `1 / top-left` On
|  `7` | `2 / top-right` On
|  `8` | `3 / bottom-left` On
|  `9` | `4 / bottom-right` On
| `10` | Rotate
| `11` | Blink based on previous setting
| `12` | Slow blink based on previous setting
| `13` | Rotate with two lights
| `14` | Persistent slow all blink
| `15` | Blink Once 🠖 previous setting

<br>
<br>

## Events

*The generic event device.*

#### Example

```sh
fftest /dev/input/by-id/usb-*360*event*
```
