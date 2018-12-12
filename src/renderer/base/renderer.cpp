/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "../../host/conimeinfo.h"
#include "../../buffer/out/textBuffer.hpp"

#include "renderer.hpp"

#pragma hdrstop

using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::Types;

// Routine Description:
// - Creates a new renderer controller for a console.
// Arguments:
// - pData - The interface to console data structures required for rendering
// - pEngine - The output engine for targeting each rendering frame
// Return Value:
// - An instance of a Renderer.
// NOTE: CAN THROW IF MEMORY ALLOCATION FAILS.
Renderer::Renderer(_In_ std::unique_ptr<IRenderData> pData,
                   _In_reads_(cEngines) IRenderEngine** const rgpEngines,
                   const size_t cEngines) :
    _pData(std::move(pData)),
    _pThread(nullptr)
{
    THROW_IF_NULL_ALLOC(_pData);

    _srViewportPrevious = { 0 };

    for (size_t i = 0; i < cEngines; i++)
    {
        IRenderEngine* engine = rgpEngines[i];
        // NOTE: THIS CAN THROW IF MEMORY ALLOCATION FAILS.
        AddRenderEngine(engine);
    }
}

// Routine Description:
// - Destroys an instance of a renderer
// Arguments:
// - <none>
// Return Value:
// - <none>
Renderer::~Renderer()
{
    if (_pThread != nullptr)
    {
        delete _pThread;
    }

    std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine) {
        delete pEngine;
    });
}

[[nodiscard]]
HRESULT Renderer::s_CreateInstance(_In_ std::unique_ptr<IRenderData> pData,
                                   _Outptr_result_nullonfailure_ Renderer** const ppRenderer)
{
    return Renderer::s_CreateInstance(std::move(pData), nullptr, 0,  ppRenderer);
}

[[nodiscard]]
HRESULT Renderer::s_CreateInstance(_In_ std::unique_ptr<IRenderData> pData,
                                   _In_reads_(cEngines) IRenderEngine** const rgpEngines,
                                   const size_t cEngines,
                                   _Outptr_result_nullonfailure_ Renderer** const ppRenderer)
{
    HRESULT hr = S_OK;

    Renderer* pNewRenderer = nullptr;
    try
    {
        pNewRenderer = new Renderer(std::move(pData), rgpEngines, cEngines);
    }
    CATCH_RETURN();

    // Attempt to create renderer thread
    if (SUCCEEDED(hr))
    {
        RenderThread* pNewThread;

        hr = RenderThread::s_CreateInstance(pNewRenderer, &pNewThread);

        if (SUCCEEDED(hr))
        {
            pNewRenderer->_pThread = pNewThread;
        }
    }

    if (SUCCEEDED(hr))
    {
        *ppRenderer = pNewRenderer;
    }
    else
    {
        delete pNewRenderer;
    }

    return hr;
}

// Routine Description:
// - Walks through the console data structures to compose a new frame based on the data that has changed since last call and outputs it to the connected rendering engine.
// Arguments:
// - <none>
// Return Value:
// - HRESULT S_OK, GDI error, Safe Math error, or state/argument errors.
[[nodiscard]]
HRESULT Renderer::PaintFrame()
{
    for (IRenderEngine* const pEngine : _rgpEngines)
    {
        LOG_IF_FAILED(_PaintFrameForEngine(pEngine));
    }

    return S_OK;
}

extern void LockConsole();
extern void UnlockConsole();

[[nodiscard]]
HRESULT Renderer::_PaintFrameForEngine(_In_ IRenderEngine* const pEngine)
{
    FAIL_FAST_IF_NULL(pEngine); // This is a programming error. Fail fast.

    LockConsole();
    auto unlock = wil::scope_exit([&]()
    {
        UnlockConsole();
    });

    // Last chance check if anything scrolled without an explicit invalidate notification since the last frame.
    _CheckViewportAndScroll();

    // Try to start painting a frame
    HRESULT const hr = pEngine->StartPaint();
    RETURN_IF_FAILED(hr);

    // Return early if there's nothing to paint.
    // The renderer itself tracks if there's something to do with the title, the
    //      engine won't know that.
    if (S_FALSE == hr)
    {
        return S_OK;
    }

    auto endPaint = wil::scope_exit([&]()
    {
        LOG_IF_FAILED(pEngine->EndPaint());
    });

    // A. Prep Colors
    RETURN_IF_FAILED(_UpdateDrawingBrushes(pEngine, _pData->GetDefaultBrushColors(), true));

    // B. Perform Scroll Operations
    RETURN_IF_FAILED(_PerformScrolling(pEngine));

    // 1. Paint Background
    RETURN_IF_FAILED(_PaintBackground(pEngine));

    // 2. Paint Rows of Text
    _PaintBufferOutput(pEngine);

    // 3. Paint IME composition area
    _PaintImeCompositionString(pEngine);

    // 4. Paint Selection
    _PaintSelection(pEngine);

    // 5. Paint Cursor
    _PaintCursor(pEngine);

    // 6. Paint window title
    RETURN_IF_FAILED(_PaintTitle(pEngine));

    // Force scope exit end paint to finish up collecting information and possibly painting
    endPaint.reset();

    // Force scope exit unlock to let go of global lock so other threads can run
    unlock.reset();

    // Trigger out-of-lock presentation for renderers that can support it
    RETURN_IF_FAILED(pEngine->Present());

    // As we leave the scope, EndPaint will be called (declared above)
    return S_OK;
}

void Renderer::_NotifyPaintFrame()
{
    // The thread will provide throttling for us.
    _pThread->NotifyPaint();
}

// Routine Description:
// - Called when the system has requested we redraw a portion of the console.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerSystemRedraw(const RECT* const prcDirtyClient)
{
    std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine) {
        LOG_IF_FAILED(pEngine->InvalidateSystem(prcDirtyClient));
    });

    _NotifyPaintFrame();
}

// Routine Description:
// - Called when a particular region within the console buffer has changed.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerRedraw(const Viewport& region)
{
    Viewport view = _pData->GetViewport();
    SMALL_RECT srUpdateRegion = region.ToExclusive();

    if (view.TrimToViewport(&srUpdateRegion))
    {
        view.ConvertToOrigin(&srUpdateRegion);
        std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine) {
            LOG_IF_FAILED(pEngine->Invalidate(&srUpdateRegion));
        });

        _NotifyPaintFrame();
    }
}

// Routine Description:
// - Called when a particular coordinate within the console buffer has changed.
// Arguments:
// - pcoord: The buffer-space coordinate that has changed.
// Return Value:
// - <none>
void Renderer::TriggerRedraw(const COORD* const pcoord)
{
    TriggerRedraw(Viewport::FromCoord(*pcoord)); // this will notify to paint if we need it.
}

// Routine Description:
// - Called when the cursor has moved in the buffer. Allows for RenderEngines to
//      differentiate between cursor movements and other invalidates.
//   Visual Renderers (ex GDI) sohuld invalidate the position, while the VT
//      engine ignores this. See MSFT:14711161.
// Arguments:
// - pcoord: The buffer-space position of the cursor.
// Return Value:
// - <none>
void Renderer::TriggerRedrawCursor(const COORD* const pcoord)
{
    Viewport view = _pData->GetViewport();
    COORD updateCoord = *pcoord;

    if (view.IsInBounds(updateCoord))
    {
        view.ConvertToOrigin(&updateCoord);
        for (IRenderEngine* pEngine : _rgpEngines)
        {
            LOG_IF_FAILED(pEngine->InvalidateCursor(&updateCoord));

            // Double-wide cursors need to invalidate the right half as well.
            if (_pData->IsCursorDoubleWidth())
            {
                updateCoord.X++;
                LOG_IF_FAILED(pEngine->InvalidateCursor(&updateCoord));
            }
        }

        _NotifyPaintFrame();
    }
}

// Routine Description:
// - Called when something that changes the output state has occurred and the entire frame is now potentially invalid.
// - NOTE: Use sparingly. Try to reduce the refresh region where possible. Only use when a global state change has occurred.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerRedrawAll()
{
    std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine) {
        LOG_IF_FAILED(pEngine->InvalidateAll());
    });

    _NotifyPaintFrame();
}

// Method Description:
// - Called when the host is about to die, to give the renderer one last chance
//      to paint before the host exits.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerTeardown()
{
    // We need to shut down the paint thread on teardown.
    _pThread->WaitForPaintCompletionAndDisable(INFINITE);

    // Then walk through and do one final paint on the caller's thread.
    for (IRenderEngine* const pEngine : _rgpEngines)
    {
        bool fEngineRequestsRepaint = false;
        HRESULT hr = pEngine->PrepareForTeardown(&fEngineRequestsRepaint);
        LOG_IF_FAILED(hr);

        if (SUCCEEDED(hr) && fEngineRequestsRepaint)
        {
            LOG_IF_FAILED(_PaintFrameForEngine(pEngine));
        }
    }
}

// Routine Description:
// - Called when the selected area in the console has changed.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerSelection()
{
    try
    {
        // Get selection rectangles
        const auto rects = _GetSelectionRects();

        std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine) {
            LOG_IF_FAILED(pEngine->InvalidateSelection(_previousSelection));
            LOG_IF_FAILED(pEngine->InvalidateSelection(rects));
        });

        _previousSelection = rects;

        _NotifyPaintFrame();
    }
    CATCH_LOG();
}

// Routine Description:
// - Called when we want to check if the viewport has moved and scroll accordingly if so.
// Arguments:
// - <none>
// Return Value:
// - True if something changed and we scrolled. False otherwise.
bool Renderer::_CheckViewportAndScroll()
{
    SMALL_RECT const srOldViewport = _srViewportPrevious;
    SMALL_RECT const srNewViewport = _pData->GetViewport().ToInclusive();

    COORD coordDelta;
    coordDelta.X = srOldViewport.Left - srNewViewport.Left;
    coordDelta.Y = srOldViewport.Top - srNewViewport.Top;

    std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine) {
        LOG_IF_FAILED(pEngine->UpdateViewport(srNewViewport));
        LOG_IF_FAILED(pEngine->InvalidateScroll(&coordDelta));
    });
    _srViewportPrevious = srNewViewport;

    return coordDelta.X != 0 || coordDelta.Y != 0;
}

// Routine Description:
// - Called when a scroll operation has occurred by manipulating the viewport.
// - This is a special case as calling out scrolls explicitly drastically improves performance.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerScroll()
{
    if (_CheckViewportAndScroll())
    {
        _NotifyPaintFrame();
    }
}

// Routine Description:
// - Called when a scroll operation wishes to explicitly adjust the frame by the given coordinate distance.
// - This is a special case as calling out scrolls explicitly drastically improves performance.
// - This should only be used when the viewport is not modified. It lets us know we can "scroll anyway" to save perf,
//   because the backing circular buffer rotated out from behind the viewport.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerScroll(const COORD* const pcoordDelta)
{
    std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine){
        LOG_IF_FAILED(pEngine->InvalidateScroll(pcoordDelta));
    });

    _NotifyPaintFrame();
}

// Routine Description:
// - Called when the text buffer is about to circle it's backing buffer.
//      A renderer might want to get painted before that happens.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerCircling()
{
    for (IRenderEngine* const pEngine : _rgpEngines)
    {
        bool fEngineRequestsRepaint = false;
        HRESULT hr = pEngine->InvalidateCircling(&fEngineRequestsRepaint);
        LOG_IF_FAILED(hr);

        if (SUCCEEDED(hr) && fEngineRequestsRepaint)
        {
            LOG_IF_FAILED(_PaintFrameForEngine(pEngine));
        }
    }
}

// Routine Description:
// - Called when the title of the console window has changed. Indicates that we
//      should update the title on the next frame.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::TriggerTitleChange()
{
    const std::wstring newTitle = _pData->GetConsoleTitle();
    for (IRenderEngine* const pEngine : _rgpEngines)
    {
        LOG_IF_FAILED(pEngine->InvalidateTitle(newTitle));
    }
    _NotifyPaintFrame();
}

// Routine Description:
// - Update the title for a particular engine.
// Arguments:
// - pEngine: the engine to update the title for.
// Return Value:
// - the HRESULT of the underlying engine's UpdateTitle call.
HRESULT Renderer::_PaintTitle(IRenderEngine* const pEngine)
{
    const std::wstring newTitle = _pData->GetConsoleTitle();
    return pEngine->UpdateTitle(newTitle);
}

// Routine Description:
// - Called when a change in font or DPI has been detected.
// Arguments:
// - iDpi - New DPI value
// - FontInfoDesired - A description of the font we would like to have.
// - FontInfo - Data that will be fixed up/filled on return with the chosen font data.
// Return Value:
// - <none>
void Renderer::TriggerFontChange(const int iDpi, const FontInfoDesired& FontInfoDesired, _Out_ FontInfo& FontInfo)
{
    std::for_each(_rgpEngines.begin(), _rgpEngines.end(), [&](IRenderEngine* const pEngine){
        LOG_IF_FAILED(pEngine->UpdateDpi(iDpi));
        LOG_IF_FAILED(pEngine->UpdateFont(FontInfoDesired, FontInfo));
    });

    _NotifyPaintFrame();
}

// Routine Description:
// - Get the information on what font we would be using if we decided to create a font with the given parameters
// - This is for use with speculative calculations.
// Arguments:
// - iDpi - The DPI of the target display
// - pFontInfoDesired - A description of the font we would like to have.
// - pFontInfo - Data that will be fixed up/filled on return with the chosen font data.
// Return Value:
// - S_OK if set successfully or relevant GDI error via HRESULT.
[[nodiscard]]
HRESULT Renderer::GetProposedFont(const int iDpi, const FontInfoDesired& FontInfoDesired, _Out_ FontInfo& FontInfo)
{
    // If there's no head, return E_FAIL. The caller should decide how to
    //      handle this.
    // Currently, the only caller is the WindowProc:WM_GETDPISCALEDSIZE handler.
    //      It will assume that the proposed font is 1x1, regardless of DPI.
    if (_rgpEngines.size() < 1)
    {
        return E_FAIL;
    }

    // There will only every really be two engines - the real head and the VT
    //      renderer. We won't know which is which, so iterate over them.
    //      Only return the result of the successful one if it's not S_FALSE (which is the VT renderer)
    // TODO: 14560740 - The Window might be able to get at this info in a more sane manner
    FAIL_FAST_IF(!(_rgpEngines.size() <= 2));
    for (IRenderEngine* const pEngine : _rgpEngines)
    {
        const HRESULT hr = LOG_IF_FAILED(pEngine->GetProposedFont(FontInfoDesired, FontInfo, iDpi));
        // We're looking for specifically S_OK, S_FALSE is not good enough.
        if (hr == S_OK)
        {
            return hr;
        }
    };

    return E_FAIL;
}

// Routine Description:
// - Retrieves the current X by Y (in pixels) size of the font in active use for drawing
// - NOTE: Generally the console host should avoid doing math in pixels unless absolutely necessary. Try to handle everything in character units and only let the renderer/window convert to pixels as necessary.
// Arguments:
// - <none>
// Return Value:
// - COORD representing the current pixel size of the selected font
COORD Renderer::GetFontSize()
{
    COORD fontSize = {1, 1};
    // There will only every really be two engines - the real head and the VT
    //      renderer. We won't know which is which, so iterate over them.
    //      Only return the result of the successful one if it's not S_FALSE (which is the VT renderer)
    // TODO: 14560740 - The Window might be able to get at this info in a more sane manner
    FAIL_FAST_IF(!(_rgpEngines.size() <= 2));

    for (IRenderEngine* const pEngine : _rgpEngines)
    {
        const HRESULT hr = LOG_IF_FAILED(pEngine->GetFontSize(&fontSize));
        // We're looking for specifically S_OK, S_FALSE is not good enough.
        if (hr == S_OK)
        {
            return fontSize;
        }
    };

    return fontSize;
}

// Routine Description:
// - Tests against the current rendering engine to see if this particular character would be considered
// full-width (inscribed in a square, twice as wide as a standard Western character, typically used for CJK
// languages) or half-width.
// - Typically used to determine how many positions in the backing buffer a particular character should fill.
// NOTE: This only handles 1 or 2 wide (in monospace terms) characters.
// Arguments:
// - glyph - the utf16 encoded codepoint to test
// Return Value:
// - True if the codepoint is full-width (two wide), false if it is half-width (one wide).
bool Renderer::IsGlyphWideByFont(const std::wstring_view glyph)
{
    bool fIsFullWidth = false;

    // There will only every really be two engines - the real head and the VT
    //      renderer. We won't know which is which, so iterate over them.
    //      Only return the result of the successful one if it's not S_FALSE (which is the VT renderer)
    // TODO: 14560740 - The Window might be able to get at this info in a more sane manner
    FAIL_FAST_IF(!(_rgpEngines.size() <= 2));
    for (IRenderEngine* const pEngine : _rgpEngines)
    {
        const HRESULT hr = LOG_IF_FAILED(pEngine->IsGlyphWideByFont(glyph, &fIsFullWidth));
        // We're looking for specifically S_OK, S_FALSE is not good enough.
        if (hr == S_OK)
        {
            return fIsFullWidth;
        }
    }

    return fIsFullWidth;
}

// Routine Description:
// - Sets an event in the render thread that allows it to proceed, thus enabling painting.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::EnablePainting()
{
    _pThread->EnablePainting();
}

// Routine Description:
// - Waits for the current paint operation to complete, if any, up to the specified timeout.
// - Resets an event in the render thread that precludes it from advancing, thus disabling rendering.
// - If no paint operation is currently underway, returns immediately.
// Arguments:
// - dwTimeoutMs - Milliseconds to wait for the current paint operation to complete, if any (can be INFINITE).
// Return Value:
// - <none>
void Renderer::WaitForPaintCompletionAndDisable(const DWORD dwTimeoutMs)
{
    _pThread->WaitForPaintCompletionAndDisable(dwTimeoutMs);
}

// Routine Description:
// - Paint helper to fill in the background color of the invalid area within the frame.
// Arguments:
// - <none>
// Return Value:
// - <none>
[[nodiscard]]
HRESULT Renderer::_PaintBackground(_In_ IRenderEngine* const pEngine)
{
    return pEngine->PaintBackground();
}

// Routine Description:
// - Paint helper to copy the primary console buffer text onto the screen.
// - This portion primarily handles figuring the current viewport, comparing it/trimming it versus the invalid portion of the frame, and queuing up, row by row, which pieces of text need to be further processed.
// - See also: Helper functions that seperate out each complexity of text rendering.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::_PaintBufferOutput(_In_ IRenderEngine* const pEngine)
{
    Viewport view = _pData->GetViewport();

    SMALL_RECT srDirty = pEngine->GetDirtyRectInChars();
    view.ConvertFromOrigin(&srDirty);

    const TextBuffer& textBuffer = _pData->GetTextBuffer();

    // The dirty rectangle may be larger than the backing buffer (anything, including the system, may have
    // requested that we render under the scroll bars). To prevent issues, trim down to the max buffer size
    // (a.k.a. ensure the viewport is between 0 and the max size of the buffer.)
    COORD const coordBufferSize = textBuffer.GetSize().Dimensions();
    srDirty.Top = std::max(srDirty.Top, 0i16);
    srDirty.Left = std::max(srDirty.Left, 0i16);
    srDirty.Right = std::min(srDirty.Right, gsl::narrow<SHORT>(coordBufferSize.X - 1));
    srDirty.Bottom = std::min(srDirty.Bottom, gsl::narrow<SHORT>(coordBufferSize.Y - 1));

    // Also ensure that the dirty rect still fits inside the screen viewport.
    srDirty.Top = std::max(srDirty.Top, view.Top());
    srDirty.Left = std::max(srDirty.Left, view.Left());
    srDirty.Right = std::min(srDirty.Right, view.RightInclusive());
    srDirty.Bottom = std::min(srDirty.Bottom, view.BottomInclusive());

    Viewport viewDirty = Viewport::FromInclusive(srDirty);
    for (SHORT iRow = viewDirty.Top(); iRow <= viewDirty.BottomInclusive(); iRow++)
    {
        // Get row of text data
        const ROW& Row = textBuffer.GetRowByOffset(iRow);

        // Get the requested left and right positions from the dirty rectangle.
        size_t iLeft = viewDirty.Left();
        size_t iRight = viewDirty.RightExclusive();

        // If there's anything to draw... draw it.
        if (iRight > iLeft)
        {
            const CharRow& charRow = Row.GetCharRow();

            std::wstring rowText;
            CharRow::const_iterator it;
            try
            {
                it = std::next(charRow.cbegin(), iLeft);
                rowText = charRow.GetTextRaw();
            }
            catch (...)
            {
                LOG_HR(wil::ResultFromCaughtException());
                return;
            }
            const CharRow::const_iterator itEnd = charRow.cend();

            // Get the pointer to the beginning of the text
            const wchar_t* const pwsLine = rowText.c_str() + iLeft;

            size_t const cchLine = iRight - iLeft;

            // Calculate the target position in the buffer where we should start writing.
            COORD coordTarget;
            coordTarget.X = (SHORT)iLeft - view.Left();
            coordTarget.Y = iRow - view.Top();

            // Determine if this line wrapped:
            const bool lineWrapped = Row.GetCharRow().WasWrapForced() && iRight == static_cast<size_t>(Row.GetCharRow().MeasureRight());

            // Now draw it.
            _PaintBufferOutputRasterFontHelper(pEngine, Row, pwsLine, it, itEnd, cchLine, iLeft, coordTarget, lineWrapped);

#if DBG
            if (_fDebug)
            {
                // Draw a frame shape around the last character of a wrapped row to identify where there are
                // soft wraps versus hard newlines.
                if (lineWrapped)
                {
                    IRenderEngine::GridLines lines = IRenderEngine::GridLines::Right | IRenderEngine::GridLines::Bottom;
                    COORD coordDebugTarget;
                    coordDebugTarget.Y = iRow - view.Top();
                    coordDebugTarget.X = (SHORT)iRight - view.Left() - 1;
                    LOG_IF_FAILED(pEngine->PaintBufferGridLines(lines, RGB(0x99, 0x77, 0x31), 1, coordDebugTarget));
                }
            }
#endif
        }
    }
}

// Routine Description:
// - Paint helper for raster font text. It will pass through to ColorHelper when it's done and cascade from there.
// - This particular helper checks the current font and converts the text, if necessary, back to the original OEM codepage.
// - This is required for raster fonts in GDI as it won't adapt them back on our behalf.
// - See also: All related helpers and buffer output functions.
// Arguments:
// - Row - reference to the row structure for the current line of text
// - pwsLine - Pointer to the first character in the string/substring to be drawn.
// - it - iterator pointing to the first grid cell in a sequence that is perfectly in sync with the pwsLine parameter. e.g. The first attribute here goes with the first character in pwsLine.
// - itEnd - corresponding end iterator to it
// - cchLine - The length of pwsLine and the minimum number of iterator steps.
// - iFirstAttr - Index into the row corresponding to pwsLine[0] to match up the appropriate color.
// - coordTarget - The X/Y coordinate position on the screen which we're attempting to render to.
// Return Value:
// - <none>
void Renderer::_PaintBufferOutputRasterFontHelper(_In_ IRenderEngine* const pEngine,
                                                  const ROW& Row,
                                                  _In_reads_(cchLine) PCWCHAR const pwsLine,
                                                  const CharRow::const_iterator it,
                                                  const CharRow::const_iterator itEnd,
                                                  _In_ size_t cchLine,
                                                  _In_ size_t iFirstAttr,
                                                  const COORD coordTarget,
                                                  const bool lineWrapped)
{
    const FontInfo* const pFontInfo = _pData->GetFontInfo();

    PWCHAR pwsConvert = nullptr;
    PCWCHAR pwsData = pwsLine; // by default, use the line data.

    // If we're not using a TrueType font, we'll have to re-interpret the line of text to make the raster font happy.
    if (!pFontInfo->IsTrueTypeFont())
    {
        UINT const uiCodePage = pFontInfo->GetCodePage();

        // dispatch conversion into our codepage

        // Find out the bytes required
        int const cbRequired = WideCharToMultiByte(uiCodePage, 0, pwsLine, (int)cchLine, nullptr, 0, nullptr, nullptr);

        if (cbRequired != 0)
        {
            // Allocate buffer for MultiByte
            PCHAR psConverted = new(std::nothrow) CHAR[cbRequired];

            if (psConverted != nullptr)
            {
                // Attempt conversion to current codepage
                int const cbConverted = WideCharToMultiByte(uiCodePage, 0, pwsLine, (int)cchLine, psConverted, cbRequired, nullptr, nullptr);

                // If successful...
                if (cbConverted != 0)
                {
                    // Now we have to convert back to Unicode but using the system ANSI codepage. Find buffer size first.
                    int const cchRequired = MultiByteToWideChar(CP_ACP, 0, psConverted, cbRequired, nullptr, 0);

                    if (cchRequired != 0)
                    {
                        pwsConvert = new(std::nothrow) WCHAR[cchRequired];

                        if (pwsConvert != nullptr)
                        {

                            // Then do the actual conversion.
                            int const cchConverted = MultiByteToWideChar(CP_ACP, 0, psConverted, cbRequired, pwsConvert, cchRequired);

                            if (cchConverted != 0)
                            {
                                // If all successful, use this instead.
                                pwsData = pwsConvert;
                            }
                        }
                    }
                }

                delete[] psConverted;
            }
        }

    }

    // If we are using a TrueType font, just call the next helper down.
    _PaintBufferOutputColorHelper(pEngine, Row, pwsData, it, itEnd, cchLine, iFirstAttr, coordTarget, lineWrapped);

    if (pwsConvert != nullptr)
    {
        delete[] pwsConvert;
    }
}

// Routine Description:
// - Paint helper for primary buffer output function.
// - This particular helper unspools the run-length-encoded color attributes, updates the brushes, and effectively substrings the text for each different color.
// - It also identifies box drawing attributes and calls the respective helper.
// - See also: All related helpers and buffer output functions.
// Arguments:
// - Row - Reference to the row structure for the current line of text
// - pwsLine - Pointer to the first character in the string/substring to be drawn.
// - it - iterator pointing to the first grid cell in a sequence that is perfectly in sync with the pwsLine parameter. e.g. The first attribute here goes with the first character in pwsLine.
// - itEnd - corresponding end iterator to it
// - cchLine - The length of pwsLine and the minimum number of iterator steps.
// - iFirstAttr - Index into the row corresponding to pwsLine[0] to match up the appropriate color.
// - coordTarget - The X/Y coordinate position on the screen which we're attempting to render to.
// Return Value:
// - <none>
void Renderer::_PaintBufferOutputColorHelper(_In_ IRenderEngine* const pEngine,
                                             const ROW& Row,
                                             _In_reads_(cchLine) PCWCHAR const pwsLine,
                                             const CharRow::const_iterator it,
                                             const CharRow::const_iterator itEnd,
                                             _In_ size_t cchLine,
                                             _In_ size_t iFirstAttr,
                                             const COORD coordTarget,
                                             const bool lineWrapped)
{
    // We may have to write this string in several pieces based on the colors.

    // Count up how many characters we've written so we know when we're done.
    size_t cchWritten = 0;

    // The offset from the target point starts at the target point (and may be adjusted rightward for another string segment
    // if this attribute/color doesn't have enough 'length' to apply to all the text we want to draw.)
    COORD coordOffset = coordTarget;

    // The line segment we'll write will start at the beginning of the text.
    PCWCHAR pwsSegment = pwsLine;

    CharRow::const_iterator itSegment = it;

    do
    {
        // First retrieve the attribute that applies starting at the target position and the length for which it applies.
        size_t cAttrApplies = 0;
        const auto attr = Row.GetAttrRow().GetAttrByColumn(iFirstAttr + cchWritten, &cAttrApplies);

        // Set the brushes in GDI to this color
        LOG_IF_FAILED(_UpdateDrawingBrushes(pEngine, attr, false));

        // The segment we'll write is the shorter of the entire segment we want to draw or the amount of applicable color (Attr applies)
        size_t cchSegment = std::min(cchLine - cchWritten, cAttrApplies);

        if (cchSegment <= 0)
        {
            // If we ever have an invalid segment length, stop looping through the rendering.
            break;
        }

        // Draw the line via double-byte helper to strip duplicates
        LOG_IF_FAILED(_PaintBufferOutputDoubleByteHelper(pEngine, pwsSegment, itSegment, itEnd, cchSegment, coordOffset, lineWrapped));

        // Draw the grid shapes without the double-byte helper as they need to be exactly proportional to what's in the buffer
        if (_pData->IsGridLineDrawingAllowed())
        {
            // We're only allowed to draw the grid lines under certain circumstances.
            _PaintBufferOutputGridLineHelper(pEngine, attr, cchSegment, coordOffset);
        }

        // Update how much we've written.
        cchWritten += cchSegment;

        // Update the offset and text segment pointer by moving right by the previously written segment length
        coordOffset.X += (SHORT)cchSegment;
        pwsSegment += cchSegment;
        itSegment += cchSegment;

    } while (cchWritten < cchLine && itSegment < itEnd);
}

// Routine Description:
// - Paint helper for primary buffer output function.
// - This particular helper processes full-width (sometimes called double-wide or double-byte) characters. They're typically stored twice in the backing buffer to represent their width, so this function will help strip that down to one copy each as it's passed along to the final output.
// - See also: All related helpers and buffer output functions.
// Arguments:
// - pwsLine - Pointer to the first character in the string/substring to be drawn.
// - it - iterator pointing to the first grid cell in a sequence that is perfectly in sync with the pwsLine parameter. e.g. The first attribute here goes with the first character in pwsLine.
// - itEnd - corresponding end iterator to it
// - cchLine - The length of pwsLine and the minimum number of iterator steps.
// - coordTarget - The X/Y coordinate position in the buffer which we're attempting to start rendering from. pwsLine[0] will be the character at position coordTarget within the original console buffer before it was prepared for this function.
// Return Value:
// - S_OK or memory allocation error
[[nodiscard]]
HRESULT Renderer::_PaintBufferOutputDoubleByteHelper(_In_ IRenderEngine* const pEngine,
                                                     _In_reads_(cchLine) PCWCHAR const pwsLine,
                                                     const CharRow::const_iterator it,
                                                     const CharRow::const_iterator itEnd,
                                                     const size_t cchLine,
                                                     const COORD coordTarget,
                                                     const bool lineWrapped)
{
    // We need the ability to move the target back to the left slightly in case we start with a trailing byte character.
    COORD coordTargetAdjustable = coordTarget;
    bool fTrimLeft = false;

    // We need to filter out double-copies of characters that get introduced for full-width characters (sometimes "double-width" or erroneously "double-byte")
    wistd::unique_ptr<WCHAR[]> pwsSegment = wil::make_unique_nothrow<WCHAR[]>(cchLine);
    RETURN_IF_NULL_ALLOC(pwsSegment);

    // We will need to pass the expected widths along so characters can be spaced to fit.
    wistd::unique_ptr<unsigned char[]> rgSegmentWidth = wil::make_unique_nothrow<unsigned char[]>(cchLine);
    RETURN_IF_NULL_ALLOC(rgSegmentWidth);

    CharRow::const_iterator itCurrent = it;
    size_t cchSegment = 0;
    // Walk through the line given character by character and copy necessary items into our local array.
    for (size_t iLine = 0; iLine < cchLine && itCurrent < itEnd; ++iLine, ++itCurrent)
    {
        // skip copy of trailing bytes. we'll copy leading and single bytes into the final write array.
        if (!itCurrent->DbcsAttr().IsTrailing())
        {
            pwsSegment[cchSegment] = pwsLine[iLine];
            rgSegmentWidth[cchSegment] = 1;

            // If this is a leading byte, add 1 more to width as it is double wide
            if (itCurrent->DbcsAttr().IsLeading())
            {
                rgSegmentWidth[cchSegment] = 2;
            }

            cchSegment++;
        }
        else if (iLine == 0)
        {
            // If we are a trailing byte, but we're the first character in the run, it's a special case.
            // Someone has asked us to redraw the right half of the character, but we can't do that.
            // We'll have to draw the entire thing at once which requires:
            // 1. We have to copy the character into the segment buffer (which we normally don't do for trailing bytes)
            // 2. We have to back the draw target up by one character width so the right half will be struck over where we expect

            // Copy character
            pwsSegment[cchSegment] = pwsLine[iLine];

            // This character is double-width
            rgSegmentWidth[cchSegment] = 2;
            cchSegment++;

            // Move the target back one so we can strike left of what we want.
            coordTargetAdjustable.X--;

            // And set the flag so the engine will trim off the extra left half of the character
            // Clipping the left half of the character is important because leaving it there will interfere with the line drawing routines
            // which have no knowledge of the half/fullwidthness of characters and won't necessarily restrike the lines on the left half of the character.
            fTrimLeft = true;
        }
    }

    // Draw the line
    RETURN_IF_FAILED(pEngine->PaintBufferLine(pwsSegment.get(), rgSegmentWidth.get(), std::min(cchSegment, cchLine), coordTargetAdjustable, fTrimLeft, lineWrapped));

    return S_OK;
}

// Method Description:
// - Generates a IRenderEngine::GridLines structure from the values in the
//      provided textAttribute
// Arguments:
// - textAttribute: the TextAttribute to generate GridLines from.
// Return Value:
// - a GridLines containing all the gridline info from the TextAtribute
IRenderEngine::GridLines Renderer::s_GetGridlines(const TextAttribute& textAttribute) const noexcept
{
    // Convert console grid line representations into rendering engine enum representations.
    IRenderEngine::GridLines lines = IRenderEngine::GridLines::None;

    if (textAttribute.IsTopHorizontalDisplayed())
    {
        lines |= IRenderEngine::GridLines::Top;
    }

    if (textAttribute.IsBottomHorizontalDisplayed())
    {
        lines |= IRenderEngine::GridLines::Bottom;
    }

    if (textAttribute.IsLeftVerticalDisplayed())
    {
        lines |= IRenderEngine::GridLines::Left;
    }

    if (textAttribute.IsRightVerticalDisplayed())
    {
        lines |= IRenderEngine::GridLines::Right;
    }
    return lines;
}

// Routine Description:
// - Paint helper for primary buffer output function.
// - This particular helper sets up the various box drawing lines that can be inscribed around any character in the buffer (left, right, top, underline).
// - See also: All related helpers and buffer output functions.
// Arguments:
// - textAttribute - The line/box drawing attributes to use for this particular run.
// - cchLine - The length of both pwsLine and pbKAttrsLine.
// - coordTarget - The X/Y coordinate position in the buffer which we're attempting to start rendering from.
// Return Value:
// - <none>
void Renderer::_PaintBufferOutputGridLineHelper(_In_ IRenderEngine* const pEngine,
                                                const TextAttribute textAttribute,
                                                const size_t cchLine,
                                                const COORD coordTarget)
{
    const COLORREF rgb = _pData->GetForegroundColor(textAttribute);

    // Convert console grid line representations into rendering engine enum representations.
    IRenderEngine::GridLines lines = Renderer::s_GetGridlines(textAttribute);

    // Draw the lines
    LOG_IF_FAILED(pEngine->PaintBufferGridLines(lines, rgb, cchLine, coordTarget));
}

// Routine Description:
// - Paint helper to draw the cursor within the buffer.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::_PaintCursor(_In_ IRenderEngine* const pEngine)
{
    if (_pData->IsCursorVisible())
    {
        // Get cursor position in buffer
        COORD coordCursor = _pData->GetCursorPosition();
        // Adjust cursor to viewport
        Viewport view = _pData->GetViewport();
        view.ConvertToOrigin(&coordCursor);

        COLORREF cursorColor = _pData->GetCursorColor();
        bool useColor = cursorColor != INVALID_COLOR;
        // Draw it within the viewport
        LOG_IF_FAILED(pEngine->PaintCursor(coordCursor,
                                           _pData->GetCursorHeight(),
                                           _pData->IsCursorDoubleWidth(),
                                           _pData->GetCursorStyle(),
                                           useColor,
                                           cursorColor));
    }
}

// Routine Description:
// - Paint helper to draw the IME within the buffer.
// - This supports composition drawing area.
// Arguments:
// - AreaInfo - Special IME area screen buffer metadata
// - textBuffer - Text backing buffer for the special IME area.
// Return Value:
// - <none>
void Renderer::_PaintIme(_In_ IRenderEngine* const pEngine,
                         const ConversionAreaInfo& AreaInfo,
                         const TextBuffer& textBuffer)
{
    // If this conversion area isn't hidden (because it is off) or hidden for a scroll operation, then draw it.
    if (!AreaInfo.IsHidden())
    {
        // First get the screen buffer's viewport.
        Viewport view = _pData->GetViewport();

        // Now get the IME's viewport and adjust it to where it is supposed to be relative to the window.
        // The IME's buffer is typically only one row in size. Some segments are the whole row, some are only a partial row.
        // Then from those, there is a "view" much like there is a view into the main console buffer.
        // Use the "window" and "view" relative to the IME-specific special buffer to figure out the coordinates to draw at within the real console buffer.

        const auto placementInfo = AreaInfo.GetAreaBufferInfo();

        SMALL_RECT srCaView = placementInfo.rcViewCaWindow;
        srCaView.Top += placementInfo.coordConView.Y;
        srCaView.Bottom += placementInfo.coordConView.Y;
        srCaView.Left += placementInfo.coordConView.X;
        srCaView.Right += placementInfo.coordConView.X;

        // Set it up in a Viewport helper structure and trim it the IME viewport to be within the full console viewport.
        Viewport viewConv = Viewport::FromInclusive(srCaView);

        SMALL_RECT srDirty = pEngine->GetDirtyRectInChars();

        // Dirty is an inclusive rectangle, but oddly enough the IME was an exclusive one, so correct it.
        srDirty.Bottom++;
        srDirty.Right++;

        if (viewConv.TrimToViewport(&srDirty))
        {
            Viewport viewDirty = Viewport::FromInclusive(srDirty);

            for (SHORT iRow = viewDirty.Top(); iRow < viewDirty.BottomInclusive(); iRow++)
            {
                // Get row of text data
                const ROW& Row = textBuffer.GetRowByOffset(iRow - placementInfo.coordConView.Y);
                const CharRow& charRow = Row.GetCharRow();

                std::wstring rowText;
                CharRow::const_iterator it;
                try
                {
                    it = std::next(charRow.cbegin(), viewDirty.Left() - placementInfo.coordConView.X);
                    rowText = charRow.GetTextRaw();
                }
                catch (...)
                {
                    LOG_HR(wil::ResultFromCaughtException());
                    return;
                }
                const CharRow::const_iterator itEnd = charRow.cend();

                // Get the pointer to the beginning of the text
                const wchar_t* const pwsLine = rowText.c_str() + viewDirty.Left() - placementInfo.coordConView.X;

                size_t const cchLine = viewDirty.Width() - 1;

                // Calculate the target position in the buffer where we should start writing.
                COORD coordTarget;
                coordTarget.X = viewDirty.Left();
                coordTarget.Y = iRow;

                _PaintBufferOutputRasterFontHelper(pEngine, Row, pwsLine, it, itEnd, cchLine, viewDirty.Left() - placementInfo.coordConView.X, coordTarget, false);

            }
        }
    }
}

// Routine Description:
// - Paint helper to draw the composition string portion of the IME.
// - This specifically is the string that appears at the cursor on the input line showing what the user is currently typing.
// - See also: Generic Paint IME helper method.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::_PaintImeCompositionString(_In_ IRenderEngine* const pEngine)
{
    const ConsoleImeInfo* const pImeData = _pData->GetImeData();

    for (size_t i = 0; i < pImeData->ConvAreaCompStr.size(); i++)
    {
        const auto& AreaInfo = pImeData->ConvAreaCompStr[i];

        try
        {
            const TextBuffer& textBuffer = _pData->GetImeCompositionStringBuffer(i);
            _PaintIme(pEngine, AreaInfo, textBuffer);
        }
        CATCH_LOG();
    }
}

// Routine Description:
// - Paint helper to draw the selected area of the window.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Renderer::_PaintSelection(_In_ IRenderEngine* const pEngine)
{
    try
    {
        SMALL_RECT srDirty = pEngine->GetDirtyRectInChars();
        Viewport dirtyView = Viewport::FromInclusive(srDirty);

        // Get selection rectangles
        const auto rectangles = _GetSelectionRects();
        for (auto rect : rectangles)
        {
            if (dirtyView.TrimToViewport(&rect))
            {
                LOG_IF_FAILED(pEngine->PaintSelection(rect));
            }
        }
    }
    CATCH_LOG();
}

// Routine Description:
// - Helper to convert the text attributes to actual RGB colors and update the rendering pen/brush within the rendering engine before the next draw operation.
// Arguments:
// - textAttributes - The 16 color foreground/background combination to set
// - fIncludeBackground - Whether or not to include the hung window/erase window brushes in this operation. (Usually only happens when the default is changed, not when each individual color is swapped in a multi-color run.)
// Return Value:
// - <none>
[[nodiscard]]
HRESULT Renderer::_UpdateDrawingBrushes(_In_ IRenderEngine* const pEngine, const TextAttribute textAttributes, const bool fIncludeBackground)
{
    const COLORREF rgbForeground = _pData->GetForegroundColor(textAttributes);
    const COLORREF rgbBackground = _pData->GetBackgroundColor(textAttributes);
    const WORD legacyAttributes = textAttributes.GetLegacyAttributes();
    const bool isBold = textAttributes.IsBold();

    // The last color need's to be each engine's responsibility. If it's local to this function,
    //      then on the next engine we might not update the color.
    RETURN_IF_FAILED(pEngine->UpdateDrawingBrushes(rgbForeground, rgbBackground, legacyAttributes, isBold, fIncludeBackground));

    return S_OK;
}

// Routine Description:
// - Helper called before a majority of paint operations to scroll most of the previous frame into the appropriate
//   position before we paint the remaining invalid area.
// - Used to save drawing time/improve performance
// Arguments:
// - <none>
// Return Value:
// - <none>
[[nodiscard]]
HRESULT Renderer::_PerformScrolling(_In_ IRenderEngine* const pEngine)
{
    return pEngine->ScrollFrame();
}

// Routine Description:
// - Helper to determine the selected region of the buffer.
// Return Value:
// - A vector of rectangles representing the regions to select, line by line.
std::vector<SMALL_RECT> Renderer::_GetSelectionRects() const
{
    auto rects = _pData->GetSelectionRects();
    // Adjust rectangles to viewport
    Viewport view = _pData->GetViewport();

    for (auto& rect : rects)
    {
        rect = view.ConvertToOrigin(Viewport::FromInclusive(rect)).ToInclusive();

        // hopefully temporary, we should be receiving the right selection sizes without correction.
        rect.Right++;
        rect.Bottom++;
    }

    return rects;
}

// Method Description:
// - Adds another Render engine to this renderer. Future rendering calls will
//      also be sent to the new renderer.
// Arguments:
// - pEngine: The new render engine to be added
// Return Value:
// - <none>
// Throws if we ran out of memory or there was some other error appending the
//      engine to our collection.
void Renderer::AddRenderEngine(_In_ IRenderEngine* const pEngine)
{
    THROW_IF_NULL_ALLOC(pEngine);
    _rgpEngines.push_back(pEngine);
}
