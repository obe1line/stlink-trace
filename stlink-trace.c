/*
  * stlink-trace.c
  *
  *  Created on: 21/02/2013
  *      Author: Chris Storah
  * Parts of the ST-Link code comes from the stlink project:
  * https://github.com/texane/stlink
  *
  * Requires an updated ST-Link V2 firmware:
  * Use the STM32 ST-LINK Utility to update to the latest version:
  * (V2.J17.S4 JTAG+SWIM Debugger)
  * See: http://www.st.com/web/en/catalog/tools/PF258168
  */

#define HEXDUMP 0
#define ASYNC	0

// for the STM32F107Z, system clock is 72MHz
// (CLK/SWO_CLK) - 1 = (72MHz/2MHz) - 1 = 35 = 0x23
#define CLOCK_DIVISOR 0x00000023
// for the STM32F207Z, system clock is 120MHz
// (CLK/SWO_CLK) - 1 = (120MHz/2MHz) - 1 = 59 = 0x3B
//#define CLOCK_DIVISOR 0x0000003B

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "ncurses.h"
#include "stlink-trace.h"
#include "stdio.h"
#include "libusb-1.0/libusb.h"
#include <getopt.h>

libusb_context* ctx = 0;
libusb_device_handle* stlinkhandle = 0;
libusb_device* stlinkdev = 0;
libusb_device** deviceList = 0;
ssize_t listSize = 0;
struct libusb_transfer* responseTransfer = 0;
struct libusb_transfer* requestTransfer = 0;

void Cleanup();
int IsStlink(libusb_device* dev);
void GetCoreId();
void EnterSWD();
int submit_wait(struct libusb_transfer* trans);
ssize_t TransferData(int terminate,
         unsigned char* transmitBuffer, size_t transmitLength,
         unsigned char* receiveBuffer, size_t receiveLength);
int FetchTraceByteCount();
void EnterDebugState();
int ReadTraceData(int toscreen, int byteCount);
void RunCore();
void StepCore();
void GetVersion();
int GetCurrentMode();
void GetTargetVoltage();
int SendAndReceive(unsigned char* txBuffer, size_t txSize, unsigned char* rxBuffer, size_t rxSize);
void Write32Bit(uint32_t address, uint32_t value);
uint32_t Read32Bit(uint32_t address);
void ExitDFUMode();
void HaltRunningSystem();
void ForceDebug();
void ResetCore();
void LocalReset();
void EnableTrace();
void UnknownCommand();
uint32_t ReadDHCSRValue();

FILE* resultsFile = NULL;
FILE* fullResultsFile = NULL;
int debugEnabled = 0;

int main(int argc, char** argv)
{
     int ret, pos, opt = 0;
     char* filename = "trace.txt";
     char* fullTraceFilename = "trace-full.txt";

     while ((opt = getopt(argc, argv, "f:t:d")) != -1) {
    	 switch (opt) {
    	 case 'd':
    		 debugEnabled = 1;
    		 break;
    	 case 't':
    		 filename = optarg;
    		 break;
    	 case 'f':
    		 fullTraceFilename = optarg;
    		 break;
    	 }
     }

	 resultsFile = fopen(filename, "w+");	// create or overwrite
	 fullResultsFile = fopen(fullTraceFilename, "w+");	// create or overwrite

     // initialise the USB session context
     ret = libusb_init(&ctx);
     if (ret != 0) {
         printf("Error initialising libusb: 0x%x\n", ret);
         Cleanup();
         exit(ret);
     }

     // turn debug messages on - full logging
     libusb_set_debug(ctx, DEBUG_LEVEL);

     // enumerate the USB devices
     listSize = libusb_get_device_list(ctx, &deviceList);
     for (pos=0; pos<listSize; pos++) {
         if (IsStlink(deviceList[pos])) {
             stlinkdev = deviceList[pos];
             break;
         }
     }

     if (stlinkdev == NULL) {
    	 printf("Unable to locate an ST-Link V2 device.\n");
    	 exit(-1);
     }

     // open ST-Link V2 adapter
     ret = libusb_open(stlinkdev, &stlinkhandle);
     if (ret != 0) {
             printf("Unable to open ST-Link V2 device.\n");
             Cleanup();
             exit(ret);
     }

     // detach from kernel if required
     if (libusb_kernel_driver_active(stlinkhandle, 0)) {
         printf("Detaching the device from the kernel\n");
         libusb_detach_kernel_driver(stlinkhandle, 0);
     }

     int config = 0;
     if (libusb_get_configuration(stlinkhandle, &config)) {
         printf("Unable to get configuration\n");
     }

     if (config != 1) {
         printf("setting new configuration (%d -> 1)\n", config);
         if (libusb_set_configuration(stlinkhandle, 1)) {
             printf("Unable to set configuration\n");
         }
     }

     ret = libusb_claim_interface(stlinkhandle, 0);
     if (ret != 0) {
         printf("Unable to claim interface.\n");
         Cleanup();
         exit(ret);
     }

     requestTransfer = libusb_alloc_transfer(0);
     if (requestTransfer == NULL) {
         printf("Allocation of request transfer failed.\n");
         Cleanup();
         exit(ret);
     }

     responseTransfer = libusb_alloc_transfer(0);
     if (responseTransfer == NULL) {
         printf("Allocation of response transfer failed.\n");
         Cleanup();
         exit(ret);
     }

     responseTransfer->flags &= ~LIBUSB_TRANSFER_SHORT_NOT_OK;

     //============================
     // identify the microcontroller, set up the debugging and step through instructions to get the trace data
     //============================

     GetCurrentMode();
     GetVersion();

     //libusb_clear_halt(stlinkhandle, 0x81);

	 ExitDFUMode();

     if (GetCurrentMode() != MODE_DBG) {
    	 EnterSWD();
     }

     GetTargetVoltage();
     EnterDebugState();
     GetCoreId();

     ResetCore();
     ForceDebug();

     EnableTrace();
     RunCore();

     unsigned char checkCount = 0;
     while (1) {
    	 usleep(100);

		 unsigned int byteCount = FetchTraceByteCount();

		 if (byteCount == 0) continue;

		 if (byteCount > 2048) {
			 int toread = 0;
			 printf("**** more than 2048 bytes in trace data!! : 0x%4x ****\n", byteCount);

			 if ((byteCount & 0xF800) == 0xF800) {
				 //byteCount -= 0xF800;
				 printf("Detected overrun packet?/n");
				 //ReadTraceData(1, byteCount);
			     //ForceDebug();
				 //RunCore();	// run it - stalled?
				 //continue;
			 }
			 if (resultsFile != NULL) fprintf(resultsFile, "\n>>> BAD PACKET START: byteCount = 0x%04x <<<\n", byteCount);

			 while (byteCount > 0) {
				 toread = byteCount > 2048 ? 2048 : byteCount;
				 ReadTraceData(0, toread);
				 byteCount -= toread;

				 // check the register values
				 //unsigned int value =
				 ReadDHCSRValue();
				 //printf("DHCSR = 0x%04x\n", value);
			 }

		     ForceDebug();
			 RunCore();	// run it - stalled?

			 if (resultsFile != NULL) fprintf(resultsFile, "\n>>> BAD PACKET END <<<\n");

			 continue;
		 }

		 ReadTraceData(1, byteCount);

		 // check the stall status regularly
		 if (checkCount++ > 4) {
			 checkCount = 0;
			 unsigned int value = ReadDHCSRValue();
			 printf("DHCSR = 0x%04x\n", value);
		 }
     }

     //============================ will not get here, but if the code is ever changed to exit the loop, it should clean up below.

     ret = libusb_release_interface(stlinkhandle, 0);
     if (ret != 0) {
         printf("Unable to release interface.\n");
         Cleanup();
         exit(ret);
     }

     // finished - clean everything
     Cleanup();

     return 0;
}

void Cleanup()
{
     if (stlinkhandle != 0) libusb_close(stlinkhandle);
     if (ctx != 0) libusb_exit(ctx);
     if (deviceList != 0) libusb_free_device_list(deviceList, 1);
}

int IsStlink(libusb_device* dev)
{
     struct libusb_device_descriptor desc;

     int ret = libusb_get_device_descriptor(dev, &desc);
     if (ret < 0) {
         printf("Unable to get device descriptor/n");
         return 0;
     }

     if ((desc.idVendor != STLINKV2_VENDOR_ID) || (desc.idProduct != STLINKV2_PRODUCT_ID))
         return 0;

     printf("Found an ST-Link V2\n");
     printf("NumConfigurations: %d\n", desc.bNumConfigurations);
     printf("DeviceClass: 0x%02x\n", desc.bDeviceClass);
     printf("VendorID: 0x%04x\n", desc.idVendor);
     printf("ProductID: 0x%04x\n", desc.idProduct);

     struct libusb_config_descriptor *config;
     const struct libusb_interface *inter;
     const struct libusb_interface_descriptor *interdesc;
     const struct libusb_endpoint_descriptor *epdesc;

     libusb_get_config_descriptor(dev, 0, &config);
     printf("Interfaces: %d\n", config->bNumInterfaces);
     int i,j,k=0;
     for (i=0; i<(int)config->bNumInterfaces; i++) {
         inter = &config->interface[i];
         printf("Number of alternate settings: %d\n",
inter->num_altsetting);
         for (j=0; j<inter->num_altsetting; j++) {
             interdesc = &inter->altsetting[j];
             printf("Interface Number: %d\n", interdesc->bInterfaceNumber);
             printf("Number of endpoints: %d\n", interdesc->bNumEndpoints);
             for (k=0; k<interdesc->bNumEndpoints; k++) {
                 epdesc = &interdesc->endpoint[k];
                 printf("Descriptor Type: 0x%02x\n",
epdesc->bDescriptorType);
                 printf("EP Address: 0x%02x\n", epdesc->bEndpointAddress);
             }
         }
     }
     libusb_free_config_descriptor(config);
     return 1;
}

void Write32Bit(uint32_t address, uint32_t value)
{
	unsigned char txBuffer[] = {STLINK_DEBUG_COMMAND, WRITE32, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	// Note: this series of commands do not return data - so send only on the first two packets

	// address to write
	txBuffer[2] = (address & 0xFF);
	txBuffer[3] = ((address >> 8) & 0xFF);
	txBuffer[4] = ((address >> 16) & 0xFF);
	txBuffer[5] = ((address >> 24) & 0xFF);
	SendAndReceive(&txBuffer[0], 16, NULL, 0);

	// data to write
	unsigned char txValueBuffer[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	txValueBuffer[0] = (value & 0xFF);
	txValueBuffer[1] = ((value >> 8) & 0xFF);
	txValueBuffer[2] = ((value >> 16) & 0xFF);
	txValueBuffer[3] = ((value >> 24) & 0xFF);
	SendAndReceive(&txValueBuffer[0], 16, NULL, 0);

	UnknownCommand();
}

uint32_t Read32Bit(uint32_t address)
{
	unsigned char txBuffer[] = {STLINK_DEBUG_COMMAND, READ32, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer[100];
	uint32_t value = 0;

	// address to read
	txBuffer[2] = (address & 0xFF);
	txBuffer[3] = ((address >> 8) & 0xFF);
	txBuffer[4] = ((address >> 16) & 0xFF);
	txBuffer[5] = ((address >> 24) & 0xFF);
	SendAndReceive(&txBuffer[0], 16, rxBuffer, 64);

	value = (rxBuffer[3] << 24) | (rxBuffer[2] << 16) | (rxBuffer[1] << 8) | (rxBuffer[0] << 0);

	UnknownCommand();

	return value;
}

void UnknownCommand()
{
	unsigned char rxBuffer[100];

	// end of data packet?
	unsigned char txEndBuffer[] = {STLINK_DEBUG_COMMAND, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	SendAndReceive(&txEndBuffer[0], 16, &rxBuffer[0], 64);
}

uint32_t ReadDHCSRValue()
{
	return Read32Bit(0xE000EDF0);
}

/*
 * Enable the ITM trace functionality
 */
void EnableTrace()
{
	// set up the ITM
	EnterDebugState();
	HaltRunningSystem();
	LocalReset();

	// Set DHCSR to C_HALT and C_DEBUGEN
	Write32Bit(0xE000EDF0, 0xA05F0003);

	// Set TRCENA flag to enable global DWT and ITM
	Write32Bit(0xE000EDFC, 0x01000000);

	// Set FP_CTRL to enable write
	Write32Bit(0xE0002000, 0x00000002);

	// Set DWT_FUNCTION0 to DWT_FUNCTION3 to disable sampling
	Write32Bit(0xE0001028, 0x00000000);
	Write32Bit(0xE0001038, 0x00000000);
	Write32Bit(0xE0001048, 0x00000000);
	Write32Bit(0xE0001058, 0x00000000);

	// Clear DWT_CTRL and other registers
	Write32Bit(0xE0001000, 0x00000000);
	Write32Bit(0xE0001004, 0x00000000);
	Write32Bit(0xE0001008, 0x00000000);
	Write32Bit(0xE000100C, 0x00000000);
	Write32Bit(0xE0001010, 0x00000000);
	Write32Bit(0xE0001014, 0x00000000);
	Write32Bit(0xE0001018, 0x00000000);

	unsigned char txBuffer1[] = {STLINK_DEBUG_COMMAND, 0x33, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer1[100];
	SendAndReceive(&txBuffer1[0], 16, &rxBuffer1[0], 64);

	unsigned char txBuffer2[] = {STLINK_DEBUG_COMMAND, 0x33, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer2[100];
	SendAndReceive(&txBuffer2[0], 16, &rxBuffer2[0], 64);

	// Set DBGMCU_CR to enable asynchronous transmission
	Write32Bit(0xE0042004, 0x00000027);

	unsigned char txBuffer3[] = {STLINK_DEBUG_COMMAND, 0x40, 0x00, 0x10, 0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer3[100];
	SendAndReceive(&txBuffer3[0], 16, &rxBuffer3[0], 64);

	// Set TPIU_CSPSR to enable trace port width of 2
	Write32Bit(0xE0040004, 0x00000001);

	// Set TPIU_ACPR clock divisor
	Write32Bit(0xE0040010, CLOCK_DIVISOR);

	// Set TPIU_SPPR to Asynchronous SWO (NRZ)
	Write32Bit(0xE00400F0, 0x00000002);

	// Set TPIU_FFCR continuous formatting)
	Write32Bit(0xE0040304, 0x00000100);

	// Unlock the ITM registers for write
	Write32Bit(0xE0000FB0, 0xC5ACCE55);

	// Set ITM_TCR flags : ITMENA,SYNCENA,DWTENA, ATB=0
	Write32Bit(0xE0000E80, 0x0001000D);

	// Enable all trace ports in ITM_TER
	Write32Bit(0xE0000E00, 0xFFFFFFFF);

	// Enable trace ports 31:24 in ITM_TPR
	//Write32Bit(0xE0000E40, 0x00000008);
	Write32Bit(0xE0000E40, 0x0000000F);		// 8 was wrong?

	// Set DWT_CTRL flags)
	Write32Bit(0xE0000E40, 0x400003FE);		// Keil one

	// Enable tracing (DEMCR - TRCENA bit)
	Write32Bit(0xE000EDFC, 0x01000000);
}

/*
 * Resets the target board by setting a bit in the AIRCR
 */
void LocalReset()
{
	//F2 35 0C ED 00 E0 04 00 FA 05
	unsigned char txBuffer[] = {STLINK_DEBUG_COMMAND, WRITE_DATA, 0x0C, 0xED, 0x00, 0xE0, 0x04, 0x00, 0xFA, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer[100];
	SendAndReceive(&txBuffer[0], 16, &rxBuffer[0], 2);

	unsigned char txBufferWait[] = {STLINK_DEBUG_COMMAND, READ_DATA, 0x0C, 0xED, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	printf("Waiting for local reset\n");
	while (1) {
		int byteCount = SendAndReceive(&txBufferWait[0], 16, &rxBuffer[0], 64);
		if (byteCount > 0) {
			// reset successful?
			if ((rxBuffer[0] == 0x80) && (rxBuffer[4] == 0x00) && (rxBuffer[5] == 0x00)
					&& (rxBuffer[6] == 0x05) && (rxBuffer[7] == 0xFA)) break;
		}
	}

	printf("Local reset complete\n");
}

void ExitDFUMode()
{
    size_t txSize = 16;
    unsigned char txBuffer[] = {STLINK_DFU_COMMAND, STLINK_DFU_EXIT, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    // do not read anything for DFU exit command
    TransferData(0, (unsigned char*) &txBuffer, txSize, NULL, 0);
    printf("Exited DFU mode\n");
}

int GetCurrentMode()
{
    unsigned char rxBuffer[100];
    size_t rxSize = 2;
    int bytesRead = 0;
    unsigned char txBuffer[] = {0xF5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    size_t txSize = 16;

    bytesRead = TransferData(0, (unsigned char*) &txBuffer, txSize, (unsigned char*) &rxBuffer, rxSize);
    if (bytesRead > 0) {
        printf("Mode: 0x%02x 0x%02x\n",
       		 rxBuffer[0], rxBuffer[1]);
        return (rxBuffer[1]<<8) | rxBuffer[0];
    }
    else {
        printf("Unable to read mode\n");
    }

    return 0;
}

int SendAndReceive(unsigned char* txBuffer, size_t txSize, unsigned char* rxBuffer, size_t rxSize)
{
    return TransferData(0, txBuffer, txSize, rxBuffer, rxSize);
}

void EnterDebugState()
{
	unsigned char txBuffer[] = {STLINK_DEBUG_COMMAND, 0x35, 0xF0, 0xED, 0x00, 0xE0, 0x03, 0x00, 0x5F, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer[100];
	SendAndReceive(&txBuffer[0], 16, &rxBuffer[0], 64);
}

void ResetCore()
{
	unsigned char txBuffer[] = {STLINK_DEBUG_COMMAND, STLINK_DEBUG_RESETSYS, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer[100];
	SendAndReceive(&txBuffer[0], 16, &rxBuffer[0], 2);
}

void ForceDebug()
{
	unsigned char txBuffer[] = {STLINK_DEBUG_COMMAND, STLINK_DEBUG_FORCEDEBUG, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer[100];
	SendAndReceive(&txBuffer[0], 16, &rxBuffer[0], 2);
}

void HaltRunningSystem()
{
	unsigned char txBuffer[] = {STLINK_DEBUG_COMMAND, 0x35, 0xFC, 0xED, 0x00, 0xE0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer[100];
	SendAndReceive(&txBuffer[0], 16, &rxBuffer[0], 64);
}

void GetTargetVoltage()
{
	unsigned char txBuffer[] = {0xF7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char rxBuffer[100];
	int bytesRead = SendAndReceive(&txBuffer[0], 16, &rxBuffer[0],64);
    if (bytesRead > 0) {
        printf("Target Voltage: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
       		 rxBuffer[0], rxBuffer[1], rxBuffer[2], rxBuffer[3], rxBuffer[4], rxBuffer[5], rxBuffer[6], rxBuffer[7]);
    }
    else {
        printf("Unable to read target voltage\n");
    }
}

void GetVersion()
{
     size_t txSize = 16;
     unsigned char rxBuffer[100];
     size_t rxSize = 6;
     int bytesRead = 0;
     unsigned char txBuffer[] = {0xF1, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

     bytesRead = TransferData(1, (unsigned char*) &txBuffer, txSize, (unsigned char*) &rxBuffer, rxSize);
     if (bytesRead > 0) {
         printf("Version: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
        		 rxBuffer[0], rxBuffer[1], rxBuffer[2], rxBuffer[3], rxBuffer[4], rxBuffer[5]);
     }
     else {
         printf("Unable to read version\n");
     }
}

void GetCoreId()
{
     size_t txSize = 16;
     unsigned char rxBuffer[100];
     size_t rxSize = 64;
     int bytesRead = 0;
     unsigned char txBuffer[] = {DEBUG_COMMAND, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

     bytesRead = TransferData(1, (unsigned char*) &txBuffer, txSize, (unsigned char*) &rxBuffer, rxSize);
     if (bytesRead > 0) {
         uint32_t coreid = (rxBuffer[3] << 24) | (rxBuffer[2] << 16) | (rxBuffer[1] << 8) | (rxBuffer[0] << 0);
         printf("Core ID: 0x%08x\n", coreid);
         if (coreid == 0x1ba01477) printf("Cortex-M3 detected\n");
     }
     else {
         printf("Unable to read core ID\n");
     }
}

void EnterSWD()
{
     size_t txSize = 16;
     unsigned char rxBuffer[100];
     size_t rxSize = 64;
     int bytesRead = 0;
     unsigned char txBuffer[] = {DEBUG_COMMAND, 0x30, 0xA3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

     bytesRead = TransferData(1, (unsigned char*) &txBuffer, txSize, (unsigned char*) &rxBuffer, rxSize);
     if (bytesRead > 0) {
         printf("Switched to SWD\n");
     }
     else {
         printf("Error switching to SWD\n");
     }
}

void StepCore()
{
     size_t txSize = 16;
     unsigned char rxBuffer[100];
     size_t rxSize = 64;
     unsigned char txBuffer[] = {DEBUG_COMMAND, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

     TransferData(1, (unsigned char*) &txBuffer, txSize, (unsigned char*) &rxBuffer, rxSize);
}

void RunCore()
{
     size_t txSize = 16;
     unsigned char rxBuffer[100];
     size_t rxSize = 64;
     unsigned char txBuffer[] = {DEBUG_COMMAND, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

     int bytesRead = TransferData(1, (unsigned char*) &txBuffer, txSize, (unsigned char*) &rxBuffer, rxSize);
     if (bytesRead > 0) {
         printf("Running\n");
     }
     else {
         printf("Error running\n");
     }
}

int FetchTraceByteCount()
{
    size_t txSize = 16;
    unsigned char rxBuffer[100];
    size_t rxSize = 100;
    int bytesRead = 0;
    unsigned char txBuffer[] = {DEBUG_COMMAND, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int traceByteCount = 0;

    bytesRead = TransferData(1, (unsigned char*) &txBuffer, txSize, (unsigned char*) &rxBuffer, rxSize);
    if (bytesRead > 0) {
    	traceByteCount = rxBuffer[0]+(rxBuffer[1] << 8);  // original one - did not handle large packets
    }
    else {
        printf("Unable to step instruction\n");
    }

    if (debugEnabled) printf("trace bytes available: %d\n", traceByteCount);
    return traceByteCount;
}

// for trace
uint8_t trace_offset = 1;

int ReadTraceData(int toscreen, int rxSize)
{
	if (debugEnabled) printf("Reading %d bytes\n", (int)rxSize);

    unsigned char rxBuffer[2050];
    int bytesRead = 0;

#if ASYNC
    libusb_fill_bulk_transfer(
            responseTransfer,
            stlinkhandle,
            3 | LIBUSB_ENDPOINT_IN,
            (unsigned char*) &rxBuffer,
            rxSize,
            NULL,
            NULL,
            0);

    // no data?
    if (submit_wait(responseTransfer)) return;

    // have trace data - display it
    bytesRead = responseTransfer->actual_length;
    printf("Read response of %d bytes\n", bytesRead);
#else
    int ret = 0;
    int totalBytes = rxSize;

    while (totalBytes > 0) {

		ret = libusb_bulk_transfer(
					 stlinkhandle,
					 3 | LIBUSB_ENDPOINT_IN,
					 rxBuffer,
					 totalBytes,
					 &bytesRead,
					 0);
		printf("Read response %d of %d bytes. ret = %d\n", bytesRead, rxSize, ret);
		if (bytesRead != rxSize) {
			printf("\n\n>>>>>>>>>>>>>>>>> Not read all trace data. <<<<<<<<<<<<<<<<<<<<\n\n");
		}
		totalBytes -= bytesRead;

	#endif
		if (bytesRead > 0) {
			//TODO: re-factor my following junk code :)
			int pos = 0;
			unsigned char ch = ' ';

	#if HEXDUMP
			printf("Trace bytes read: %d\n", bytesRead);
			int width=16; //8;
			unsigned char line[9] = "\0";
			for (pos=0; pos < bytesRead; pos++) {
				ch = rxBuffer[pos];
				line[pos%width] = ((ch > 31) && (ch < 128)) ? ch : '.';
				line[(pos%width)+1] = '\0';
				if (toscreen) printf("%02x ", ch);
				if (pos%width > (width-2)) {
					if (toscreen) printf("  %s\n", line);
				}
			}
			if (pos%width != 0) {
				int p;
				for (p=0; p<width-(pos%width); p++) printf("   ");	//padding
				if (toscreen) printf("  %s\n\n", line);
			}
	#endif
			pos = 0;
			while (pos < bytesRead) {
				// assume 1 byte trace for now - only because this is what we are testing with!
				//packetSize = rxBuffer[pos];	// 1, 2 or 4 bytes
				ch = rxBuffer[pos];
				if (fullResultsFile != NULL) fprintf(fullResultsFile, "%c",ch);
				pos += 2;
			}

			pos = 0;
			trace_offset = 0;
			if (rxBuffer[0] == 0x01) {
				trace_offset = 1;
			}
			while (pos < bytesRead-trace_offset) {
				// assume 1 byte trace for now - only because this is what we are testing with!
				//packetSize = rxBuffer[pos];	// 1, 2 or 4 bytes
				ch = rxBuffer[pos+trace_offset];
				if (toscreen) {
					printf("%c",((ch < 31) | (ch > 127)) ? '.' : ch);
				}
				if (resultsFile != NULL) fprintf(resultsFile, "%c",ch);
				pos += 2;
			}

			//trace_offset = ((bytesRead+trace_offset) & 0x01);
			if (resultsFile != NULL) fflush(resultsFile);
			if (fullResultsFile != NULL) fflush(fullResultsFile);
		}
		else {
			printf("Unable to read trace data\n");
		}
    }

   	return bytesRead;
}

ssize_t TransferData(int terminate,
         unsigned char* transmitBuffer, size_t transmitLength,
         unsigned char* receiveBuffer, size_t receiveLength)
{
     int res = 0;

#if ASYNC
     libusb_fill_bulk_transfer(
             requestTransfer,
             stlinkhandle,
             2 | LIBUSB_ENDPOINT_OUT,
             transmitBuffer,
             transmitLength,
             NULL,
             NULL,
             0);

     if (debugEnabled) printf("TransferData - request\n");

     if (submit_wait(requestTransfer)) return -1;

     // response required?
     if (receiveBuffer != NULL) {
		 libusb_fill_bulk_transfer(
				 responseTransfer,
				 stlinkhandle,
				 1 | LIBUSB_ENDPOINT_IN,
				 receiveBuffer,
				 receiveLength,
				 NULL,
				 NULL,
				 0);

		 if (debugEnabled) printf("TransferData - response\n");

		 if (submit_wait(responseTransfer)) return -1;
		 res = responseTransfer->actual_length;
     }
#else
     int bytesTransferred = 0;
     int ret = 0;

     ret = libusb_bulk_transfer(
                  stlinkhandle,
                  2 | LIBUSB_ENDPOINT_OUT,
                  transmitBuffer,
                  transmitLength,
                  &bytesTransferred,
                  0);

     if (debugEnabled) printf("TransferData - request, %d of %d bytes written, ret = %d\n", bytesTransferred, (int)transmitLength, ret);
     if (bytesTransferred != transmitLength) {
         printf("\n\n>>>>>>>>>>>>>>>>> Not written all data. <<<<<<<<<<<<<<<<<<<<\n\n");
     }

     // response required?
     if (receiveBuffer != NULL) {
  		 ret = libusb_bulk_transfer(
				 stlinkhandle,
				 1 | LIBUSB_ENDPOINT_IN,
				 receiveBuffer,
				 receiveLength,
                 &bytesTransferred,
				 0);

		 if (debugEnabled) printf("TransferData - response, ret = %d\n", ret);
//	     if (bytesTransferred != receiveLength) {
//	         printf("\n\n>>>>>>>>>>>>>>>>> Not read all data. <<<<<<<<<<<<<<<<<<<<\n\n");
//	     }

		 res = bytesTransferred;
	  }
#endif

     return res;
}

#if ASYNC
struct trans_ctx {
#define TRANS_FLAGS_IS_DONE (1 << 0)
#define TRANS_FLAGS_HAS_ERROR (1 << 1)
     volatile unsigned long flags;
};

static void LIBUSB_CALL on_trans_done(struct libusb_transfer * trans) {
     struct trans_ctx * const ctx = trans->user_data;

     if (trans->status != LIBUSB_TRANSFER_COMPLETED) {
    	 if (trans->status != LIBUSB_TRANSFER_STALL) {
    		 ctx->flags |= TRANS_FLAGS_HAS_ERROR;
    	 }
     }

     ctx->flags |= TRANS_FLAGS_IS_DONE;
}

int submit_wait(struct libusb_transfer* trans)
{
     struct timeval start;
     struct timeval now;
     struct timeval diff;
     struct trans_ctx trans_ctx;
     enum libusb_error error;

     trans_ctx.flags = 0;

     /* brief intrusion inside the libusb interface */
     trans->callback = on_trans_done;
     trans->user_data = &trans_ctx;

     if ((error = libusb_submit_transfer(trans))) {
         printf("libusb_submit_transfer(%d)\n", error);
        	 return -1;
     }

     gettimeofday(&start, NULL);

     while (trans_ctx.flags == 0) {
    	 struct timeval timeout;
         timeout.tv_sec = 10;
         timeout.tv_usec = 0;
         if (libusb_handle_events_timeout(ctx, &timeout)) {
             printf("libusb_handle_events_timeout()\n");
             return -1;
         }

         gettimeofday(&now, NULL);
         timersub(&now, &start, &diff);
         if (diff.tv_sec >= 10) {
             printf("libusb_handle_events_timeout() timeout\n");
             return -1;
         }
     }

     if (trans_ctx.flags & TRANS_FLAGS_HAS_ERROR) {
         printf("libusb_handle_events_timeout() | has_error\n");
         return -1;
     }

     return 0;
}
#endif
