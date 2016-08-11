#include "stdafx.h"
#include "..\ServerBaseApi\Entrypoints.h"

#include "DeviceHandle.h"
#include "IoThread.h"

#include <NT\winbasep.h>

std::vector<std::unique_ptr<IoThread>> ioThreads;

NTSTATUS Entrypoints::StartConsoleForServerHandle(_In_ HANDLE const ServerHandle,
                                                  _In_ IApiResponders* const pResponder)
{
    std::unique_ptr<IoThread> pNewThread = std::make_unique<IoThread>(ServerHandle, pResponder);
    ioThreads.push_back(std::move(pNewThread));

    return STATUS_SUCCESS;
}

NTSTATUS Entrypoints::StartConsoleForCmdLine(_In_ PCWSTR CmdLine,
                                             _In_ IApiResponders* const pResponder)
{
    // Create a scope because we're going to exit thread if everything goes well.
    // This scope will ensure all C++ objects and smart pointers get a chance to destruct before ExitThread is called.
    {
        // Create the server and reference handles and create the console object.
        wil::unique_handle ServerHandle;
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateServerHandle(ServerHandle.addressof(), FALSE));

        wil::unique_handle ReferenceHandle;
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateClientHandle(ReferenceHandle.addressof(),
                                                                   ServerHandle.get(),
                                                                   L"\\Reference",
                                                                   FALSE));

        RETURN_IF_NTSTATUS_FAILED(Entrypoints::StartConsoleForServerHandle(ServerHandle.get(), pResponder));

        // If we get to here, we have transferred ownership of the server handle to the console, so release it.
        // Keep a copy of the value so we can open the client handles even though we're no longer the owner.
        HANDLE hServer = ServerHandle.release();

        // Now that the console object was created, we're in a state that lets us
        // create the default io objects.
        wil::unique_handle ClientHandle[3];

        // Input
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateClientHandle(ClientHandle[0].addressof(),
                                                                   hServer,
                                                                   L"\\Input",
                                                                   TRUE));

        // Output
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateClientHandle(ClientHandle[1].addressof(),
                                                                   hServer,
                                                                   L"\\Output",
                                                                   TRUE));

        ServerHandle.release();

        // Error is a copy of Output
        RETURN_IF_WIN32_BOOL_FALSE(DuplicateHandle(GetCurrentProcess(),
                                                   ClientHandle[1].get(),
                                                   GetCurrentProcess(),
                                                   ClientHandle[2].addressof(),
                                                   0,
                                                   TRUE,
                                                   DUPLICATE_SAME_ACCESS));


        // Create the child process. We will temporarily overwrite the values in the
        // PEB to force them to be inherited.

        STARTUPINFOEX StartupInformation = { 0 };
        StartupInformation.StartupInfo.cb = sizeof(STARTUPINFOEX);
        StartupInformation.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        StartupInformation.StartupInfo.hStdInput = ClientHandle[0].get();
        StartupInformation.StartupInfo.hStdOutput = ClientHandle[1].get();
        StartupInformation.StartupInfo.hStdError = ClientHandle[2].get();

        // Create the extended attributes list that will pass the console server information into the child process.

        // Call first time to find size
        SIZE_T AttributeListSize;
        InitializeProcThreadAttributeList(NULL,
                                          2,
                                          0,
                                          &AttributeListSize);

        // Alloc space
        std::unique_ptr<BYTE[]> AttributeList = std::make_unique<BYTE[]>(AttributeListSize);
        StartupInformation.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(AttributeList.get());

        // Call second time to actually initialize space.
        RETURN_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(StartupInformation.lpAttributeList,
                                                                     2,
                                                                     0,
                                                                     &AttributeListSize));
        // Set cleanup data for ProcThreadAttributeList when successful.
        auto CleanupProcThreadAttribute = wil::ScopeExit([StartupInformation]
        {
            DeleteProcThreadAttributeList(StartupInformation.lpAttributeList);
        });


        RETURN_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(StartupInformation.lpAttributeList,
                                                             0,
                                                             PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE,
                                                             ReferenceHandle.addressof(),
                                                             sizeof(HANDLE),
                                                             NULL,
                                                             NULL));

        // UpdateProcThreadAttributes wants this as a bare array of handles and doesn't like our smart structures, 
        // so set it up for its temporary use.
        HANDLE HandleList[3];
        HandleList[0] = StartupInformation.StartupInfo.hStdInput;
        HandleList[1] = StartupInformation.StartupInfo.hStdOutput;
        HandleList[2] = StartupInformation.StartupInfo.hStdError;

        RETURN_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(StartupInformation.lpAttributeList,
                                                             0,
                                                             PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                             &HandleList[0],
                                                             sizeof HandleList,
                                                             NULL,
                                                             NULL));

        // We have to copy the command line string we're given because CreateProcessW has to be called with mutable data.
        if (wcslen(CmdLine) == 0)
        {
            // If they didn't give us one, just launch cmd.exe.
            CmdLine = L"cmd.exe";
        }

        size_t length = wcslen(CmdLine) + 1;
        std::unique_ptr<wchar_t[]> CmdLineMutable = std::make_unique<wchar_t[]>(length);

        wcscpy_s(CmdLineMutable.get(), length, CmdLine);
        CmdLineMutable[length - 1] = L'\0';

        wil::unique_process_information ProcessInformation;
        RETURN_IF_WIN32_BOOL_FALSE(CreateProcessW(NULL,
                                                  CmdLineMutable.get(),
                                                  NULL,
                                                  NULL,
                                                  TRUE,
                                                  EXTENDED_STARTUPINFO_PRESENT,
                                                  NULL,
                                                  NULL,
                                                  &StartupInformation.StartupInfo,
                                                  ProcessInformation.addressof()));
    }

    // Exit the thread so the CRT won't clean us up and kill. The IO thread owns the lifetime now.
    ExitThread(STATUS_SUCCESS);

    // We won't hit this. The ExitThread above will kill the caller at this point.
    return STATUS_SUCCESS;
}