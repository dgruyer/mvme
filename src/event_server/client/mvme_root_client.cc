/* TODO:
 * - license comment
 * - description of what this program does
 * - add return codes to the event handler functions or replace do_exit with a
 *   return code value. This should be done so that main can exit with a
 *   non-zero code in case of errors.
 * - replace the asserts with actual error handling code + messages
* user objects:
  generate on each beginRun. compare with on disk verison.
  if differences:
      if not loaded yet: run make and load
      else: warn and tell user to restart FIXME: exit or continue running?

* filename + output directory + flag about overwriting
* raw histos?
* replay handling from file?
// FIXME: use return code and GetLastErrorString() or something similar in the
// context message handlers.
// Maybe change the prototypes to return int so that each user specific
// subclass of Parser can decide what to do. (control is finally returned to the user
// when handleMessage returns).
 * - Should the output event tree be filled before or after calling the
 *   analysis event handler function? After could allow the user to modify the
 *   data that's going to be stored...
 * FIXME: eventNumber
 * randomness: try pcg instead of ROOTs gRandom
 *
 * Do not overwrite files that should not be overwritten, diff other files.
 *
 */

#include <fstream>
#include <string>

#include <getopt.h>
#include <signal.h>
#include <dlfcn.h> // dlopen, dlsym, dlclose

// ROOT
#include <TFile.h>
#include <TRandom.h> // gRandom
#include <TROOT.h> // gROOT
#include <TSystem.h> // gSystem

// mvme
#include <Mustache/mustache.hpp>
#include "common/event_server_lib.h"
#include "mvme_root_event_objects.h"

using std::cerr;
using std::cout;
using std::endl;

namespace mu = kainjow::mustache;
using namespace mvme::event_server;

// The c++11 way of including text strings into the binary. Uses the new R"()"
// raw string syntax and the preprocessor to embed string data.

static const char *exportHeaderTemplate =
#include "templates/user_objects.h.mustache"
;

static const char *exportImplTemplate =
#include "templates/user_objects.cxx.mustache"
;

static const char *exportLinkDefTemplate =
#include "templates/user_objects_LinkDef.h.mustache"
;

static const char *analysisImplTemplate =
#include "templates/analysis.cxx.mustache"
;

static const char *analysisMkTemplate =
#include "templates/analysis.mk.mustache"
;

static const char *makefileTemplate =
#include "templates/Makefile.mustache"
;

// Analysis
struct UserAnalysis
{
    using InitFunc      = bool (*)(const std::vector<std::string> &args);
    using ShutdownFunc  = bool (*)();
    using BeginRunFunc  = bool (*)(const std::string &inputSource, const std::string &runId,
                                   bool isReplay);
    using EndRunFunc    = bool (*)();
    using EventFunc     = bool (*)(const MVMEEvent *event);

    InitFunc init;
    ShutdownFunc shutdown;
    BeginRunFunc beginRun;
    EndRunFunc endRun;
    // Per event analysis functions ordered by event index (same order as
    // MVMEExperiment::GetEvents())
    std::vector<EventFunc> eventFunctions;
};

//
// ClientContext
//
struct Options
{
    using Opt_t = unsigned;
    static const Opt_t ConvertNaNsToZero = 1u << 0;
    static const Opt_t ShowStreamInfo = 1u << 1;
    static const Opt_t VerboseMacroLoad = 1u << 2;
};

class ClientContext: public mvme::event_server::Parser
{
    public:
        struct RunStats
        {
            using ClockType = std::chrono::high_resolution_clock;
            ClockType::time_point tStart;
            ClockType::time_point tEnd;
            size_t totalDataBytes = 0;
            std::vector<size_t> eventHits;
        };

        ClientContext(const std::string &outputDirectory, const Options::Opt_t &options)
            : m_outputDirectory(outputDirectory)
            , m_options(options)
        { }

        RunStats GetRunStats() const { return m_stats; }
        bool ShouldQuit() const { return m_quit; }
        void setHostAndPort(const std::string &host, const std::string &port)
        {
            m_host = host;
            m_port = port;
        }

    protected:
        virtual void serverInfo(const Message &msg, const json &info) override;
        virtual void beginRun(const Message &msg, const StreamInfo &streamInfo) override;

        virtual void eventData(const Message &msg, int eventIndex,
                                const std::vector<DataSourceContents> &contents) override;

        virtual void endRun(const Message &msg, const json &info) override;

        virtual void error(const Message &msg, const std::exception &e) override;

    private:
        std::string m_outputDirectory;
        Options::Opt_t m_options;
        std::unique_ptr<MVMEExperiment> m_exp;
        std::unique_ptr<TFile> m_outFile;
        std::vector<TTree *> m_eventTrees;
        RunStats m_stats;
        bool m_quit = false;
        bool m_codeGeneratedAndLoaded = false;
        void *m_analysisDLHandle = nullptr;
        UserAnalysis m_analysis = {};
        std::string m_host;
        std::string m_port;
};

void ClientContext::serverInfo(const Message &msg, const json &info)
{
    cout << "serverInfo:" << endl << info.dump(2) << endl;
}

static mu::data build_event_template_data(const StreamInfo &streamInfo)
{
    mu::data mu_vmeEvents = mu::data::type::list;

    for (const auto &event: streamInfo.vmeTree.events)
    {
        mu::data mu_vmeModules = mu::data::type::list;

        for (const auto &module: event.modules)
        {
            mu::data mu_moduleDataMembers = mu::data::type::list;
            mu::data mu_moduleRefMembers = mu::data::type::list;

            int dsIndex = 0;
            for (const auto &edd: streamInfo.eventDataDescriptions)
            {
                if (edd.eventIndex != event.eventIndex) continue;

                for (const auto &dsd: edd.dataSources)
                {
                    if (dsd.moduleIndex != module.moduleIndex) continue;

                    mu::data mu_dataMember = mu::data::type::object;
                    mu_dataMember["name"] = dsd.name;
                    mu_dataMember["size"] = std::to_string(dsd.size);
                    mu_dataMember["dsIndex"] = std::to_string(dsIndex);

                    mu_moduleDataMembers.push_back(mu_dataMember);

                    size_t paramCount = std::min(dsd.paramNames.size(),
                                                 static_cast<size_t>(dsd.size));

                    for (size_t paramIndex = 0;
                         paramIndex < paramCount;
                         paramIndex++)
                    {
                        mu::data mu_refMember = mu::data::type::object;
                        mu_refMember["name"] = dsd.paramNames[paramIndex];
                        mu_refMember["index"] = std::to_string(paramIndex);
                        mu_refMember["target"] = dsd.name;

                        mu_moduleRefMembers.push_back(mu_refMember);
                    }
                }
                dsIndex++;
            }

            mu::data mu_module = mu::data::type::object;
            mu_module["struct_name"] = "Module_" + module.name;
            mu_module["name"] = module.name;
            mu_module["title"] = "Module " + module.name;
            mu_module["var_name"] = module.name;
            mu_module["data_members"] = mu::data{mu_moduleDataMembers};
            mu_module["ref_members"] = mu::data{mu_moduleRefMembers};
            mu_module["event_name"] = event.name;
            mu_vmeModules.push_back(mu_module);
        }

        mu::data mu_event = mu::data::type::object;
        mu_event["struct_name"] = "Event_" + event.name;
        mu_event["title"] = "Storage for event '" + event.name + "'";
        mu_event["name"] = event.name;
        mu_event["var_name"] = event.name;
        mu_event["modules"] = mu::data{mu_vmeModules};
        mu_vmeEvents.push_back(mu_event);
    }

    return mu_vmeEvents;
}

template<typename T>
T load_sym(void *handle, const char *name)
{
    return reinterpret_cast<T>(dlsym(handle, name));
}

void ClientContext::beginRun(const Message &msg, const StreamInfo &streamInfo)
{
    if (m_options & Options::ShowStreamInfo)
    {
        cout << "Incoming BeginRun Stream Information:" << endl
            << streamInfo.infoJson.dump(2) << endl;
    }

    cout << __FUNCTION__ << ": runId=" << streamInfo.runId
        << endl;

    std::string expName = streamInfo.infoJson["ExperimentName"];
    std::string expTitle = streamInfo.infoJson["ExperimentTitle"];

    if (!m_codeGeneratedAndLoaded)
    {
        cout << __FUNCTION__
            << ": generating ROOT classes for experiment " << expName << endl;

        std::string headerFilename = expName + "_mvme.h";
        std::string headerFilepath = m_outputDirectory + "/" + headerFilename;
        std::string implFilename = expName + "_mvme.cxx";
        std::string implFilepath = m_outputDirectory + "/" + implFilename;
        std::string linkdefFilename = expName + "_mvme_LinkDef.h";
        std::string linkdefFilepath = m_outputDirectory + "/" + linkdefFilename;
        std::string analysisFilename = "analysis.cxx";
        std::string analysisFilepath = m_outputDirectory + "/" + analysisFilename;
        std::string makefileFilename = "Makefile";
        std::string makefileFilepath = m_outputDirectory + "/" + makefileFilename;
        std::string analysisMkFilename = "analysis.mk";
        std::string analysisMkFilepath = m_outputDirectory + "/" + analysisMkFilename;

        mu::data mu_vmeEvents = build_event_template_data(streamInfo);

        // build the final template data object
        mu::data mu_data;
        mu_data["vme_events"] = mu::data{mu_vmeEvents};
        mu_data["exp_name"] = expName;
        std::string experimentStructName = expName;
        mu_data["exp_struct_name"] = experimentStructName;
        mu_data["exp_title"] = expTitle;
        mu_data["header_guard"] = expName;
        mu_data["header_filename"] = headerFilename;
        mu_data["impl_filename"] = implFilename;

        // Create files
        {
            auto do_render = [](const mu::data &mu_data,
                                const std::string &templateFile,
                                const std::string &outFile)
            {
                mu::mustache tmpl(templateFile);
                std::string rendered = tmpl.render(mu_data);
                std::ofstream out(outFile);
                out << rendered;
            };

            cout << "Writing experiment header file " << headerFilepath << endl;
            do_render(mu_data, exportHeaderTemplate, headerFilepath);

            cout << "Writing experiment implementation file " << implFilepath << endl;
            do_render(mu_data, exportImplTemplate, implFilepath);

            cout << "Writing experiment linkdef file " << linkdefFilepath << endl;
            do_render(mu_data, exportLinkDefTemplate, linkdefFilepath);

            cout << "Writing skeleton analysis file " << analysisFilepath << endl;
            do_render(mu_data, analysisImplTemplate, analysisFilepath);

            cout << "Writing analysis customization Makefile " << analysisMkFilepath << endl;
            do_render(mu_data, analysisMkTemplate, analysisMkFilepath);

            cout << "Writing Makefile" << endl;
            do_render(mu_data, makefileTemplate, makefileFilepath);
        }

        // Run make
        {
            cout << "Running make" << endl;
            int res = gSystem->Exec("make");

            if (res != 0)
            {
                m_quit = true;
                return;
            }
        }

        // Load experiment library
        {
            std::string libName = "lib" + expName + "_mvme.so";
            cout << "Loading experiment library " << libName << endl;
            int res = gSystem->Load(libName.c_str());

            cout << "res=" << res << endl;

            if (res != 0 && res != 1)
            {
                cout << "Error loading experiment library " << libName << endl;
                m_quit = true;
                return;
            }
        }

        // Create an instance of the generated experiment class
        std::string cmd = "new " + experimentStructName + "();";
        m_exp = std::unique_ptr<MVMEExperiment>(reinterpret_cast<MVMEExperiment *>(
                gROOT->ProcessLineSync(cmd.c_str())));

        if (!m_exp)
        {
            cout << "Error creating experiment specific class '"
                << experimentStructName << "'" << endl;
            m_outFile = {};
            m_eventTrees = {};
            m_quit = true;
            return;
        }

        if (streamInfo.eventDataDescriptions.size() != m_exp->GetNumberOfEvents())
        {
            cout << "Error: number of Event definitions declared in StreamInfo does not equal "
                "the number of Event classes present in the generated Experiment code."
                << endl
                << "Please restart the client to regenerate the code."
                << endl;
            m_quit = true;
            return;
        }

        for (size_t eventIndex = 0; eventIndex < m_exp->GetNumberOfEvents(); eventIndex++)
        {
            auto &edd = streamInfo.eventDataDescriptions.at(eventIndex);
            auto event = m_exp->GetEvent(eventIndex);

            if (edd.dataSources.size() != event->GetDataSourceStorages().size())
            {
                cout << "Warning: eventIndex=" << eventIndex << ", eventName=" << event->GetName()
                    << ": number of data sources in the StreamInfo and in the generated Event class "
                    " differ (streamInfo:" << edd.dataSources.size()
                    << ", class:" << event->GetDataSourceStorages().size() << ")."
                    << endl
                    << "Please run `make' and restart the client."
                    << endl;
                m_quit = true;
                return;
            }
        }

#if 0   // TODO: test unloading
        // Unload analysis
        if (m_analysisDLHandle)
        {
            if (m_analysis.shutdown)
                m_analysis.shutdown();
            dlclose(m_analysisDLHandle);
            m_analysisDLHandle = nullptr;
            m_analysis = {};
        }
#endif

        // Load analysis
        if (!m_analysisDLHandle)
        {
            cout << "Loading analysis.so" << endl;
            void *handle = dlopen("analysis.so", RTLD_NOW | RTLD_GLOBAL);
            if (!handle)
            {
                cout << "Error loading analysis.so: " << dlerror() << endl;
                m_quit = true;
                return;
            }

            m_analysisDLHandle = handle;
            m_analysis = {};

            m_analysis.init = load_sym<UserAnalysis::InitFunc>(handle, "init_analysis");
            m_analysis.shutdown = load_sym<UserAnalysis::ShutdownFunc>(handle, "shutdown_analysis");
            m_analysis.beginRun = load_sym<UserAnalysis::BeginRunFunc>(handle, "begin_run");
            m_analysis.endRun = load_sym<UserAnalysis::EndRunFunc>(handle, "end_run");

            for (auto &event: m_exp->GetEvents())
            {
                auto fname = std::string("analyze_") + event->GetName();
                auto func = load_sym<UserAnalysis::EventFunc>(handle, fname.c_str());
                m_analysis.eventFunctions.push_back(func);
            }
        }

        m_codeGeneratedAndLoaded = true;

        if (m_analysis.init)
        {
            bool res = m_analysis.init({ "FIXME", "pass", "some", "args", "here"});

            if (!res)
            {
                cout << "Analysis init function returned false, aborting" << endl;
                m_quit = true;
                return;
            }
        }
    }
    else
    {
        cout << __FUNCTION__
            << ": Reusing previously loaded experiment and analysis code." << endl;
    }

    // generate output filename for event tree output and open the file
    std::string filename;

    if (streamInfo.runId.empty())
    {
        cout << __FUNCTION__ << ": Warning: got an empty runId!" << endl;
        filename = "unknown_run.root";
    }
    else
    {
        filename = streamInfo.runId + ".root";
    }

    cout << "Opening output file " << filename << endl;
    m_outFile = std::make_unique<TFile>(filename.c_str(), "recreate");

    if (m_outFile->IsZombie() || !m_outFile->IsOpen())
    {
        cout << "Error opening output file " << filename << " for writing: "
            << strerror(m_outFile->GetErrno()) << endl;
        m_quit = true;
        return;
    }

    cout << "Creating output trees" << endl;
    m_eventTrees = m_exp->MakeTrees();
    for (auto &tree: m_eventTrees)
    {
        assert(tree);
        cout << "  " << tree << " " << tree->GetName() << "\t" << tree->GetTitle() << endl;
    }
    assert(m_eventTrees.size() == m_exp->GetNumberOfEvents());

    // call custom user analysis code
    if (m_analysis.beginRun)
    {
        m_analysis.beginRun("mvme://" + m_host + ":" + m_port,
                            streamInfo.runId, streamInfo.isReplay);
    }

    m_stats = {};
    m_stats.eventHits = std::vector<size_t>(streamInfo.eventDataDescriptions.size());
    m_stats.tStart = RunStats::ClockType::now();

    cout << "BeginRun procedure done, receiving data..." << endl;
}

void ClientContext::eventData(const Message &msg, int eventIndex,
                        const std::vector<DataSourceContents> &contents)
{
    // Streaminfo received with the BeginRun message
    auto &streamInfo = getStreamInfo();

    assert(0 <= eventIndex && static_cast<size_t>(eventIndex) < m_eventTrees.size());
    assert(streamInfo.eventDataDescriptions.size() == m_eventTrees.size());

    if (!m_exp)
    {
        cout << "Error in " << __FUNCTION__
            << ": no MVMEExperiment instance was created" << endl;
        m_quit = true;
        return;
    }

    auto event = m_exp->GetEvent(eventIndex);

    if (!event)
    {
        cout << "Error in " << __FUNCTION__ << ": eventIndex "
            << eventIndex << " out of range" << endl;
        m_quit = true;
        return;
    }

    auto &edd = streamInfo.eventDataDescriptions.at(eventIndex);
#if 0
    assert(edd.eventIndex == eventIndex);
    cout << eventIndex << endl;
    cout << edd.dataSources.size() << endl;
    cout << event->GetDataSourceStorages().size() << endl;

    assert(edd.dataSources.size() == event->GetDataSourceStorages().size());
    assert(contents.size() == edd.dataSources.size());
#endif

    m_stats.eventHits[eventIndex]++;

    // Copy incoming data into the data members of the generated classes
    for (size_t dsIndex = 0; dsIndex < contents.size(); dsIndex++)
    {
        const DataSourceContents &dsc = contents.at(dsIndex);
        const uint8_t *dscEnd = get_end_pointer(dsc);

        // Pointer into the generated array member of the module class. This is
        // where the incoming data will be written to.
        auto userStorage = event->GetDataSourceStorage(dsIndex);
        assert(userStorage.ptr);
        assert(userStorage.size = edd.dataSources.at(dsIndex).size);

        for (auto entryIndex = 0; entryIndex < dsc.count; entryIndex++)
        {
            const uint8_t *indexPtr = dsc.firstIndex + entryIndex * get_entry_size(dsc);
            const uint8_t *valuePtr = indexPtr + get_storage_type_size(dsc.indexType);

            if (indexPtr >= dscEnd || valuePtr >= dscEnd)
            {
                cout << "Error: incoming data source contents are inconsistent: buffer size exceeded."
                    << " eventIndex=" << eventIndex
                    << ", dataSourceIndex=" << dsIndex
                    << ", entryIndex=" << entryIndex
                    << endl;
                m_quit = true;
                return;
            }

            uint32_t index = read_storage<uint32_t>(dsc.indexType, indexPtr);
            double value = read_storage<double>(dsc.valueType, valuePtr);

            // Add a random in (0, 1) to avoid binning issues.
            //value += gRandom->Uniform();

            // Perform the copy
            if (index < userStorage.size)
            {
                userStorage.ptr[index] = value;
            }
            else
            {
                cout << "Error: index value " << index << " out of range."
                    << " eventIndex=" << eventIndex
                    << ", dataSourceIndex=" << dsIndex
                    << ", entryIndex=" << entryIndex
                    << ", userStorage.size=" << userStorage.size
                    << endl;
                m_quit = true;
                return;
            }
        }

        size_t bytes = get_entry_size(dsc) * dsc.count;
        m_stats.totalDataBytes += bytes;
    }

    // At this point the event storages have been filled with incoming data.
    // Now fill the tree for this event and run the analysis code.
    m_eventTrees.at(eventIndex)->Fill();

    auto eventFunc = m_analysis.eventFunctions.at(eventIndex);

    if (eventFunc)
    {
        eventFunc(event);
    }
}

void ClientContext::endRun(const Message &msg, const json &info)
{
    cout << __FUNCTION__ << ": endRun info:" << endl << info.dump(2) << endl;

    if (m_analysis.endRun)
        m_analysis.endRun();

    if (m_outFile)
    {
        cout << "  Writing additional info to output file..." << endl;

        std::map<std::string, std::string> info;

        info["ExperimentName"] = m_exp->GetName();
        info["RunID"] = getStreamInfo().runId;
        // TODO: store analysis efficiency data this needs to be transferred
        // with the endRun message.
        // TODO: use a TDirectory to hold mvme stuff
        // also try a TMap to hold either TStrings or more TMaps
        // figure out how freeing that memory then works
        m_outFile->WriteObject(&info, "MVMERunInfo");

        cout << "  Closing output file " << m_outFile->GetName() << "..." << endl;
        m_outFile->Write();
        m_outFile.release();
    }

    cout << "  HitCounts by event:" << endl;
    for (size_t ei=0; ei < m_stats.eventHits.size(); ei++)
    {
        cout << "    ei=" << ei << ", hits=" << m_stats.eventHits[ei] << endl;
    }

    cout << endl;

    m_stats.tEnd = RunStats::ClockType::now();
    std::chrono::duration<float> elapsed = m_stats.tEnd - m_stats.tStart;
    float elapsed_s = elapsed.count();
    float bytesPerSecond = m_stats.totalDataBytes / elapsed_s;
    float MBPerSecond = bytesPerSecond / (1024 * 1024);

    cout << " duration: " << elapsed_s << "s" << endl;

    cout << " data: "
        << m_stats.totalDataBytes << " bytes, "
        << m_stats.totalDataBytes / (1024 * 1024.0) << " MB"
        << endl;
    cout << " rate: "
        << bytesPerSecond << " B/s, "
        << MBPerSecond << " MB/s"
        << endl;
}

void ClientContext::error(const Message &msg, const std::exception &e)
{
    cout << "A protocol error occured: " << e.what() << endl;

    if (m_outFile)
    {
        cout << "Closing output file " << m_outFile->GetName() << "..." << endl;
        m_outFile.release();
    }

    m_quit = true;
}

static bool signal_received = false;

void signal_handler(int signum)
{
    cout << "signal " << signum << endl;
    cout.flush();
    signal_received = true;
}

void setup_signal_handlers()
{
    /* Set up the structure to specify the new action. */
    struct sigaction new_action;
    new_action.sa_handler = signal_handler;
    sigemptyset (&new_action.sa_mask);
    new_action.sa_flags = 0;

    for (auto signum: { SIGINT, SIGHUP, SIGTERM })
    {
        if (sigaction(signum, &new_action, NULL) != 0)
            throw std::system_error(errno, std::generic_category(), "setup_signal_handlers");
    }
}

//
// main
//
int main(int argc, char *argv[])
{
/*
    [--host=localhost]
    [--port=13801]

    Parse options, if remaining args switch to replay mode and use remaining args
    as names of root files.

    Pass all args after the separating '--' to the analysis init function.
    How does the init function know what we're doing? Life run or replay?

*/

#if 1
    // host, port, quit after one run?,
    // output filename? if not specified is taken from the runId
    // send out a reply is response to the EndRun message?
    std::string host = "localhost";
    std::string port = "13801";
    std::string outputDirectory = ".";
    bool singleRun = false;
    bool showHelp = false;

    using Opts = Options;
    Opts::Opt_t clientOpts = {};

    while (true)
    {
        static struct option long_options[] =
        {
            { "single-run", no_argument, nullptr, 0 },
            { "convert-nans", no_argument, nullptr, 0 },
            { "output-directory", required_argument, nullptr, 0 },
            { "show-stream-info", no_argument, nullptr, 0 },
            { "verbose-macro-load", no_argument, nullptr, 0 },
            { "host", no_argument, nullptr, 0 },
            { "port", no_argument, nullptr, 0 },
            { "help", no_argument, nullptr, 0 },
            { nullptr, 0, nullptr, 0 },
        };

        int option_index = 0;
        int c = getopt_long(argc, argv, "o:", long_options, &option_index);

        if (c == -1) break;

        switch (c)
        {
            case '?':
                // Unrecognized option
                return 1;
            case 'o':
                outputDirectory = optarg;
                break;

            case 0:
                // long options
                {
                    std::string opt_name(long_options[option_index].name);

                    if (opt_name == "single-run") singleRun = true;
                    else if (opt_name == "convert-nans") clientOpts |= Opts::ConvertNaNsToZero;
                    else if (opt_name == "output-directory") outputDirectory = optarg;
                    else if (opt_name == "show-stream-info") clientOpts |= Opts::ShowStreamInfo;
                    else if (opt_name == "verbose-macro-load") clientOpts |= Opts::VerboseMacroLoad;
                    else if (opt_name == "host") host = optarg;
                    else if (opt_name == "port") port = optarg;
                    else if (opt_name == "help") showHelp = true;
                    else
                    {
                        assert(!"unhandled long option");
                    }
                }
        }
    }

    if (showHelp)
    {
#if 0
        cout << "Usage: " << argv[0]
            << " [--single-run] [--convert-nans] [-o|--output-directory <dir>]"
               " [host=localhost] [port=13801]"
            << endl << endl
            ;

        cout << "  If single-run is set the process will exit after receiving" << endl
             << "  data from one run. Otherwise it will wait for the next run to" << endl
             << "  start." << endl
             << endl
             << "  If convert-nans is set incoming NaN data values will be" << endl
             << "  converted to 0.0 before they are written to their respective ROOT" << endl
             << "  tree Branch." << endl
             << endl
             ;

        return 0;
#else
        cout << "TODO: Write a help text!" << endl;
        return 0;
#endif
    }
#endif

    setup_signal_handlers();

    if (int res = mvme::event_server::lib_init() != 0)
    {
        cerr << "mvme::event_server::lib_init() failed with code " << res << endl;
        return 1;
    }

    ClientContext ctx(outputDirectory, clientOpts);

    // A single message object, whose buffer is reused for each incoming
    // message.
    Message msg;
    int sockfd = -1;
    int retval = 0;
    bool doQuit = false;

    while (!doQuit && !signal_received)
    {
        if (sockfd < 0)
        {
            cout << "Connecting to " << host << ":" << port << " ..." << endl;
        }

        // auto reconnect loop until connected or a signal arrived
        while (sockfd < 0 && !signal_received)
        {
            try
            {
                sockfd = connect_to(host.c_str(), port.c_str());
            }
            catch (const mvme::event_server::exception &e)
            {
                sockfd = -1;
            }

            if (sockfd >= 0)
            {
                cout << "Connected to " << host << ":" << port << endl;
                ctx.setHostAndPort(host, port);
                break;
            }

            if (usleep(1000 * 1000) != 0 && errno != EINTR)
            {
                throw std::system_error(errno, std::generic_category(), "usleep");
            }
        }

        if (signal_received) break;

        try
        {
            read_message(sockfd, msg);
            ctx.handleMessage(msg);

            if (singleRun && msg.type == MessageType::EndRun)
            {
                doQuit = true;
            }
            else
            {
                doQuit = ctx.ShouldQuit();
            }
        }
        catch (const mvme::event_server::connection_closed &)
        {
            cout << "Error: The remote host closed the connection." << endl;
            sockfd = -1;
            // Reset context state as we're going to attempt to reconnect.
            ctx.reset();
        }
        catch (const mvme::event_server::exception &e)
        {
            cout << "An error occured: " << e.what() << endl;
            retval = 1;
            break;
        }
        catch (const std::system_error &e)
        {
            cout << "Disconnected from " << host << ":" << port
                << ", reason: " << e.what() << endl;
            sockfd = -1;
            retval = 1;
            break;
        }
    }

    mvme::event_server::lib_shutdown();
    return retval;
}

// Previous code that uses ACLIC to compile and load the generated code
// on the fly.
#if 0
    // Using TROOT::LoadMacro() to compile and immediately load the generated
    // code, then create the project specific MVMEExperiment subclass.
    {

        if (!m_codeGeneratedAndLoaded)
        {
            std::string cmd = implFilepath + "+";

            if (m_options & Options::VerboseMacroLoad)
            {
                cmd += "v";
            }
            cout << "LoadMacro " + cmd << endl;
            int error = 0;
            auto res = gROOT->LoadMacro(cmd.c_str(), &error);
            cout << "res=" << res << ", error=" << error << endl;
        }

        std::string cmd = "new " + experimentStructName + "();";
        m_exp = std::unique_ptr<MVMEExperiment>(reinterpret_cast<MVMEExperiment *>(
                gROOT->ProcessLineSync(cmd.c_str())));

        if (!m_exp)
        {
            cout << "Error creating experiment specific class '"
                << experimentStructName << "'" << endl;
            m_outFile = {};
            m_eventTrees = {};
            m_quit = true;
            return;
        }

        if (streamInfo.eventDataDescriptions.size() != m_exp->GetNumberOfEvents())
        {
            cout << "Error: number of events declared in StreamInfo does not equal "
                "the number of events present in the generated Experiment class."
                << endl
                << "Please run `make' and restart the client."
                << endl;
            m_quit = true;
            return;
        }

        for (size_t eventIndex = 0; eventIndex < m_exp->GetNumberOfEvents(); eventIndex++)
        {
            auto &edd = streamInfo.eventDataDescriptions.at(eventIndex);
            auto event = m_exp->GetEvent(eventIndex);

            if (edd.dataSources.size() != event->GetDataSourceStorages().size())
            {
                cout << "Warning: eventIndex=" << eventIndex << ", eventName=" << event->GetName()
                    << ": number of data sources in the StreamInfo and in the generated Event class "
                    " differ (streamInfo:" << edd.dataSources.size()
                    << ", class:" << event->GetDataSourceStorages().size() << ")."
                    << endl
                    << "Please run `make' and restart the client."
                    << endl;
                m_quit = true;
                return;
            }
        }

        m_codeGeneratedAndLoaded = true;

        // generate output filename open the file
        std::string filename;

        if (streamInfo.runId.empty())
        {
            cout << __FUNCTION__ << ": Warning: got an empty runId!" << endl;
            filename = "unknown_run.root";
        }
        else
        {
            filename = streamInfo.runId + ".root";
        }

        // open the file and create the event trees
        cout << "Opening output file " << filename << endl;
        m_outFile = std::make_unique<TFile>(filename.c_str(), "recreate");
        cout << "Creating output trees" << endl;
        m_eventTrees = m_exp->MakeTrees();
        assert(m_eventTrees.size() == m_exp->GetNumberOfEvents());
    }
#endif

