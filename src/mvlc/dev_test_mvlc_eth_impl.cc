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
#include "mvlc/mvlc_impl_factory.h"
#include "mvlc/mvlc_impl_eth.h"
#include "mvlc/mvlc_qt_object.h"
#include "mvlc/mvlc_util.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace mesytec::mvlc;
using namespace mesytec::mvlc::eth;

using std::cout;
using std::cerr;
using std::endl;

enum class TestType
{
    Memory,
    VME,
};

struct ErrorWithMessage
{
    std::error_code ec;
    std::string msg;
};

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        cerr << "Usage: " << argv[0] << " <hostname|ip-address>"
            " <regtest|vmetest> <iterations>" << endl;
        cerr << "  'regtest' writes and reads mvlc memory" << endl;
        cerr << "  'vmetest' writes and reads to/from vme address 0x0000601A" << endl;
        cerr << endl;
    }

    const char *host  = argv[1];
    const std::string stest = argv[2];
    TestType testType;

    if (stest == "regtest")
        testType = TestType::Memory;
    else if (stest == "vmetest")
        testType = TestType::VME;
    else
    {
        cerr << "Unknown test type given" << endl;
        return 1;
    }

    const size_t iterations = std::atoi(argv[3]);


    MVLCObject mvlc(make_mvlc_eth(host));

    mvlc.setReadTimeout(Pipe::Command, 2000);
    mvlc.setWriteTimeout(Pipe::Command, 2000);

    try
    {
        if (auto ec = mvlc.connect())
            throw ec;

        assert(mvlc.isConnected());

        size_t iteration = 0u;

        for (iteration = 0; iteration < iterations; iteration++)
        {
            switch (testType)
            {
                case TestType::Memory:
                    {
                        if (auto ec = mvlc.writeRegister(0x2000 + 512, iteration))
                            throw ErrorWithMessage{ec, "writeRegister"};

                        u32 regVal = 0u;
                        if (auto ec = mvlc.readRegister(0x2000 + 512, regVal))
                            throw ErrorWithMessage{ec, "readRegister"};

                        assert(regVal == iteration);
                    } break;

                case TestType::VME:
                    {
                        u32 value = iteration % 0xFFFFu;
                        if (value == 0) value = 1;

                        if (auto ec = mvlc.vmeSingleWrite(0x0000601A, value,
                                                          vme_address_modes::A32, VMEDataWidth::D16))
                        {
                            throw ec;
                        }

                        u32 result = 0u;

                        if (auto ec = mvlc.vmeSingleRead(0x0000601A, result,
                                                         vme_address_modes::A32, VMEDataWidth::D16))
                        {
                            throw ec;
                        }

                        assert(result == value);
                    } break;
            }
        }

    }
    catch (const std::error_code &ec)
    {
        cerr << "caught an error_code: " << ec.category().name() << ": " << ec.message()
            << " (" << ec.value() << ")" << endl;
        return 1;
    }
    catch (const ErrorWithMessage &e)
    {
        auto &ec = e.ec;
        cerr << "caught ErrorWithMessage: ec=" << ec.category().name() << ": " << ec.message()
            << "; (" << ec.value() << ")"
            << endl
            << "  message=" << e.msg
            << endl;
        return 1;
    }

    cout << "Hit enter to exit";
    getchar();

    return 0;
}
