#include <ftd3xx.h>
#include <iostream>

using std::cout;
using std::cerr;
using std::endl;

int main(int argc, char *argv[])
{
    printf("===== FT_GetDeviceInfoList =====\n");
    {
        FT_STATUS ftStatus;
        DWORD numDevs = 0;
        ftStatus = FT_CreateDeviceInfoList(&numDevs);
        if (!FT_FAILED(ftStatus) && numDevs > 0)
        {
            FT_DEVICE_LIST_INFO_NODE *devInfo = (FT_DEVICE_LIST_INFO_NODE*)malloc(
                sizeof(FT_DEVICE_LIST_INFO_NODE) * numDevs);
            ftStatus = FT_GetDeviceInfoList(devInfo, &numDevs);
            if (!FT_FAILED(ftStatus))
            {
                printf("List of Connected Devices!\n\n");
                for (DWORD i = 0; i < numDevs; i++)
                {
                    printf("Device[%d]\n", i);
                    printf("\tFlags: 0x%x %s | Type: %d | ID: 0x%08X | ftHandle=0x%p\n",
                           devInfo[i].Flags,
                           devInfo[i].Flags & FT_FLAGS_SUPERSPEED ? "[USB 3]" :
                           devInfo[i].Flags & FT_FLAGS_HISPEED ? "[USB 2]" :
                           devInfo[i].Flags & FT_FLAGS_OPENED ? "[OPENED]" : "",
                           devInfo[i].Type,
                           devInfo[i].ID,
                           devInfo[i].ftHandle);
                    printf("\tSerialNumber=%s\n", devInfo[i].SerialNumber);
                    printf("\tDescription=%s\n", devInfo[i].Description);
                }
            }
            free(devInfo);
        }
    }

    printf("===== FT_GetDeviceInfoDetail =====\n");
    {
        FT_STATUS ftStatus;
        DWORD numDevs = 0;
        ftStatus = FT_CreateDeviceInfoList(&numDevs);
        if (!FT_FAILED(ftStatus) && numDevs > 0)
        {
            FT_HANDLE ftHandle = NULL;
            DWORD Flags = 0;
            DWORD Type = 0;
            DWORD ID = 0;
            char SerialNumber[16] = { 0 };
            char Description[32] = { 0 };
            printf("List of Connected Devices!\n\n");
            for (DWORD i = 0; i < numDevs; i++)
            {
                ftStatus = FT_GetDeviceInfoDetail(i, &Flags, &Type, &ID, NULL,
                                                  SerialNumber, Description, &ftHandle);
                if (!FT_FAILED(ftStatus))
                {
                    printf("Device[%d]\n", i);
                    printf("\tFlags: 0x%x %s | Type: %d | ID: 0x%08X | ftHandle=0x%p\n",
                           Flags,
                           Flags & FT_FLAGS_SUPERSPEED ? "[USB 3]" :
                           Flags & FT_FLAGS_HISPEED ? "[USB 2]" :
                           Flags & FT_FLAGS_OPENED ? "[OPENED]" : "",
                           Type,
                           ID,
                           ftHandle);
                    printf("\tSerialNumber=%s\n", SerialNumber);
                    printf("\tDescription=%s\n", Description);
                }
            }
        }
    }

    return 0;
}
