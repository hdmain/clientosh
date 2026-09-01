#include "RdpClipboard.h"

#include "RdpFreeRdpIncludes.h"

#include <QByteArray>
#include <QString>

#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

constexpr unsigned int kChannelOk = 0;
constexpr unsigned int kUnicodeTextFormat = 13;
constexpr unsigned int kAnsiTextFormat = 1;

#if defined(_WIN32)
unsigned int clipboardSequence()
{
    return GetClipboardSequenceNumber();
}

bool openClipboard()
{
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        Sleep(10);
    }
    return false;
}
#endif

bool formatListHasText(const CLIPRDR_FORMAT_LIST* formatList, unsigned int* formatIdOut)
{
    if (!formatList || !formatIdOut) {
        return false;
    }

    unsigned int best = 0;
    for (UINT32 i = 0; i < formatList->numFormats; ++i) {
        const CLIPRDR_FORMAT* format = &formatList->formats[i];
        if (format->formatId == kUnicodeTextFormat) {
            *formatIdOut = kUnicodeTextFormat;
            return true;
        }
        if (format->formatId == kAnsiTextFormat && best == 0) {
            best = kAnsiTextFormat;
        }
        if (format->formatName) {
            if (_stricmp(format->formatName, "UTF8_STRING") == 0
                || _stricmp(format->formatName, "text/plain") == 0) {
                best = format->formatId;
            }
        }
    }

    if (best != 0) {
        *formatIdOut = best;
        return true;
    }
    return false;
}

RdpClipboard* clipboardFromContext(CliprdrClientContext* context)
{
    return context ? static_cast<RdpClipboard*>(context->custom) : nullptr;
}

UINT monitorReady(CliprdrClientContext* context, const CLIPRDR_MONITOR_READY* monitorReady)
{
    Q_UNUSED(monitorReady)
    RdpClipboard* clipboard = clipboardFromContext(context);
    if (!clipboard) {
        return ERROR_INTERNAL_ERROR;
    }
    return clipboard->handleMonitorReady();
}

UINT serverCapabilities(CliprdrClientContext* context, const CLIPRDR_CAPABILITIES* capabilities)
{
    Q_UNUSED(capabilities)
    Q_UNUSED(context)
    return kChannelOk;
}

UINT serverFormatList(CliprdrClientContext* context, const CLIPRDR_FORMAT_LIST* formatList)
{
    RdpClipboard* clipboard = clipboardFromContext(context);
    if (!clipboard) {
        return ERROR_INTERNAL_ERROR;
    }
    return clipboard->handleServerFormatList(formatList);
}

UINT serverFormatListResponse(CliprdrClientContext* context,
                              const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
    Q_UNUSED(context)
    if (formatListResponse && (formatListResponse->common.msgFlags & CB_RESPONSE_FAIL) != 0) {
        return kChannelOk;
    }
    return kChannelOk;
}

UINT serverFormatDataRequest(CliprdrClientContext* context,
                             const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
    RdpClipboard* clipboard = clipboardFromContext(context);
    if (!clipboard) {
        return ERROR_INTERNAL_ERROR;
    }
    return clipboard->handleServerFormatDataRequest(formatDataRequest);
}

UINT serverFormatDataResponse(CliprdrClientContext* context,
                              const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
    RdpClipboard* clipboard = clipboardFromContext(context);
    if (!clipboard) {
        return ERROR_INTERNAL_ERROR;
    }
    return clipboard->handleServerFormatDataResponse(formatDataResponse);
}

} // namespace

unsigned int RdpClipboard::handleMonitorReady()
{
    m_sync = true;
    const unsigned int rc = sendCapabilities();
    if (rc != kChannelOk) {
        return rc;
    }
    return sendFormatList();
}

unsigned int RdpClipboard::handleServerFormatList(const void* formatListPtr)
{
    const auto* formatList = static_cast<const CLIPRDR_FORMAT_LIST*>(formatListPtr);
    if (!formatList) {
        return ERROR_INTERNAL_ERROR;
    }

    unsigned int remoteFormat = 0;
    const bool hasText = formatListHasText(formatList, &remoteFormat);
    const unsigned int rc = sendFormatListResponse(true);
    if (rc != kChannelOk || !hasText) {
        return rc;
    }
    return requestRemoteFormat(remoteFormat);
}

unsigned int RdpClipboard::handleServerFormatDataRequest(const void* formatDataRequestPtr)
{
    const auto* formatDataRequest =
        static_cast<const CLIPRDR_FORMAT_DATA_REQUEST*>(formatDataRequestPtr);
    if (!formatDataRequest) {
        return ERROR_INTERNAL_ERROR;
    }

    QByteArray data;
    if (!readLocalUnicodeText(&data)) {
        return sendFormatDataResponse(nullptr, 0, false);
    }
    return sendFormatDataResponse(reinterpret_cast<const unsigned char*>(data.constData()),
                                  static_cast<unsigned int>(data.size()), true);
}

unsigned int RdpClipboard::handleServerFormatDataResponse(const void* formatDataResponsePtr)
{
    const auto* formatDataResponse =
        static_cast<const CLIPRDR_FORMAT_DATA_RESPONSE*>(formatDataResponsePtr);
    if (!formatDataResponse) {
        return ERROR_INTERNAL_ERROR;
    }

    if ((formatDataResponse->common.msgFlags & CB_RESPONSE_OK) == 0
        || formatDataResponse->common.dataLen == 0
        || !formatDataResponse->requestedFormatData) {
        m_pendingRemoteFormat = 0;
        return kChannelOk;
    }

    setLocalUnicodeText(formatDataResponse->requestedFormatData,
                        formatDataResponse->common.dataLen);
    m_pendingRemoteFormat = 0;
    return kChannelOk;
}

void RdpClipboard::attach(void* cliprdrContext)
{
    auto* context = static_cast<CliprdrClientContext*>(cliprdrContext);
    if (!context) {
        return;
    }
    m_context = context;
    context->custom = this;
    context->MonitorReady = monitorReady;
    context->ServerCapabilities = serverCapabilities;
    context->ServerFormatList = serverFormatList;
    context->ServerFormatListResponse = serverFormatListResponse;
    context->ServerFormatDataRequest = serverFormatDataRequest;
    context->ServerFormatDataResponse = serverFormatDataResponse;
#if defined(_WIN32)
    m_lastSequence = clipboardSequence();
#endif
}

void RdpClipboard::detach()
{
    auto* context = static_cast<CliprdrClientContext*>(m_context);
    if (context) {
        context->MonitorReady = nullptr;
        context->ServerCapabilities = nullptr;
        context->ServerFormatList = nullptr;
        context->ServerFormatListResponse = nullptr;
        context->ServerFormatDataRequest = nullptr;
        context->ServerFormatDataResponse = nullptr;
        context->custom = nullptr;
    }
    m_context = nullptr;
    m_sync = false;
    m_pendingRemoteFormat = 0;
}

void RdpClipboard::pollLocalChanges()
{
#if defined(_WIN32)
    if (!m_sync || !m_context || m_updatingLocal) {
        return;
    }
    const unsigned int seq = clipboardSequence();
    if (seq == m_lastSequence) {
        return;
    }
    m_lastSequence = seq;
    sendFormatList();
#endif
}

unsigned int RdpClipboard::sendCapabilities()
{
    auto* context = static_cast<CliprdrClientContext*>(m_context);
    if (!context || !context->ClientCapabilities) {
        return ERROR_INTERNAL_ERROR;
    }

    CLIPRDR_GENERAL_CAPABILITY_SET general = {};
    general.capabilitySetType = CB_CAPSTYPE_GENERAL;
    general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    general.version = CB_CAPS_VERSION_2;
    general.generalFlags = CB_USE_LONG_FORMAT_NAMES;

    CLIPRDR_CAPABILITIES capabilities = {};
    capabilities.common.msgType = CB_CLIP_CAPS;
    capabilities.cCapabilitiesSets = 1;
    capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
    return context->ClientCapabilities(context, &capabilities);
}

unsigned int RdpClipboard::sendFormatList()
{
    auto* context = static_cast<CliprdrClientContext*>(m_context);
    if (!context || !context->ClientFormatList) {
        return ERROR_INTERNAL_ERROR;
    }

    CLIPRDR_FORMAT formats[2] = {};
    UINT32 count = 0;

#if defined(_WIN32)
    if (openClipboard()) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            formats[count].formatId = CF_UNICODETEXT;
            ++count;
        } else if (IsClipboardFormatAvailable(CF_TEXT)) {
            formats[count].formatId = CF_TEXT;
            ++count;
        }
        CloseClipboard();
    }
#endif

    CLIPRDR_FORMAT_LIST formatList = {};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.numFormats = count;
    formatList.formats = count > 0 ? formats : nullptr;
    return context->ClientFormatList(context, &formatList);
}

unsigned int RdpClipboard::sendFormatListResponse(bool ok)
{
    auto* context = static_cast<CliprdrClientContext*>(m_context);
    if (!context || !context->ClientFormatListResponse) {
        return ERROR_INTERNAL_ERROR;
    }

    CLIPRDR_FORMAT_LIST_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    return context->ClientFormatListResponse(context, &response);
}

unsigned int RdpClipboard::requestRemoteFormat(unsigned int formatId)
{
    auto* context = static_cast<CliprdrClientContext*>(m_context);
    if (!context || !context->ClientFormatDataRequest || formatId == 0) {
        return ERROR_INTERNAL_ERROR;
    }

    CLIPRDR_FORMAT_DATA_REQUEST request = {};
    request.common.msgType = CB_FORMAT_DATA_REQUEST;
    request.requestedFormatId = formatId;
    m_pendingRemoteFormat = formatId;
    return context->ClientFormatDataRequest(context, &request);
}

unsigned int RdpClipboard::sendFormatDataResponse(const unsigned char* data, unsigned int size,
                                                  bool ok)
{
    auto* context = static_cast<CliprdrClientContext*>(m_context);
    if (!context || !context->ClientFormatDataResponse) {
        return ERROR_INTERNAL_ERROR;
    }

    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = ok ? size : 0;
    response.requestedFormatData = ok ? data : nullptr;
    return context->ClientFormatDataResponse(context, &response);
}

bool RdpClipboard::readLocalUnicodeText(QByteArray* out) const
{
    if (!out) {
        return false;
    }
    out->clear();

#if defined(_WIN32)
    if (!openClipboard()) {
        return false;
    }

    bool ok = false;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(handle))) {
            const int chars = static_cast<int>(wcslen(text));
            if (chars >= 0) {
                const int bytes = (chars + 1) * static_cast<int>(sizeof(wchar_t));
                out->resize(bytes);
                memcpy(out->data(), text, static_cast<size_t>(bytes));
                ok = true;
            }
            GlobalUnlock(handle);
        }
    } else if (HANDLE handle = GetClipboardData(CF_TEXT)) {
        if (const auto* text = static_cast<const char*>(GlobalLock(handle))) {
            const QString converted = QString::fromLocal8Bit(text);
            const std::wstring wide = converted.toStdWString();
            const int bytes = static_cast<int>((wide.size() + 1) * sizeof(wchar_t));
            out->resize(bytes);
            memcpy(out->data(), wide.c_str(), static_cast<size_t>(bytes));
            ok = true;
        }
    }

    CloseClipboard();
    return ok;
#else
    return false;
#endif
}

bool RdpClipboard::setLocalUnicodeText(const unsigned char* data, unsigned int size)
{
#if defined(_WIN32)
    if (!data || size < sizeof(wchar_t)) {
        return false;
    }
    if (!openClipboard()) {
        return false;
    }

    const size_t wcharBytes = size - (size % sizeof(wchar_t));
    const size_t wcharCount = wcharBytes / sizeof(wchar_t);
    if (wcharCount == 0) {
        CloseClipboard();
        return false;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, wcharBytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }

    auto* dest = static_cast<wchar_t*>(GlobalLock(memory));
    if (!dest) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(dest, data, wcharBytes);
    if (dest[wcharCount - 1] != L'\0') {
        dest[wcharCount - 1] = L'\0';
    }
    GlobalUnlock(memory);

    m_updatingLocal = true;
    const bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!ok) {
        GlobalFree(memory);
    }
    m_lastSequence = clipboardSequence();
    m_updatingLocal = false;
    CloseClipboard();
    return ok;
#else
    Q_UNUSED(data)
    Q_UNUSED(size)
    return false;
#endif
}
