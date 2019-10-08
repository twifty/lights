WARNING
=======

This project is a work in progress. There may be, and probably are, a few bugs.
I would advise __NOT__ installing in it's current form, and instead wait for a
stable release.
However, if you are interested in collaborating/testing, please open an issue.
While I have tried to make the code work for all, I can only test on my limited
set of available devices.

Some files contain paths which are hard coded for my personal system. Trying to
build may result in errors. File paths and names may change from one push to the
next, as may function prototypes.

I will state this again, this is a work in progress. I've pushed the code for
the benefit of testers, issue tracking and version control.

structure
---------

The code is a collection of kernel modules.
* *lights*  
This is the main module, which all others depend upon. It exports an interface
for creating files within the `/dev/lights/` and `/sys/class/lights/` directories.
* *aura*  
A module for interacting with ASUS AURA devices, including:
    + Motherboard (including headers)
    + RAM
    + GPU
* *more to follow*

sysfs
-----

Each module creates one or more directories within the systems `/dev/files/`
folder. Each of these directories maps to an RGB *zone* available on the
hardware. Each zone may contain the files:
+ `color` A read/write hex string color code.
+ `mode` A read/write string which specifies the zones effect.
+ `leds` A write only binary string. The enables setting of individual LEDs on
ARGB peripherals.
+ `speed` A read/write numeric string (between 1 and 5).

Each zone also has a directory available in `/sys/class/lights/`. Here, device
specific settings can be found. For example the `caps` file lists all the
available color modes of a device.

permissions
-----------

A `udev` rule exists in the `lights/udev/` directory (copy it to
`/etc/udev/rules.d`). This simply applies the 'lights' group to all device
files. Users need only create a 'lights' group on their system and add
permitted users to that group.

future
------

Once the modules are working without problems, I intend to create a system
service and GUI application. The service will be able to handle creating extra
effects which are not natively supported, and handle synchronizing colors
between devices which may cycle at differing speeds.

Known Issues
------------
* Navi GPU's have changed the i2c bus. I am having a difficult time trying to
figure out the new registers. It may be a while before they are supported.
