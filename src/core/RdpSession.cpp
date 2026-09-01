#include "RdpSession.h"

#include "RdpFreeRdpIncludes.h"
#include "RdpClipboard.h"

#include <QByteArray>
#include <QVector>

#include <cstring>

namespace {

thread_local RdpSession* g_activeRdpSession = nullptr;

struct ClientoshRdpContext {
    rdpClientContext client;
    RdpSession* owner = nullptr;
    RdpClipboard clipboard;

    void onEndPaint(rdpContext* context)
    {
        if (owner) {
            owner->handleEndPaint(context);
        }
    }

    void onDesktopResize(rdpContext* context)
    {
        if (owner) {
            owner->handleDesktopResize(context);
        }
    }
};

static ClientoshRdpContext* contextFromRdp(rdpContext* context)
{
    return reinterpret_cast<ClientoshRdpContext*>(context);
}

static DWORD client_verify_certificate_ex(freerdp* /*instance*/, const char* /*host*/, UINT16 /*port*/,
                                          const char* /*common_name*/, const char* /*subject*/,
                                          const char* /*issuer*/, const char* /*fingerprint*/,
                                          DWORD /*flags*/)
{
    return 1;
}

static BOOL client_begin_paint(rdpContext* context)
{
    rdpGdi* gdi = context ? context->gdi : nullptr;
    if (!gdi || !gdi->primary || !gdi->primary->hdc || !gdi->primary->hdc->hwnd
        || !gdi->primary->hdc->hwnd->invalid) {
        return TRUE;
    }
    gdi->primary->hdc->hwnd->invalid->null = TRUE;
    return TRUE;
}

static BOOL client_end_paint(rdpContext* context)
{
    if (!context) {
        return TRUE;
    }
    if (ClientoshRdpContext* wrapper = contextFromRdp(context)) {
        wrapper->onEndPaint(context);
    }
    return TRUE;
}

static BOOL client_desktop_resize(rdpContext* context)
{
    if (!context) {
        return FALSE;
    }
    if (ClientoshRdpContext* wrapper = contextFromRdp(context)) {
        wrapper->onDesktopResize(context);
    }
    rdpGdi* gdi = context->gdi;
    rdpSettings* settings = context->settings;
    if (!gdi || !settings) {
        return FALSE;
    }
    return gdi_resize(gdi, freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
                      freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight));
}

static void client_channel_connected(void* context, const ChannelConnectedEventArgs* e);
static void client_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e);

static BOOL client_pre_connect(freerdp* instance)
{
    if (!instance || !instance->context || !instance->context->settings) {
        return FALSE;
    }
    rdpSettings* settings = instance->context->settings;
    freerdp_settings_set_bool(settings, FreeRDP_CertificateCallbackPreferPEM, TRUE);

    if (PubSub_SubscribeChannelConnected(instance->context->pubSub, client_channel_connected) < 0) {
        return FALSE;
    }
    if (PubSub_SubscribeChannelDisconnected(instance->context->pubSub, client_channel_disconnected)
        < 0) {
        return FALSE;
    }
    return TRUE;
}

static void client_channel_connected(void* context, const ChannelConnectedEventArgs* e)
{
    freerdp_client_OnChannelConnectedEventHandler(context, e);
    if (!context || !e || !e->name) {
        return;
    }
    if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) != 0) {
        return;
    }
    auto* wrapper = contextFromRdp(static_cast<rdpContext*>(context));
    if (!wrapper) {
        return;
    }
    wrapper->clipboard.attach(e->pInterface);
}

static void client_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e)
{
    freerdp_client_OnChannelDisconnectedEventHandler(context, e);
    if (!context || !e || !e->name) {
        return;
    }
    if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) != 0) {
        return;
    }
    auto* wrapper = contextFromRdp(static_cast<rdpContext*>(context));
    if (!wrapper) {
        return;
    }
    wrapper->clipboard.detach();
}

static BOOL client_post_connect(freerdp* instance)
{
    if (!instance || !instance->context || !instance->context->update) {
        return FALSE;
    }
    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
        return FALSE;
    }
    rdpContext* context = instance->context;
    context->update->BeginPaint = client_begin_paint;
    context->update->EndPaint = client_end_paint;
    context->update->DesktopResize = client_desktop_resize;
    return TRUE;
}

static void client_post_disconnect(freerdp* instance)
{
    if (!instance) {
        return;
    }
    if (instance->context && instance->context->pubSub) {
        PubSub_UnsubscribeChannelConnected(instance->context->pubSub, client_channel_connected);
        PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub,
                                              client_channel_disconnected);
    }
    gdi_free(instance);
}

static BOOL client_new(freerdp* instance, rdpContext* context)
{
    if (!instance || !context) {
        return FALSE;
    }
    auto* wrapper = contextFromRdp(context);
    wrapper->owner = g_activeRdpSession;
    instance->PreConnect = client_pre_connect;
    instance->PostConnect = client_post_connect;
    instance->PostDisconnect = client_post_disconnect;
    instance->VerifyCertificateEx = client_verify_certificate_ex;
    return TRUE;
}

static void client_free(freerdp* /*instance*/, rdpContext* context)
{
    if (!context) {
        return;
    }
    auto* wrapper = contextFromRdp(context);
    wrapper->owner = nullptr;
}

static void applyHighQualitySettings(rdpSettings* settings, int width, int height,
                                     int deviceScalePercent)
{
    const int evenWidth = qMax(640, width) & ~1;
    const int evenHeight = qMax(480, height) & ~1;
    const int scale = qBound(100, deviceScalePercent, 500);

    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, static_cast<UINT32>(evenWidth));
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, static_cast<UINT32>(evenHeight));
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopScaleFactor, static_cast<UINT32>(scale));
    freerdp_settings_set_uint32(settings, FreeRDP_DeviceScaleFactor, static_cast<UINT32>(scale));
    freerdp_settings_set_uint32(settings, FreeRDP_CompressionLevel, 0);

    freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SmartSizing, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_CompressionEnabled, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_BitmapCompressionDisabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_BitmapCacheEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AsyncUpdate, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AsyncChannels, TRUE);

    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxSendQoeAck, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, FALSE);

    freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_ClipboardFeatureMask, CLIPRDR_FLAG_DEFAULT_MASK);
    freerdp_settings_set_bool(settings, FreeRDP_GrabKeyboard, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GrabMouse, FALSE);
}

static BOOL parseCommandLine(rdpSettings* settings, const QStringList& args, QString* errorOut)
{
    QVector<QByteArray> storage;
    storage.reserve(args.size() + 1);
    QVector<char*> argv;
    argv.reserve(args.size() + 2);
    storage.push_back(QByteArray("clientosh-rdp"));
    argv.push_back(storage.last().data());
    for (const QString& arg : args) {
        storage.push_back(arg.toLocal8Bit());
        argv.push_back(storage.last().data());
    }
    argv.push_back(nullptr);

    const int status = freerdp_client_settings_parse_command_line_arguments(
        settings, static_cast<int>(argv.size()) - 1, argv.data(), FALSE);
    if (status != 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("invalid RDP settings (code %1)").arg(status);
        }
        return FALSE;
    }
    return TRUE;
}

static BOOL applyCommandLineSettings(rdpSettings* settings, const SessionProfile& profile,
                                     int width, int height, int deviceScalePercent,
                                     QString* errorOut)
{
    const int port = profile.port > 0 ? profile.port : 3389;
    const int evenWidth = qMax(640, width) & ~1;
    const int evenHeight = qMax(480, height) & ~1;

    QStringList baseArgs;
    baseArgs << QStringLiteral("/v:%1:%2").arg(profile.host.trimmed()).arg(port);
    if (!profile.user.trimmed().isEmpty()) {
        baseArgs << QStringLiteral("/u:%1").arg(profile.user.trimmed());
    }
    if (!profile.password.isEmpty()) {
        baseArgs << QStringLiteral("/p:%1").arg(profile.password);
    }
    if (!profile.rdpDomain.trimmed().isEmpty()) {
        baseArgs << QStringLiteral("/d:%1").arg(profile.rdpDomain.trimmed());
    }
    baseArgs << QStringLiteral("/size:%1x%2").arg(evenWidth).arg(evenHeight);
    baseArgs << QStringLiteral("/bpp:32");
    baseArgs << QStringLiteral("/gdi:sw");
    baseArgs << QStringLiteral("/cert:ignore");

    QStringList qualityArgs = baseArgs;
    qualityArgs << QStringLiteral("/network:auto");
    qualityArgs << QStringLiteral("/multitransport");
    qualityArgs << QStringLiteral("/gfx:AVC444");
    qualityArgs << QStringLiteral("/gfx-h264:AVC444");
    qualityArgs << QStringLiteral("/rfx");
    qualityArgs << QStringLiteral("+fonts");
    qualityArgs << QStringLiteral("-compression");
    qualityArgs << QStringLiteral("+clipboard");
    qualityArgs << QStringLiteral("/clipboard:direction-to:all");
    qualityArgs << QStringLiteral("+grab-keyboard");

    QString parseError;
    if (!parseCommandLine(settings, qualityArgs, &parseError)
        && !parseCommandLine(settings, baseArgs, errorOut)) {
        return FALSE;
    }

    applyHighQualitySettings(settings, evenWidth, evenHeight, deviceScalePercent);
    return TRUE;
}

} // namespace

RdpSession::RdpSession(QObject* parent)
    : QThread(parent)
{
}

RdpSession::~RdpSession()
{
    stopSession();
    wait(5000);
}

void RdpSession::startSession(const SessionProfile& profile, const QSize& desktopSize,
                              int deviceScalePercent)
{
    {
        QMutexLocker lock(&m_mutex);
        m_profile = profile;
        m_desktopSize = desktopSize.isValid() ? desktopSize : QSize(1280, 720);
        m_deviceScalePercent = qBound(100, deviceScalePercent, 500);
        m_stopRequested = false;
        m_connected = false;
        m_instance = nullptr;
    }
    if (isRunning()) {
        wait(3000);
    }
    start();
}

void RdpSession::stopSession()
{
    freerdp* instance = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        m_stopRequested = true;
        instance = static_cast<freerdp*>(m_instance);
    }
    if (instance && instance->context) {
        freerdp_abort_connect_context(instance->context);
    }
}

bool RdpSession::isConnected() const
{
    QMutexLocker lock(&m_mutex);
    return m_connected;
}

void RdpSession::sendMouseEvent(quint16 flags, int x, int y)
{
    QMutexLocker lock(&m_mutex);
    if (!m_connected || !m_instance) {
        return;
    }
    auto* instance = static_cast<freerdp*>(m_instance);
    if (!instance->context || !instance->context->input) {
        return;
    }
    freerdp_input_send_mouse_event(instance->context->input, flags,
                                   static_cast<UINT16>(qBound(0, x, 65535)),
                                   static_cast<UINT16>(qBound(0, y, 65535)));
}

void RdpSession::sendWheelEvent(quint16 flags)
{
    QMutexLocker lock(&m_mutex);
    if (!m_connected || !m_instance) {
        return;
    }
    auto* instance = static_cast<freerdp*>(m_instance);
    if (!instance->context) {
        return;
    }
    auto* wrapper = reinterpret_cast<ClientoshRdpContext*>(instance->context);
    freerdp_client_send_wheel_event(&wrapper->client, flags);
}

void RdpSession::sendUnicodeChar(QChar ch, bool down)
{
    QMutexLocker lock(&m_mutex);
    if (!m_connected || !m_instance) {
        return;
    }
    auto* instance = static_cast<freerdp*>(m_instance);
    if (!instance->context || !instance->context->input) {
        return;
    }
    const UINT16 flag = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;
    freerdp_input_send_unicode_keyboard_event(instance->context->input, flag,
                                              static_cast<UINT16>(ch.unicode()));
}

void RdpSession::sendScanCode(quint32 scanCode, bool down)
{
    QMutexLocker lock(&m_mutex);
    if (!m_connected || !m_instance) {
        return;
    }
    auto* instance = static_cast<freerdp*>(m_instance);
    if (!instance->context || !instance->context->input) {
        return;
    }
    freerdp_input_send_keyboard_event_ex(instance->context->input, down ? TRUE : FALSE, FALSE,
                                         scanCode);
}

void RdpSession::sendFocusIn(quint16 toggleStates)
{
    QMutexLocker lock(&m_mutex);
    if (!m_connected || !m_instance) {
        return;
    }
    auto* instance = static_cast<freerdp*>(m_instance);
    if (!instance->context || !instance->context->input) {
        return;
    }
    freerdp_input_send_focus_in_event(instance->context->input, toggleStates);
}

void RdpSession::sendSecureAttentionSequence()
{
    QMutexLocker lock(&m_mutex);
    if (!m_connected || !m_instance) {
        return;
    }
    auto* instance = static_cast<freerdp*>(m_instance);
    if (!instance->context || !instance->context->input) {
        return;
    }
    rdpInput* input = instance->context->input;
    freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LCONTROL);
    freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LMENU);
    freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_DELETE);
    freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_DELETE);
    freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LMENU);
    freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LCONTROL);
}

void RdpSession::handleEndPaint(void* ctx)
{
    auto* context = static_cast<rdpContext*>(ctx);
    rdpGdi* gdi = context ? context->gdi : nullptr;
    if (!gdi || !gdi->primary_buffer || gdi->width <= 0 || gdi->height <= 0) {
        return;
    }

    HGDI_DC hdc = gdi->primary ? gdi->primary->hdc : nullptr;
    if (hdc && hdc->hwnd && hdc->hwnd->invalid && hdc->hwnd->invalid->null) {
        return;
    }

    const int width = gdi->width;
    const int height = gdi->height;
    const UINT32 srcFormat = gdi->dstFormat ? gdi->dstFormat : PIXEL_FORMAT_BGRA32;

    QImage image(width, height, QImage::Format_ARGB32);
    if (image.isNull()) {
        return;
    }

    if (srcFormat == PIXEL_FORMAT_BGRA32 && gdi->stride == static_cast<UINT32>(image.bytesPerLine())) {
        memcpy(image.bits(), gdi->primary_buffer,
               static_cast<size_t>(image.bytesPerLine()) * static_cast<size_t>(height));
    } else if (!freerdp_image_copy_no_overlap(
                   image.bits(), PIXEL_FORMAT_BGRA32, static_cast<UINT32>(image.bytesPerLine()), 0,
                   0, static_cast<UINT32>(width), static_cast<UINT32>(height), gdi->primary_buffer,
                   srcFormat, gdi->stride, 0, 0, nullptr, FREERDP_FLIP_NONE)) {
        return;
    }
    emit frameReady(image);
}

void RdpSession::handleDesktopResize(void* /*ctx*/)
{
    emit statusChanged(QStringLiteral("remote desktop resized"));
}

void RdpSession::run()
{
    g_activeRdpSession = this;
    emit statusChanged(QStringLiteral("connecting..."));

    SessionProfile profile;
    QSize desktopSize;
    int deviceScalePercent = 100;
    {
        QMutexLocker lock(&m_mutex);
        profile = m_profile;
        desktopSize = m_desktopSize;
        deviceScalePercent = m_deviceScalePercent;
    }

    RDP_CLIENT_ENTRY_POINTS ep{};
    ep.Size = sizeof(RDP_CLIENT_ENTRY_POINTS);
    ep.Version = RDP_CLIENT_INTERFACE_VERSION;
    ep.ContextSize = sizeof(ClientoshRdpContext);
    ep.ClientNew = client_new;
    ep.ClientFree = client_free;

    rdpContext* context = freerdp_client_context_new(&ep);
    if (!context || !context->instance || !context->settings) {
        emit errorOccurred(QStringLiteral("failed to create RDP client context"));
        g_activeRdpSession = nullptr;
        return;
    }

    freerdp* instance = context->instance;
    {
        QMutexLocker lock(&m_mutex);
        m_instance = instance;
    }

    QString settingsError;
    if (!applyCommandLineSettings(context->settings, profile, desktopSize.width(),
                                  desktopSize.height(), deviceScalePercent, &settingsError)) {
        emit errorOccurred(settingsError.isEmpty() ? QStringLiteral("failed to configure RDP session")
                                                   : settingsError);
        freerdp_client_context_free(context);
        {
            QMutexLocker lock(&m_mutex);
            m_instance = nullptr;
        }
        g_activeRdpSession = nullptr;
        return;
    }

    if (freerdp_client_start(context) != 0) {
        emit errorOccurred(QStringLiteral("failed to start RDP client"));
        freerdp_client_context_free(context);
        {
            QMutexLocker lock(&m_mutex);
            m_instance = nullptr;
        }
        g_activeRdpSession = nullptr;
        return;
    }

    const BOOL ok = freerdp_connect(instance);
    if (!ok) {
        const UINT32 err = freerdp_get_last_error(context);
        emit errorOccurred(QStringLiteral("RDP connect failed (0x%1)").arg(err, 0, 16));
        freerdp_client_stop(context);
        freerdp_client_context_free(context);
        {
            QMutexLocker lock(&m_mutex);
            m_instance = nullptr;
        }
        g_activeRdpSession = nullptr;
        return;
    }

    {
        QMutexLocker lock(&m_mutex);
        m_connected = true;
    }
    emit connected();
    emit statusChanged(QStringLiteral("connected"));

    while (true) {
        bool stop = false;
        {
            QMutexLocker lock(&m_mutex);
            stop = m_stopRequested;
        }
        if (stop || freerdp_shall_disconnect_context(context)) {
            break;
        }

        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        const DWORD count = freerdp_get_event_handles(context, handles, ARRAYSIZE(handles));
        if (count == 0) {
            break;
        }
        const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
        if (status == WAIT_FAILED) {
            break;
        }
        if (!freerdp_check_event_handles(context)) {
            break;
        }
        if (ClientoshRdpContext* wrapper = contextFromRdp(context)) {
            wrapper->clipboard.pollLocalChanges();
        }
    }

    freerdp_disconnect(instance);
    freerdp_client_stop(context);
    freerdp_client_context_free(context);

    {
        QMutexLocker lock(&m_mutex);
        m_connected = false;
        m_instance = nullptr;
    }
    g_activeRdpSession = nullptr;
    emit disconnected();
    emit statusChanged(QStringLiteral("disconnected"));
}
