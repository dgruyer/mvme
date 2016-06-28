/***************************************************************************
 *   Copyright (C) 2014 by Gregor Montermann                               *
 *   g.montermann@mesytec.com                                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifndef VMUSB_H
#define VMUSB_H

#include <qobject.h>
#include <libxxusb.h>

class CVMUSBReadoutList;


/**
represents vm_usb controller

	@author Gregor Montermann <g.montermann@mesytec.com>
*/
class vmUsb : public QObject
{

public:
    vmUsb();

    ~vmUsb();
    void readAllRegisters(void);
    bool openUsbDevice(void);
    void closeUsbDevice(void);
    void getUsbDevices(void);
    void checkUsbDevices(void);

	int getFirmwareId();
	int getMode();
	int getDaqSettings();
	int getLedSources();
	int getDeviceSources();
	int getDggA();
	int getDggB();
	int getScalerAdata();
	int getScalerBdata();
	int getNumberMask();
	int getIrq(int vec);
	int getDggSettings();
	int getUsbSettings();
	
	int setFirmwareId(int val);
	int setMode(int val);
	int setDaqSettings(int val);
	int setLedSources(int val);
	int setDeviceSources(int val);
	int setDggA(int val);
	int setDggB(int val);
	int setScalerAdata(int val);
	int setScalerBdata(int val);
	int setNumberMask(int val);
	int setIrq(int vec, int val);
	int setDggSettings(int val);
	int setUsbSettings(int val);
    short vmeWrite32(long addr, long data);
    short vmeRead32(long addr, long* data);
	short vmeWrite16(long addr, long data);
	short vmeRead16(long addr, long* data);
    int vmeBltRead32(long addr, int count, quint32* data);
    int vmeMbltRead32(long addr, int count, quint32* data);
	void swap32(long* val);
	void swap16(long* val);
    int stackWrite(int id, long* data);
    int stackRead(int id, long* data);
    int stackExecute(long* data);
    int readBuffer(unsigned short* data);
    int readLongBuffer(int* data);
    int usbRegisterWrite(int addr, int value);
    void initialize();
    int setScalerTiming(unsigned int frequency, unsigned char period, unsigned char delay);
	void setEndianess(bool big);

    /* Executes the given stack (in the form of a readout list) and reads the
     * response into readBuffer. The actual number of bytes read is stored in
     * bytesRead. */
    int listExecute(CVMUSBReadoutList *list, void *readBuffer, size_t readBufferSize, size_t *bytesRead);


    /* Writes the given writePacket to the VM_USB and reads the response back into readPacket. */
    int transaction(void* writePacket, size_t writeSize,
                void* readPacket,  size_t readSize, int timeout_ms = 1000);
	
	xxusb_device_type pUsbDevice[5];
    char numDevices;
    usb_dev_handle* hUsbDevice;
    short ret;

protected:
	int firmwareId;
	int globalMode;
	int daqSettings;
	int ledSources;
	int deviceSources;
	int dggAsettings;
	int dggBsettings;
	int scalerAdata;
	int scalerBdata;
	int numberMask;
	int irqV[4];
	int extDggSettings;
	int usbBulkSetup;
    long int retval;
	bool bigendian;

};

// Constants:

// Identifying marks for the VM-usb:

static const short USB_WIENER_VENDOR_ID(0x16dc);
static const short USB_VMUSB_PRODUCT_ID(0xb);

// Bulk transfer endpoints

static const int ENDPOINT_OUT(2);
static const int ENDPOINT_IN(0x86);

// Timeouts:

static const int DEFAULT_TIMEOUT(2000);	// ms.

// Retries for flushing the fifo/stopping data taking:

static const int DRAIN_RETRIES(5);    // Retries.

// The register offsets:

static const unsigned int FIDRegister(0);       // Firmware id.
static const unsigned int GMODERegister(4);     // Global mode register.
static const unsigned int DAQSetRegister(8);    // DAQ settings register.
static const unsigned int LEDSrcRegister(0xc);	// LED source register.
static const unsigned int DEVSrcRegister(0x10);	// Device source register.
static const unsigned int DGGARegister(0x14);   // GDD A settings.
static const unsigned int DGGBRegister(0x18);   // GDD B settings.
static const unsigned int ScalerA(0x1c);        // Scaler A counter.
static const unsigned int ScalerB(0x20);        // Scaler B data.
static const unsigned int ExtractMask(0x24);    // CountExtract mask.
static const unsigned int ISV12(0x28);          // Interrupt 1/2 dispatch.
static const unsigned int ISV34(0x2c);          // Interrupt 3/4 dispatch.
static const unsigned int ISV56(0x30);          // Interrupt 5/6 dispatch.
static const unsigned int ISV78(0x34);          //  Interrupt 7/8 dispatch.
static const unsigned int DGGExtended(0x38);    // DGG Additional bits.
static const unsigned int USBSetup(0x3c);       // USB Bulk transfer setup. 
static const unsigned int USBVHIGH1(0x40);       // additional bits of some of the interrupt vectors.
static const unsigned int USBVHIGH2(0x44);       // additional bits of the other interrupt vectors.


// Bits in the list target address word:

static const uint16_t TAVcsID0(1); // Bit mask of Stack id bit 0.
static const uint16_t TAVcsSel(2); // Bit mask to select list dnload
static const uint16_t TAVcsWrite(4); // Write bitmask.
static const uint16_t TAVcsIMMED(8); // Target the VCS immediately.
static const uint16_t TAVcsID1(0x10);
static const uint16_t TAVcsID2(0x20);
static const uint16_t TAVcsID12MASK(0x30); // Mask for top 2 id bits
static const uint16_t TAVcsID12SHIFT(4);

uint16_t*
listToOutPacket(uint16_t ta, CVMUSBReadoutList* list,
                        size_t* outSize, off_t offset = 0);

#endif
