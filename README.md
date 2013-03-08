stlink-trace
============

ST-Link V2 ITM trace utility

This utility can be used with an ST-Link V2 JTAG device connected to an STM32Fxxx series microcontroller to capture the ITM data sent via the printf port (ITM stimulus port 0).

Build
-----
Eclipse project files can be used. Alternatively use the following:

gcc stlink-trace.c -lusb-1.0 -L/usr/local/lib -o stlink-trace

TODO
----
* Fix the problem where a packet with 0xF8xx length is received containing junk data - for now it is read, but indicates some error condition that needs to be investigated further. Possibly overrun?
* Merge into stlink or openOCD projects
* Add a user interface to handle the different trace output
* Support multiple trace stimulus ports i.e. remove hard-coding :)
* Clean-up the code
* Anything else that comes to mind... time permitting

----
Chris 
