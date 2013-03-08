/*
 * stlink-trace.h
 *
 *  Created on: 21/02/2013
 *      Author: Chris Storah
 *
 * Parts of the ST-Link code comes from the stlink project:
 * https://github.com/texane/stlink
 *
 */

#ifndef STLINK_TRACE_H_
#define STLINK_TRACE_H_

// Bus 002 Device 052: ID 0483:3748 SGS Thomson Microelectronics ST-LINK/V2

#define STLINKV2_VENDOR_ID    0x0483
#define STLINKV2_PRODUCT_ID   0x3748

#define DEBUG_LEVEL           3

#define DEBUG_COMMAND         0xF2

#define READ32                0x07
#define WRITE32               0x08

#define WRITE_DATA            0x35
#define READ_DATA             0x36

/*
 * USB modes:
 * DFU = Device Firmware Update
 * MSD = Mass Storage Device
 * DBG = Debug
 */
#define MODE_DFU              0x0000
#define MODE_MSD              0x0001
#define MODE_DBG              0x0002

#define STLINK_DEBUG_COMMAND  0xF2
#define STLINK_DFU_COMMAND    0xF3
#define STLINK_DFU_EXIT       0x07
//?? #define STLINK_DFU_ENTER      0x08

#define STLINK_DEBUG_FORCEDEBUG  0x02
#define STLINK_DEBUG_RESETSYS    0x03

#endif /* STLINK_TRACE_H_ */
