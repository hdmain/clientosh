#pragma once

class QByteArray;

/** Bidirectional text clipboard bridge for an RDP cliprdr channel. */
class RdpClipboard
{
public:
    void attach(void* cliprdrContext);
    void detach();
    void pollLocalChanges();

    unsigned int handleMonitorReady();
    unsigned int handleServerFormatList(const void* formatList);
    unsigned int handleServerFormatDataRequest(const void* formatDataRequest);
    unsigned int handleServerFormatDataResponse(const void* formatDataResponse);

private:
    unsigned int sendCapabilities();
    unsigned int sendFormatList();
    unsigned int sendFormatListResponse(bool ok);
    unsigned int requestRemoteFormat(unsigned int formatId);
    unsigned int sendFormatDataResponse(const unsigned char* data, unsigned int size, bool ok);
    bool setLocalUnicodeText(const unsigned char* data, unsigned int size);
    bool readLocalUnicodeText(QByteArray* out) const;

    void* m_context = nullptr;
    bool m_sync = false;
    bool m_updatingLocal = false;
    unsigned int m_lastSequence = 0;
    unsigned int m_pendingRemoteFormat = 0;
};
