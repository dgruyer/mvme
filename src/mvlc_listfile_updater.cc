#include <iostream>
#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <fmt/format.h>

#include <mesytec-mvlc/mesytec-mvlc.h>
#include "mvme_session.h"
#include "listfile_replay.h"
#include "mvlc/vmeconfig_to_crateconfig.h"
#include "mvme_mvlc_listfile.h"

using std::cerr;
using std::cout;
using std::endl;

using namespace mesytec;
using namespace mesytec::mvme;

class PeriodicEventTrimmer
{
    public:
        // Removes most of the periodic event1 data from the ReadoutBuffer.
        // Attempts to keep one event per second.
        // Expects the buffer to contain full readout frames or ethernet
        // packets only.
        void trimBuffer(mvlc::ReadoutBuffer &workBuffer);

    private:
        mvlc::ReadoutBuffer tmpBuffer = mvlc::ReadoutBuffer(Megabytes(1));
        // 1 s = 1e9 ns; divide by the 16ns timer period and subtract 1.
        // Skipping that many events will keep one per second.
        const size_t EventSkipCount = 1e9 / 16.0 - 1;
        const size_t currentSkipCount = 0;
};

void PeriodicEventTrimmer::trimBuffer(mvlc::ReadoutBuffer &workBuffer)
{
    using namespace mesytec::mvlc;

    tmpBuffer.clear();
    auto view = workBuffer.viewU32();

    auto copy_to_tmp_update_view = [this, &view] (const u32 *data, size_t size)
    {
        tmpBuffer.ensureFreeSpace(size);
        std::memcpy(tmpBuffer.data() + tmpBuffer.used(), data, size);
        tmpBuffer.use(size);
        view.remove_prefix(size);
    };

    while (!view.empty())
    {
        u32 header = view[0];

        if (get_frame_type(header) == frame_headers::SystemEvent)
        {
            copy_to_tmp_update_view(view.data(), 1u + extract_frame_info(header).len);
            continue;
        }

        assert(view.size() >= eth::HeaderWords);
        // This is going to get tricky...

        u32 header2 = view[1];
        eth::PayloadHeaderInfo ethHdrs{ header, header2 };
        size_t packetWords = eth::HeaderWords + ethHdrs.dataWordCount();

        if (view.size() >= packetWords)
        {
            if (ethHdrs.isNextHeaderPointerPresent())
            {
                u32 frameHeader = view[eth::HeaderWords + ethHdrs.nextHeaderPointer()];
                cout << fmt::format(
                    "frameHeader: 0x{:08X} @{}",
                    frameHeader, (void *)(view.data() + eth::HeaderWords + ethHdrs.nextHeaderPointer()) ) << endl;
                copy_to_tmp_update_view(view.data(), packetWords);
            }
            else // leave the packet intact ignoring frames cut at packet boundaries...
            {
                copy_to_tmp_update_view(view.data(), packetWords);
            }
        }
        else
        {
                copy_to_tmp_update_view(view.data(), view.size());
        }
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    mvme_init();

    if (argc != 2)
    {
        cerr << "Usage: " << argv[0] << " <inputListfile>" << endl;
        return 1;
    }

    QString inputFilename(argv[1]);
    QString outputFilename = QFileInfo(inputFilename).baseName() + "_updated.zip";

#define DO_CATCH 1

#if DO_CATCH
    try
#endif
    {
        // Open the input listfile, read the mvme VMEConfig from it and convert
        // it to an mvlc CrateConfig.
        auto listfileHandle = open_listfile(inputFilename);
        std::unique_ptr<VMEConfig> vmeConfig;

        std::error_code ec;
        std::tie(vmeConfig, ec) = read_vme_config_from_listfile(listfileHandle);
        if (ec) throw ec;

        auto mvlcCrateConfig = vmeconfig_to_crateconfig(vmeConfig.get());

        // Special handling for an out of control 16ns periodic readout in event1.
        bool trimPeriodicEvents = false;

        if (vmeConfig->getEventConfigs().size() > 1
            && vmeConfig->getEventConfig(1)->triggerCondition == TriggerCondition::Periodic)
        {
            cout << "Found event1 to be a periodic event. Activating trimming to a 1s frequency." << endl;
            trimPeriodicEvents = true;
        }

        // Reopen the input listfile using the mesytec::mvlc::ZipReader
        mvlc::listfile::ZipReader zipReader;
        zipReader.openArchive(listfileHandle.inputFilename.toStdString());
        auto readHandle = zipReader.openEntry(listfileHandle.listfileFilename.toStdString());

        // read the preamble and print out some details
        auto preamble = mvlc::listfile::read_preamble(*readHandle);

        cout << "preamble.magic=" << preamble.magic << endl;
        cout << "preamble.#systemEvents=" << preamble.systemEvents.size() << endl;

        for (const auto &sysEvent: preamble.systemEvents)
        {
            cout << "preamble.systemEvent.type="
                << mvlc::system_event_type_to_string(sysEvent.type) << endl;

            cout << "preamble.systemEvent.size(words)="
                << sysEvent.contents.size() / sizeof(u32) << endl;
        }

        // Create and open the output listfile
        cout << "Opening " << outputFilename.toStdString() << " for writing" << endl;
        mvlc::listfile::ZipCreator zipCreator;
        zipCreator.createArchive(outputFilename.toStdString());
        auto writeHandle = zipCreator.createLZ4Entry(
            listfileHandle.listfileFilename.toStdString());

        // Write the standard mvlc preamble followed by the mvme VMEConfig.
        mvlc::listfile::listfile_write_preamble(*writeHandle, mvlcCrateConfig);
        mvme_mvlc_listfile::listfile_write_mvme_config(*writeHandle, vmeConfig.get());

        mvlc::ReadoutBuffer workBuffer(Megabytes(1));
        mvlc::ReadoutBuffer previousData(workBuffer.capacity());

        struct Counters
        {
            size_t totalBytesRead = 0;
            size_t totalBytesWritten = 0;
        };

        Counters counters = {};
        PeriodicEventTrimmer trimmer;

        // Main loop copying data from readHandle to writeHandle.
        while (true)
        {
            if (previousData.used())
            {
                workBuffer.ensureFreeSpace(previousData.used());
                std::memcpy(workBuffer.data() + workBuffer.used(),
                            previousData.data(), previousData.used());
                workBuffer.use(previousData.used());
                previousData.clear();
            }

            size_t bytesRead = readHandle->read(
                workBuffer.data() + workBuffer.used(),
                workBuffer.free());
            workBuffer.use(bytesRead);

            if (bytesRead == 0)
                break;

            counters.totalBytesRead += bytesRead;

            // Buffer cleanup so that we do not have incomplete frames in the
            // workBuffer.
            mvlc::fixup_buffer(
                mvlcCrateConfig.connectionType,
                workBuffer, previousData);

            // Do any processing here.
            if (mvlcCrateConfig.connectionType == mvlc::ConnectionType::ETH)
            {
                trimmer.trimBuffer(workBuffer);
            }

            // Write to the output file
            counters.totalBytesWritten += writeHandle->write(
                workBuffer.data(), workBuffer.used());
            workBuffer.clear();
        }

        cout << "totalBytesRead=" << counters.totalBytesRead << endl;
        cout << "totalBytesWritten=" << counters.totalBytesWritten << endl;
    }
#if DO_CATCH
    catch (const QString &s)
    {
        cerr << "Caught an exception: " << s.toStdString() << endl;
        return 1;
    }
    catch (const std::error_code &ec)
    {
        cerr << "Caught an error_code: " << ec.message() << endl;
        return 1;
    }
    catch(const std::runtime_error &e)
    {
        cerr << "Caught an exception: " << e.what() << endl;
        return 1;
    }
#endif

    mvme_shutdown();
    return 0;

}
