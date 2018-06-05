/*++
Copyright (c) Microsoft Corporation

Module Name:
- history.h

Abstract:
- Encapsulates the cmdline functions and structures specifically related to
        command history functionality.
--*/

#pragma once

// CommandHistory Flags
#define CLE_ALLOCATED 0x00000001
#define CLE_RESET     0x00000002

#include "popup.h"

class CommandHistory
{
public:
    static CommandHistory* s_Allocate(const std::wstring_view appName, const HANDLE processHandle);
    static CommandHistory* s_Find(const HANDLE processHandle);
    static CommandHistory* s_FindByExe(const std::wstring_view appName);
    static void s_ReallocExeToFront(const std::wstring_view appName, const size_t commands);
    static void s_Free(const HANDLE processHandle);
    static void s_ResizeAll(const size_t commands);
    static void s_UpdatePopups(const WORD NewAttributes,
                               const WORD NewPopupAttributes,
                               const WORD OldAttributes,
                               const WORD OldPopupAttributes);
    static size_t s_CountOfHistories();

    enum class MatchOptions
    {
        None = 0x0,
        ExactMatch = 0x1,
        JustLooking = 0x2
    };
    bool FindMatchingCommand(const std::wstring_view command,
                             const SHORT startingIndex,
                             SHORT& indexFound,
                             const MatchOptions options);
    bool IsAppNameMatch(const std::wstring_view other) const;

    [[nodiscard]]
    HRESULT Add(const std::wstring_view command,
                const bool suppressDuplicates);

    [[nodiscard]]
    HRESULT Retrieve(const WORD virtualKeyCode,
                     const gsl::span<wchar_t> buffer,
                     size_t& commandSize);

    [[nodiscard]]
    HRESULT RetrieveNth(const SHORT index,
                        const gsl::span<wchar_t> buffer,
                        size_t& commandSize);

    size_t GetNumberOfCommands() const;
    std::wstring_view GetNth(const SHORT index) const;

    void Realloc(const size_t commands);
    void Empty();

    [[nodiscard]]
    Popup* BeginPopup(SCREEN_INFORMATION& screenInfo, const COORD size, Popup::PopFunc func);
    [[nodiscard]]
    HRESULT EndPopup();

    bool AtFirstCommand() const;
    bool AtLastCommand() const;

    std::wstring_view GetLastCommand() const;

private:
    void _Reset();
    
    std::wstring _Remove(const SHORT iDel);

    // _Next and _Prev go to the next and prev command
    // _Inc  and _Dec go to the next and prev slots
    // Don't get the two confused - it matters when the cmd history is not full!
    void _Prev(SHORT& ind) const;
    void _Next(SHORT& ind) const;
    void _Dec(SHORT& ind) const;
    void _Inc(SHORT& ind) const;

    
    std::vector<std::wstring> _commands;
    SHORT _maxCommands;

    std::wstring _appName;
    HANDLE _processHandle;

    static std::deque<CommandHistory> s_historyLists;

public:
    DWORD Flags;
    SHORT LastDisplayed;
    
    std::deque<Popup*> PopupList; // pointer to top-level popup

#ifdef UNIT_TESTING
    static void s_ClearHistoryListStorage();
    friend class HistoryTests;
#endif
};

DEFINE_ENUM_FLAG_OPERATORS(CommandHistory::MatchOptions);