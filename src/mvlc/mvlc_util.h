#ifndef __MVME_MVLC_UTIL_H__
#define __MVME_MVLC_UTIL_H__

#include "vme_script.h"
#include "mvlc_constants.h"
#include <QVector>

namespace mesytec
{
namespace mvlc
{

// vme_script -> mvlc constant
AddressMode convert_amod(vme_script::AddressMode mode);
VMEDataWidth convert_data_width(vme_script::DataWidth width);

// mvlc constant -> vme_script
vme_script::AddressMode convert_amod(AddressMode amod);
vme_script::DataWidth convert_data_width(VMEDataWidth dataWidth);

// AddressMode classification
bool is_block_amod(AddressMode amod);

// Returns the raw stack without any interleaved super commands.
// The stack result will be written to the given output pipe.
std::vector<u32> build_stack(const vme_script::VMEScript &script, u8 outPipe);

// Returns a Command Buffer List which writes the contents of the given stack
// or VMEScript to the MVLC stack memory area.
// The returned list will begin with CmdBufferStart and end with CmdBufferEnd.
std::vector<u32> build_upload_commands(const vme_script::VMEScript &script, u8 outPipe,
                                       u16 startAddress);
std::vector<u32> build_upload_commands(const std::vector<u32> &stack, u16 startAddress);

void log_buffer(const u32 *buffer, size_t size, const std::string &info = {});
void log_buffer(const std::vector<u32> &buffer, const std::string &info = {});
void log_buffer(const QVector<u32> &buffer, const QString &info = {});

const std::map<u32, std::string> &get_super_command_table();
const std::map<u32, std::string> &get_stack_command_table();

} // end namespace mvlc
} // end namespace mesytec

#endif /* __MVLC_UTIL_H__ */
