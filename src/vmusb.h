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

#include "util.h"
#include "vme_controller.h"
#include "vmusb_constants.h"

#include <QObject>
#include <QPair>
#include <QVector>
#include <QMutex>

class CVMUSBReadoutList;
struct usb_dev_handle;
struct usb_device;

struct vmusb_device_info
{
    usb_device *usbdev = nullptr;
    char serial[7] = {};
};

/*
 * Opening/closing and error handling:
 * - Close: leave daq mode if active, close and release the device
 * - Errors: timeout, no_device, other errors
 *   on timeout: close
 *   no_device: close
 *   other errors:
 */


/**
represents vm_usb controller

  @author Gregor Montermann <g.montermann@mesytec.com>
*/
class VMUSB: public VMEController
{
    Q_OBJECT
    signals:
        void daqModeEntered();
        void daqModeLeft();
        void daqModeChanged(bool);

    public:
        VMUSB();
        ~VMUSB();

        virtual bool isOpen() const override { return hUsbDevice; }
        virtual QString getIdentifyingString() const override;

        VMEError enterDaqMode();
        VMEError leaveDaqMode();
        bool isInDaqMode() const { return m_daqMode; }

        VMEError readRegister(u32 address, u32 *outValue);
        VMEError writeRegister(u32 address, u32 value);

        /* Write to the special action register. This is the only write
         * operation that works in autonomous daq mode. */
        VMEError writeActionRegister(uint16_t value);

        //void readAllRegisters(void);

        int getFirmwareId();
        int getMode();
        int getDaqSettings();
        int getLedSources();
        int getDeviceSources();
        int getDggA();
        int getDggB();
        int getScalerAdata();
        int getScalerBdata();
        u32 getEventsPerBuffer();
        uint16_t getIrq(int vec);
        int getDggSettings();
        int getUsbSettings();

        int setFirmwareId(int val);
        VMEError setMode(int val);
        VMEError setDaqSettings(int val);
        int setLedSources(int val);
        int setDeviceSources(int val);
        int setDggA(int val);
        int setDggB(int val);
        int setScalerAdata(int val);
        int setScalerBdata(int val);
        u32 setEventsPerBuffer(u32 val);
        VMEError setIrq(int vec, uint16_t val);
        int setDggSettings(int val);
        int setUsbSettings(int val);
#if 0
        short vmeWrite32(long addr, long data);
        short vmeWrite32(long addr, long data, uint8_t amod);
        short vmeRead32(long addr, long* data);
        short vmeWrite16(long addr, long data);
        short vmeWrite16(long addr, long data, uint8_t amod);
        short vmeRead16(long addr, long* data);
        int vmeBltRead32(long addr, int count, quint32* data);
        int vmeMbltRead32(long addr, int count, quint32* data);
        void swap32(long* val);
        void swap16(long* val);
#endif

        int setScalerTiming(unsigned int frequency, unsigned char period, unsigned char delay);

        /* Loads the given stack to stackID using the given memory offset. */
        VMEError listLoad(CVMUSBReadoutList *list, uint8_t stackID, size_t stackMemoryOffset, int timeout_ms = 1000);

        VMEError stackWrite(u8 stackNumber, u32 loadOffset, const QVector<u32> &stackData);
        // returns the stack words and the stack load offset
        VMEError stackRead(u8 stackNumber, QVector<u32> &stackOut, u32 &loadOffsetOut);
        VMEError stackExecute(const QVector<u32> &stackData, size_t resultMaxWords, QVector<u32> *dest);


        int bulkRead(void *outBuffer, size_t outBufferSize, int timeout_ms = 1000);

        VMEError tryErrorRecovery();

        //
        // VMEController interface
        //

        virtual VMEControllerType getType() const { return VMEControllerType::VMUSB; }

        virtual VMEError write32(u32 address, u32 value, u8 amod) override;
        virtual VMEError write16(u32 address, u16 value, u8 amod) override;

        virtual VMEError read32(u32 address, u32 *value, u8 amod) override;
        virtual VMEError read16(u32 address, u16 *value, u8 amod) override;

        virtual VMEError blockRead(u32 address, u32 transfers, QVector<u32> *dest, u8 amod, bool fifo) override;

        virtual VMEError openFirstDevice() override;
        virtual VMEError close() override;
        virtual ControllerState getState() const { return m_state; }

    private:
        /* Executes the given stack (in the form of a readout list) and reads the
         * response into readBuffer. The actual number of bytes read is stored in
         * bytesRead. */
        VMEError listExecute(CVMUSBReadoutList *list, void *readBuffer, size_t readBufferSize, size_t *bytesRead);

        /* Writes the given writePacket to the VM_USB and reads the response back into readPacket. */
        VMEError transaction(void* writePacket, size_t writeSize,
                void* readPacket,  size_t readSize, size_t *bytesRead, int timeout_ms = 1000);

        void getUsbDevices(void);

        QVector<vmusb_device_info> deviceInfos;
        usb_dev_handle* hUsbDevice = nullptr;

        int firmwareId;
        int globalMode;
        int daqSettings;
        int ledSources;
        int deviceSources;
        int dggAsettings;
        int dggBsettings;
        int scalerAdata;
        int scalerBdata;
        u32 eventsPerBuffer;
        int irqV[4];
        int extDggSettings;
        int usbBulkSetup;
        bool m_daqMode = false;
        QString m_currentSerialNumber;

        // timeout used for all operations except daq mode bulk transfers
        // TODO: use these everywhere
        size_t defaultTimeout_ms = 100;
        ControllerState m_state;
        QMutex m_lock;
};

static const int VMUSBBufferSize = 27 * 1024;

// Bulk transfer endpoints

static const int ENDPOINT_OUT(2);
static const int ENDPOINT_IN(0x86);

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

namespace TransferSetupRegister
{
    static const uint32_t multiBufferCountMask   = 0xff;
    static const uint32_t multiBufferCountShift  = 0;

    static const uint32_t timeoutMask            = 0xf00;
    static const uint32_t timeoutShift           = 8;
}

namespace ISVWord // half of a ISV register
{
    static const uint32_t stackIDShift  = 12;
    static const uint32_t irqLevelShift = 8;
}

namespace DaqSettingsRegister
{
    static const uint32_t ScalerReadoutFrequencyShift = 16;
    static const uint32_t ScalerReadoutFrequencyMask  = 0xffff0000;
    static const uint32_t ScalerReadoutPerdiodShift   = 8;
    static const uint32_t ScalerReadoutPerdiodMask    = 0x0000ff00;
    static const uint32_t ReadoutTriggerDelayShift    = 0;
    static const uint32_t ReadoutTriggerDelayMask     = 0x000000ff;
}

namespace GlobalModeRegister
{
    static const uint32_t MixedBufferShift = 5;
    static const uint32_t MixedBufferMask  = 0x00000020;
}

uint16_t*
listToOutPacket(uint16_t ta, CVMUSBReadoutList* list,
                        size_t* outSize, off_t offset = 0);


#endif
