// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Xbox gamepad driver
 *
 * Copyright (c) 2002 Marko Friedemann <mfr@bmx-chemnitz.de>
 *               2004 Oliver Schwartz <Oliver.Schwartz@gmx.de>,
 *                    Steven Toth <steve@toth.demon.co.uk>,
 *                    Franz Lehner <franz@caos.at>,
 *                    Ivan Hawkes <blackhawk@ivanhawkes.com>
 *               2005 Dominic Cerquetti <binary1230@yahoo.com>
 *               2006 Adam Buchbinder <adam.buchbinder@gmail.com>
 *               2007 Jan Kratochvil <honza@jikos.cz>
 *               2010 Christoph Fritz <chf.fritz@googlemail.com>
 *
 * This driver is based on:
 *  - information from     http://euc.jp/periphs/xbox-controller.ja.html
 *  - the iForce driver    drivers/char/joystick/iforce.c
 *  - the skeleton-driver  drivers/usb/usb-skeleton.c
 *  - Xbox 360 information http://www.free60.org/wiki/Gamepad
 *  - Xbox One information https://github.com/quantus/xbox-one-controller-protocol
 *
 * Thanks to:
 *  - ITO Takayuki for providing essential xpad information on his website
 *  - Vojtech Pavlik     - iforce driver / input subsystem
 *  - Greg Kroah-Hartman - usb-skeleton driver
 *  - Xbox Linux project - extra USB IDs
 *  - Pekka Pöyry (quantus) - Xbox One controller reverse-engineering
 *
 * TODO:
 *  - fine tune axes (especially trigger axes)
 *  - fix "analog" buttons (reported as digital now)
 *  - get rumble working
 *  - need USB IDs for other dance pads
 *
 * History:
 *
 * 2002-06-27 - 0.0.1 : first version, just said "XBOX HID controller"
 *
 * 2002-07-02 - 0.0.2 : basic working version
 *  - all axes and 9 of the 10 buttons work (german InterAct device)
 *  - the black button does not work
 *
 * 2002-07-14 - 0.0.3 : rework by Vojtech Pavlik
 *  - indentation fixes
 *  - usb + input init sequence fixes
 *
 * 2002-07-16 - 0.0.4 : minor changes, merge with Vojtech's v0.0.3
 *  - verified the lack of HID and report descriptors
 *  - verified that ALL buttons WORK
 *  - fixed d-pad to axes mapping
 *
 * 2002-07-17 - 0.0.5 : simplified d-pad handling
 *
 * 2004-10-02 - 0.0.6 : DDR pad support
 *  - borrowed from the Xbox Linux kernel
 *  - USB id's for commonly used dance pads are present
 *  - dance pads will map D-PAD to buttons, not axes
 *  - pass the module paramater 'dpad_to_buttons' to force
 *    the D-PAD to map to buttons if your pad is not detected
 *
 * Later changes can be tracked in SCM.
 */

// #define DEBUG
#include <linux/version.h>
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/usb/quirks.h>
#include <linux/timer.h>

// backward compatibility. del_timer_sync is renamed to timer_delete_sync since 6.15.0
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,15,0)
#define timer_delete_sync del_timer_sync
#endif

// backward compatibility. from_timer is renamed to timer_container_of since 6.16.0
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,16,0)
#define timer_container_of from_timer
#endif

// enable compilation on pre 6.1 kernels
#ifndef ABS_PROFILE
#define ABS_PROFILE ABS_MISC
#endif

#define XPAD_PKT_LEN 64

/* The Guitar Hero Live (GHL) Xbox One dongles require a poke 
 * every 8 seconds.
 */
#define GHL_GUITAR_POKE_INTERVAL 8 /* In seconds */

/*
 * xbox d-pads should map to buttons, as is required for DDR pads
 * but we map them to axes when possible to simplify things
 */
#define MAP_DPAD_TO_BUTTONS		(1 << 0)
#define MAP_TRIGGERS_TO_BUTTONS		(1 << 1)
#define MAP_STICKS_TO_NULL		(1 << 2)
#define MAP_SELECT_BUTTON		(1 << 3)
#define MAP_PADDLES			(1 << 4)
#define MAP_PROFILE_BUTTON		(1 << 5)

#define DANCEPAD_MAP_CONFIG	(MAP_DPAD_TO_BUTTONS |			\
				MAP_TRIGGERS_TO_BUTTONS | MAP_STICKS_TO_NULL)

#define XTYPE_XBOX        0
#define XTYPE_XBOX360     1
#define XTYPE_XBOX360W    2
#define XTYPE_XBOXONE     3
#define XTYPE_UNKNOWN     4

/* Send power-off packet to xpad360w after holding the mode button for this many
 * seconds
 */
#define XPAD360W_POWEROFF_TIMEOUT 5

#define PKT_XB              0
#define PKT_XBE1            1
#define PKT_XBE2_FW_OLD     2
#define PKT_XBE2_FW_5_EARLY 3
#define PKT_XBE2_FW_5_11    4

#define QUIRK_360_START_PKT_1	(1 << 0)
#define QUIRK_360_START_PKT_2	(1 << 1)
#define QUIRK_360_START_PKT_3	(1 << 2)
#define QUIRK_GHL_XBOXONE	(1 << 3)
#define QUIRK_360_START (QUIRK_360_START_PKT_1 |			\
				QUIRK_360_START_PKT_2 | QUIRK_360_START_PKT_3)

static bool dpad_to_buttons;
module_param(dpad_to_buttons, bool, S_IRUGO);
MODULE_PARM_DESC(dpad_to_buttons, "Map D-PAD to buttons rather than axes for unknown pads");

static bool triggers_to_buttons;
module_param(triggers_to_buttons, bool, S_IRUGO);
MODULE_PARM_DESC(triggers_to_buttons, "Map triggers to buttons rather than axes for unknown pads");

static bool sticks_to_null;
module_param(sticks_to_null, bool, S_IRUGO);
MODULE_PARM_DESC(sticks_to_null, "Do not map sticks at all for unknown pads");

static bool auto_poweroff = true;
module_param(auto_poweroff, bool, S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(auto_poweroff, "Power off wireless controllers on suspend");

static bool chatpad = true;
module_param(chatpad, bool, S_IRUGO);
MODULE_PARM_DESC(chatpad, "Enable Xbox 360 chatpad keyboard accessory support");

static const struct xpad_device {
	u16 idVendor;
	u16 idProduct;
	char *name;
	u8 mapping;
	u8 xtype;
	u8 quirks;
} xpad_device[] = {
	/* Please keep this list sorted by vendor and product ID. */
	{ 0x0079, 0x18d4, "GPD Win 2 X-Box Controller", 0, XTYPE_XBOX360 },
	{ 0x03eb, 0xff01, "Wooting One (Legacy)", 0, XTYPE_XBOX360 },
	{ 0x03eb, 0xff02, "Wooting Two (Legacy)", 0, XTYPE_XBOX360 },
	{ 0x03f0, 0x038D, "HyperX Clutch", 0, XTYPE_XBOX360 },			/* wired */
	{ 0x03f0, 0x048D, "HyperX Clutch", 0, XTYPE_XBOX360 },			/* wireless */
	{ 0x03f0, 0x0495, "HyperX Clutch Gladiate", 0, XTYPE_XBOXONE },
	{ 0x03f0, 0x07A0, "HyperX Clutch Gladiate RGB", 0, XTYPE_XBOXONE },
	{ 0x03f0, 0x08B6, "HyperX Clutch Gladiate", 0, XTYPE_XBOXONE },		/* v2 */
	{ 0x03f0, 0x09B4, "HyperX Clutch Tanto", 0, XTYPE_XBOXONE },
	{ 0x044f, 0x0f00, "Thrustmaster Wheel", 0, XTYPE_XBOX },
	{ 0x044f, 0x0f03, "Thrustmaster Wheel", 0, XTYPE_XBOX },
	{ 0x044f, 0x0f07, "Thrustmaster, Inc. Controller", 0, XTYPE_XBOX },
	{ 0x044f, 0xd01e, "ThrustMaster, Inc. ESWAP X 2 ELDEN RING EDITION", 0, XTYPE_XBOXONE },
	{ 0x044f, 0x0f10, "Thrustmaster Modena GT Wheel", 0, XTYPE_XBOX },
	{ 0x044f, 0xb326, "Thrustmaster Gamepad GP XID", 0, XTYPE_XBOX360 },
	{ 0x045e, 0x0202, "Microsoft X-Box pad v1 (US)", 0, XTYPE_XBOX },
	{ 0x045e, 0x0285, "Microsoft X-Box pad (Japan)", 0, XTYPE_XBOX },
	{ 0x045e, 0x0287, "Microsoft Xbox Controller S", 0, XTYPE_XBOX },
	{ 0x045e, 0x0288, "Microsoft Xbox Controller S v2", 0, XTYPE_XBOX },
	{ 0x045e, 0x0289, "Microsoft X-Box pad v2 (US)", 0, XTYPE_XBOX },
	{ 0x045e, 0x028e, "Microsoft X-Box 360 pad", 0, XTYPE_XBOX360 },
	{ 0x045e, 0x028f, "Microsoft X-Box 360 pad v2", 0, XTYPE_XBOX360 },
	{ 0x045e, 0x0291, "Xbox 360 Wireless Receiver (XBOX)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360W },
	{ 0x045e, 0x02a9, "Xbox 360 Wireless Receiver (Unofficial)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360W },
	{ 0x045e, 0x02d1, "Microsoft X-Box One pad", 0, XTYPE_XBOXONE },
	{ 0x045e, 0x02dd, "Microsoft X-Box One pad (Firmware 2015)", 0, XTYPE_XBOXONE },
	{ 0x045e, 0x02e3, "Microsoft X-Box One Elite pad", MAP_PADDLES, XTYPE_XBOXONE },
	{ 0x045e, 0x02ea, "Microsoft X-Box One S pad", 0, XTYPE_XBOXONE },
	{ 0x045e, 0x0719, "Xbox 360 Wireless Receiver", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360W },
	{ 0x045e, 0x0b00, "Microsoft X-Box One Elite 2 pad", MAP_PADDLES, XTYPE_XBOXONE },
	{ 0x045e, 0x0b0a, "Microsoft X-Box Adaptive Controller", MAP_PROFILE_BUTTON, XTYPE_XBOXONE },
	{ 0x045e, 0x0b12, "Microsoft Xbox Series S|X Controller", MAP_SELECT_BUTTON, XTYPE_XBOXONE },
	{ 0x046d, 0xc21d, "Logitech Gamepad F310", 0, XTYPE_XBOX360 },
	{ 0x046d, 0xc21e, "Logitech Gamepad F510", 0, XTYPE_XBOX360 },
	{ 0x046d, 0xc21f, "Logitech Gamepad F710", 0, XTYPE_XBOX360 },
	{ 0x046d, 0xc242, "Logitech Chillstream Controller", 0, XTYPE_XBOX360 },
	{ 0x046d, 0xca84, "Logitech Xbox Cordless Controller", 0, XTYPE_XBOX },
	{ 0x046d, 0xca88, "Logitech Compact Controller for Xbox", 0, XTYPE_XBOX },
	{ 0x046d, 0xca8a, "Logitech Precision Vibration Feedback Wheel", 0, XTYPE_XBOX },
	{ 0x046d, 0xcaa3, "Logitech DriveFx Racing Wheel", 0, XTYPE_XBOX360 },
	{ 0x056e, 0x2004, "Elecom JC-U3613M", 0, XTYPE_XBOX360 },
	{ 0x05ac, 0x055b, "Gamesir-G3w", 0, XTYPE_XBOX360, QUIRK_360_START },
	{ 0x05fd, 0x1007, "Mad Catz Controller (unverified)", 0, XTYPE_XBOX },
	{ 0x05fd, 0x107a, "InterAct 'PowerPad Pro' X-Box pad (Germany)", 0, XTYPE_XBOX },
	{ 0x05fe, 0x3030, "Chic Controller", 0, XTYPE_XBOX },
	{ 0x05fe, 0x3031, "Chic Controller", 0, XTYPE_XBOX },
	{ 0x062a, 0x0020, "Logic3 Xbox GamePad", 0, XTYPE_XBOX },
	{ 0x062a, 0x0033, "Competition Pro Steering Wheel", 0, XTYPE_XBOX },
	{ 0x06a3, 0x0200, "Saitek Racing Wheel", 0, XTYPE_XBOX },
	{ 0x06a3, 0x0201, "Saitek Adrenalin", 0, XTYPE_XBOX },
	{ 0x06a3, 0xf51a, "Saitek P3600", 0, XTYPE_XBOX360 },
	{ 0x0738, 0x4503, "Mad Catz Racing Wheel", 0, XTYPE_XBOXONE },
	{ 0x0738, 0x4506, "Mad Catz 4506 Wireless Controller", 0, XTYPE_XBOX },
	{ 0x0738, 0x4516, "Mad Catz Control Pad", 0, XTYPE_XBOX },
	{ 0x0738, 0x4520, "Mad Catz Control Pad Pro", 0, XTYPE_XBOX },
	{ 0x0738, 0x4522, "Mad Catz LumiCON", 0, XTYPE_XBOX },
	{ 0x0738, 0x4526, "Mad Catz Control Pad Pro", 0, XTYPE_XBOX },
	{ 0x0738, 0x4530, "Mad Catz Universal MC2 Racing Wheel and Pedals", 0, XTYPE_XBOX },
	{ 0x0738, 0x4536, "Mad Catz MicroCON", 0, XTYPE_XBOX },
	{ 0x0738, 0x4540, "Mad Catz Beat Pad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0738, 0x4556, "Mad Catz Lynx Wireless Controller", 0, XTYPE_XBOX },
	{ 0x0738, 0x4586, "Mad Catz MicroCon Wireless Controller", 0, XTYPE_XBOX },
	{ 0x0738, 0x4588, "Mad Catz Blaster", 0, XTYPE_XBOX },
	{ 0x0738, 0x45ff, "Mad Catz Beat Pad (w/ Handle)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0738, 0x4716, "Mad Catz Wired Xbox 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x0738, 0x4718, "Mad Catz Street Fighter IV FightStick SE", 0, XTYPE_XBOX360 },
	{ 0x0738, 0x4726, "Mad Catz Xbox 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x0738, 0x4728, "Mad Catz Street Fighter IV FightPad", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0738, 0x4736, "Mad Catz MicroCon Gamepad", 0, XTYPE_XBOX360 },
	{ 0x0738, 0x4738, "Mad Catz Wired Xbox 360 Controller (SFIV)", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0738, 0x4740, "Mad Catz Beat Pad", 0, XTYPE_XBOX360 },
	{ 0x0738, 0x4743, "Mad Catz Beat Pad Pro", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0738, 0x4758, "Mad Catz Arcade Game Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0738, 0x4a01, "Mad Catz FightStick TE 2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
	{ 0x0738, 0x6040, "Mad Catz Beat Pad Pro", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0738, 0x9871, "Mad Catz Portable Drum", 0, XTYPE_XBOX360 },
	{ 0x0738, 0xb726, "Mad Catz Xbox controller - MW2", 0, XTYPE_XBOX360 },
	{ 0x0738, 0xb738, "Mad Catz MVC2TE Stick 2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0738, 0xbeef, "Mad Catz JOYTECH NEO SE Advanced GamePad", XTYPE_XBOX360 },
	{ 0x0738, 0xcb02, "Saitek Cyborg Rumble Pad - PC/Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x0738, 0xcb03, "Saitek P3200 Rumble Pad - PC/Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x0738, 0xcb29, "Saitek Aviator Stick AV8R02", 0, XTYPE_XBOX360 },
	{ 0x0738, 0xf738, "Super SFIV FightStick TE S", 0, XTYPE_XBOX360 },
	{ 0x07ff, 0xffff, "Mad Catz GamePad", 0, XTYPE_XBOX360 },
	{ 0x0b05, 0x1a38, "ASUS ROG RAIKIRI", 0, XTYPE_XBOXONE },
	{ 0x0b05, 0x1abb, "ASUS ROG RAIKIRI PRO", 0, XTYPE_XBOXONE },
	{ 0x0c12, 0x0005, "Intec wireless", 0, XTYPE_XBOX },
	{ 0x0c12, 0x8801, "Nyko Xbox Controller", 0, XTYPE_XBOX },
	{ 0x0c12, 0x8802, "Zeroplus Xbox Controller", 0, XTYPE_XBOX },
	{ 0x0c12, 0x8809, "RedOctane Xbox Dance Pad", DANCEPAD_MAP_CONFIG, XTYPE_XBOX },
	{ 0x0c12, 0x880a, "Pelican Eclipse PL-2023", 0, XTYPE_XBOX },
	{ 0x0c12, 0x8810, "Zeroplus Xbox Controller", 0, XTYPE_XBOX },
	{ 0x0c12, 0x9902, "HAMA VibraX - *FAULTY HARDWARE*", 0, XTYPE_XBOX },
	{ 0x0d2f, 0x0002, "Andamiro Pump It Up pad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0db0, 0x1901, "Micro Star International Xbox360 Controller for Windows", 0, XTYPE_XBOX360 },
	{ 0x0e4c, 0x1097, "Radica Gamester Controller", 0, XTYPE_XBOX },
	{ 0x0e4c, 0x1103, "Radica Gamester Reflex", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0e4c, 0x2390, "Radica Games Jtech Controller", 0, XTYPE_XBOX },
	{ 0x0e4c, 0x3510, "Radica Gamester", 0, XTYPE_XBOX },
	{ 0x0e6f, 0x0003, "Logic3 Freebird wireless Controller", 0, XTYPE_XBOX },
	{ 0x0e6f, 0x0005, "Eclipse wireless Controller", 0, XTYPE_XBOX },
	{ 0x0e6f, 0x0006, "Edge wireless Controller", 0, XTYPE_XBOX },
	{ 0x0e6f, 0x0008, "After Glow Pro Controller", 0, XTYPE_XBOX },
	{ 0x0e6f, 0x0105, "HSM3 Xbox360 dancepad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0113, "Afterglow AX.1 Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x011f, "Rock Candy Gamepad Wired Controller", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0131, "PDP EA Sports Controller", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0133, "Xbox 360 Wired Controller", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0139, "Afterglow Prismatic Wired Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x013a, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0146, "Rock Candy Wired Controller for Xbox One", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0147, "PDP Marvel Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x015c, "PDP Xbox One Arcade Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
	{ 0x0e6f, 0x015d, "PDP Mirror's Edge Official Wired Controller for Xbox One", XTYPE_XBOXONE },
	{ 0x0e6f, 0x0161, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0162, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0163, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0164, "PDP Battlefield One", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0165, "PDP Titanfall 2", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0201, "Pelican PL-3601 'TSZ' Wired Xbox 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0213, "Afterglow Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x021f, "Rock Candy Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0246, "Rock Candy Gamepad for Xbox One 2015", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02a0, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02a1, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02a2, "PDP Wired Controller for Xbox One - Crimson Red", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02a4, "PDP Wired Controller for Xbox One - Stealth Series", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02a6, "PDP Wired Controller for Xbox One - Camo Series", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02a7, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02a8, "PDP Xbox One Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02ab, "PDP Controller for Xbox One", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02ad, "PDP Wired Controller for Xbox One - Stealth Series", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02b3, "Afterglow Prismatic Wired Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x02b8, "Afterglow Prismatic Wired Controller", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0301, "Logic3 Controller", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0346, "Rock Candy Gamepad for Xbox One 2016", 0, XTYPE_XBOXONE },
	{ 0x0e6f, 0x0401, "Logic3 Controller", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0413, "Afterglow AX.1 Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0x0501, "PDP Xbox 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x0e6f, 0xf900, "PDP Afterglow AX.1", 0, XTYPE_XBOX360 },
	{ 0x0e8f, 0x0201, "SmartJoy Frag Xpad/PS2 adaptor", 0, XTYPE_XBOX },
	{ 0x0e8f, 0x3008, "Generic xbox control (dealextreme)", 0, XTYPE_XBOX },
	{ 0x0f0d, 0x000a, "Hori Co. DOA4 FightStick", 0, XTYPE_XBOX360 },
	{ 0x0f0d, 0x000c, "Hori PadEX Turbo", 0, XTYPE_XBOX360 },
	{ 0x0f0d, 0x000d, "Hori Fighting Stick EX2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0f0d, 0x0016, "Hori Real Arcade Pro.EX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0f0d, 0x001b, "Hori Real Arcade Pro VX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0f0d, 0x0063, "Hori Real Arcade Pro Hayabusa (USA) Xbox One", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
	{ 0x0f0d, 0x0067, "HORIPAD ONE", 0, XTYPE_XBOXONE },
	{ 0x0f0d, 0x0078, "Hori Real Arcade Pro V Kai Xbox One", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
	{ 0x0f0d, 0x00c5, "Hori Fighting Commander ONE", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
	{ 0x0f0d, 0x00dc, "HORIPAD FPS for Nintendo Switch", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x0f0d, 0x0152, "Hori Racing Wheel Overdrive for Xbox Series X", 0, XTYPE_XBOXONE },
	{ 0x0f0d, 0x0151, "Hori Racing Wheel Overdrive for Xbox Series X", 0, XTYPE_XBOXONE },
	{ 0x0f30, 0x010b, "Philips Recoil", 0, XTYPE_XBOX },
	{ 0x0f30, 0x0202, "Joytech Advanced Controller", 0, XTYPE_XBOX },
	{ 0x0f30, 0x8888, "BigBen XBMiniPad Controller", 0, XTYPE_XBOX },
	{ 0x102c, 0xff0c, "Joytech Wireless Advanced Controller", 0, XTYPE_XBOX },
	{ 0x1038, 0x1430, "SteelSeries Stratus Duo", 0, XTYPE_XBOX360 },
	{ 0x1038, 0x1431, "SteelSeries Stratus Duo", 0, XTYPE_XBOX360 },
	{ 0x10f5, 0x7005, "Turtle Beach Recon Controller", 0, XTYPE_XBOXONE },
	{ 0x11c9, 0x55f0, "Nacon GC-100XF", 0, XTYPE_XBOX360 },
	{ 0x11ff, 0x0511, "PXN V900", 0, XTYPE_XBOX360 },
	{ 0x1209, 0x2882, "Ardwiino Controller", 0, XTYPE_XBOX360 },
	{ 0x12ab, 0x0004, "Honey Bee Xbox360 dancepad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x12ab, 0x0301, "PDP AFTERGLOW AX.1", 0, XTYPE_XBOX360 },
	{ 0x12ab, 0x0303, "Mortal Kombat Klassic FightStick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x12ab, 0x8809, "Xbox DDR dancepad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x1430, 0x079B, "RedOctane GHL Controller", 0, XTYPE_XBOXONE, QUIRK_GHL_XBOXONE },
	{ 0x1430, 0x4748, "RedOctane Guitar Hero X-plorer", 0, XTYPE_XBOX360 },
	{ 0x1430, 0x8888, "TX6500+ Dance Pad (first generation)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x1430, 0xf801, "RedOctane Controller", 0, XTYPE_XBOX360 },
	{ 0x146b, 0x0601, "BigBen Interactive XBOX 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x146b, 0x0604, "Bigben Interactive DAIJA Arcade Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1532, 0x0a00, "Razer Atrox Arcade Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOXONE },
	{ 0x1532, 0x0a03, "Razer Wildcat", 0, XTYPE_XBOXONE },
	{ 0x1532, 0x0a29, "Razer Wolverine V2", 0, XTYPE_XBOXONE },
	{ 0x15e4, 0x3f00, "Power A Mini Pro Elite", 0, XTYPE_XBOX360 },
	{ 0x15e4, 0x3f0a, "Xbox Airflo wired controller", 0, XTYPE_XBOX360 },
	{ 0x15e4, 0x3f10, "Batarang Xbox 360 controller", 0, XTYPE_XBOX360 },
	{ 0x162e, 0xbeef, "Joytech Neo-Se Take2", 0, XTYPE_XBOX360 },
	{ 0x1689, 0xfd00, "Razer Onza Tournament Edition", 0, XTYPE_XBOX360 },
	{ 0x1689, 0xfd01, "Razer Onza Classic Edition", 0, XTYPE_XBOX360 },
	{ 0x1689, 0xfe00, "Razer Sabertooth", 0, XTYPE_XBOX360 },
	{ 0x17ef, 0x6182, "Lenovo Legion Controller for Windows", 0, XTYPE_XBOX360 },
	{ 0x1949, 0x041a, "Amazon Game Controller", 0, XTYPE_XBOX360 },
	{ 0x1a86, 0xe310, "QH Electronics Controller", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0x0002, "Harmonix Rock Band Guitar", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0x0003, "Harmonix Rock Band Drumkit", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0x0130, "Ion Drum Rocker", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf016, "Mad Catz Xbox 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf018, "Mad Catz Street Fighter IV SE Fighting Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf019, "Mad Catz Brawlstick for Xbox 360", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf021, "Mad Cats Ghost Recon FS GamePad", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf023, "MLG Pro Circuit Controller (Xbox)", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf025, "Mad Catz Call Of Duty", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf027, "Mad Catz FPS Pro", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf028, "Street Fighter IV FightPad", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf02e, "Mad Catz Fightpad", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf030, "Mad Catz Xbox 360 MC2 MicroCon Racing Wheel", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf036, "Mad Catz MicroCon GamePad Pro", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf038, "Street Fighter IV FightStick TE", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf039, "Mad Catz MvC2 TE", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf03a, "Mad Catz SFxT Fightstick Pro", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf03d, "Street Fighter IV Arcade Stick TE - Chun Li", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf03e, "Mad Catz MLG FightStick TE", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf03f, "Mad Catz FightStick SoulCaliber", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf042, "Mad Catz FightStick TES+", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf080, "Mad Catz FightStick TE2", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf501, "HoriPad EX2 Turbo", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf502, "Hori Real Arcade Pro.VX SA", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf503, "Hori Fighting Stick VX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf504, "Hori Real Arcade Pro. EX", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf505, "Hori Fighting Stick EX2B", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xf506, "Hori Real Arcade Pro.EX Premium VLX", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf900, "Harmonix Xbox 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf901, "Gamestop Xbox 360 Controller", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf903, "Tron Xbox 360 controller", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf904, "PDP Versus Fighting Pad", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xf906, "MortalKombat FightStick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x1bad, 0xfa01, "MadCatz GamePad", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xfd00, "Razer Onza TE", 0, XTYPE_XBOX360 },
	{ 0x1bad, 0xfd01, "Razer Onza", 0, XTYPE_XBOX360 },
	{ 0x20d6, 0x2001, "BDA Xbox Series X Wired Controller", 0, XTYPE_XBOXONE },
	{ 0x20d6, 0x2009, "PowerA Enhanced Wired Controller for Xbox Series X|S", 0, XTYPE_XBOXONE },
	{ 0x20d6, 0x281f, "PowerA Wired Controller For Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x2345, 0xe00b, "Machenike G5 Pro Controller", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5000, "Razer Atrox Arcade Stick", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x24c6, 0x5300, "PowerA MINI PROEX Controller", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5303, "Xbox Airflo wired controller", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x530a, "Xbox 360 Pro EX Controller", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x531a, "PowerA Pro Ex", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5397, "FUS1ON Tournament Controller", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x541a, "PowerA Xbox One Mini Wired Controller", 0, XTYPE_XBOXONE },
	{ 0x24c6, 0x542a, "Xbox ONE spectra", 0, XTYPE_XBOXONE },
	{ 0x24c6, 0x543a, "PowerA Xbox One wired controller", 0, XTYPE_XBOXONE },
	{ 0x24c6, 0x5500, "Hori XBOX 360 EX 2 with Turbo", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5501, "Hori Real Arcade Pro VX-SA", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5502, "Hori Fighting Stick VX Alt", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x24c6, 0x5503, "Hori Fighting Edge", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x24c6, 0x5506, "Hori SOULCALIBUR V Stick", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x550d, "Hori GEM Xbox controller", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x550e, "Hori Real Arcade Pro V Kai 360", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x24c6, 0x5510, "Hori Fighting Commander ONE (Xbox 360/PC Mode)", MAP_TRIGGERS_TO_BUTTONS, XTYPE_XBOX360 },
	{ 0x24c6, 0x551a, "PowerA FUSION Pro Controller", 0, XTYPE_XBOXONE },
	{ 0x24c6, 0x561a, "PowerA FUSION Controller", 0, XTYPE_XBOXONE },
	{ 0x24c6, 0x581a, "ThrustMaster XB1 Classic Controller", 0, XTYPE_XBOXONE },
	{ 0x24c6, 0x5b00, "ThrustMaster Ferrari 458 Racing Wheel", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5b02, "Thrustmaster, Inc. GPX Controller", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5b03, "Thrustmaster Ferrari 458 Racing Wheel", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0x5d04, "Razer Sabertooth", 0, XTYPE_XBOX360 },
	{ 0x24c6, 0xfafe, "Rock Candy Gamepad for Xbox 360", 0, XTYPE_XBOX360 },
	{ 0x2563, 0x058d, "OneXPlayer Gamepad", 0, XTYPE_XBOX360 },
	{ 0x294b, 0x3303, "Snakebyte GAMEPAD BASE X", 0, XTYPE_XBOXONE },
	{ 0x294b, 0x3404, "Snakebyte GAMEPAD RGB X", 0, XTYPE_XBOXONE },
	{ 0x2dc8, 0x2000, "8BitDo Pro 2 Wired Controller fox Xbox", 0, XTYPE_XBOXONE },
	{ 0x2dc8, 0x3106, "8BitDo Ultimate Wireless / Pro 2 Wired Controller", 0, XTYPE_XBOX360 },
	{ 0x2dc8, 0x3109, "8BitDo Ultimate Wireless Bluetooth", 0, XTYPE_XBOX360 },
	{ 0x2dc8, 0x310a, "8BitDo Ultimate 2C Wireless Controller", 0, XTYPE_XBOX360 },
	{ 0x2e24, 0x0652, "Hyperkin Duke X-Box One pad", 0, XTYPE_XBOXONE },
	{ 0x2e95, 0x0504, "SCUF Gaming Controller", MAP_SELECT_BUTTON, XTYPE_XBOXONE },
	{ 0x31e3, 0x1100, "Wooting One", 0, XTYPE_XBOX360 },
	{ 0x31e3, 0x1200, "Wooting Two", 0, XTYPE_XBOX360 },
	{ 0x31e3, 0x1210, "Wooting Lekker", 0, XTYPE_XBOX360 },
	{ 0x31e3, 0x1220, "Wooting Two HE", 0, XTYPE_XBOX360 },
	{ 0x31e3, 0x1230, "Wooting Two HE (ARM)", 0, XTYPE_XBOX360 },
	{ 0x31e3, 0x1300, "Wooting 60HE (AVR)", 0, XTYPE_XBOX360 },
	{ 0x31e3, 0x1310, "Wooting 60HE (ARM)", 0, XTYPE_XBOX360 },
	{ 0x3285, 0x0603, "Nacon Pro Compact controller for Xbox", 0, XTYPE_XBOXONE },
	{ 0x3285, 0x0607, "Nacon GC-100", 0, XTYPE_XBOX360 },
	{ 0x3285, 0x0614, "Nacon Pro Compact", 0, XTYPE_XBOXONE },
	{ 0x3285, 0x0662, "Nacon Revolution5 Pro", 0, XTYPE_XBOX360 },
	{ 0x3285, 0x0663, "Nacon Evol-X", 0, XTYPE_XBOXONE },
	{ 0x3537, 0x1004, "GameSir T4 Kaleid", 0, XTYPE_XBOX360 },
	{ 0x3767, 0x0101, "Fanatec Speedster 3 Forceshock Wheel", 0, XTYPE_XBOX },
	{ 0x413d, 0x2104, "Black Shark Green Ghost Gamepad", 0, XTYPE_XBOX360 },
	{ 0xffff, 0xffff, "Chinese-made Xbox Controller", 0, XTYPE_XBOX },
	{ 0x0000, 0x0000, "Generic X-Box pad", 0, XTYPE_UNKNOWN }
};

/* buttons shared with xbox and xbox360 */
static const signed short xpad_common_btn[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y,			/* "analog" buttons */
	BTN_START, BTN_SELECT, BTN_THUMBL, BTN_THUMBR,	/* start/back/sticks */
	-1						/* terminating entry */
};

/* original xbox controllers only */
static const signed short xpad_btn[] = {
	BTN_C, BTN_Z,		/* "analog" buttons */
	-1			/* terminating entry */
};

/* used when dpad is mapped to buttons */
static const signed short xpad_btn_pad[] = {
	BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2,		/* d-pad left, right */
	BTN_TRIGGER_HAPPY3, BTN_TRIGGER_HAPPY4,		/* d-pad up, down */
	-1				/* terminating entry */
};

/* used when triggers are mapped to buttons */
static const signed short xpad_btn_triggers[] = {
	BTN_TL2, BTN_TR2,		/* triggers left/right */
	-1
};

static const signed short xpad360_btn[] = {  /* buttons for x360 controller */
	BTN_TL, BTN_TR,		/* Button LB/RB */
	BTN_MODE,		/* The big X button */
	-1
};

static const signed short xpad_abs[] = {
	ABS_X, ABS_Y,		/* left stick */
	ABS_RX, ABS_RY,		/* right stick */
	-1			/* terminating entry */
};

/* used when dpad is mapped to axes */
static const signed short xpad_abs_pad[] = {
	ABS_HAT0X, ABS_HAT0Y,	/* d-pad axes */
	-1			/* terminating entry */
};

/* used when triggers are mapped to axes */
static const signed short xpad_abs_triggers[] = {
	ABS_Z, ABS_RZ,		/* triggers left/right */
	-1
};

/* used when the controller has extra paddle buttons */
static const signed short xpad_btn_paddles[] = {
	BTN_TRIGGER_HAPPY5, BTN_TRIGGER_HAPPY6, /* paddle upper right, lower right */
	BTN_TRIGGER_HAPPY7, BTN_TRIGGER_HAPPY8, /* paddle upper left, lower left */
	-1						/* terminating entry */
};

/*
 * Xbox 360 Chatpad
 *
 * The chatpad is a small QWERTY keyboard accessory that clips onto the
 * controller. On wired controllers it is exposed on a dedicated, vendor
 * specific USB interface; on wireless controllers its key reports are
 * interleaved into the normal wireless data stream. In both cases the
 * controller has to be told to enable the chatpad and then kept awake with
 * periodic keep-alive messages.
 *
 * The magic numbers below were reverse-engineered by the xboxdrv project.
 */

/* Chatpad modifier bit flags as reported by the hardware */
#define CHATPAD_MOD_SHIFT	0x01
#define CHATPAD_MOD_GREEN	0x02
#define CHATPAD_MOD_ORANGE	0x04
#define CHATPAD_MOD_PEOPLE	0x08

/* Interval between chatpad keep-alive messages, in msecs */
#define CHATPAD_KEEPALIVE_INTERVAL 1000

/*
 * Translation from the chatpad's vendor specific scancodes to Linux input
 * keycodes. The largest scancode is 0x77, so a 128 entry table is enough.
 * Scancodes that are not mapped translate to KEY_RESERVED and are ignored.
 */
static const unsigned short xpad_chatpad_keycodes[128] = {
	[0x17] = KEY_1,		[0x16] = KEY_2,		[0x15] = KEY_3,
	[0x14] = KEY_4,		[0x13] = KEY_5,		[0x12] = KEY_6,
	[0x11] = KEY_7,		[0x67] = KEY_8,		[0x66] = KEY_9,
	[0x65] = KEY_0,
	[0x27] = KEY_Q,		[0x26] = KEY_W,		[0x25] = KEY_E,
	[0x24] = KEY_R,		[0x23] = KEY_T,		[0x22] = KEY_Y,
	[0x21] = KEY_U,		[0x76] = KEY_I,		[0x75] = KEY_O,
	[0x64] = KEY_P,
	[0x37] = KEY_A,		[0x36] = KEY_S,		[0x35] = KEY_D,
	[0x34] = KEY_F,		[0x33] = KEY_G,		[0x32] = KEY_H,
	[0x31] = KEY_J,		[0x77] = KEY_K,		[0x72] = KEY_L,
	[0x46] = KEY_Z,		[0x45] = KEY_X,		[0x44] = KEY_C,
	[0x43] = KEY_V,		[0x42] = KEY_B,		[0x41] = KEY_N,
	[0x52] = KEY_M,
	[0x62] = KEY_COMMA,	[0x53] = KEY_DOT,	[0x63] = KEY_ENTER,
	[0x71] = KEY_BACKSPACE,	[0x54] = KEY_SPACE,	[0x55] = KEY_LEFT,
	[0x51] = KEY_RIGHT,
};

/*
 * The green and orange modifier keys select alternate symbols printed on the
 * keycaps. We translate those to the matching Linux keycode, synthesizing
 * Shift for symbols that live on a shifted key (e.g. green Q '!' -> Shift+1).
 *
 * Entries are encoded as a keycode optionally OR'd with XPAD_CP_SHIFT. The
 * chatpad sold internationally also prints accented letters and a few symbols
 * (e.g. EUR, diaeresis) that have no Linux keycode; those are marked
 * XPAD_CP_NONE and produce nothing. A zero entry means the key has no legend
 * in that layer, so the base keycode is used instead (e.g. green + a digit
 * still types the digit).
 */
#define XPAD_CP_SHIFT	0x8000		/* emit with Shift held */
#define XPAD_CP_NONE	0x7fff		/* legend exists but is not representable */
#define XPAD_CP_KEYMASK	0x0fff		/* keycode portion of an entry */

static const u16 xpad_chatpad_green[128] = {
	[0x27] = KEY_1 | XPAD_CP_SHIFT,		/* Q ! */
	[0x26] = KEY_2 | XPAD_CP_SHIFT,		/* W @ */
	[0x25] = XPAD_CP_NONE,			/* E EUR */
	[0x24] = KEY_3 | XPAD_CP_SHIFT,		/* R # */
	[0x23] = KEY_5 | XPAD_CP_SHIFT,		/* T % */
	[0x22] = KEY_6 | XPAD_CP_SHIFT,		/* Y ^ */
	[0x21] = KEY_7 | XPAD_CP_SHIFT,		/* U & */
	[0x76] = KEY_8 | XPAD_CP_SHIFT,		/* I * */
	[0x75] = KEY_9 | XPAD_CP_SHIFT,		/* O ( */
	[0x64] = KEY_0 | XPAD_CP_SHIFT,		/* P ) */
	[0x37] = KEY_GRAVE | XPAD_CP_SHIFT,	/* A ~ */
	[0x36] = XPAD_CP_NONE,			/* S s-caron */
	[0x35] = KEY_LEFTBRACE | XPAD_CP_SHIFT,	/* D { */
	[0x34] = KEY_RIGHTBRACE | XPAD_CP_SHIFT,/* F } */
	[0x33] = XPAD_CP_NONE,			/* G diaeresis */
	[0x32] = KEY_SLASH,			/* H / */
	[0x31] = KEY_APOSTROPHE,		/* J ' */
	[0x77] = KEY_LEFTBRACE,			/* K [ */
	[0x72] = KEY_RIGHTBRACE,		/* L ] */
	[0x62] = KEY_SEMICOLON | XPAD_CP_SHIFT,	/* , : */
	[0x46] = KEY_GRAVE,			/* Z ` */
	[0x45] = XPAD_CP_NONE,			/* X << */
	[0x44] = XPAD_CP_NONE,			/* C >> */
	[0x43] = KEY_MINUS,			/* V - */
	[0x42] = KEY_BACKSLASH | XPAD_CP_SHIFT,	/* B | */
	[0x41] = KEY_COMMA | XPAD_CP_SHIFT,	/* N < */
	[0x52] = KEY_DOT | XPAD_CP_SHIFT,	/* M > */
	[0x53] = KEY_SLASH | XPAD_CP_SHIFT,	/* . ? */
};

static const u16 xpad_chatpad_orange[128] = {
	[0x27] = XPAD_CP_NONE,			/* Q inverted-! */
	[0x26] = XPAD_CP_NONE,			/* W a-ring */
	[0x25] = XPAD_CP_NONE,			/* E e-acute */
	[0x24] = KEY_4 | XPAD_CP_SHIFT,		/* R $ */
	[0x23] = XPAD_CP_NONE,			/* T thorn */
	[0x22] = XPAD_CP_NONE,			/* Y y-acute */
	[0x21] = XPAD_CP_NONE,			/* U u-acute */
	[0x76] = XPAD_CP_NONE,			/* I i-acute */
	[0x75] = XPAD_CP_NONE,			/* O o-acute */
	[0x64] = KEY_EQUAL,			/* P = */
	[0x37] = XPAD_CP_NONE,			/* A a-acute */
	[0x36] = XPAD_CP_NONE,			/* S sharp-s */
	[0x35] = XPAD_CP_NONE,			/* D eth */
	[0x34] = XPAD_CP_NONE,			/* F pound */
	[0x33] = XPAD_CP_NONE,			/* G yen */
	[0x32] = KEY_BACKSLASH,			/* H \ */
	[0x31] = KEY_APOSTROPHE | XPAD_CP_SHIFT,/* J " */
	[0x77] = XPAD_CP_NONE,			/* K smiley */
	[0x72] = XPAD_CP_NONE,			/* L o-stroke */
	[0x62] = KEY_SEMICOLON,			/* , ; */
	[0x46] = XPAD_CP_NONE,			/* Z ae */
	[0x45] = XPAD_CP_NONE,			/* X oe */
	[0x44] = XPAD_CP_NONE,			/* C c-cedilla */
	[0x43] = KEY_MINUS | XPAD_CP_SHIFT,	/* V _ */
	[0x42] = KEY_EQUAL | XPAD_CP_SHIFT,	/* B + */
	[0x41] = XPAD_CP_NONE,			/* N n-tilde */
	[0x52] = XPAD_CP_NONE,			/* M micro */
	[0x53] = XPAD_CP_NONE,			/* . inverted-? */
};

/* used for GHL dpad mapping */
static const struct {int x; int y; } dpad_mapping[] = {
	{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1},
	{0, 0}
};

/*
 * Xbox 360 has a vendor-specific class, so we cannot match it with only
 * USB_INTERFACE_INFO (also specifically refused by USB subsystem), so we
 * match against vendor id as well. Wired Xbox 360 devices have protocol 1,
 * wireless controllers have protocol 129.
 */
#define XPAD_XBOX360_VENDOR_PROTOCOL(vend, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (vend), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 93, \
	.bInterfaceProtocol = (pr)
#define XPAD_XBOX360_VENDOR(vend) \
	{ XPAD_XBOX360_VENDOR_PROTOCOL((vend), 1) }, \
	{ XPAD_XBOX360_VENDOR_PROTOCOL((vend), 129) }

/* The Xbox One controller uses subclass 71 and protocol 208. */
#define XPAD_XBOXONE_VENDOR_PROTOCOL(vend, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (vend), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 71, \
	.bInterfaceProtocol = (pr)
#define XPAD_XBOXONE_VENDOR(vend) \
	{ XPAD_XBOXONE_VENDOR_PROTOCOL((vend), 208) }

static const struct usb_device_id xpad_table[] = {
	/*
	 * Please keep this list sorted by vendor ID. Note that there are 2
	 * macros - XPAD_XBOX360_VENDOR and XPAD_XBOXONE_VENDOR.
	 */
	{ USB_INTERFACE_INFO('X', 'B', 0) },	/* Xbox USB-IF not-approved class */
	XPAD_XBOX360_VENDOR(0x0079),		/* GPD Win 2 controller */
	XPAD_XBOX360_VENDOR(0x03eb),		/* Wooting Keyboards (Legacy) */
	XPAD_XBOX360_VENDOR(0x03f0),		/* HP HyperX Xbox 360 controllers */
	XPAD_XBOXONE_VENDOR(0x03f0),		/* HP HyperX Xbox One controllers */
	XPAD_XBOX360_VENDOR(0x044f),		/* Thrustmaster Xbox 360 controllers */
	XPAD_XBOXONE_VENDOR(0x044f),		/* Thrustmaster Xbox One controllers */
	XPAD_XBOX360_VENDOR(0x045e),		/* Microsoft Xbox 360 controllers */
	XPAD_XBOXONE_VENDOR(0x045e),		/* Microsoft Xbox One controllers */
	XPAD_XBOX360_VENDOR(0x046d),		/* Logitech Xbox 360-style controllers */
	XPAD_XBOX360_VENDOR(0x056e),		/* Elecom JC-U3613M */
	XPAD_XBOX360_VENDOR(0x06a3),		/* Saitek P3600 */
	XPAD_XBOX360_VENDOR(0x0738),		/* Mad Catz Xbox 360 controllers */
	{ USB_DEVICE(0x0738, 0x4540) },		/* Mad Catz Beat Pad */
	XPAD_XBOXONE_VENDOR(0x0738),		/* Mad Catz FightStick TE 2 */
	XPAD_XBOX360_VENDOR(0x07ff),		/* Mad Catz Gamepad */
	XPAD_XBOXONE_VENDOR(0x0b05),		/* ASUS controllers */
	XPAD_XBOX360_VENDOR(0x0c12),		/* Zeroplus X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x0db0),		/* Micro Star International X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x0e6f),		/* 0x0e6f Xbox 360 controllers */
	XPAD_XBOXONE_VENDOR(0x0e6f),		/* 0x0e6f Xbox One controllers */
	XPAD_XBOX360_VENDOR(0x0f0d),		/* Hori controllers */
	XPAD_XBOXONE_VENDOR(0x0f0d),		/* Hori controllers */
	XPAD_XBOX360_VENDOR(0x1038),		/* SteelSeries controllers */
	XPAD_XBOXONE_VENDOR(0x10f5),		/* Turtle Beach Controllers */
	XPAD_XBOX360_VENDOR(0x11c9),		/* Nacon GC100XF */
	XPAD_XBOX360_VENDOR(0x11ff),		/* PXN V900 */
	XPAD_XBOX360_VENDOR(0x1209),		/* Ardwiino Controllers */
	XPAD_XBOX360_VENDOR(0x12ab),		/* Xbox 360 dance pads */
	XPAD_XBOX360_VENDOR(0x1430),		/* RedOctane Xbox 360 controllers */
	XPAD_XBOXONE_VENDOR(0x1430),		/* RedOctane X-Box One controllers */
	XPAD_XBOX360_VENDOR(0x146b),		/* Bigben Interactive controllers */
	XPAD_XBOX360_VENDOR(0x1532),		/* Razer Sabertooth */
	XPAD_XBOXONE_VENDOR(0x1532),		/* Razer Wildcat */
	XPAD_XBOX360_VENDOR(0x15e4),		/* Numark Xbox 360 controllers */
	XPAD_XBOX360_VENDOR(0x162e),		/* Joytech Xbox 360 controllers */
	XPAD_XBOX360_VENDOR(0x1689),		/* Razer Onza */
	XPAD_XBOX360_VENDOR(0x17ef),		/* Lenovo */
	XPAD_XBOX360_VENDOR(0x1949),		/* Amazon controllers */
	XPAD_XBOX360_VENDOR(0x1a86),		/* QH Electronics */
	XPAD_XBOX360_VENDOR(0x1bad),		/* Harmonix Rock Band guitar and drums */
	XPAD_XBOX360_VENDOR(0x20d6),		/* PowerA controllers */
	XPAD_XBOXONE_VENDOR(0x20d6),		/* PowerA controllers */
	XPAD_XBOX360_VENDOR(0x2345),		/* Machenike Controllers */
	XPAD_XBOX360_VENDOR(0x24c6),		/* PowerA controllers */
	XPAD_XBOXONE_VENDOR(0x24c6),		/* PowerA controllers */
	XPAD_XBOX360_VENDOR(0x2563),		/* OneXPlayer Gamepad */
	XPAD_XBOX360_VENDOR(0x260d),		/* Dareu H101 */
       XPAD_XBOXONE_VENDOR(0x294b),            /* Snakebyte */
	XPAD_XBOXONE_VENDOR(0x2c16),		/* Surge Controllers */
	XPAD_XBOX360_VENDOR(0x2c22),		/* Qanba Controllers */
	XPAD_XBOX360_VENDOR(0x2dc8),            /* 8BitDo Pro 2 Wired Controller */
	XPAD_XBOXONE_VENDOR(0x2dc8),		/* 8BitDo Pro 2 Wired Controller for Xbox */
	XPAD_XBOXONE_VENDOR(0x2e24),		/* Hyperkin Duke Xbox One pad */
	XPAD_XBOX360_VENDOR(0x2f24),		/* GameSir controllers */
	XPAD_XBOXONE_VENDOR(0x2e95),		/* SCUF Gaming Controller */
	XPAD_XBOX360_VENDOR(0x31e3),		/* Wooting Keyboards */
	XPAD_XBOX360_VENDOR(0x3285),		/* Nacon GC-100 */
	XPAD_XBOXONE_VENDOR(0x3285),		/* Nacon Evol-X */
	XPAD_XBOX360_VENDOR(0x3537),		/* GameSir Controllers */
	XPAD_XBOXONE_VENDOR(0x3537),		/* GameSir Controllers */
	XPAD_XBOX360_VENDOR(0x413d),		/* Black Shark Green Ghost Controller */
	{ }
};

MODULE_DEVICE_TABLE(usb, xpad_table);

struct xboxone_init_packet {
	u16 idVendor;
	u16 idProduct;
	const u8 *data;
	u8 len;
};

#define XBOXONE_INIT_PKT(_vid, _pid, _data)		\
	{						\
		.idVendor	= (_vid),		\
		.idProduct	= (_pid),		\
		.data		= (_data),		\
		.len		= ARRAY_SIZE(_data),	\
	}

/*
 * starting with xbox one, the game input protocol is used
 * magic numbers are taken from
 * - https://github.com/xpadneo/gip-dissector/blob/main/src/gip-dissector.lua
 * - https://github.com/medusalix/xone/blob/master/bus/protocol.c
 */
#define GIP_CMD_ACK      0x01
#define GIP_CMD_IDENTIFY 0x04
#define GIP_CMD_POWER    0x05
#define GIP_CMD_AUTHENTICATE 0x06
#define GIP_CMD_VIRTUAL_KEY  0x07
#define GIP_CMD_RUMBLE   0x09
#define GIP_CMD_LED      0x0a
#define GIP_CMD_FIRMWARE 0x0c
#define GIP_CMD_INPUT    0x20

#define GIP_SEQ0 0x00

#define GIP_OPT_ACK      0x10
#define GIP_OPT_INTERNAL 0x20

/*
 * length of the command payload encoded with
 * https://en.wikipedia.org/wiki/LEB128
 * which is a no-op for N < 128
 */
#define GIP_PL_LEN(N) (N)

/*
 * payload specific defines
 */
#define GIP_PWR_ON 0x00
#define GIP_LED_ON 0x01

#define GIP_MOTOR_R  BIT(0)
#define GIP_MOTOR_L  BIT(1)
#define GIP_MOTOR_RT BIT(2)
#define GIP_MOTOR_LT BIT(3)
#define GIP_MOTOR_ALL (GIP_MOTOR_R | GIP_MOTOR_L | GIP_MOTOR_RT | GIP_MOTOR_LT)

#define GIP_WIRED_INTF_DATA 0
#define GIP_WIRED_INTF_AUDIO 1

/*
 * This packet is required for all Xbox One pads with 2015
 * or later firmware installed (or present from the factory).
 */
static const u8 xboxone_power_on[] = {
	GIP_CMD_POWER, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(1), GIP_PWR_ON
};

/*
 * This packet is required for Xbox One S (0x045e:0x02ea)
 * and Xbox One Elite Series 2 (0x045e:0x0b00) pads to
 * initialize the controller that was previously used in
 * Bluetooth mode.
 */
static const u8 xboxone_s_init[] = {
	GIP_CMD_POWER, GIP_OPT_INTERNAL, GIP_SEQ0, 0x0f, 0x06
};

/*
 * This packet is required to get additional input data
 * from Xbox One Elite Series 2 (0x045e:0x0b00) pads.
 * We mostly do this right now to get paddle data
 */
static const u8 extra_input_packet_init[] = {
	0x4d, 0x10, 0x01, 0x02, 0x07, 0x00
};

/*
 * This packet is required for the Titanfall 2 Xbox One pads
 * (0x0e6f:0x0165) to finish initialization and for Hori pads
 * (0x0f0d:0x0067) to make the analog sticks work.
 */
static const u8 xboxone_hori_ack_id[] = {
	GIP_CMD_ACK, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(9),
	0x00, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00
};

/*
 * This packet is required for most (all?) of the PDP pads to start
 * sending input reports. These pads include: (0x0e6f:0x02ab),
 * (0x0e6f:0x02a4), (0x0e6f:0x02a6).
 */
static const u8 xboxone_pdp_led_on[] = {
	GIP_CMD_LED, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(3), 0x00, GIP_LED_ON, 0x14
};

/*
 * This packet is required for most (all?) of the PDP pads to start
 * sending input reports. These pads include: (0x0e6f:0x02ab),
 * (0x0e6f:0x02a4), (0x0e6f:0x02a6).
 */
static const u8 xboxone_pdp_auth[] = {
	GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(2), 0x01, 0x00
};

/*
 * A specific rumble packet is required for some PowerA pads to start
 * sending input reports. One of those pads is (0x24c6:0x543a).
 */
static const u8 xboxone_rumblebegin_init[] = {
	GIP_CMD_RUMBLE, 0x00, GIP_SEQ0, GIP_PL_LEN(9),
	0x00, GIP_MOTOR_ALL, 0x00, 0x00, 0x1D, 0x1D, 0xFF, 0x00, 0x00
};

/*
 * A rumble packet with zero FF intensity will immediately
 * terminate the rumbling required to init PowerA pads.
 * This should happen fast enough that the motors don't
 * spin up to enough speed to actually vibrate the gamepad.
 */
static const u8 xboxone_rumbleend_init[] = {
	GIP_CMD_RUMBLE, 0x00, GIP_SEQ0, GIP_PL_LEN(9),
	0x00, GIP_MOTOR_ALL, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* GHL Xbox One magic data */
static const char ghl_xboxone_magic_data[] = {
	0x22, 0x00, 0x00, 0x08, 0x02, 0x08, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * This specifies the selection of init packets that a gamepad
 * will be sent on init *and* the order in which they will be
 * sent. The correct sequence number will be added when the
 * packet is going to be sent.
 */
static const struct xboxone_init_packet xboxone_init_packets[] = {
	XBOXONE_INIT_PKT(0x0e6f, 0x0165, xboxone_hori_ack_id),
	XBOXONE_INIT_PKT(0x0f0d, 0x0067, xboxone_hori_ack_id),
	XBOXONE_INIT_PKT(0x1430, 0x079b, xboxone_hori_ack_id),
	XBOXONE_INIT_PKT(0x0000, 0x0000, xboxone_power_on),
	XBOXONE_INIT_PKT(0x045e, 0x02ea, xboxone_s_init),
	XBOXONE_INIT_PKT(0x045e, 0x0b00, xboxone_s_init),
	XBOXONE_INIT_PKT(0x045e, 0x0b00, extra_input_packet_init),
	XBOXONE_INIT_PKT(0x0e6f, 0x0000, xboxone_pdp_led_on),
	XBOXONE_INIT_PKT(0x1430, 0x079b, xboxone_pdp_led_on),
	XBOXONE_INIT_PKT(0x20d6, 0xa01a, xboxone_pdp_led_on),
	XBOXONE_INIT_PKT(0x0e6f, 0x0000, xboxone_pdp_auth),
	XBOXONE_INIT_PKT(0x1430, 0x079b, xboxone_pdp_auth),
	XBOXONE_INIT_PKT(0x20d6, 0xa01a, xboxone_pdp_auth),
	XBOXONE_INIT_PKT(0x24c6, 0x541a, xboxone_rumblebegin_init),
	XBOXONE_INIT_PKT(0x24c6, 0x542a, xboxone_rumblebegin_init),
	XBOXONE_INIT_PKT(0x24c6, 0x543a, xboxone_rumblebegin_init),
	XBOXONE_INIT_PKT(0x24c6, 0x541a, xboxone_rumbleend_init),
	XBOXONE_INIT_PKT(0x24c6, 0x542a, xboxone_rumbleend_init),
	XBOXONE_INIT_PKT(0x24c6, 0x543a, xboxone_rumbleend_init),
};

struct xpad_output_packet {
	u8 data[XPAD_PKT_LEN];
	u8 len;
	bool pending;
};

#define XPAD_OUT_CMD_IDX	0
#define XPAD_OUT_FF_IDX		1
#define XPAD_OUT_LED_IDX	(1 + IS_ENABLED(CONFIG_JOYSTICK_XPAD_FF))
#define XPAD_NUM_OUT_PACKETS	(1 + \
				 IS_ENABLED(CONFIG_JOYSTICK_XPAD_FF) + \
				 IS_ENABLED(CONFIG_JOYSTICK_XPAD_LEDS))

struct usb_xpad {
	struct input_dev *dev;		/* input device interface */
	struct input_dev __rcu *x360w_dev;
	struct usb_device *udev;	/* usb device */
	struct usb_interface *intf;	/* usb interface */

	bool pad_present;
	bool input_created;

	struct urb *irq_in;		/* urb for interrupt in report */
	unsigned char *idata;		/* input data */
	dma_addr_t idata_dma;

	struct urb *irq_out;		/* urb for interrupt out report */
	struct usb_anchor irq_out_anchor;
	bool irq_out_active;		/* we must not use an active URB */
	u8 odata_serial;		/* serial number for xbox one protocol */
	unsigned char *odata;		/* output data */
	dma_addr_t odata_dma;
	spinlock_t odata_lock;

	struct xpad_output_packet out_packets[XPAD_NUM_OUT_PACKETS];
	int last_out_packet;
	int init_seq;

#if defined(CONFIG_JOYSTICK_XPAD_LEDS)
	struct xpad_led *led;
#endif

	char phys[64];			/* physical device path */

	int mapping;			/* map d-pad to buttons or to axes */
	int xtype;			/* type of xbox device */
	int packet_type;		/* type of the extended packet */
	int pad_nr;			/* the order x360 pads were attached */
	int quirks;
	const char *name;		/* name of the device */
	struct work_struct work;	/* init/remove device from callback */
	struct delayed_work poweroff_work; /* work struct for poweroff on mode long press */
	time64_t mode_btn_down_ts;
	struct urb *ghl_urb;		/* URB for GHL Xbox One magic data */
	struct timer_list ghl_poke_timer;	/* Timer for periodic poke of GHL magic data */

	/* Xbox 360 chatpad keyboard accessory */
	bool chatpad;			/* chatpad support enabled for this device */
	struct usb_interface *chatpad_intf;	/* claimed wired chatpad interface */
	struct input_dev *chatpad_dev;	/* chatpad keyboard input device */
	struct urb *chatpad_irq_in;	/* urb for wired chatpad input report */
	unsigned char *chatpad_idata;	/* wired chatpad input data */
	dma_addr_t chatpad_idata_dma;
	struct delayed_work chatpad_keepalive;	/* periodic chatpad keep-alive */
	bool chatpad_keepalive_toggle;	/* wired: alternate 0x1f / 0x1e */
	u8 chatpad_held_sc[2];		/* physical scancodes currently held */
	u16 chatpad_held_kc[2];		/* emitted keycode for each held key */
	bool chatpad_shift;		/* synthetic Shift currently asserted */
	bool chatpad_meta;		/* People (Meta) currently asserted */
	u8 chatpad_last_mod;		/* last modifier byte (for LED sync) */
	bool chatpad_capslock;		/* system caps lock state (from EV_LED) */
	struct work_struct chatpad_led_work;	/* sync chatpad modifier LEDs */
	u8 chatpad_led_state;		/* modifier LED bits currently in hw */
	u8 chatpad_led_target;		/* desired modifier LED bits */
	char chatpad_name[128];		/* chatpad input device name */
	char chatpad_phys[64];		/* chatpad physical device path */
};

static int xpad_init_input(struct usb_xpad *xpad);
static void xpad_deinit_input(struct usb_xpad *xpad);
static void xpadone_ack_mode_report(struct usb_xpad *xpad, u8 seq_num);
static void xpad360w_poweroff_controller(struct usb_xpad *xpad);
static struct usb_driver xpad_driver;
static int xpad_chatpad_init_input(struct usb_xpad *xpad);
static void xpad_chatpad_deinit_input(struct usb_xpad *xpad);
static void xpad360w_chatpad_process(struct usb_xpad *xpad, unsigned char *data);
static void xpad_chatpad_wireless_send(struct usb_xpad *xpad, u8 cmd);

/*
 *	ghl_magic_poke_cb
 *
 *	Call back function that resets the timer for the next magic data poke.
 */
static void ghl_magic_poke_cb(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;

	if (urb->status < 0)
		pr_warn("URB transfer failed.\n");

	mod_timer(&xpad->ghl_poke_timer, jiffies + GHL_GUITAR_POKE_INTERVAL*HZ);
}

/*
 *	ghl_magic_poke
 *
 *	Submits the GHL magic_data URB.
 */
static void ghl_magic_poke(struct timer_list *t)
{
	int ret;
	struct usb_xpad *xpad = timer_container_of(xpad, t, ghl_poke_timer);

	ret = usb_submit_urb(xpad->ghl_urb, GFP_ATOMIC);
	if (ret < 0)
		pr_warn("URB transfer failed.\n");
}

/*
 *	ghl_init_urb
 *
 *	Prepares the interrupt URB for GHL magic_data.
 */
static int ghl_init_urb(struct usb_xpad *xpad, struct usb_device *usbdev,
		const char ghl_magic_data[], u16 poke_size, struct usb_endpoint_descriptor *ep_irq_out)
{
	u8 *databuf;
	unsigned int pipe;

	pipe = usb_sndintpipe(usbdev, ep_irq_out->bEndpointAddress);

	databuf = devm_kzalloc(&xpad->udev->dev, poke_size, GFP_ATOMIC);
	if (databuf == NULL)
		return -ENOMEM;

	memcpy(databuf, ghl_magic_data, poke_size);

	usb_fill_int_urb(
		xpad->ghl_urb, usbdev, pipe,
		databuf, poke_size,
		ghl_magic_poke_cb, xpad, ep_irq_out->bInterval);

	return 0;
}

/*
 *	xpad_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem.
 *
 *	The used report descriptor was taken from ITO Takayuki's website:
 *	 http://euc.jp/periphs/xbox-controller.ja.html
 */
static void xpad_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev = xpad->dev;

	if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
		/* left stick */
		input_report_abs(dev, ABS_X,
				 (__s16) le16_to_cpup((__le16 *)(data + 12)));
		input_report_abs(dev, ABS_Y,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 14)));

		/* right stick */
		input_report_abs(dev, ABS_RX,
				 (__s16) le16_to_cpup((__le16 *)(data + 16)));
		input_report_abs(dev, ABS_RY,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 18)));
	}

	/* triggers left/right */
	if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
		input_report_key(dev, BTN_TL2, data[10]);
		input_report_key(dev, BTN_TR2, data[11]);
	} else {
		input_report_abs(dev, ABS_Z, data[10]);
		input_report_abs(dev, ABS_RZ, data[11]);
	}

	/* digital pad */
	if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
		/* dpad as buttons (left, right, up, down) */
		input_report_key(dev, BTN_TRIGGER_HAPPY1, data[2] & BIT(2));
		input_report_key(dev, BTN_TRIGGER_HAPPY2, data[2] & BIT(3));
		input_report_key(dev, BTN_TRIGGER_HAPPY3, data[2] & BIT(0));
		input_report_key(dev, BTN_TRIGGER_HAPPY4, data[2] & BIT(1));
	} else {
		input_report_abs(dev, ABS_HAT0X,
				 !!(data[2] & 0x08) - !!(data[2] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				 !!(data[2] & 0x02) - !!(data[2] & 0x01));
	}

	/* start/back buttons and stick press left/right */
	input_report_key(dev, BTN_START,  data[2] & BIT(4));
	input_report_key(dev, BTN_SELECT, data[2] & BIT(5));
	input_report_key(dev, BTN_THUMBL, data[2] & BIT(6));
	input_report_key(dev, BTN_THUMBR, data[2] & BIT(7));

	/* "analog" buttons A, B, X, Y */
	input_report_key(dev, BTN_A, data[4]);
	input_report_key(dev, BTN_B, data[5]);
	input_report_key(dev, BTN_X, data[6]);
	input_report_key(dev, BTN_Y, data[7]);

	/* "analog" buttons black, white */
	input_report_key(dev, BTN_C, data[8]);
	input_report_key(dev, BTN_Z, data[9]);


	input_sync(dev);
}

/*
 *	xpad360_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem. It is version for xbox 360 controller
 *
 *	The used report descriptor was taken from:
 *		http://www.free60.org/wiki/Gamepad
 */

static void xpad360_process_packet(struct usb_xpad *xpad, struct input_dev *dev,
				   u16 cmd, unsigned char *data)
{
	/* valid pad data */
	if (data[0] != 0x00)
		return;

	/* digital pad */
	if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
		/* dpad as buttons (left, right, up, down) */
		input_report_key(dev, BTN_TRIGGER_HAPPY1, data[2] & BIT(2));
		input_report_key(dev, BTN_TRIGGER_HAPPY2, data[2] & BIT(3));
		input_report_key(dev, BTN_TRIGGER_HAPPY3, data[2] & BIT(0));
		input_report_key(dev, BTN_TRIGGER_HAPPY4, data[2] & BIT(1));
	}

	/*
	 * This should be a simple else block. However historically
	 * xbox360w has mapped DPAD to buttons while xbox360 did not. This
	 * made no sense, but now we can not just switch back and have to
	 * support both behaviors.
	 */
	if (!(xpad->mapping & MAP_DPAD_TO_BUTTONS) ||
	    xpad->xtype == XTYPE_XBOX360W) {
		input_report_abs(dev, ABS_HAT0X,
				 !!(data[2] & 0x08) - !!(data[2] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				 !!(data[2] & 0x02) - !!(data[2] & 0x01));
	}

	/* start/back buttons */
	input_report_key(dev, BTN_START,  data[2] & BIT(4));
	input_report_key(dev, BTN_SELECT, data[2] & BIT(5));

	/* stick press left/right */
	input_report_key(dev, BTN_THUMBL, data[2] & BIT(6));
	input_report_key(dev, BTN_THUMBR, data[2] & BIT(7));

	/* buttons A,B,X,Y,TL,TR and MODE */
	input_report_key(dev, BTN_A,	data[3] & BIT(4));
	input_report_key(dev, BTN_B,	data[3] & BIT(5));
	input_report_key(dev, BTN_X,	data[3] & BIT(6));
	input_report_key(dev, BTN_Y,	data[3] & BIT(7));
	input_report_key(dev, BTN_TL,	data[3] & BIT(0));
	input_report_key(dev, BTN_TR,	data[3] & BIT(1));
	input_report_key(dev, BTN_MODE,	data[3] & BIT(2));

	if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
		/* left stick */
		input_report_abs(dev, ABS_X,
				 (__s16) le16_to_cpup((__le16 *)(data + 6)));
		input_report_abs(dev, ABS_Y,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 8)));

		/* right stick */
		input_report_abs(dev, ABS_RX,
				 (__s16) le16_to_cpup((__le16 *)(data + 10)));
		input_report_abs(dev, ABS_RY,
				 ~(__s16) le16_to_cpup((__le16 *)(data + 12)));
	}

	/* triggers left/right */
	if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
		input_report_key(dev, BTN_TL2, data[4]);
		input_report_key(dev, BTN_TR2, data[5]);
	} else {
		input_report_abs(dev, ABS_Z, data[4]);
		input_report_abs(dev, ABS_RZ, data[5]);
	}

	input_sync(dev);

	/* XBOX360W controllers can't be turned off without driver assistance */
	if (xpad->xtype == XTYPE_XBOX360W) {
		if (data[3] & BIT(2)) {
			if (xpad->mode_btn_down_ts == 0)
				xpad->mode_btn_down_ts = ktime_get_seconds();
			schedule_delayed_work(&xpad->poweroff_work, msecs_to_jiffies(0));
		} else {
				xpad->mode_btn_down_ts = 0;
		}
	}
}

static void xpad360w_poweroff_work(struct work_struct *work) {
	struct usb_xpad *xpad = container_of(to_delayed_work(work), struct usb_xpad, poweroff_work);

	if (xpad->mode_btn_down_ts == 0)
		return;

	if ((ktime_get_seconds() - xpad->mode_btn_down_ts) >= XPAD360W_POWEROFF_TIMEOUT) {
		xpad360w_poweroff_controller(xpad);
		xpad->mode_btn_down_ts = 0;
		return;
	}

	schedule_delayed_work(&xpad->poweroff_work, msecs_to_jiffies(200));
}

static void xpad_presence_work(struct work_struct *work)
{
	struct usb_xpad *xpad = container_of(work, struct usb_xpad, work);
	int error;

	if (xpad->pad_present) {
		error = xpad_init_input(xpad);
		if (error) {
			/* complain only, not much else we can do here */
			dev_err(&xpad->dev->dev,
				"unable to init device: %d\n", error);
		} else {
			/*
			 * Create the chatpad keyboard before publishing the
			 * gamepad so that, once x360w_dev is visible, the
			 * chatpad input device is guaranteed to be present for
			 * xpad360w_process_packet.
			 */
			if (xpad->chatpad)
				xpad_chatpad_init_input(xpad);
			rcu_assign_pointer(xpad->x360w_dev, xpad->dev);
		}
	} else {
		RCU_INIT_POINTER(xpad->x360w_dev, NULL);
		synchronize_rcu();
		/*
		 * Now that we are sure xpad360w_process_packet is not
		 * using input device we can get rid of it.
		 */
		if (xpad->chatpad)
			xpad_chatpad_deinit_input(xpad);
		xpad_deinit_input(xpad);
	}
}

/*
 * xpad360w_process_packet
 *
 * Completes a request by converting the data into events for the
 * input subsystem. It is version for xbox 360 wireless controller.
 *
 * Byte.Bit
 * 00.1 - Status change: The controller or headset has connected/disconnected
 *                       Bits 01.7 and 01.6 are valid
 * 01.7 - Controller present
 * 01.6 - Headset present
 * 01.1 - Pad state (Bytes 4+) valid
 *
 */
static void xpad360w_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev;
	bool present;

	/* Presence change */
	if (data[0] & 0x08) {
		present = (data[1] & 0x80) != 0;

		if (xpad->pad_present != present) {
			xpad->pad_present = present;
			schedule_work(&xpad->work);
		}
	}

	/*
	 * Chatpad keystrokes arrive interleaved with the normal data stream.
	 * They are gated on x360w_dev being live so that the chatpad input
	 * device (created just before x360w_dev is published) is valid here.
	 */
	if (xpad->chatpad && data[0] == 0x00 && data[1] == 0x02 &&
	    data[2] == 0x00 && data[3] == 0xf0) {
		rcu_read_lock();
		if (rcu_dereference(xpad->x360w_dev))
			xpad360w_chatpad_process(xpad, data);
		rcu_read_unlock();
		return;
	}

	/* Valid pad data */
	if (data[1] != 0x1)
		return;

	rcu_read_lock();
	dev = rcu_dereference(xpad->x360w_dev);
	if (dev)
		xpad360_process_packet(xpad, dev, cmd, &data[4]);
	rcu_read_unlock();
}

/*
 *	xpadone_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem. This version is for the Xbox One controller.
 *
 *	The report format was gleaned from
 *	https://github.com/kylelemons/xbox/blob/master/xbox.go
 */
static void xpadone_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev = xpad->dev;
	bool do_sync = false;
	int dpad_value;

	/* the xbox button has its own special report */
	if (data[0] == GIP_CMD_VIRTUAL_KEY) {
		/*
		 * The Xbox One S controller requires these reports to be
		 * acked otherwise it continues sending them forever and
		 * won't report further mode button events.
		 */
		if (data[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL))
			xpadone_ack_mode_report(xpad, data[2]);

		input_report_key(dev, BTN_MODE, data[4] & GENMASK(1, 0));
		input_sync(dev);

		do_sync = true;
	} else if (data[0] == GIP_CMD_FIRMWARE) {
		/* Some packet formats force us to use this separate to poll paddle inputs */
		if (xpad->packet_type == PKT_XBE2_FW_5_11) {
			/* Mute paddles if controller is in a custom profile slot
			 * Checked by looking at the active profile slot to
			 * verify it's the default slot
			 */
			if (data[19] != 0)
				data[18] = 0;

			/* Elite Series 2 split packet paddle bits */
			input_report_key(dev, BTN_TRIGGER_HAPPY5, data[18] & BIT(0));
			input_report_key(dev, BTN_TRIGGER_HAPPY6, data[18] & BIT(1));
			input_report_key(dev, BTN_TRIGGER_HAPPY7, data[18] & BIT(2));
			input_report_key(dev, BTN_TRIGGER_HAPPY8, data[18] & BIT(3));

			do_sync = true;
		}
	} else if (data[0] == GIP_CMD_INPUT) { /* The main valid packet type for inputs */
		/* menu/view buttons */
		input_report_key(dev, BTN_START,  data[4] & BIT(2));
		input_report_key(dev, BTN_SELECT, data[4] & BIT(3));
		if (xpad->mapping & MAP_SELECT_BUTTON)
			input_report_key(dev, KEY_RECORD, data[22] & BIT(0) || /* 8BitDo: */ data[18] & BIT(0));

		/* buttons A,B,X,Y */
		input_report_key(dev, BTN_A,	data[4] & BIT(4));
		input_report_key(dev, BTN_B,	data[4] & BIT(5));
		input_report_key(dev, BTN_X,	data[4] & BIT(6));
		input_report_key(dev, BTN_Y,	data[4] & BIT(7));

		/* digital pad */
		if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
			/* dpad as buttons (left, right, up, down) */
			input_report_key(dev, BTN_TRIGGER_HAPPY1, data[5] & BIT(2));
			input_report_key(dev, BTN_TRIGGER_HAPPY2, data[5] & BIT(3));
			input_report_key(dev, BTN_TRIGGER_HAPPY3, data[5] & BIT(0));
			input_report_key(dev, BTN_TRIGGER_HAPPY4, data[5] & BIT(1));
		} else {
			input_report_abs(dev, ABS_HAT0X,
					!!(data[5] & 0x08) - !!(data[5] & 0x04));
			input_report_abs(dev, ABS_HAT0Y,
					!!(data[5] & 0x02) - !!(data[5] & 0x01));
		}

		/* TL/TR */
		input_report_key(dev, BTN_TL,	data[5] & BIT(4));
		input_report_key(dev, BTN_TR,	data[5] & BIT(5));

		/* stick press left/right */
		input_report_key(dev, BTN_THUMBL, data[5] & BIT(6));
		input_report_key(dev, BTN_THUMBR, data[5] & BIT(7));

		if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
			/* left stick */
			input_report_abs(dev, ABS_X,
					(__s16) le16_to_cpup((__le16 *)(data + 10)));
			input_report_abs(dev, ABS_Y,
					~(__s16) le16_to_cpup((__le16 *)(data + 12)));

			/* right stick */
			input_report_abs(dev, ABS_RX,
					(__s16) le16_to_cpup((__le16 *)(data + 14)));
			input_report_abs(dev, ABS_RY,
					~(__s16) le16_to_cpup((__le16 *)(data + 16)));
		}

		/* triggers left/right */
		if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
			input_report_key(dev, BTN_TL2,
					(__u16) le16_to_cpup((__le16 *)(data + 6)));
			input_report_key(dev, BTN_TR2,
					(__u16) le16_to_cpup((__le16 *)(data + 8)));
		} else {
			input_report_abs(dev, ABS_Z,
					(__u16) le16_to_cpup((__le16 *)(data + 6)));
			input_report_abs(dev, ABS_RZ,
					(__u16) le16_to_cpup((__le16 *)(data + 8)));
		}

		/* Profile button has a value of 0-3, so it is reported as an axis */
		if (xpad->mapping & MAP_PROFILE_BUTTON)
			input_report_abs(dev, ABS_PROFILE, data[34]);

		/* paddle handling */
		/* based on SDL's SDL_hidapi_xboxone.c */
		if (xpad->mapping & MAP_PADDLES) {
			if (xpad->packet_type == PKT_XBE1) {
				/* Mute paddles if controller has a custom mapping applied.
				 * Checked by comparing the current mapping
				 * config against the factory mapping config
				 */
				if (memcmp(&data[4], &data[18], 2) != 0)
					data[32] = 0;

				/* OG Elite Series Controller paddle bits */
				input_report_key(dev, BTN_TRIGGER_HAPPY5, data[32] & BIT(1));
				input_report_key(dev, BTN_TRIGGER_HAPPY6, data[32] & BIT(3));
				input_report_key(dev, BTN_TRIGGER_HAPPY7, data[32] & BIT(0));
				input_report_key(dev, BTN_TRIGGER_HAPPY8, data[32] & BIT(2));
			} else if (xpad->packet_type == PKT_XBE2_FW_OLD) {
				/* Mute paddles if controller has a custom mapping applied.
				 * Checked by comparing the current mapping
				 * config against the factory mapping config
				 */
				if (data[19] != 0)
					data[18] = 0;

				/* Elite Series 2 4.x firmware paddle bits */
				input_report_key(dev, BTN_TRIGGER_HAPPY5, data[18] & BIT(0));
				input_report_key(dev, BTN_TRIGGER_HAPPY6, data[18] & BIT(1));
				input_report_key(dev, BTN_TRIGGER_HAPPY7, data[18] & BIT(2));
				input_report_key(dev, BTN_TRIGGER_HAPPY8, data[18] & BIT(3));
			} else if (xpad->packet_type == PKT_XBE2_FW_5_EARLY) {
				/* Mute paddles if controller has a custom mapping applied.
				 * Checked by comparing the current mapping
				 * config against the factory mapping config
				 */
				if (data[23] != 0)
					data[22] = 0;

				/* Elite Series 2 5.x firmware paddle bits
				 * (before the packet was split)
				 */
				input_report_key(dev, BTN_TRIGGER_HAPPY5, data[22] & BIT(0));
				input_report_key(dev, BTN_TRIGGER_HAPPY6, data[22] & BIT(1));
				input_report_key(dev, BTN_TRIGGER_HAPPY7, data[22] & BIT(2));
				input_report_key(dev, BTN_TRIGGER_HAPPY8, data[22] & BIT(3));
			}
		}

		do_sync = true;

	} else if (data[0] == 0X21) { /* The main valid packet type for GHL inputs */
		/* Mapping chosen to be coherent with GHL dongles of other consoles */

		/* The 6 fret buttons */
		input_report_key(dev, BTN_B, data[4] & BIT(1));
		input_report_key(dev, BTN_X, data[4] & BIT(2));
		input_report_key(dev, BTN_Y, data[4] & BIT(3));
		input_report_key(dev, BTN_A, data[4] & BIT(0));
		input_report_key(dev, BTN_TL, data[4] & BIT(4));
		input_report_key(dev, BTN_TR, data[4] & BIT(5));

		/* D-pad */
		dpad_value = data[6] & 0xF;
		if (dpad_value > 7)
			dpad_value = 8;

		input_report_abs(dev, ABS_HAT0X, dpad_mapping[dpad_value].x);
		input_report_abs(dev, ABS_HAT0Y, dpad_mapping[dpad_value].y);

		/* Strum bar */
		input_report_abs(dev, ABS_Y, ((data[8] - 0x80) << 9));

		/* Tilt Sensor */
		input_report_abs(dev, ABS_Z, ((data[9] - 0x80) << 9));

		/* Whammy bar */
		input_report_abs(dev, ABS_RZ, ((data[10] - 0x80) << 9));

		/* Power Button */
		input_report_key(dev, BTN_THUMBR, data[5] & BIT(4));

		/* GHTV button */
		input_report_key(dev, BTN_START, data[5] & BIT(2));

		/* Hero Power button */
		input_report_key(dev, BTN_MODE,	data[5] & BIT(0));

		/* Pause button */
		input_report_key(dev, BTN_THUMBL, data[5] & BIT(1));

		do_sync = true;
	}

	if (do_sync)
		input_sync(dev);
}

static void xpad_irq_in(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int retval, status;

	status = urb->status;

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		return;
	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		goto exit;
	}

#if defined(DEBUG_VERBOSE)
	/* If you set rowsize to larger than 32 it defaults to 16?
	 * Otherwise I would set it to XPAD_PKT_LEN                  V
	 */
	print_hex_dump(KERN_DEBUG, "xpad-dbg: ", DUMP_PREFIX_OFFSET, 32, 1, xpad->idata, XPAD_PKT_LEN, 0);
#endif

	switch (xpad->xtype) {
	case XTYPE_XBOX360:
		xpad360_process_packet(xpad, xpad->dev, 0, xpad->idata);
		break;
	case XTYPE_XBOX360W:
		xpad360w_process_packet(xpad, 0, xpad->idata);
		break;
	case XTYPE_XBOXONE:
		xpadone_process_packet(xpad, 0, xpad->idata);
		break;
	default:
		xpad_process_packet(xpad, 0, xpad->idata);
	}

exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
			__func__, retval);
}

/* Callers must hold xpad->odata_lock spinlock */
static bool xpad_prepare_next_init_packet(struct usb_xpad *xpad)
{
	const struct xboxone_init_packet *init_packet;

	if (xpad->xtype != XTYPE_XBOXONE)
		return false;

	/* Perform initialization sequence for Xbox One pads that require it */
	while (xpad->init_seq < ARRAY_SIZE(xboxone_init_packets)) {
		init_packet = &xboxone_init_packets[xpad->init_seq++];

		if (init_packet->idVendor != 0 &&
		    init_packet->idVendor != xpad->dev->id.vendor)
			continue;

		if (init_packet->idProduct != 0 &&
		    init_packet->idProduct != xpad->dev->id.product)
			continue;

		/* This packet applies to our device, so prepare to send it */
		memcpy(xpad->odata, init_packet->data, init_packet->len);
		xpad->irq_out->transfer_buffer_length = init_packet->len;

		/* Update packet with current sequence number */
		xpad->odata[2] = xpad->odata_serial++;
		return true;
	}

	return false;
}

/* Callers must hold xpad->odata_lock spinlock */
static bool xpad_prepare_next_out_packet(struct usb_xpad *xpad)
{
	struct xpad_output_packet *pkt, *packet = NULL;
	int i;

	/* We may have init packets to send before we can send user commands */
	if (xpad_prepare_next_init_packet(xpad))
		return true;

	for (i = 0; i < XPAD_NUM_OUT_PACKETS; i++) {
		if (++xpad->last_out_packet >= XPAD_NUM_OUT_PACKETS)
			xpad->last_out_packet = 0;

		pkt = &xpad->out_packets[xpad->last_out_packet];
		if (pkt->pending) {
			dev_dbg(&xpad->intf->dev,
				"%s - found pending output packet %d\n",
				__func__, xpad->last_out_packet);
			packet = pkt;
			break;
		}
	}

	if (packet) {
		memcpy(xpad->odata, packet->data, packet->len);
		xpad->irq_out->transfer_buffer_length = packet->len;
		packet->pending = false;
		return true;
	}

	return false;
}

/* Callers must hold xpad->odata_lock spinlock */
static int xpad_try_sending_next_out_packet(struct usb_xpad *xpad)
{
	int error;

	if (!xpad->irq_out_active && xpad_prepare_next_out_packet(xpad)) {
		usb_anchor_urb(xpad->irq_out, &xpad->irq_out_anchor);
		error = usb_submit_urb(xpad->irq_out, GFP_ATOMIC);
		if (error) {
			dev_err(&xpad->intf->dev,
				"%s - usb_submit_urb failed with result %d\n",
				__func__, error);
			usb_unanchor_urb(xpad->irq_out);
			return -EIO;
		}

		xpad->irq_out_active = true;
	}

	return 0;
}

static void xpad_irq_out(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int status = urb->status;
	int error;
	unsigned long flags;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	switch (status) {
	case 0:
		/* success */
		xpad->irq_out_active = xpad_prepare_next_out_packet(xpad);
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		xpad->irq_out_active = false;
		break;

	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		break;
	}

	if (xpad->irq_out_active) {
		usb_anchor_urb(urb, &xpad->irq_out_anchor);
		error = usb_submit_urb(urb, GFP_ATOMIC);
		if (error) {
			dev_err(dev,
				"%s - usb_submit_urb failed with result %d\n",
				__func__, error);
			usb_unanchor_urb(urb);
			xpad->irq_out_active = false;
		}
	}

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

static int xpad_init_output(struct usb_interface *intf, struct usb_xpad *xpad,
			struct usb_endpoint_descriptor *ep_irq_out)
{
	int error;

	if (xpad->xtype == XTYPE_UNKNOWN)
		return 0;

	init_usb_anchor(&xpad->irq_out_anchor);

	xpad->odata = usb_alloc_coherent(xpad->udev, XPAD_PKT_LEN,
					 GFP_KERNEL, &xpad->odata_dma);
	if (!xpad->odata)
		return -ENOMEM;

	spin_lock_init(&xpad->odata_lock);

	xpad->irq_out = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_out) {
		error = -ENOMEM;
		goto err_free_coherent;
	}

	usb_fill_int_urb(xpad->irq_out, xpad->udev,
			 usb_sndintpipe(xpad->udev, ep_irq_out->bEndpointAddress),
			 xpad->odata, XPAD_PKT_LEN,
			 xpad_irq_out, xpad, ep_irq_out->bInterval);
	xpad->irq_out->transfer_dma = xpad->odata_dma;
	xpad->irq_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	return 0;

err_free_coherent:
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->odata, xpad->odata_dma);
	return error;
}

static void xpad_stop_output(struct usb_xpad *xpad)
{
	if (xpad->xtype != XTYPE_UNKNOWN) {
		if (!usb_wait_anchor_empty_timeout(&xpad->irq_out_anchor,
						   5000)) {
			dev_warn(&xpad->intf->dev,
				 "timed out waiting for output URB to complete, killing\n");
			usb_kill_anchored_urbs(&xpad->irq_out_anchor);
		}
	}
}

static void xpad_deinit_output(struct usb_xpad *xpad)
{
	if (xpad->xtype != XTYPE_UNKNOWN) {
		usb_free_urb(xpad->irq_out);
		usb_free_coherent(xpad->udev, XPAD_PKT_LEN,
				xpad->odata, xpad->odata_dma);
	}
}

static int xpad_inquiry_pad_presence(struct usb_xpad *xpad)
{
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_CMD_IDX];
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->data[0] = 0x08;
	packet->data[1] = 0x00;
	packet->data[2] = 0x0F;
	packet->data[3] = 0xC0;
	packet->data[4] = 0x00;
	packet->data[5] = 0x00;
	packet->data[6] = 0x00;
	packet->data[7] = 0x00;
	packet->data[8] = 0x00;
	packet->data[9] = 0x00;
	packet->data[10] = 0x00;
	packet->data[11] = 0x00;
	packet->len = 12;
	packet->pending = true;

	/* Reset the sequence so we send out presence first */
	xpad->last_out_packet = -1;
	retval = xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);

	return retval;
}

static int xpad_start_xbox_one(struct usb_xpad *xpad)
{
	unsigned long flags;
	int retval;

	if (usb_ifnum_to_if(xpad->udev, GIP_WIRED_INTF_AUDIO)) {
		/*
		 * Explicitly disable the audio interface. This is needed
		 * for some controllers, such as the PowerA Enhanced Wired
		 * Controller for Series X|S (0x20d6:0x200e) to report the
		 * guide button.
		 */
		retval = usb_set_interface(xpad->udev,
					   GIP_WIRED_INTF_AUDIO, 0);
		if (retval)
			dev_warn(&xpad->dev->dev,
				 "unable to disable audio interface: %d\n",
				 retval);
	}

	spin_lock_irqsave(&xpad->odata_lock, flags);

	/*
	 * Begin the init sequence by attempting to send a packet.
	 * We will cycle through the init packet sequence before
	 * sending any packets from the output ring.
	 */
	xpad->init_seq = 0;
	retval = xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);

	return retval;
}

static int xpad_start_xbox_360(struct usb_xpad *xpad)
{
	int status;

	char *data = kzalloc(20, GFP_KERNEL);

	int TIMEOUT = 100;

	/*
	this init sequence is needed for the gamesir g3w controller
	and for shanwan controllers in xpad mode.
	Unfortunately, in this mode they identify as 0x045e, 0x028e, so we
	have to inspect the manufacturer string.
	Sending this sequence to other controllers will break initialization.
	*/
	bool is_shanwan = xpad->udev->manufacturer && strcasecmp("shanwan", xpad->udev->manufacturer) == 0;
	if (!(xpad->quirks & QUIRK_360_START) && !is_shanwan) {
		status = 0;
		goto err_free_ctrl_data;
	}

	if ((xpad->quirks & QUIRK_360_START_PKT_1) || is_shanwan) {
	    status = usb_control_msg(xpad->udev,
		    usb_rcvctrlpipe(xpad->udev, 0),
		    0x1, 0xc1,
		    cpu_to_le16(0x100), cpu_to_le16(0x0), data, cpu_to_le16(20),
		    TIMEOUT);

#ifdef DEBUG
	    dev_dbg(&xpad->intf->dev,
		    "%s - control message 1 returned %d\n", __func__, status);
#endif

	    if (status < 0) {
		    goto err_free_ctrl_data;
	    }
#ifdef DEBUG
	    else {
		    print_hex_dump(KERN_DEBUG, "xpad-dbg: ", DUMP_PREFIX_OFFSET, 32, 1, data, 20, 0);
	    }
#endif
	}

	if ((xpad->quirks & QUIRK_360_START_PKT_2) || is_shanwan) {
	    status = usb_control_msg(xpad->udev,
		    usb_rcvctrlpipe(xpad->udev, 0),
		    0x1, 0xc1,
		    cpu_to_le16(0x0), cpu_to_le16(0x0), data, cpu_to_le16(8),
		    TIMEOUT);
#ifdef DEBUG
	    dev_dbg(&xpad->intf->dev,
		    "%s - control message 2 returned %d\n", __func__, status);
#endif

	    if (status < 0) {
		    goto err_free_ctrl_data;
	    }
#ifdef DEBUG
	    else {
		    print_hex_dump(KERN_DEBUG, "xpad-dbg: ", DUMP_PREFIX_OFFSET, 32, 1, data, 8, 0);
	    }
#endif
	}

	if ((xpad->quirks & QUIRK_360_START_PKT_3) || is_shanwan) {
	    status = usb_control_msg(xpad->udev,
		    usb_rcvctrlpipe(xpad->udev, 0),
		    0x1, 0xc0,
		    cpu_to_le16(0x0), cpu_to_le16(0x0), data, cpu_to_le16(4),
		    TIMEOUT);
#ifdef DEBUG
	    dev_dbg(&xpad->intf->dev,
		    "%s - control message 3 returned %d\n", __func__, status);
#endif

	    if (status < 0) {
		    goto err_free_ctrl_data;
	    }
#ifdef DEBUG
	    else {
		    print_hex_dump(KERN_DEBUG, "xpad-dbg: ", DUMP_PREFIX_OFFSET, 32, 1, data, 4, 0);
	    }
#endif
	}

	status = 0;

err_free_ctrl_data:
	kfree(data);
	return status;
}

static void xpadone_ack_mode_report(struct usb_xpad *xpad, u8 seq_num)
{
	unsigned long flags;
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_CMD_IDX];
	static const u8 mode_report_ack[] = {
		GIP_CMD_ACK, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(9),
		0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->len = sizeof(mode_report_ack);
	memcpy(packet->data, mode_report_ack, packet->len);
	packet->data[2] = seq_num;
	packet->pending = true;

	/* Reset the sequence so we send out the ack now */
	xpad->last_out_packet = -1;
	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

#ifdef CONFIG_JOYSTICK_XPAD_FF
static int xpad_play_effect(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	struct xpad_output_packet *packet = &xpad->out_packets[XPAD_OUT_FF_IDX];
	__u16 strong;
	__u16 weak;
	int retval;
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	strong = effect->u.rumble.strong_magnitude;
	weak = effect->u.rumble.weak_magnitude;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	switch (xpad->xtype) {
	case XTYPE_XBOX:
		packet->data[0] = 0x00;
		packet->data[1] = 0x06;
		packet->data[2] = 0x00;
		packet->data[3] = strong / 256;	/* left actuator */
		packet->data[4] = 0x00;
		packet->data[5] = weak / 256;	/* right actuator */
		packet->len = 6;
		packet->pending = true;
		break;

	case XTYPE_XBOX360:
		packet->data[0] = 0x00;
		packet->data[1] = 0x08;
		packet->data[2] = 0x00;
		packet->data[3] = strong / 256;  /* left actuator? */
		packet->data[4] = weak / 256;	/* right actuator? */
		packet->data[5] = 0x00;
		packet->data[6] = 0x00;
		packet->data[7] = 0x00;
		packet->len = 8;
		packet->pending = true;
		break;

	case XTYPE_XBOX360W:
		packet->data[0] = 0x00;
		packet->data[1] = 0x01;
		packet->data[2] = 0x0F;
		packet->data[3] = 0xC0;
		packet->data[4] = 0x00;
		packet->data[5] = strong / 256;
		packet->data[6] = weak / 256;
		packet->data[7] = 0x00;
		packet->data[8] = 0x00;
		packet->data[9] = 0x00;
		packet->data[10] = 0x00;
		packet->data[11] = 0x00;
		packet->len = 12;
		packet->pending = true;
		break;

	case XTYPE_XBOXONE:
		packet->data[0] = GIP_CMD_RUMBLE; /* activate rumble */
		packet->data[1] = 0x00;
		packet->data[2] = xpad->odata_serial++;
		packet->data[3] = GIP_PL_LEN(9);
		packet->data[4] = 0x00;
		packet->data[5] = GIP_MOTOR_ALL;
		packet->data[6] = 0x00; /* left trigger */
		packet->data[7] = 0x00; /* right trigger */
		packet->data[8] = strong / 512;	/* left actuator */
		packet->data[9] = weak / 512;	/* right actuator */
		packet->data[10] = 0xFF; /* on period */
		packet->data[11] = 0x00; /* off period */
		packet->data[12] = 0xFF; /* repeat count */
		packet->len = 13;
		packet->pending = true;
		break;

	default:
		dev_dbg(&xpad->dev->dev,
			"%s - rumble command sent to unsupported xpad type: %d\n",
			__func__, xpad->xtype);
		retval = -EINVAL;
		goto out;
	}

	retval = xpad_try_sending_next_out_packet(xpad);

out:
	spin_unlock_irqrestore(&xpad->odata_lock, flags);
	return retval;
}

static int xpad_init_ff(struct usb_xpad *xpad)
{
	if (xpad->xtype == XTYPE_UNKNOWN)
		return 0;

	input_set_capability(xpad->dev, EV_FF, FF_RUMBLE);

	return input_ff_create_memless(xpad->dev, NULL, xpad_play_effect);
}

#else
static int xpad_init_ff(struct usb_xpad *xpad) { return 0; }
#endif

#if defined(CONFIG_JOYSTICK_XPAD_LEDS)
#include <linux/leds.h>
#include <linux/idr.h>

static DEFINE_IDA(xpad_pad_seq);

struct xpad_led {
	char name[16];
	struct led_classdev led_cdev;
	struct usb_xpad *xpad;
};

/*
 * set the LEDs on Xbox 360 / Wireless Controllers
 * @param command
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
 */
static void xpad_send_led_command(struct usb_xpad *xpad, int command)
{
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_LED_IDX];
	unsigned long flags;

	command %= 16;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	switch (xpad->xtype) {
	case XTYPE_XBOX360:
		packet->data[0] = 0x01;
		packet->data[1] = 0x03;
		packet->data[2] = command;
		packet->len = 3;
		packet->pending = true;
		break;

	case XTYPE_XBOX360W:
		packet->data[0] = 0x00;
		packet->data[1] = 0x00;
		packet->data[2] = 0x08;
		packet->data[3] = 0x40 + command;
		packet->data[4] = 0x00;
		packet->data[5] = 0x00;
		packet->data[6] = 0x00;
		packet->data[7] = 0x00;
		packet->data[8] = 0x00;
		packet->data[9] = 0x00;
		packet->data[10] = 0x00;
		packet->data[11] = 0x00;
		packet->len = 12;
		packet->pending = true;
		break;
	}

	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

/*
 * Light up the segment corresponding to the pad number on
 * Xbox 360 Controllers.
 */
static void xpad_identify_controller(struct usb_xpad *xpad)
{
	led_set_brightness(&xpad->led->led_cdev, (xpad->pad_nr % 4) + 2);
}

static void xpad_led_set(struct led_classdev *led_cdev,
			 enum led_brightness value)
{
	struct xpad_led *xpad_led = container_of(led_cdev,
						 struct xpad_led, led_cdev);

	xpad_send_led_command(xpad_led->xpad, value);
}

static int xpad_led_probe(struct usb_xpad *xpad)
{
	struct xpad_led *led;
	struct led_classdev *led_cdev;
	int error;

	if (xpad->xtype != XTYPE_XBOX360 && xpad->xtype != XTYPE_XBOX360W)
		return 0;

	xpad->led = led = kzalloc(sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	xpad->pad_nr = ida_alloc(&xpad_pad_seq, GFP_KERNEL);
	if (xpad->pad_nr < 0) {
		error = xpad->pad_nr;
		goto err_free_mem;
	}

	snprintf(led->name, sizeof(led->name), "xpad%d", xpad->pad_nr);
	led->xpad = xpad;

	led_cdev = &led->led_cdev;
	led_cdev->name = led->name;
	led_cdev->brightness_set = xpad_led_set;
	led_cdev->flags = LED_CORE_SUSPENDRESUME;

	error = led_classdev_register(&xpad->udev->dev, led_cdev);
	if (error)
		goto err_free_id;

	xpad_identify_controller(xpad);

	return 0;

err_free_id:
	ida_free(&xpad_pad_seq, xpad->pad_nr);
err_free_mem:
	kfree(led);
	xpad->led = NULL;
	return error;
}

static void xpad_led_disconnect(struct usb_xpad *xpad)
{
	struct xpad_led *xpad_led = xpad->led;

	if (xpad_led) {
		led_classdev_unregister(&xpad_led->led_cdev);
		ida_free(&xpad_pad_seq, xpad->pad_nr);
		kfree(xpad_led);
	}
}
#else
static int xpad_led_probe(struct usb_xpad *xpad) { return 0; }
static void xpad_led_disconnect(struct usb_xpad *xpad) { }
#endif

static int xpad_start_input(struct usb_xpad *xpad)
{
	int error;

	if (xpad->xtype == XTYPE_XBOX360) {
		error = xpad_start_xbox_360(xpad);
		if (error)
			return error;
	}

	if (usb_submit_urb(xpad->irq_in, GFP_KERNEL))
		return -EIO;

	if (xpad->xtype == XTYPE_XBOXONE) {
		error = xpad_start_xbox_one(xpad);
		if (error) {
			usb_kill_urb(xpad->irq_in);
			return error;
		}
	}
	if (xpad->xtype == XTYPE_XBOX360) {
		/*
		 * Some third-party controllers Xbox 360-style controllers
		 * require this message to finish initialization.
		 */
		u8 dummy[20];

		error = usb_control_msg_recv(xpad->udev, 0,
					     /* bRequest */ 0x01,
					     /* bmRequestType */
					     USB_TYPE_VENDOR | USB_DIR_IN |
						USB_RECIP_INTERFACE,
					     /* wValue */ 0x100,
					     /* wIndex */ 0x00,
					     dummy, sizeof(dummy),
					     25, GFP_KERNEL);
		if (error)
			dev_warn(&xpad->dev->dev,
				 "unable to receive magic message: %d\n",
				 error);
	}

	return 0;
}

static void xpad_stop_input(struct usb_xpad *xpad)
{
	usb_kill_urb(xpad->irq_in);
}

static void xpad360w_poweroff_controller(struct usb_xpad *xpad)
{
	unsigned long flags;
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_CMD_IDX];

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->data[0] = 0x00;
	packet->data[1] = 0x00;
	packet->data[2] = 0x08;
	packet->data[3] = 0xC0;
	packet->data[4] = 0x00;
	packet->data[5] = 0x00;
	packet->data[6] = 0x00;
	packet->data[7] = 0x00;
	packet->data[8] = 0x00;
	packet->data[9] = 0x00;
	packet->data[10] = 0x00;
	packet->data[11] = 0x00;
	packet->len = 12;
	packet->pending = true;

	/* Reset the sequence so we send out poweroff now */
	xpad->last_out_packet = -1;
	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

static int xpad360w_start_input(struct usb_xpad *xpad)
{
	int error;

	error = usb_submit_urb(xpad->irq_in, GFP_KERNEL);
	if (error)
		return -EIO;

	/*
	 * Send presence packet.
	 * This will force the controller to resend connection packets.
	 * This is useful in the case we activate the module after the
	 * adapter has been plugged in, as it won't automatically
	 * send us info about the controllers.
	 */
	error = xpad_inquiry_pad_presence(xpad);
	if (error) {
		usb_kill_urb(xpad->irq_in);
		return error;
	}

	INIT_DELAYED_WORK(&xpad->poweroff_work, xpad360w_poweroff_work);

	return 0;
}

static void xpad360w_stop_input(struct usb_xpad *xpad)
{
	usb_kill_urb(xpad->irq_in);

	/* Make sure we are done with presence work if it was scheduled */
	flush_work(&xpad->work);
	flush_delayed_work(&xpad->poweroff_work);
}

static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	return xpad_start_input(xpad);
}

static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	xpad_stop_input(xpad);
}

static void xpad_set_up_abs(struct input_dev *input_dev, signed short abs)
{
	struct usb_xpad *xpad = input_get_drvdata(input_dev);

	switch (abs) {
	case ABS_X:
	case ABS_Y:
		/* GHL Strum bar */
		if ((xpad->xtype == XTYPE_XBOXONE) && (xpad->quirks & QUIRK_GHL_XBOXONE)) {
			input_set_abs_params(input_dev, abs, -32767, 32767, 0, 0);
			break;
		}
	case ABS_RX:
	case ABS_RY:	/* the two sticks */
		input_set_abs_params(input_dev, abs, -32768, 32767, 16, 128);
		break;
	case ABS_Z:
		/* GHL Tilt sensor */
		if ((xpad->xtype == XTYPE_XBOXONE) && (xpad->quirks & QUIRK_GHL_XBOXONE)) {
			input_set_abs_params(input_dev, abs, -32767, 32767, 0, 0);
			break;
		}
	case ABS_RZ:	/* the triggers (if mapped to axes) */
		if (xpad->xtype == XTYPE_XBOXONE) {
			/* GHL Whammy bar */
			if (xpad->quirks & QUIRK_GHL_XBOXONE)
				input_set_abs_params(input_dev, abs, -32767, 32767, 0, 0);
			else
				input_set_abs_params(input_dev, abs, 0, 1023, 0, 0);
		} else
			input_set_abs_params(input_dev, abs, 0, 255, 0, 0);
		break;
	case ABS_HAT0X:
	case ABS_HAT0Y:	/* the d-pad (only if dpad is mapped to axes */
		input_set_abs_params(input_dev, abs, -1, 1, 0, 0);
		break;
	case ABS_PROFILE: /* 4 value profile button (such as on XAC) */
		input_set_abs_params(input_dev, abs, 0, 4, 0, 0);
		break;
	default:
		input_set_abs_params(input_dev, abs, 0, 0, 0, 0);
		break;
	}
}

static void xpad_deinit_input(struct usb_xpad *xpad)
{
	if (xpad->input_created) {
		xpad->input_created = false;
		xpad_led_disconnect(xpad);
		input_unregister_device(xpad->dev);
	}
}

/*
 * The modifier LEDs, ordered to match the hardware command byte: the LED
 * command value is the index for "off" and 0x08 | index for "on". This is
 * also the bit order used in chatpad_led_target / chatpad_led_state.
 */
static const u8 xpad_chatpad_mod_bits[] = {
	CHATPAD_MOD_SHIFT, CHATPAD_MOD_GREEN, CHATPAD_MOD_ORANGE, CHATPAD_MOD_PEOPLE,
};

/*
 *	xpad_chatpad_send_cmd
 *
 *	Sends a chatpad control command byte over whichever transport this
 *	controller uses. The same command bytes (enable/keep-alive 0x1b/0x1e/0x1f
 *	and the LED values 0x00-0x0b) work on both transports: wired delivers them
 *	as a control transfer, wireless wraps them in a 00 00 0c <cmd> packet on
 *	the command endpoint. (The wireless mapping isn't publicly documented; it
 *	was found by matching the wired command byte to the wireless channel.)
 */
static void xpad_chatpad_send_cmd(struct usb_xpad *xpad, u8 cmd)
{
	if (xpad->xtype == XTYPE_XBOX360W)
		xpad_chatpad_wireless_send(xpad, cmd);
	else
		usb_control_msg_send(xpad->udev, 0, 0x00, 0x41, cmd, 0x0002,
				     NULL, 0, 100, GFP_KERNEL);
}

/*
 *	xpad_chatpad_set_led
 *
 *	Turns one of the chatpad's modifier backlight LEDs (by index into
 *	xpad_chatpad_mod_bits) on or off. Must be called from a sleepable context.
 */
static void xpad_chatpad_set_led(struct usb_xpad *xpad, unsigned int led, bool on)
{
	xpad_chatpad_send_cmd(xpad, led | (on ? 0x08 : 0x00));
}

/*
 *	xpad_chatpad_led_work
 *
 *	The modifier LEDs are driven by commands that can sleep (wired control
 *	transfers) or take the output lock (wireless), neither of which is safe
 *	from the URB completion handler that detects key state changes. This
 *	work brings the hardware LEDs in line with the requested state.
 */
static void xpad_chatpad_led_work(struct work_struct *work)
{
	struct usb_xpad *xpad =
		container_of(work, struct usb_xpad, chatpad_led_work);
	u8 target, changed;
	unsigned int i;

	do {
		target = READ_ONCE(xpad->chatpad_led_target);
		changed = target ^ xpad->chatpad_led_state;

		for (i = 0; i < ARRAY_SIZE(xpad_chatpad_mod_bits); i++)
			if (changed & xpad_chatpad_mod_bits[i])
				xpad_chatpad_set_led(xpad, i,
						     target & xpad_chatpad_mod_bits[i]);

		xpad->chatpad_led_state = target;
	} while (target != READ_ONCE(xpad->chatpad_led_target));
}

/*
 *	xpad_chatpad_update_leds
 *
 *	Computes the desired modifier-LED state and kicks the LED work if it
 *	changed. The shift LED tracks the held shift modifier and, additionally,
 *	stays lit while system caps lock is on, turning it into a caps-lock
 *	indicator. Safe to call from atomic context.
 */
static void xpad_chatpad_update_leds(struct usb_xpad *xpad, u8 modifier)
{
	u8 target = modifier & 0x0f;

	if (xpad->chatpad_capslock)
		target |= CHATPAD_MOD_SHIFT;

	if (target != READ_ONCE(xpad->chatpad_led_target)) {
		WRITE_ONCE(xpad->chatpad_led_target, target);
		schedule_work(&xpad->chatpad_led_work);
	}
}

/*
 *	xpad_chatpad_event
 *
 *	Input core callback used to push LED state to the device. We use it to
 *	learn the real system caps-lock state (driven by libinput/console on any
 *	keyboard) and mirror it onto the shift backlight. Runs in atomic context.
 */
static int xpad_chatpad_event(struct input_dev *dev, unsigned int type,
			      unsigned int code, int value)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	if (type == EV_LED && code == LED_CAPSL) {
		xpad->chatpad_capslock = value;
		if (xpad->xtype == XTYPE_XBOX360 ||
		    xpad->xtype == XTYPE_XBOX360W)
			xpad_chatpad_update_leds(xpad, xpad->chatpad_last_mod);
	}

	return 0;
}

/*
 *	xpad_chatpad_resolve
 *
 *	Resolves a chatpad scancode to a Linux keycode for the currently
 *	selected layer. Holding green or orange selects the symbol printed on
 *	the keycap in that colour. The return value is a keycode, optionally
 *	OR'd with XPAD_CP_SHIFT when the symbol lives on a shifted key, or 0
 *	when nothing should be emitted (unmapped scancode, or an international
 *	legend that has no Linux keycode).
 */
static u16 xpad_chatpad_resolve(u8 modifier, u8 scancode)
{
	u16 e = 0;

	scancode &= 0x7f;

	if (modifier & CHATPAD_MOD_GREEN)
		e = xpad_chatpad_green[scancode];
	else if (modifier & CHATPAD_MOD_ORANGE)
		e = xpad_chatpad_orange[scancode];

	if (e == XPAD_CP_NONE)
		return 0;
	if (e)
		return e;

	/* No legend in this layer: fall back to the base keycode. */
	return xpad_chatpad_keycodes[scancode];
}

/*
 *	xpad_chatpad_reset
 *
 *	Releases everything the chatpad input device currently holds. Used when
 *	the wireless chatpad reports a wake-up (which implies all keys were
 *	released) so that no key gets stuck down.
 */
static void xpad_chatpad_reset(struct usb_xpad *xpad)
{
	struct input_dev *dev = xpad->chatpad_dev;
	bool sync = false;
	int i;

	for (i = 0; i < 2; i++) {
		if (xpad->chatpad_held_sc[i]) {
			if (dev)
				input_report_key(dev, xpad->chatpad_held_kc[i], 0);
			xpad->chatpad_held_sc[i] = 0;
			xpad->chatpad_held_kc[i] = 0;
			sync = true;
		}
	}
	if (xpad->chatpad_shift) {
		if (dev)
			input_report_key(dev, KEY_LEFTSHIFT, 0);
		xpad->chatpad_shift = false;
		sync = true;
	}
	if (xpad->chatpad_meta) {
		if (dev)
			input_report_key(dev, KEY_LEFTMETA, 0);
		xpad->chatpad_meta = false;
		sync = true;
	}

	if (dev && sync)
		input_sync(dev);
}

/* Press or release a synthesized modifier key, tracking its current state. */
static void xpad_chatpad_hold(struct input_dev *dev, unsigned int key,
			      bool *held, bool want)
{
	if (*held != want) {
		input_report_key(dev, key, want);
		*held = want;
	}
}

/*
 *	xpad_chatpad_report_keys
 *
 *	Converts a chatpad key report ([modifier][key1][key2]) into input
 *	events. The chatpad reports up to two simultaneous keys plus a set of
 *	modifier flags. Green/orange select alternate symbol layers (with Shift
 *	synthesized as needed); shift and people are exposed as Shift/Meta.
 *	State is reconciled against the keys we previously emitted so that the
 *	correct keycode is released even if the layer changed meanwhile.
 *	Shared by the wired and wireless code paths.
 */
static void xpad_chatpad_report_keys(struct usb_xpad *xpad,
				     u8 modifier, u8 key1, u8 key2)
{
	struct input_dev *dev = xpad->chatpad_dev;
	u8 raw[2] = { key1, key2 };
	u8 newkeys[2];
	u16 newcodes[2];
	bool want_shift, want_meta;
	int i, j, n = 0;

	if (!dev)
		return;

	/*
	 * Orange+Shift is the "CAPS" legend on the shift key. On the rising
	 * edge of that combination, tap Caps Lock so userspace toggles it.
	 */
	if ((modifier & (CHATPAD_MOD_ORANGE | CHATPAD_MOD_SHIFT)) ==
		(CHATPAD_MOD_ORANGE | CHATPAD_MOD_SHIFT) &&
	    (xpad->chatpad_last_mod & (CHATPAD_MOD_ORANGE | CHATPAD_MOD_SHIFT)) !=
		(CHATPAD_MOD_ORANGE | CHATPAD_MOD_SHIFT)) {
		input_report_key(dev, KEY_CAPSLOCK, 1);
		input_sync(dev);
		input_report_key(dev, KEY_CAPSLOCK, 0);
		input_sync(dev);
	}

	/*
	 * Mirror the modifier keys (and caps lock) on the chatpad's backlight
	 * LEDs. The LED commands can sleep (wired) or take the output lock
	 * (wireless), so they are deferred to a work item.
	 */
	if (xpad->xtype == XTYPE_XBOX360 || xpad->xtype == XTYPE_XBOX360W)
		xpad_chatpad_update_leds(xpad, modifier);
	xpad->chatpad_last_mod = modifier & 0x0f;

	want_meta = !!(modifier & CHATPAD_MOD_PEOPLE);
	want_shift = !!(modifier & CHATPAD_MOD_SHIFT);

	/* Resolve the up-to-two pressed keys for the active layer */
	for (i = 0; i < 2; i++) {
		u16 e;

		if (!raw[i])
			continue;
		e = xpad_chatpad_resolve(modifier, raw[i]);
		if (!e)
			continue;
		newkeys[n] = raw[i];
		newcodes[n] = e & XPAD_CP_KEYMASK;
		if (e & XPAD_CP_SHIFT)
			want_shift = true;
		n++;
	}

	/*
	 * Reconcile Shift/Meta here, before the keys: a key-down must see the
	 * right modifier state, and a key-up never produces a character, so
	 * doing both directions at this point (all within the one input_sync
	 * frame below) is correct.
	 */
	xpad_chatpad_hold(dev, KEY_LEFTSHIFT, &xpad->chatpad_shift, want_shift);
	xpad_chatpad_hold(dev, KEY_LEFTMETA, &xpad->chatpad_meta, want_meta);

	/* Release physical keys that are no longer held */
	for (i = 0; i < 2; i++) {
		u8 sc = xpad->chatpad_held_sc[i];
		bool held = false;

		if (!sc)
			continue;
		for (j = 0; j < n; j++)
			if (newkeys[j] == sc)
				held = true;
		if (!held) {
			input_report_key(dev, xpad->chatpad_held_kc[i], 0);
			xpad->chatpad_held_sc[i] = 0;
			xpad->chatpad_held_kc[i] = 0;
		}
	}

	/* Press physical keys that are newly held */
	for (j = 0; j < n; j++) {
		bool already = false;

		for (i = 0; i < 2; i++)
			if (xpad->chatpad_held_sc[i] == newkeys[j])
				already = true;
		if (already)
			continue;
		for (i = 0; i < 2; i++) {
			if (!xpad->chatpad_held_sc[i]) {
				xpad->chatpad_held_sc[i] = newkeys[j];
				xpad->chatpad_held_kc[i] = newcodes[j];
				input_report_key(dev, newcodes[j], 1);
				break;
			}
		}
	}

	input_sync(dev);
}

/*
 *	xpad_chatpad_wireless_send
 *
 *	Queues a chatpad command on the wireless controller's command OUT
 *	endpoint. 0x1f enables/keeps the chatpad awake, 0x1b is sent to ack a
 *	wake-up notification.
 */
static void xpad_chatpad_wireless_send(struct usb_xpad *xpad, u8 cmd)
{
	struct xpad_output_packet *packet =
			&xpad->out_packets[XPAD_OUT_CMD_IDX];
	unsigned long flags;

	spin_lock_irqsave(&xpad->odata_lock, flags);

	packet->data[0] = 0x00;
	packet->data[1] = 0x00;
	packet->data[2] = 0x0c;
	packet->data[3] = cmd;
	memset(&packet->data[4], 0, 8);
	packet->len = 12;
	packet->pending = true;

	xpad_try_sending_next_out_packet(xpad);

	spin_unlock_irqrestore(&xpad->odata_lock, flags);
}

/*
 *	xpad360w_chatpad_process
 *
 *	Handles a wireless chatpad packet (header 00 02 00 f0). The payload at
 *	offset 24 is either a status sub-packet (0xf0 ...) or a keystroke
 *	(0x00 [modifier] [key1] [key2]).
 */
static void xpad360w_chatpad_process(struct usb_xpad *xpad, unsigned char *data)
{
	switch (data[24]) {
	case 0xf0:
		/* 0xf0 0x03 means the chatpad just woke up; ack it */
		if (data[25] == 0x03) {
			xpad_chatpad_reset(xpad);
			xpad_chatpad_wireless_send(xpad, 0x1b);
		}
		break;
	case 0x00:
		xpad_chatpad_report_keys(xpad, data[25], data[26], data[27]);
		break;
	}
}

/*
 *	xpad_chatpad_irq_in
 *
 *	Completion handler for the wired chatpad interrupt IN urb. Wired key
 *	reports have the form [0x00][modifier][key1][key2].
 */
static void xpad_chatpad_irq_in(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	unsigned char *data = xpad->chatpad_idata;
	int retval, status = urb->status;

	switch (status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, status);
		return;
	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, status);
		goto exit;
	}

	if (data[0] == 0x00)
		xpad_chatpad_report_keys(xpad, data[1], data[2], data[3]);

exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
			__func__, retval);
}

/*
 *	xpad_chatpad_send_init
 *
 *	Sends the magic control transfer handshake that enables the chatpad on
 *	a wired controller. Values were reverse-engineered by xboxdrv and
 *	differ slightly between the two known controller revisions.
 */
static int xpad_chatpad_send_init(struct usb_xpad *xpad)
{
	struct usb_device *udev = xpad->udev;
	u8 code[2] = { 0x01, 0x02 };
	int error;

	if (le16_to_cpu(udev->descriptor.bcdDevice) == 0x0114) {
		code[0] = 0x09;
		code[1] = 0x00;
	}

	error = usb_control_msg_send(udev, 0, 0xa9, 0x40, 0xa30c, 0x4423,
				     NULL, 0, 100, GFP_KERNEL);
	if (error)
		return error;
	error = usb_control_msg_send(udev, 0, 0xa9, 0x40, 0x2344, 0x7f03,
				     NULL, 0, 100, GFP_KERNEL);
	if (error)
		return error;
	error = usb_control_msg_send(udev, 0, 0xa9, 0x40, 0x5839, 0x6832,
				     NULL, 0, 100, GFP_KERNEL);
	if (error)
		return error;
	error = usb_control_msg_recv(udev, 0, 0xa1, 0xc0, 0x0000, 0xe416,
				     code, 2, 100, GFP_KERNEL);
	if (error)
		return error;
	error = usb_control_msg_send(udev, 0, 0xa1, 0x40, 0x0000, 0xe416,
				     code, 2, 100, GFP_KERNEL);
	if (error)
		return error;
	error = usb_control_msg_recv(udev, 0, 0xa1, 0xc0, 0x0000, 0xe416,
				     code, 2, 100, GFP_KERNEL);
	if (error)
		return error;

	/* finally, enable the chatpad */
	return usb_control_msg_send(udev, 0, 0x00, 0x41, 0x1b, 0x02,
				    NULL, 0, 100, GFP_KERNEL);
}

/*
 *	xpad_chatpad_keepalive_work
 *
 *	The chatpad falls asleep after a few seconds unless it is regularly
 *	poked. Wired controllers expect alternating 0x1f / 0x1e pokes, wireless
 *	controllers just want 0x1f.
 */
static void xpad_chatpad_keepalive_work(struct work_struct *work)
{
	struct usb_xpad *xpad = container_of(to_delayed_work(work),
					     struct usb_xpad, chatpad_keepalive);
	u8 cmd = 0x1f;

	if (xpad->xtype == XTYPE_XBOX360) {
		cmd = xpad->chatpad_keepalive_toggle ? 0x1e : 0x1f;
		xpad->chatpad_keepalive_toggle = !xpad->chatpad_keepalive_toggle;
	}

	xpad_chatpad_send_cmd(xpad, cmd);

	schedule_delayed_work(&xpad->chatpad_keepalive,
			      msecs_to_jiffies(CHATPAD_KEEPALIVE_INTERVAL));
}

/*
 *	xpad_chatpad_init_input
 *
 *	Creates and registers the chatpad keyboard input device. For wireless
 *	controllers this is called on controller arrival; for wired ones it is
 *	called once at probe time.
 */
static int xpad_chatpad_init_input(struct usb_xpad *xpad)
{
	struct input_dev *input;
	int i, error;

	if (xpad->chatpad_dev)
		return 0;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input->name = xpad->chatpad_name;
	input->phys = xpad->chatpad_phys;
	usb_to_input_id(xpad->udev, &input->id);
	input->dev.parent = &xpad->intf->dev;
	input->event = xpad_chatpad_event;
	input_set_drvdata(input, xpad);

	/* Advertise every keycode reachable through the base/green/orange layers */
	for (i = 0; i < ARRAY_SIZE(xpad_chatpad_keycodes); i++) {
		if (xpad_chatpad_keycodes[i])
			input_set_capability(input, EV_KEY,
					     xpad_chatpad_keycodes[i]);
		if (xpad_chatpad_green[i] && xpad_chatpad_green[i] != XPAD_CP_NONE)
			input_set_capability(input, EV_KEY,
					     xpad_chatpad_green[i] & XPAD_CP_KEYMASK);
		if (xpad_chatpad_orange[i] && xpad_chatpad_orange[i] != XPAD_CP_NONE)
			input_set_capability(input, EV_KEY,
					     xpad_chatpad_orange[i] & XPAD_CP_KEYMASK);
	}

	input_set_capability(input, EV_KEY, KEY_LEFTSHIFT);
	input_set_capability(input, EV_KEY, KEY_LEFTMETA);
	input_set_capability(input, EV_KEY, KEY_CAPSLOCK);

	/* Receive caps-lock LED state so the shift backlight can mirror it */
	input_set_capability(input, EV_LED, LED_CAPSL);

	error = input_register_device(input);
	if (error) {
		input_free_device(input);
		return error;
	}

	memset(xpad->chatpad_held_sc, 0, sizeof(xpad->chatpad_held_sc));
	memset(xpad->chatpad_held_kc, 0, sizeof(xpad->chatpad_held_kc));
	xpad->chatpad_shift = false;
	xpad->chatpad_meta = false;
	xpad->chatpad_last_mod = 0;
	xpad->chatpad_capslock = false;
	xpad->chatpad_dev = input;
	return 0;
}

static void xpad_chatpad_deinit_input(struct usb_xpad *xpad)
{
	if (xpad->chatpad_dev) {
		input_unregister_device(xpad->chatpad_dev);
		xpad->chatpad_dev = NULL;
	}
}

/*
 *	xpad_chatpad_init_wired
 *
 *	Claims the controller's dedicated chatpad interface, sets up its
 *	interrupt IN urb, runs the enable handshake and starts listening for
 *	key reports.
 */
static int xpad_chatpad_init_wired(struct usb_xpad *xpad)
{
	struct usb_device *udev = xpad->udev;
	struct usb_interface *intf;
	struct usb_host_interface *alt;
	struct usb_endpoint_descriptor *ep_irq_in = NULL;
	int i, error;

	/* The chatpad is exposed on the controller's third interface */
	intf = usb_ifnum_to_if(udev, 2);
	if (!intf)
		return -ENODEV;

	error = usb_driver_claim_interface(&xpad_driver, intf, xpad);
	if (error)
		return error;
	xpad->chatpad_intf = intf;

	alt = intf->cur_altsetting;
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		if (usb_endpoint_is_int_in(&alt->endpoint[i].desc)) {
			ep_irq_in = &alt->endpoint[i].desc;
			break;
		}
	}
	if (!ep_irq_in) {
		error = -ENODEV;
		goto err_release;
	}

	xpad->chatpad_idata = usb_alloc_coherent(udev, XPAD_PKT_LEN, GFP_KERNEL,
						 &xpad->chatpad_idata_dma);
	if (!xpad->chatpad_idata) {
		error = -ENOMEM;
		goto err_release;
	}

	xpad->chatpad_irq_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->chatpad_irq_in) {
		error = -ENOMEM;
		goto err_free_idata;
	}

	usb_fill_int_urb(xpad->chatpad_irq_in, udev,
			 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
			 xpad->chatpad_idata, XPAD_PKT_LEN,
			 xpad_chatpad_irq_in, xpad, ep_irq_in->bInterval);
	xpad->chatpad_irq_in->transfer_dma = xpad->chatpad_idata_dma;
	xpad->chatpad_irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	error = xpad_chatpad_send_init(xpad);
	if (error) {
		dev_warn(&xpad->intf->dev,
			 "could not initialise chatpad: %d\n", error);
		goto err_free_urb;
	}

	error = usb_submit_urb(xpad->chatpad_irq_in, GFP_KERNEL);
	if (error)
		goto err_free_urb;

	return 0;

err_free_urb:
	usb_free_urb(xpad->chatpad_irq_in);
	xpad->chatpad_irq_in = NULL;
err_free_idata:
	usb_free_coherent(udev, XPAD_PKT_LEN, xpad->chatpad_idata,
			  xpad->chatpad_idata_dma);
	xpad->chatpad_idata = NULL;
err_release:
	usb_driver_release_interface(&xpad_driver, intf);
	xpad->chatpad_intf = NULL;
	return error;
}

static void xpad_chatpad_deinit_wired(struct usb_xpad *xpad)
{
	if (xpad->chatpad_irq_in) {
		usb_kill_urb(xpad->chatpad_irq_in);
		usb_free_urb(xpad->chatpad_irq_in);
		xpad->chatpad_irq_in = NULL;
	}
	if (xpad->chatpad_idata) {
		usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->chatpad_idata,
				  xpad->chatpad_idata_dma);
		xpad->chatpad_idata = NULL;
	}
	if (xpad->chatpad_intf) {
		usb_driver_release_interface(&xpad_driver, xpad->chatpad_intf);
		xpad->chatpad_intf = NULL;
	}
}

/*
 *	xpad_chatpad_start / xpad_chatpad_stop
 *
 *	Bring the chatpad up and down. Wireless chatpad input devices are
 *	created on demand together with the controller (see
 *	xpad_presence_work), so here we only set up the wired keyboard and the
 *	keep-alive that applies to both transports.
 */
static int xpad_chatpad_start(struct usb_xpad *xpad)
{
	int error;

	snprintf(xpad->chatpad_name, sizeof(xpad->chatpad_name),
		 "%s Chatpad", xpad->name);
	usb_make_path(xpad->udev, xpad->chatpad_phys,
		      sizeof(xpad->chatpad_phys));
	strlcat(xpad->chatpad_phys, "/input1", sizeof(xpad->chatpad_phys));

	INIT_DELAYED_WORK(&xpad->chatpad_keepalive, xpad_chatpad_keepalive_work);
	INIT_WORK(&xpad->chatpad_led_work, xpad_chatpad_led_work);

	if (xpad->xtype == XTYPE_XBOX360) {
		error = xpad_chatpad_init_input(xpad);
		if (error)
			return error;

		error = xpad_chatpad_init_wired(xpad);
		if (error) {
			xpad_chatpad_deinit_input(xpad);
			return error;
		}
	}

	schedule_delayed_work(&xpad->chatpad_keepalive,
			      msecs_to_jiffies(CHATPAD_KEEPALIVE_INTERVAL));
	return 0;
}

static void xpad_chatpad_stop(struct usb_xpad *xpad)
{
	if (!xpad->chatpad)
		return;

	cancel_delayed_work_sync(&xpad->chatpad_keepalive);
	cancel_work_sync(&xpad->chatpad_led_work);

	if (xpad->xtype == XTYPE_XBOX360)
		xpad_chatpad_deinit_wired(xpad);

	xpad_chatpad_deinit_input(xpad);
}

static int xpad_init_input(struct usb_xpad *xpad)
{
	struct input_dev *input_dev;
	int i, error;

	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	xpad->dev = input_dev;
	input_dev->name = xpad->name;
	input_dev->phys = xpad->phys;
	usb_to_input_id(xpad->udev, &input_dev->id);

	if (xpad->xtype == XTYPE_XBOX360W) {
		/* x360w controllers and the receiver have different ids */
		input_dev->id.product = 0x02a1;
	}

	input_dev->dev.parent = &xpad->intf->dev;

	input_set_drvdata(input_dev, xpad);

	if (xpad->xtype != XTYPE_XBOX360W) {
		input_dev->open = xpad_open;
		input_dev->close = xpad_close;
	}

	if (!(xpad->mapping & MAP_STICKS_TO_NULL)) {
		/* set up axes */
		for (i = 0; xpad_abs[i] >= 0; i++)
			xpad_set_up_abs(input_dev, xpad_abs[i]);
	}

	/* set up standard buttons */
	for (i = 0; xpad_common_btn[i] >= 0; i++)
		input_set_capability(input_dev, EV_KEY, xpad_common_btn[i]);

	/* set up model-specific ones */
	if (xpad->xtype == XTYPE_XBOX360 || xpad->xtype == XTYPE_XBOX360W ||
	    xpad->xtype == XTYPE_XBOXONE) {
		for (i = 0; xpad360_btn[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY, xpad360_btn[i]);
		if (xpad->mapping & MAP_SELECT_BUTTON)
			input_set_capability(input_dev, EV_KEY, KEY_RECORD);
	} else {
		for (i = 0; xpad_btn[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY, xpad_btn[i]);
	}

	if (xpad->mapping & MAP_DPAD_TO_BUTTONS) {
		for (i = 0; xpad_btn_pad[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY,
					     xpad_btn_pad[i]);
	}

	/* set up paddles if the controller has them */
	if (xpad->mapping & MAP_PADDLES) {
		for (i = 0; xpad_btn_paddles[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY, xpad_btn_paddles[i]);
	}

	/*
	 * This should be a simple else block. However historically
	 * xbox360w has mapped DPAD to buttons while xbox360 did not. This
	 * made no sense, but now we can not just switch back and have to
	 * support both behaviors.
	 */
	if (!(xpad->mapping & MAP_DPAD_TO_BUTTONS) ||
	    xpad->xtype == XTYPE_XBOX360W) {
		for (i = 0; xpad_abs_pad[i] >= 0; i++)
			xpad_set_up_abs(input_dev, xpad_abs_pad[i]);
	}

	if (xpad->mapping & MAP_TRIGGERS_TO_BUTTONS) {
		for (i = 0; xpad_btn_triggers[i] >= 0; i++)
			input_set_capability(input_dev, EV_KEY,
					     xpad_btn_triggers[i]);
	} else {
		for (i = 0; xpad_abs_triggers[i] >= 0; i++)
			xpad_set_up_abs(input_dev, xpad_abs_triggers[i]);
	}

	/* setup profile button as an axis with 4 possible values */
	if (xpad->mapping & MAP_PROFILE_BUTTON)
		xpad_set_up_abs(input_dev, ABS_PROFILE);

	error = xpad_init_ff(xpad);
	if (error)
		goto err_free_input;

	error = xpad_led_probe(xpad);
	if (error)
		goto err_destroy_ff;

	error = input_register_device(xpad->dev);
	if (error)
		goto err_disconnect_led;

	xpad->input_created = true;
	return 0;

err_disconnect_led:
	xpad_led_disconnect(xpad);
err_destroy_ff:
	input_ff_destroy(input_dev);
err_free_input:
	input_free_device(input_dev);
	return error;
}

static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_xpad *xpad;
	struct usb_endpoint_descriptor *ep_irq_in, *ep_irq_out;
	int i, error;

	if (intf->cur_altsetting->desc.bNumEndpoints != 2)
		return -ENODEV;

	for (i = 0; xpad_device[i].idVendor; i++) {
		if ((le16_to_cpu(udev->descriptor.idVendor) == xpad_device[i].idVendor) &&
		    (le16_to_cpu(udev->descriptor.idProduct) == xpad_device[i].idProduct))
			break;
	}

	xpad = kzalloc(sizeof(*xpad), GFP_KERNEL);
	if (!xpad)
		return -ENOMEM;

	usb_make_path(udev, xpad->phys, sizeof(xpad->phys));
	strlcat(xpad->phys, "/input0", sizeof(xpad->phys));

	xpad->idata = usb_alloc_coherent(udev, XPAD_PKT_LEN,
					 GFP_KERNEL, &xpad->idata_dma);
	if (!xpad->idata) {
		error = -ENOMEM;
		goto err_free_mem;
	}

	xpad->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_in) {
		error = -ENOMEM;
		goto err_free_idata;
	}

	xpad->udev = udev;
	xpad->intf = intf;
	xpad->mapping = xpad_device[i].mapping;
	xpad->xtype = xpad_device[i].xtype;
	xpad->name = xpad_device[i].name;
	xpad->quirks = xpad_device[i].quirks;
	xpad->packet_type = PKT_XB;
	INIT_WORK(&xpad->work, xpad_presence_work);

	if (xpad->xtype == XTYPE_UNKNOWN) {
		if (intf->cur_altsetting->desc.bInterfaceClass == USB_CLASS_VENDOR_SPEC) {
			if (intf->cur_altsetting->desc.bInterfaceProtocol == 129)
				xpad->xtype = XTYPE_XBOX360W;
			else if (intf->cur_altsetting->desc.bInterfaceProtocol == 208)
				xpad->xtype = XTYPE_XBOXONE;
			else
				xpad->xtype = XTYPE_XBOX360;
		} else {
			xpad->xtype = XTYPE_XBOX;
		}

		if (dpad_to_buttons)
			xpad->mapping |= MAP_DPAD_TO_BUTTONS;
		if (triggers_to_buttons)
			xpad->mapping |= MAP_TRIGGERS_TO_BUTTONS;
		if (sticks_to_null)
			xpad->mapping |= MAP_STICKS_TO_NULL;
	}

	if (xpad->xtype == XTYPE_XBOXONE &&
	    intf->cur_altsetting->desc.bInterfaceNumber != GIP_WIRED_INTF_DATA) {
		/*
		 * The Xbox One controller lists three interfaces all with the
		 * same interface class, subclass and protocol. Differentiate by
		 * interface number.
		 */
		error = -ENODEV;
		goto err_free_in_urb;
	}

	ep_irq_in = ep_irq_out = NULL;

	for (i = 0; i < 2; i++) {
		struct usb_endpoint_descriptor *ep =
				&intf->cur_altsetting->endpoint[i].desc;

		if (usb_endpoint_xfer_int(ep)) {
			if (usb_endpoint_dir_in(ep))
				ep_irq_in = ep;
			else
				ep_irq_out = ep;
		}
	}

	if (!ep_irq_in || !ep_irq_out) {
		error = -ENODEV;
		goto err_free_in_urb;
	}

	error = xpad_init_output(intf, xpad, ep_irq_out);
	if (error)
		goto err_free_in_urb;

	usb_fill_int_urb(xpad->irq_in, udev,
			 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
			 xpad->idata, XPAD_PKT_LEN, xpad_irq_in,
			 xpad, ep_irq_in->bInterval);
	xpad->irq_in->transfer_dma = xpad->idata_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_set_intfdata(intf, xpad);

	/* Packet type detection */
	if (le16_to_cpu(udev->descriptor.idVendor) == 0x045e) { /* Microsoft controllers */
		if (le16_to_cpu(udev->descriptor.idProduct) == 0x02e3) {
			/* The original elite controller always uses the oldest
			 * type of extended packet
			 */
			xpad->packet_type = PKT_XBE1;
		} else if (le16_to_cpu(udev->descriptor.idProduct) == 0x0b00) {
			/* The elite 2 controller has seen multiple packet
			 * revisions. These are tied to specific firmware
			 * versions
			 */
			if (le16_to_cpu(udev->descriptor.bcdDevice) < 0x0500) {
				/* This is the format that the Elite 2 used
				 * prior to the BLE update
				 */
				xpad->packet_type = PKT_XBE2_FW_OLD;
			} else if (le16_to_cpu(udev->descriptor.bcdDevice) <
				   0x050b) {
				/* This is the format that the Elite 2 used
				 * prior to the update that split the packet
				 */
				xpad->packet_type = PKT_XBE2_FW_5_EARLY;
			} else {
				/* The split packet format that was introduced
				 * in firmware v5.11
				 */
				xpad->packet_type = PKT_XBE2_FW_5_11;
			}
		}
	}

	if (xpad->xtype == XTYPE_XBOX360W) {
		/*
		 * Submit the int URB immediately rather than waiting for open
		 * because we get status messages from the device whether
		 * or not any controllers are attached.  In fact, it's
		 * exactly the message that a controller has arrived that
		 * we're waiting for.
		 */
		error = xpad360w_start_input(xpad);
		if (error)
			goto err_deinit_output;
		/*
		 * Wireless controllers require RESET_RESUME to work properly
		 * after suspend. Ideally this quirk should be in usb core
		 * quirk list, but we have too many vendors producing these
		 * controllers and we'd need to maintain 2 identical lists
		 * here in this driver and in usb core.
		 */
		udev->quirks |= USB_QUIRK_RESET_RESUME;
	} else {
		error = xpad_init_input(xpad);
		if (error)
			goto err_deinit_output;
	}

	if (xpad->quirks & QUIRK_GHL_XBOXONE) {

		xpad->ghl_urb = usb_alloc_urb(0, GFP_ATOMIC);
		if (!xpad->ghl_urb)
			return -ENOMEM;

		error = ghl_init_urb(xpad, udev, ghl_xboxone_magic_data, ARRAY_SIZE(ghl_xboxone_magic_data), ep_irq_out);

		if (error)
			return error;

		timer_setup(&xpad->ghl_poke_timer, ghl_magic_poke, 0);
		mod_timer(&xpad->ghl_poke_timer, jiffies + GHL_GUITAR_POKE_INTERVAL*HZ);
	}

	/*
	 * The chatpad is supported on the wired Microsoft Xbox 360 controller
	 * (045e:028e) and on any wireless controller. Failing to bring it up
	 * is not fatal: the gamepad keeps working without it.
	 */
	if (chatpad &&
	    ((xpad->xtype == XTYPE_XBOX360 &&
	      le16_to_cpu(udev->descriptor.idVendor) == 0x045e &&
	      le16_to_cpu(udev->descriptor.idProduct) == 0x028e) ||
	     xpad->xtype == XTYPE_XBOX360W)) {
		xpad->chatpad = true;
		error = xpad_chatpad_start(xpad);
		if (error) {
			xpad->chatpad = false;
			dev_warn(&intf->dev,
				 "could not enable chatpad support: %d\n", error);
		}
	}

	return 0;

err_deinit_output:
	xpad_deinit_output(xpad);
err_free_in_urb:
	usb_free_urb(xpad->irq_in);
err_free_idata:
	usb_free_coherent(udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
err_free_mem:
	kfree(xpad);
	return error;
}

static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);

	if (!xpad)
		return;

	/*
	 * For wired controllers we also claim the dedicated chatpad interface,
	 * so disconnect() is invoked for it as well. All teardown is driven
	 * from the primary (data) interface; for the chatpad interface we just
	 * drop our reference so the primary path can free everything safely.
	 */
	if (intf == xpad->chatpad_intf) {
		usb_set_intfdata(intf, NULL);
		return;
	}

	if (xpad->xtype == XTYPE_XBOX360W)
		xpad360w_stop_input(xpad);

	xpad_chatpad_stop(xpad);

	xpad_deinit_input(xpad);

	/*
	 * Now that both input device and LED device are gone we can
	 * stop output URB.
	 */
	xpad_stop_output(xpad);

	xpad_deinit_output(xpad);

	usb_free_urb(xpad->irq_in);

	if (xpad->quirks & QUIRK_GHL_XBOXONE) {
		usb_free_urb(xpad->ghl_urb);
		timer_delete_sync(&xpad->ghl_poke_timer);
	}

	usb_free_coherent(xpad->udev, XPAD_PKT_LEN,
			xpad->idata, xpad->idata_dma);

	kfree(xpad);

	usb_set_intfdata(intf, NULL);
}

static int xpad_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct input_dev *input;

	/* The chatpad interface is suspended together with the data interface */
	if (!xpad || intf == xpad->chatpad_intf)
		return 0;

	input = xpad->dev;

	if (xpad->chatpad) {
		cancel_delayed_work_sync(&xpad->chatpad_keepalive);
		cancel_work_sync(&xpad->chatpad_led_work);
		if (xpad->chatpad_irq_in)
			usb_kill_urb(xpad->chatpad_irq_in);
	}

	if (xpad->xtype == XTYPE_XBOX360W) {
		/*
		 * Wireless controllers always listen to input so
		 * they are notified when controller shows up
		 * or goes away.
		 */
		xpad360w_stop_input(xpad);

		/*
		 * The wireless adapter is going off now, so the
		 * gamepads are going to become disconnected.
		 * Unless explicitly disabled, power them down
		 * so they don't just sit there flashing.
		 */
		if (auto_poweroff && xpad->pad_present)
			xpad360w_poweroff_controller(xpad);
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			xpad_stop_input(xpad);
		mutex_unlock(&input->mutex);
	}

	xpad_stop_output(xpad);

	return 0;
}

static int xpad_resume(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct input_dev *input;
	int retval = 0;

	/* The chatpad interface is resumed together with the data interface */
	if (!xpad || intf == xpad->chatpad_intf)
		return 0;

	input = xpad->dev;

	if (xpad->chatpad) {
		/* re-run the wired enable handshake; the controller may have reset */
		if (xpad->chatpad_irq_in) {
			xpad_chatpad_send_init(xpad);
			usb_submit_urb(xpad->chatpad_irq_in, GFP_KERNEL);
		}
		/* hardware LEDs are off after a reset; re-sync lazily */
		xpad->chatpad_led_state = 0;
		schedule_delayed_work(&xpad->chatpad_keepalive,
				      msecs_to_jiffies(CHATPAD_KEEPALIVE_INTERVAL));
	}

	if (xpad->xtype == XTYPE_XBOX360W) {
		retval = xpad360w_start_input(xpad);
	} else {
		mutex_lock(&input->mutex);
		if (input->users) {
			retval = xpad_start_input(xpad);
		} else if (xpad->xtype == XTYPE_XBOXONE) {
			/*
			 * Even if there are no users, we'll send Xbox One pads
			 * the startup sequence so they don't sit there and
			 * blink until somebody opens the input device again.
			 */
			retval = xpad_start_xbox_one(xpad);
		}
		mutex_unlock(&input->mutex);
	}

	return retval;
}

static struct usb_driver xpad_driver = {
	.name		= "xpad",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.suspend	= xpad_suspend,
	.resume		= xpad_resume,
	.id_table	= xpad_table,
};

module_usb_driver(xpad_driver);

MODULE_AUTHOR("Marko Friedemann <mfr@bmx-chemnitz.de>");
MODULE_DESCRIPTION("Xbox pad driver");
MODULE_LICENSE("GPL");
