/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2020 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#ifndef __VME_DAQ_H__
#define __VME_DAQ_H__

#include "libmvme_export.h"

#include "vme_config.h"
#include "vme_controller.h"
#include "vme_readout_worker.h"
#include "vme_script.h"
#include "vme_script_exec.h"

/* Both init functions throw on error:
 * QString, std::runtime_error, vme_script::ParseError
 */

/* Runs the following vme scripts from the vme configuration using the given
 * vme controller:
 * - global DAQ start scripts
 * - for each event:
 *     - for each module:
 *       - module reset script
 *       - module init scripts
 * - for each event:
 *     - event DAQ start script
 */

struct ScriptWithResults
{
    // Non-owning pointer to the vme script config that produced the result
    // list.
    // TODO: change this to a shared_ptr, weak_ptr or use a copy of the script.
    const VMEScriptConfig *scriptConfig = nullptr;

    // The symbol tables used when evaluating the script.
    //const vme_script::SymbolTables symbols;

    // List of results of running the script.
    const vme_script::ResultList results;

    // A ParseError instance or nullptr if parsing the script was sucessful.
    std::shared_ptr<vme_script::ParseError> parseError = {};
};

QVector<ScriptWithResults> LIBMVME_EXPORT
vme_daq_init(
    VMEConfig *vmeConfig,
    VMEController *controller,
    std::function<void (const QString &)> logger,
    vme_script::run_script_options::Flag opts = 0u);

QVector<ScriptWithResults> LIBMVME_EXPORT
vme_daq_init(
    VMEConfig *vmeConfig,
    VMEController *controller,
    std::function<void (const QString &)> logger,
    std::function<void (const QString &)> errorLogger,
    vme_script::run_script_options::Flag opts = 0u);

/* Counterpart to vme_daq_init. Runs
 * - for each event
 *     - event DAQ stop script
 * - global DAQ stop scripts
 */
QVector<ScriptWithResults>
vme_daq_shutdown(
    VMEConfig *vmeConfig,
    VMEController *controller,
    std::function<void (const QString &)> logger,
    vme_script::run_script_options::Flag opts = 0);

QVector<ScriptWithResults>
vme_daq_shutdown(
    VMEConfig *vmeConfig,
    VMEController *controller,
    std::function<void (const QString &)> logger,
    std::function<void (const QString &)> errorLogger,
    vme_script::run_script_options::Flag opts = 0);

bool has_errors(const QVector<ScriptWithResults> &results);

void log_errors(const QVector<ScriptWithResults> &results,
                std::function<void (const QString &)> logger);


struct EventReadoutBuildFlags
{
    static const u8 None = 0u;
    static const u8 NoModuleEndMarker = 1u;
};

/* Builds a vme script containing the readout commands for the given event:
 * - event readout start ("cycle start" in the GUI)
 * - for each module:
 *     - module readout script (empty if module is disabled)
 *     - EndMarker command
 * - event readout end ("cycle end" in the GUI)
 */
vme_script::VMEScript build_event_readout_script(
    EventConfig *eventConfig,
    u8 flags = EventReadoutBuildFlags::None);

struct DAQReadoutListfileHelperPrivate;

class DAQReadoutListfileHelper
{
    public:
        explicit DAQReadoutListfileHelper(VMEReadoutWorkerContext &readoutContext);
        ~DAQReadoutListfileHelper();

        void beginRun();
        void endRun();
        void writeBuffer(DataBuffer *buffer);
        void writeBuffer(const u8 *buffer, size_t size);
        void writeTimetickSection();
        void writePauseSection();
        void writeResumeSection();

    private:
        std::unique_ptr<DAQReadoutListfileHelperPrivate> m_d;
        VMEReadoutWorkerContext &m_readoutContext;
};

/* Throws if neither UseRunNumber nor UseTimestamp is set and the file already
 * exists. Otherwise tries until it hits a non-existant filename. In the odd
 * case where a timestamped filename exists and only UseTimestamp is set this
 * process will take 1s!
 *
 * Also note that the file handling code does not in any way guard against race
 * conditions when someone else is also creating files.
 *
 * Note: Increments the runNumber of outInfo if UseRunNumber is set in the
 * output flags.
 */
QString make_new_listfile_name(ListFileOutputInfo *outInfo);

#endif /* __VME_DAQ_H__ */
