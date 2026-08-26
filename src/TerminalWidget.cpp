#include "TerminalWidget.h"

#include "PlatformFonts.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QInputEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>

namespace {
bool isCsiFinal(unsigned char c)
{
    return c >= 0x40 && c <= 0x7e;
}

QVector<int> parseParams(const QByteArray& params)
{
    QVector<int> out;
    if (params.isEmpty()) {
        out.append(0);
        return out;
    }

    int value = 0;
    bool hasDigit = false;
    for (unsigned char c : params) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            hasDigit = true;
        } else if (c == ';' || c == ':') {
            // ':' is the ISO 8613-6 subparameter separator (38:2:R:G:B, 38:5:N)
            out.append(hasDigit ? value : 0);
            value = 0;
            hasDigit = false;
        }
    }
    out.append(hasDigit ? value : 0);
    return out;
}

int utf8ExpectedLength(unsigned char lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xe0) == 0xc0) {
        return 2;
    }
    if ((lead & 0xf0) == 0xe0) {
        return 3;
    }
    if ((lead & 0xf8) == 0xf0) {
        return 4;
    }
    return -1;
}

bool decodeUtf8(const QByteArray& bytes, char32_t* out)
{
    const int n = bytes.size();
    if (n <= 0) {
        return false;
    }
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.constData());
    if (n == 1) {
        *out = b[0];
        return true;
    }
    if (n == 2 && (b[1] & 0xc0) == 0x80) {
        *out = ((b[0] & 0x1f) << 6) | (b[1] & 0x3f);
        return true;
    }
    if (n == 3 && (b[1] & 0xc0) == 0x80 && (b[2] & 0xc0) == 0x80) {
        *out = ((b[0] & 0x0f) << 12) | ((b[1] & 0x3f) << 6) | (b[2] & 0x3f);
        return true;
    }
    if (n == 4 && (b[1] & 0xc0) == 0x80 && (b[2] & 0xc0) == 0x80 && (b[3] & 0xc0) == 0x80) {
        *out = ((b[0] & 0x07) << 18) | ((b[1] & 0x3f) << 12) | ((b[2] & 0x3f) << 6) | (b[3] & 0x3f);
        return true;
    }
    return false;
}
} // namespace

TerminalWidget::TerminalWidget(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setMouseTracking(false);
    setCursor(Qt::IBeamCursor);

    setFont(clientoshMonospaceFont(AppSettings::fontSize(), AppSettings::fontFamily()));

    m_defaultFg = AppSettings::terminalFg();
    m_defaultBg = AppSettings::terminalBg();

    m_vScroll = new QScrollBar(Qt::Vertical, this);
    m_vScroll->setObjectName(QStringLiteral("termScrollBar"));
    m_vScroll->setFocusPolicy(Qt::NoFocus);
    m_vScroll->setCursor(Qt::ArrowCursor);
    m_vScroll->hide();
    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_updatingScrollBar) {
            return;
        }
        setViewOffset(maxViewOffset() - value);
    });

    resetAttributes();
    ensureGrid(m_cols, m_rows);
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_g0 = Charset::Ascii;
    m_g1 = Charset::DecSpecial;
}

void TerminalWidget::resetAttributes()
{
    m_attr = Cell{};
    m_attr.fg = m_defaultFg.rgb();
    m_attr.bg = m_defaultBg.rgb();
}

TerminalWidget::Cell TerminalWidget::makeBlankCell() const
{
    Cell c;
    c.fg = m_defaultFg.rgb();
    c.bg = m_defaultBg.rgb();
    return c;
}

void TerminalWidget::remapDefaultColors(const QColor& oldFg, const QColor& oldBg, const QColor& newFg,
                                        const QColor& newBg)
{
    auto remap = [&](Cell& cell) {
        const QRgb oFg = oldFg.rgb();
        const QRgb oBg = oldBg.rgb();
        if (cell.fg == oFg) {
            cell.fg = newFg.rgb();
        }
        if (cell.bg == oBg) {
            cell.bg = newBg.rgb();
        }
    };
    for (Cell& cell : m_cells) {
        remap(cell);
    }
    for (Cell& cell : m_altCells) {
        remap(cell);
    }
    for (Cell& cell : m_mainCells) {
        remap(cell);
    }
    for (QVector<Cell>& line : m_scrollback) {
        for (Cell& cell : line) {
            remap(cell);
        }
    }
    remap(m_attr);
    remap(m_savedAttr);
    remap(m_decSavedAttr);
}

void TerminalWidget::applyAppearanceFromSettings()
{
    const QColor newFg = AppSettings::terminalFg();
    const QColor newBg = AppSettings::terminalBg();

    const QString bgImagePath = AppSettings::terminalBgImage();
    const int newBlur = AppSettings::terminalBgBlur();
    const qreal newOpacity = AppSettings::terminalBgOpacity();

    if (!bgImagePath.isEmpty()) {
        if (m_bgImagePixmap.isNull() || m_bgBlurRadius != newBlur) {
            QImage img(bgImagePath);
            if (!img.isNull()) {
                // Simple blur approximation: scale down then up.
                const int blur = qBound(0, newBlur, 100);
                if (blur > 0) {
                    const qreal scale = qMax(0.1, 1.0 - (blur / 100.0));
                    const int w = qMax(1, int(img.width() * scale));
                    const int h = qMax(1, int(img.height() * scale));
                    img = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .scaled(img.width(), img.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                }
                m_bgImagePixmap = QPixmap::fromImage(img);
            } else {
                m_bgImagePixmap = QPixmap();
            }
        }
    } else {
        m_bgImagePixmap = QPixmap();
    }
    m_bgBlurRadius = newBlur;
    m_bgOpacity = newOpacity;

    if (newFg == m_defaultFg && newBg == m_defaultBg) {
        setFont(clientoshMonospaceFont(AppSettings::fontSize(), AppSettings::fontFamily()));
        emitSizeIfChanged();
        update();
        return;
    }
    remapDefaultColors(m_defaultFg, m_defaultBg, newFg, newBg);
    m_defaultFg = newFg;
    m_defaultBg = newBg;
    setFont(clientoshMonospaceFont(AppSettings::fontSize(), AppSettings::fontFamily()));
    emitSizeIfChanged();
    update();
}

int TerminalWidget::adjustFontSize(int delta)
{
    const int next = qBound(9, AppSettings::fontSize() + delta, 22);
    setFontSizePt(next);
    showFontSizeOverlay(next);
    return next;
}

void TerminalWidget::setFontSizePt(int points)
{
    const int next = qBound(9, points, 22);
    const bool sizeChanged = (next != AppSettings::fontSize()) || (font().pointSize() != next);

    // Always persist so Ctrl+scroll zoom survives restarts.
    AppSettings::setFontSize(next);

    if (sizeChanged) {
        setFont(clientoshMonospaceFont(next, AppSettings::fontFamily()));
        emitSizeIfChanged();
        update();
    }
    emit terminalFontSizeChanged(next);
}

void TerminalWidget::syncPtySize(bool forceEmit)
{
    int cols = 0;
    int rows = 0;
    estimatePtySize(&cols, &rows);
    if (cols != m_cols || rows != m_rows) {
        resizeGrid(cols, rows);
        clampCursor();
        emit ptySizeChanged(cols, rows);
        update();
        return;
    }
    if (forceEmit) {
        emit ptySizeChanged(cols, rows);
    }
}

void TerminalWidget::ensureFontZoomOverlay()
{
    if (m_fontZoomLabel) {
        return;
    }
    m_fontZoomLabel = new QLabel(this);
    m_fontZoomLabel->setObjectName(QStringLiteral("termFontZoomOverlay"));
    m_fontZoomLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_fontZoomLabel->setAlignment(Qt::AlignCenter);
    m_fontZoomLabel->setStyleSheet(QStringLiteral(
        "QLabel#termFontZoomOverlay {"
        "  color: #f0f0f0;"
        "  background: rgba(20, 20, 20, 200);"
        "  border: 1px solid rgba(255, 255, 255, 40);"
        "  padding: 6px 12px;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "}"));
    m_fontZoomOpacity = new QGraphicsOpacityEffect(m_fontZoomLabel);
    m_fontZoomLabel->setGraphicsEffect(m_fontZoomOpacity);
    m_fontZoomFade = new QPropertyAnimation(m_fontZoomOpacity, "opacity", m_fontZoomLabel);
    m_fontZoomFade->setDuration(280);
    m_fontZoomFade->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_fontZoomFade, &QPropertyAnimation::finished, this, [this]() {
        if (m_fontZoomOpacity && m_fontZoomOpacity->opacity() < 0.05) {
            m_fontZoomLabel->hide();
        }
    });

    m_fontZoomHideTimer = new QTimer(this);
    m_fontZoomHideTimer->setSingleShot(true);
    m_fontZoomHideTimer->setInterval(750);
    connect(m_fontZoomHideTimer, &QTimer::timeout, this, &TerminalWidget::fadeOutFontZoomOverlay);
    m_fontZoomLabel->hide();
}

void TerminalWidget::positionFontZoomOverlay()
{
    if (!m_fontZoomLabel || !m_fontZoomLabel->isVisible()) {
        return;
    }
    m_fontZoomLabel->adjustSize();
    const int margin = 12;
    int x = width() - m_fontZoomLabel->width() - margin;
    int y = height() - m_fontZoomLabel->height() - margin;
    if (m_vScroll && m_vScroll->isVisible()) {
        x -= m_vScroll->width();
    }
    m_fontZoomLabel->move(qMax(margin, x), qMax(margin, y));
    m_fontZoomLabel->raise();
}

void TerminalWidget::showFontSizeOverlay(int points)
{
    ensureFontZoomOverlay();
    if (m_fontZoomFade) {
        m_fontZoomFade->stop();
    }
    m_fontZoomLabel->setText(QStringLiteral("%1pt").arg(points));
    m_fontZoomOpacity->setOpacity(1.0);
    m_fontZoomLabel->show();
    positionFontZoomOverlay();
    m_fontZoomHideTimer->start();
}

void TerminalWidget::fadeOutFontZoomOverlay()
{
    if (!m_fontZoomLabel || !m_fontZoomOpacity || !m_fontZoomFade) {
        return;
    }
    m_fontZoomFade->stop();
    m_fontZoomFade->setStartValue(m_fontZoomOpacity->opacity());
    m_fontZoomFade->setEndValue(0.0);
    m_fontZoomFade->start();
}

QColor TerminalWidget::ansiColor(int index, bool bright) const
{
    static const QColor normal[8] = {
        QColor(0x1a, 0x1a, 0x1a),
        QColor(0xcd, 0x31, 0x31),
        QColor(0x0d, 0xbc, 0x79),
        QColor(0xe5, 0xc0, 0x7b),
        QColor(0x5c, 0xa1, 0xf6),
        QColor(0xc6, 0x78, 0xdd),
        QColor(0x56, 0xb6, 0xc2),
        QColor(0xc8, 0xc8, 0xc8),
    };
    static const QColor brightColors[8] = {
        QColor(0x5a, 0x5a, 0x5a),
        QColor(0xf4, 0x47, 0x47),
        QColor(0x23, 0xd1, 0x8b),
        QColor(0xf0, 0xd0, 0x8a),
        QColor(0x7a, 0xb6, 0xff),
        QColor(0xd6, 0x8e, 0xef),
        QColor(0x6a, 0xcd, 0xd9),
        QColor(0xff, 0xff, 0xff),
    };
    index = qBound(0, index, 7);
    return bright ? brightColors[index] : normal[index];
}

QColor TerminalWidget::xterm256(int index) const
{
    if (index < 0) {
        return QColor::fromRgb(m_attr.fg);
    }
    if (index < 8) {
        return ansiColor(index, false);
    }
    if (index < 16) {
        return ansiColor(index - 8, true);
    }
    if (index < 232) {
        index -= 16;
        const int r = index / 36;
        const int g = (index / 6) % 6;
        const int b = index % 6;
        auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(level(r), level(g), level(b));
    }
    if (index < 256) {
        const int gray = 8 + (index - 232) * 10;
        return QColor(gray, gray, gray);
    }
    return QColor::fromRgb(m_attr.fg);
}

int TerminalWidget::cellWidth() const
{
    // CRITICAL: pass this paint device so metrics match High-DPI painting.
    // QFontMetrics(font) without a device can report ~2x advances on scaled displays.
    QFont f = font();
    f.setBold(false);
    f.setKerning(false);
    f.setHintingPreference(QFont::PreferFullHinting);
    const QFontMetrics fm(f, this);
    return qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
}

int TerminalWidget::cellHeight() const
{
    QFont f = font();
    f.setBold(false);
    f.setKerning(false);
    f.setHintingPreference(QFont::PreferFullHinting);
    const QFontMetrics fm(f, this);
    return qMax(1, fm.ascent() + fm.descent());
}

void TerminalWidget::ensureGrid(int cols, int rows)
{
    cols = qMax(20, cols);
    rows = qMax(5, rows);
    if (m_cells.size() == cols * rows && m_cols == cols && m_rows == rows) {
        return;
    }
    resizeGrid(cols, rows);
}

void TerminalWidget::resizeGrid(int cols, int rows)
{
    auto resizeBuf = [&](QVector<Cell>& buf, int oldCols, int oldRows) {
        QVector<Cell> next(cols * rows);
        const int copyRows = qMin(rows, oldRows);
        const int copyCols = qMin(cols, oldCols);
        for (int r = 0; r < copyRows; ++r) {
            for (int c = 0; c < copyCols; ++c) {
                if (!buf.isEmpty() && buf.size() >= oldCols * oldRows) {
                    next[r * cols + c] = buf[r * oldCols + c];
                }
            }
        }
        buf = std::move(next);
    };

    const int oldCols = m_cols;
    const int oldRows = m_rows;
    const bool fullHeightScrollRegion = m_scrollTop == 0
        && m_scrollBottom == oldRows - 1;
    const bool fullHeightMainScrollRegion = m_mainScrollTop == 0
        && m_mainScrollBottom == oldRows - 1;

    if (m_altScreen) {
        resizeBuf(m_cells, oldCols, oldRows);
        if (m_mainCells.size() == oldCols * oldRows) {
            resizeBuf(m_mainCells, oldCols, oldRows);
        }
        m_altCells = m_cells;
    } else {
        resizeBuf(m_cells, oldCols, oldRows);
        if (m_altCells.size() == oldCols * oldRows) {
            resizeBuf(m_altCells, oldCols, oldRows);
        }
        m_mainCells = m_cells;
    }

    m_cols = cols;
    m_rows = rows;
    m_cx = qBound(0, m_cx, m_cols - 1);
    m_cy = qBound(0, m_cy, m_rows - 1);

    // A normal full-screen terminal region must grow again after a temporary
    // shrink (notably the 2 px -> 50% splitter opening animation). Previously
    // the bottom stayed clamped to the smallest intermediate height forever.
    if (fullHeightScrollRegion) {
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
    } else {
        m_scrollTop = qMin(m_scrollTop, m_rows - 1);
        m_scrollBottom = qMin(m_scrollBottom, m_rows - 1);
        if (m_scrollTop > m_scrollBottom) {
            m_scrollTop = 0;
            m_scrollBottom = m_rows - 1;
        }
    }
    if (fullHeightMainScrollRegion) {
        m_mainScrollTop = 0;
        m_mainScrollBottom = m_rows - 1;
    } else {
        m_mainScrollTop = qMin(m_mainScrollTop, m_rows - 1);
        m_mainScrollBottom = qMin(m_mainScrollBottom, m_rows - 1);
    }

    for (QVector<Cell>& line : m_scrollback) {
        line.resize(cols);
    }
    m_viewOffset = qMin(m_viewOffset, maxViewOffset());
    syncScrollBar();
}

TerminalWidget::Cell& TerminalWidget::cellAt(int row, int col)
{
    return m_cells[row * m_cols + col];
}

const TerminalWidget::Cell& TerminalWidget::cellAt(int row, int col) const
{
    return m_cells[row * m_cols + col];
}

void TerminalWidget::setInteractive(bool enabled)
{
    m_interactive = enabled;
    if (enabled) {
        setFocus();
    }
    update();
}

void TerminalWidget::setXmodemAvailable(bool enabled)
{
    m_xmodemAvailable = enabled;
    if (m_xmodemSeparator) m_xmodemSeparator->setVisible(enabled);
    if (m_xmodemAction) {
        m_xmodemAction->setVisible(enabled);
        m_xmodemAction->setEnabled(enabled);
    }
}

void TerminalWidget::clearTerminal()
{
    resetTerminalState();
    clearScrollback();
    for (Cell& cell : m_cells) {
        cell = makeBlankCell();
    }
    update();
}

void TerminalWidget::resetTerminalState()
{
    m_state = ParseState::Ground;
    m_seq.clear();
    m_utf8Pending.clear();
    resetAttributes();
    m_cx = 0;
    m_cy = 0;
    m_pendingWrap = false;
    m_originMode = false;
    m_autoWrap = true;
    m_altScreen = false;
    m_appCursorKeys = false;
    m_appKeypad = false;
    m_mouseMode = 0;
    m_mouseSgr = false;
    m_mouseUrxvt = false;
    m_mouseUtf8 = false;
    m_altScroll = true; // restore resource default, not off
    m_focusReport = false;
    m_mousePressedBtn = -1;
    m_lastMouseCell = QPoint(-1, -1);
    updateQtMouseTracking();
    m_lastChar = U' ';
    m_scrollTop = 0;
    m_scrollBottom = qMax(0, m_rows - 1);
    m_g0 = Charset::Ascii;
    m_g1 = Charset::DecSpecial;
    m_gl = 0;
    m_selecting = false;
    m_selAnchor = -1;
    m_selStart = -1;
    m_selEnd = -1;
}

void TerminalWidget::estimatePtySize(int* cols, int* rows) const
{
    int w = width();
    if (m_vScroll && m_vScroll->isVisible()) {
        w -= m_vScroll->sizeHint().width();
    }
    *cols = qMax(20, w / cellWidth());
    *rows = qMax(5, height() / cellHeight());
}

void TerminalWidget::emitSizeIfChanged()
{
    syncPtySize(false);
}

void TerminalWidget::clampCursor()
{
    m_cx = qBound(0, m_cx, m_cols - 1);
    const int top = m_originMode ? m_scrollTop : 0;
    const int bottom = m_originMode ? m_scrollBottom : (m_rows - 1);
    m_cy = qBound(top, m_cy, bottom);
}

void TerminalWidget::scrollUp(int n)
{
    const int top = m_scrollTop;
    const int bottom = m_scrollBottom;
    const int height = bottom - top + 1;
    n = qBound(1, n, height);

    if (!m_altScreen && top == 0) {
        pushScrollbackLines(top, n);
    }

    for (int r = top; r <= bottom - n; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = cellAt(r + n, c);
        }
    }
    for (int r = bottom - n + 1; r <= bottom; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = Cell{};
            cellAt(r, c).fg = m_attr.fg;
            cellAt(r, c).bg = m_attr.bg;
        }
    }
}

void TerminalWidget::scrollDown(int n)
{
    const int top = m_scrollTop;
    const int bottom = m_scrollBottom;
    const int height = bottom - top + 1;
    n = qBound(1, n, height);

    for (int r = bottom; r >= top + n; --r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = cellAt(r - n, c);
        }
    }
    for (int r = top; r < top + n; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = Cell{};
            cellAt(r, c).fg = m_attr.fg;
            cellAt(r, c).bg = m_attr.bg;
        }
    }
}

void TerminalWidget::insertLines(int n)
{
    if (m_cy < m_scrollTop || m_cy > m_scrollBottom) {
        return;
    }
    const int bottom = m_scrollBottom;
    const int height = bottom - m_cy + 1;
    n = qBound(1, n, height);

    for (int r = bottom; r >= m_cy + n; --r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = cellAt(r - n, c);
        }
    }
    for (int r = m_cy; r < m_cy + n; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = Cell{};
            cellAt(r, c).fg = m_attr.fg;
            cellAt(r, c).bg = m_attr.bg;
        }
    }
}

void TerminalWidget::deleteLines(int n)
{
    if (m_cy < m_scrollTop || m_cy > m_scrollBottom) {
        return;
    }
    const int bottom = m_scrollBottom;
    const int height = bottom - m_cy + 1;
    n = qBound(1, n, height);

    for (int r = m_cy; r <= bottom - n; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = cellAt(r + n, c);
        }
    }
    for (int r = bottom - n + 1; r <= bottom; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            cellAt(r, c) = Cell{};
            cellAt(r, c).fg = m_attr.fg;
            cellAt(r, c).bg = m_attr.bg;
        }
    }
}

void TerminalWidget::newLine()
{
    m_pendingWrap = false;
    if (m_cy == m_scrollBottom) {
        scrollUp(1);
    } else if (m_cy < m_rows - 1) {
        ++m_cy;
    }
}

void TerminalWidget::reverseIndex()
{
    m_pendingWrap = false;
    if (m_cy == m_scrollTop) {
        scrollDown(1);
    } else if (m_cy > 0) {
        --m_cy;
    }
}

void TerminalWidget::carriageReturn()
{
    m_pendingWrap = false;
    m_cx = 0;
}

void TerminalWidget::backspace()
{
    m_pendingWrap = false;
    if (m_cx > 0) {
        --m_cx;
    }
}

void TerminalWidget::tab()
{
    m_pendingWrap = false;
    m_cx = qMin(m_cols - 1, ((m_cx / 8) + 1) * 8);
}

char32_t TerminalWidget::mapDecSpecial(unsigned char byte) const
{
    // VT100 DEC Special Graphics → Unicode box drawing / symbols
    switch (byte) {
    case '`': return U'◆';
    case 'a': return U'▒';
    case 'b': return U'\u2409'; // HT symbol
    case 'c': return U'\u240c'; // FF
    case 'd': return U'\u240d'; // CR
    case 'e': return U'\u240a'; // LF
    case 'f': return U'°';
    case 'g': return U'±';
    case 'h': return U'\u2424'; // NL
    case 'i': return U'\u240b'; // VT
    case 'j': return U'┘';
    case 'k': return U'┐';
    case 'l': return U'┌';
    case 'm': return U'└';
    case 'n': return U'┼';
    case 'o': return U'─'; // scanlines often missing in fonts → solid hline
    case 'p': return U'─';
    case 'q': return U'─';
    case 'r': return U'─';
    case 's': return U'─';
    case 't': return U'├';
    case 'u': return U'┤';
    case 'v': return U'┴';
    case 'w': return U'┬';
    case 'x': return U'│';
    case 'y': return U'≤';
    case 'z': return U'≥';
    case '{': return U'π';
    case '|': return U'≠';
    case '}': return U'£';
    case '~': return U'·';
    case '_': return U' ';
    default: return byte;
    }
}

TerminalWidget::Charset TerminalWidget::activeCharset() const
{
    return m_gl == 0 ? m_g0 : m_g1;
}

void TerminalWidget::designateCharset(int slot, char name)
{
    Charset set = Charset::Ascii;
    if (name == '0' || name == '2') {
        set = Charset::DecSpecial;
    } else {
        set = Charset::Ascii; // B, A, U, etc.
    }
    if (slot == 0) {
        m_g0 = set;
    } else {
        m_g1 = set;
    }
}

void TerminalWidget::mapAndPutAscii(unsigned char byte)
{
    char32_t ch = byte;
    if (activeCharset() == Charset::DecSpecial && byte >= 0x5f && byte <= 0x7e) {
        ch = mapDecSpecial(byte);
    }
    putChar(ch);
}

int TerminalWidget::charDisplayWidth(char32_t ucs) const
{
    if (ucs == 0 || ucs == 0x7f) {
        return 0;
    }
    if (ucs < 0x20) {
        return 0;
    }

    // Zero-width / format chars (ZWSP U+200B is common between box-drawing runs)
    if (ucs == 0x00AD || ucs == 0x034F || ucs == 0x061C
        || (ucs >= 0x17B4 && ucs <= 0x17B5)
        || (ucs >= 0x180B && ucs <= 0x180E)
        || (ucs >= 0x200B && ucs <= 0x200F)
        || (ucs >= 0x202A && ucs <= 0x202E)
        || (ucs >= 0x2060 && ucs <= 0x2064)
        || (ucs >= 0x2066 && ucs <= 0x206F)
        || ucs == 0xFEFF
        || (ucs >= 0xFE00 && ucs <= 0xFE0F)
        || (ucs >= 0xE0100 && ucs <= 0xE01EF)) {
        return 0;
    }

    // Non-spacing / enclosing combining marks
    const QString s = QString::fromUcs4(&ucs, 1);
    if (!s.isEmpty()) {
        const QChar::Category cat = s.at(0).category();
        if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_Enclosing) {
            return 0;
        }
    }

    // Ambiguous / wide (simplified wcwidth — CJK, fullwidth, emoji blocks)
    if (ucs >= 0x1100) {
        if ((ucs <= 0x115F)
            || ucs == 0x2329 || ucs == 0x232A
            || (ucs >= 0x2E80 && ucs <= 0xA4CF && ucs != 0x303F)
            || (ucs >= 0xAC00 && ucs <= 0xD7A3)
            || (ucs >= 0xF900 && ucs <= 0xFAFF)
            || (ucs >= 0xFE10 && ucs <= 0xFE19)
            || (ucs >= 0xFE30 && ucs <= 0xFE6F)
            || (ucs >= 0xFF00 && ucs <= 0xFF60)
            || (ucs >= 0xFFE0 && ucs <= 0xFFE6)
            || (ucs >= 0x1F300 && ucs <= 0x1F64F)
            || (ucs >= 0x1F900 && ucs <= 0x1F9FF)
            || (ucs >= 0x20000 && ucs <= 0x3FFFD)) {
            return 2;
        }
    }
    return 1;
}

void TerminalWidget::putChar(char32_t ch)
{
    if (ch == 0) {
        return;
    }

    const int width = charDisplayWidth(ch);
    if (width <= 0) {
        // Zero-width (e.g. U+200B): do not consume a cell — otherwise ═​═ looks spaced
        return;
    }

    if (m_pendingWrap && m_autoWrap) {
        carriageReturn();
        newLine();
    }
    if (m_cx >= m_cols) {
        if (m_autoWrap) {
            carriageReturn();
            newLine();
        } else {
            m_cx = m_cols - 1;
        }
    }

    // Not enough room for a wide glyph → wrap first
    if (width > 1 && m_cx + width > m_cols && m_autoWrap) {
        carriageReturn();
        newLine();
    }

    Cell& cell = cellAt(m_cy, m_cx);
    cell = m_attr;
    cell.ch = ch;
    m_lastChar = ch;

    for (int i = 1; i < width && (m_cx + i) < m_cols; ++i) {
        Cell& cont = cellAt(m_cy, m_cx + i);
        cont = m_attr;
        cont.ch = U' '; // wide-tail placeholder (background only)
    }

    if (m_cx + width >= m_cols) {
        m_pendingWrap = m_autoWrap;
        m_cx = m_cols - 1;
    } else {
        m_cx += width;
        m_pendingWrap = false;
    }
}

void TerminalWidget::saveCursor()
{
    m_savedCx = m_cx;
    m_savedCy = m_cy;
    m_savedAttr = m_attr;
    m_savedGl = m_gl;
}

void TerminalWidget::restoreCursor()
{
    m_cx = m_savedCx;
    m_cy = m_savedCy;
    m_attr = m_savedAttr;
    m_gl = m_savedGl;
    m_pendingWrap = false;
    clampCursor();
}

void TerminalWidget::reply(const QByteArray& data)
{
    if (!data.isEmpty()) {
        emit inputReady(data);
    }
}

bool TerminalWidget::mouseReportingActive() const
{
    return m_mouseMode == 1000 || m_mouseMode == 1002 || m_mouseMode == 1003;
}

bool TerminalWidget::shouldReportMouse(const QInputEvent* event) const
{
    if (!mouseReportingActive() || !m_interactive) {
        return false;
    }
    // xterm: Shift bypasses mouse reporting so the user can select text
    if (event && (event->modifiers() & Qt::ShiftModifier)) {
        return false;
    }
    return true;
}

void TerminalWidget::applyMouseMode(int mode, bool set)
{
    if (set) {
        m_mouseMode = mode;
    } else if (m_mouseMode == mode) {
        m_mouseMode = 0;
        m_mousePressedBtn = -1;
    }
    updateQtMouseTracking();
}

void TerminalWidget::updateQtMouseTracking()
{
    setMouseTracking(m_mouseMode == 1003);
}

int TerminalWidget::encodeMouseButton(Qt::MouseButton button, bool motion) const
{
    int code = 3; // release / none
    if (button == Qt::LeftButton) {
        code = 0;
    } else if (button == Qt::MiddleButton) {
        code = 1;
    } else if (button == Qt::RightButton) {
        code = 2;
    }
    if (motion) {
        code += 32;
    }
    return code;
}

int TerminalWidget::encodeMouseModifiers(Qt::KeyboardModifiers mods) const
{
    int add = 0;
    // Shift is reserved for bypass; still encode if somehow reporting with shift
    if (mods & Qt::ShiftModifier) {
        add += 4;
    }
    if (mods & Qt::AltModifier) {
        add += 8;
    }
    if (mods & Qt::ControlModifier) {
        add += 16;
    }
    return add;
}

void TerminalWidget::sendMouseReport(int buttonCode, int col0, int row0, bool release)
{
    const int col = qBound(1, col0 + 1, 9999);
    const int row = qBound(1, row0 + 1, 9999);
    const int cb = buttonCode + encodeMouseModifiers(QApplication::keyboardModifiers());

    // Prefer SGR whenever it is enabled, or when no legacy encoding was requested.
    // Most modern TUIs (btop, etc.) parse SGR; X10 breaks past column 223.
    const bool useSgr = m_mouseSgr || (!m_mouseUrxvt && !m_mouseUtf8);

    if (useSgr) {
        const char final = release ? 'm' : 'M';
        reply(QStringLiteral("\x1b[<%1;%2;%3%4")
                  .arg(cb)
                  .arg(col)
                  .arg(row)
                  .arg(QLatin1Char(final))
                  .toLatin1());
        return;
    }

    if (m_mouseUrxvt) {
        reply(QStringLiteral("\x1b[%1;%2;%3M").arg(cb + 32).arg(col).arg(row).toLatin1());
        return;
    }

    // Legacy X10 — 1 byte each, capped at 223
    const int x10Btn = qBound(0, (release ? 3 : buttonCode) + encodeMouseModifiers(QApplication::keyboardModifiers()), 255);
    const int x10Col = qBound(0, col, 223);
    const int x10Row = qBound(0, row, 223);
    QByteArray seq("\x1b[M");
    seq.append(static_cast<char>(32 + x10Btn));
    seq.append(static_cast<char>(32 + x10Col));
    seq.append(static_cast<char>(32 + x10Row));
    reply(seq);
}

bool TerminalWidget::sendWheelToApp(QWheelEvent* event)
{
    if (!m_interactive) {
        return false;
    }

    int delta = event->angleDelta().y();
    if (delta == 0) {
        delta = event->pixelDelta().y();
    }
    // Also accept horizontal-only wheels mapped as vertical by some drivers
    if (delta == 0) {
        delta = event->angleDelta().x();
    }
    if (delta == 0) {
        delta = event->pixelDelta().x();
    }
    if (delta == 0) {
        return false;
    }

    const QPoint cell = cellFromPos(event->position().toPoint());
    // One notch ≈ 120° in eighths; keep at least one step for fine trackpads.
    // "Scroll Sensitivity" scales how far the TUI moves per notch (1 = one line).
    const int sensitivity = AppSettings::scrollSensitivity();
    const int steps = qMax(1, qAbs(delta) / 120) * sensitivity;

    // 1) Mouse-tracking apps (btop, vim mouse=a, …): SGR/X10 wheel buttons
    if (mouseReportingActive() && shouldReportMouse(event)) {
        const int btn = (delta > 0) ? 64 : 65; // 64=up, 65=down
        for (int i = 0; i < steps; ++i) {
            sendMouseReport(btn, cell.x(), cell.y(), false);
        }
        return true;
    }

    // 2) Alternate screen without mouse (less, man, nano, vim default, …):
    //    translate wheel → cursor keys (xterm alternateScroll / DECSET 1007).
    if (m_altScreen && m_altScroll) {
        const QByteArray up = m_appCursorKeys ? QByteArray("\x1bOA") : QByteArray("\x1b[A");
        const QByteArray down = m_appCursorKeys ? QByteArray("\x1bOB") : QByteArray("\x1b[B");
        const QByteArray& seq = (delta > 0) ? up : down;
        for (int i = 0; i < steps; ++i) {
            reply(seq);
        }
        return true;
    }

    return false;
}

void TerminalWidget::enterAltScreen(bool clear)
{
    if (!m_altScreen) {
        m_mainCells = m_cells;
        m_mainCx = m_cx;
        m_mainCy = m_cy;
        m_mainScrollTop = m_scrollTop;
        m_mainScrollBottom = m_scrollBottom;
        if (m_altCells.size() != m_cols * m_rows) {
            m_altCells = QVector<Cell>(m_cols * m_rows);
        }
        m_cells = m_altCells;
        m_altScreen = true;
    }
    if (clear) {
        for (Cell& cell : m_cells) {
            cell = makeBlankCell();
        }
        m_cx = 0;
        m_cy = 0;
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
        m_pendingWrap = false;
    }
    m_viewOffset = 0;
    syncScrollBar();
}

void TerminalWidget::leaveAltScreen(bool clear)
{
    if (m_altScreen) {
        m_altCells = m_cells;
        if (m_mainCells.size() == m_cols * m_rows) {
            m_cells = m_mainCells;
        } else {
            m_cells = QVector<Cell>(m_cols * m_rows);
        }
        m_cx = m_mainCx;
        m_cy = m_mainCy;
        m_scrollTop = m_mainScrollTop;
        m_scrollBottom = qMin(m_mainScrollBottom, m_rows - 1);
        m_altScreen = false;
        m_pendingWrap = false;
        clampCursor();
    }
    if (clear) {
        for (Cell& cell : m_cells) {
            cell = makeBlankCell();
        }
    }
    m_viewOffset = 0;
    syncScrollBar();
}

void TerminalWidget::eraseInDisplay(int mode)
{
    auto clearCell = [&](int r, int c) {
        cellAt(r, c) = Cell{};
        cellAt(r, c).fg = m_attr.fg;
        cellAt(r, c).bg = m_attr.bg;
    };

    if (mode == 2 || mode == 3) {
        for (int r = 0; r < m_rows; ++r) {
            for (int c = 0; c < m_cols; ++c) {
                clearCell(r, c);
            }
        }
        if (mode == 3) {
            clearScrollback();
        }
        // VT/xterm: ED2 does not move the cursor
        return;
    }

    if (mode == 0) {
        for (int c = m_cx; c < m_cols; ++c) {
            clearCell(m_cy, c);
        }
        for (int r = m_cy + 1; r < m_rows; ++r) {
            for (int c = 0; c < m_cols; ++c) {
                clearCell(r, c);
            }
        }
        return;
    }

    if (mode == 1) {
        for (int r = 0; r < m_cy; ++r) {
            for (int c = 0; c < m_cols; ++c) {
                clearCell(r, c);
            }
        }
        for (int c = 0; c <= m_cx; ++c) {
            clearCell(m_cy, c);
        }
    }
}

void TerminalWidget::eraseInLine(int mode)
{
    int from = 0;
    int to = m_cols - 1;
    if (mode == 0) {
        from = m_cx;
    } else if (mode == 1) {
        to = m_cx;
    }
    for (int c = from; c <= to; ++c) {
        cellAt(m_cy, c) = Cell{};
        cellAt(m_cy, c).fg = m_attr.fg;
        cellAt(m_cy, c).bg = m_attr.bg;
    }
}

void TerminalWidget::applySgr(const QVector<int>& params)
{
    for (int i = 0; i < params.size(); ++i) {
        const int p = params[i];
        if (p == 0) {
            resetAttributes();
        } else if (p == 1) {
            m_attr.bold = true;
        } else if (p == 4) {
            m_attr.underline = true;
        } else if (p == 7) {
            m_attr.inverse = true;
        } else if (p == 22) {
            m_attr.bold = false;
        } else if (p == 24) {
            m_attr.underline = false;
        } else if (p == 27) {
            m_attr.inverse = false;
        } else if (p >= 30 && p <= 37) {
            m_attr.fg = ansiColor(p - 30, m_attr.bold).rgb();
        } else if (p == 38) {
            if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size()) {
                m_attr.fg = xterm256(params[i + 2]).rgb();
                i += 2;
            } else if (i + 1 < params.size() && params[i + 1] == 2) {
                // 38;2;R;G;B  or  38:2:R:G:B  or  38:2:Cs:R:G:B
                int r = 0;
                int g = 0;
                int b = 0;
                if (i + 5 < params.size() && params[i + 2] <= 1) {
                    r = params[i + 3];
                    g = params[i + 4];
                    b = params[i + 5];
                    i += 5;
                } else if (i + 4 < params.size()) {
                    r = params[i + 2];
                    g = params[i + 3];
                    b = params[i + 4];
                    i += 4;
                }
                m_attr.fg = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
            }
        } else if (p == 39) {
            m_attr.fg = m_defaultFg.rgb();
        } else if (p >= 40 && p <= 47) {
            m_attr.bg = ansiColor(p - 40, false).rgb();
        } else if (p == 48) {
            if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size()) {
                m_attr.bg = xterm256(params[i + 2]).rgb();
                i += 2;
            } else if (i + 1 < params.size() && params[i + 1] == 2) {
                int r = 0;
                int g = 0;
                int b = 0;
                if (i + 5 < params.size() && params[i + 2] <= 1) {
                    r = params[i + 3];
                    g = params[i + 4];
                    b = params[i + 5];
                    i += 5;
                } else if (i + 4 < params.size()) {
                    r = params[i + 2];
                    g = params[i + 3];
                    b = params[i + 4];
                    i += 4;
                }
                m_attr.bg = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
            }
        } else if (p == 49) {
            m_attr.bg = m_defaultBg.rgb();
        } else if (p >= 90 && p <= 97) {
            m_attr.fg = ansiColor(p - 90, true).rgb();
        } else if (p >= 100 && p <= 107) {
            m_attr.bg = ansiColor(p - 100, true).rgb();
        }
    }
}

void TerminalWidget::handleCsi(char finalByte, const QByteArray& paramsRaw)
{
    QByteArray numeric;
    bool isPrivate = false;
    bool isGt = false;
    for (unsigned char c : paramsRaw) {
        if (c == '?') {
            isPrivate = true;
            continue;
        }
        if (c == '>') {
            isGt = true;
            continue;
        }
        if (c == '!') {
            continue;
        }
        numeric.append(static_cast<char>(c));
    }
    const QVector<int> params = parseParams(numeric);
    const int p1 = params.isEmpty() ? 0 : params[0];
    const int n = (p1 == 0 ? 1 : p1);

    auto cursorRowMin = [&]() { return m_originMode ? m_scrollTop : 0; };
    auto cursorRowMax = [&]() { return m_originMode ? m_scrollBottom : (m_rows - 1); };

    switch (finalByte) {
    case 'A':
        m_pendingWrap = false;
        m_cy = qMax(cursorRowMin(), m_cy - n);
        break;
    case 'B':
        m_pendingWrap = false;
        m_cy = qMin(cursorRowMax(), m_cy + n);
        break;
    case 'C':
        m_pendingWrap = false;
        m_cx = qMin(m_cols - 1, m_cx + n);
        break;
    case 'D':
        m_pendingWrap = false;
        m_cx = qMax(0, m_cx - n);
        break;
    case 'G':
        m_pendingWrap = false;
        m_cx = qBound(0, (p1 == 0 ? 1 : p1) - 1, m_cols - 1);
        break;
    case 'd': {
        m_pendingWrap = false;
        const int row = (p1 == 0 ? 1 : p1) - 1;
        if (m_originMode) {
            m_cy = qBound(m_scrollTop, m_scrollTop + row, m_scrollBottom);
        } else {
            m_cy = qBound(0, row, m_rows - 1);
        }
        break;
    }
    case 'H':
    case 'f': {
        m_pendingWrap = false;
        const int row = params.isEmpty() ? 1 : (params[0] == 0 ? 1 : params[0]);
        const int col = params.size() < 2 ? 1 : (params[1] == 0 ? 1 : params[1]);
        if (m_originMode) {
            m_cy = qBound(m_scrollTop, m_scrollTop + row - 1, m_scrollBottom);
        } else {
            m_cy = qBound(0, row - 1, m_rows - 1);
        }
        m_cx = qBound(0, col - 1, m_cols - 1);
        break;
    }
    case 'J':
        eraseInDisplay(p1);
        break;
    case 'K':
        eraseInLine(p1);
        break;
    case 'L':
        insertLines(n);
        break;
    case 'M':
        deleteLines(n);
        break;
    case 'S':
        scrollUp(n);
        break;
    case 'T':
        scrollDown(n);
        break;
    case 'b': { // REP — repeat last character
        const int count = qBound(1, n, m_cols * m_rows);
        for (int i = 0; i < count; ++i) {
            putChar(m_lastChar);
        }
        break;
    }
    case 'r': {
        int top = 0;
        int bottom = m_rows - 1;
        if (!paramsRaw.isEmpty()) {
            top = (params.isEmpty() || params[0] == 0) ? 1 : params[0];
            bottom = (params.size() < 2 || params[1] == 0) ? m_rows : params[1];
            top = qBound(1, top, m_rows) - 1;
            bottom = qBound(1, bottom, m_rows) - 1;
            if (bottom <= top) {
                top = 0;
                bottom = m_rows - 1;
            }
        }
        m_scrollTop = top;
        m_scrollBottom = bottom;
        m_cx = 0;
        m_cy = m_originMode ? m_scrollTop : 0;
        m_pendingWrap = false;
        break;
    }
    case 'm':
        applySgr(params);
        break;
    case 'n': { // DSR
        if (isPrivate && p1 == 6) {
            // CPR — cursor position report
            reply(QStringLiteral("\x1b[%1;%2R").arg(m_cy + 1).arg(m_cx + 1).toLatin1());
        } else if (p1 == 5) {
            reply(QByteArray("\x1b[0n")); // terminal OK
        } else if (p1 == 6) {
            reply(QStringLiteral("\x1b[%1;%2R").arg(m_cy + 1).arg(m_cx + 1).toLatin1());
        }
        break;
    }
    case 'c': { // DA — device attributes (ncurses probes this)
        if (isGt) {
            // Secondary DA — report as xterm-ish VT220
            reply(QByteArray("\x1b[>0;276;0c"));
        } else {
            // Primary DA — VT100 with Advanced Video Option
            reply(QByteArray("\x1b[?1;2c"));
        }
        break;
    }
    case 'h':
    case 'l': {
        const bool set = (finalByte == 'h');
        if (isPrivate) {
            for (int mode : params) {
                if (mode == 1) { // DECCKM — application cursor keys
                    m_appCursorKeys = set;
                } else if (mode == 66) { // DECNKM — numeric keypad mode
                    m_appKeypad = set;
                } else if (mode == 6) {
                    m_originMode = set;
                    m_cx = 0;
                    m_cy = set ? m_scrollTop : 0;
                    m_pendingWrap = false;
                } else if (mode == 7) {
                    m_autoWrap = set;
                } else if (mode == 25) {
                    m_cursorVisible = set;
                } else if (mode == 47 || mode == 1047) {
                    if (set) {
                        enterAltScreen(false);
                    } else {
                        leaveAltScreen(mode == 1047);
                    }
                } else if (mode == 1048) {
                    if (set) {
                        m_decSavedCx = m_cx;
                        m_decSavedCy = m_cy;
                        m_decSavedAttr = m_attr;
                    } else {
                        m_cx = m_decSavedCx;
                        m_cy = m_decSavedCy;
                        m_attr = m_decSavedAttr;
                        clampCursor();
                    }
                } else if (mode == 1049) {
                    if (set) {
                        m_decSavedCx = m_cx;
                        m_decSavedCy = m_cy;
                        m_decSavedAttr = m_attr;
                        enterAltScreen(true);
                    } else {
                        leaveAltScreen(true);
                        m_cx = m_decSavedCx;
                        m_cy = m_decSavedCy;
                        m_attr = m_decSavedAttr;
                        clampCursor();
                    }
                } else if (mode == 1000 || mode == 1002 || mode == 1003) {
                    applyMouseMode(mode, set);
                } else if (mode == 1004) {
                    m_focusReport = set;
                } else if (mode == 1005) {
                    m_mouseUtf8 = set;
                } else if (mode == 1006) {
                    m_mouseSgr = set;
                } else if (mode == 1007) {
                    m_altScroll = set;
                } else if (mode == 1015) {
                    m_mouseUrxvt = set;
                }
            }
        }
        break;
    }
    case 's':
        saveCursor();
        break;
    case 'u':
        restoreCursor();
        break;
    case '@': {
        const int count = qMin(n, m_cols - m_cx);
        for (int c = m_cols - 1; c >= m_cx + count; --c) {
            cellAt(m_cy, c) = cellAt(m_cy, c - count);
        }
        for (int c = m_cx; c < m_cx + count; ++c) {
            cellAt(m_cy, c) = Cell{};
            cellAt(m_cy, c).fg = m_attr.fg;
            cellAt(m_cy, c).bg = m_attr.bg;
        }
        break;
    }
    case 'P': {
        const int count = qMin(n, m_cols - m_cx);
        for (int c = m_cx; c < m_cols - count; ++c) {
            cellAt(m_cy, c) = cellAt(m_cy, c + count);
        }
        for (int c = m_cols - count; c < m_cols; ++c) {
            cellAt(m_cy, c) = Cell{};
            cellAt(m_cy, c).fg = m_attr.fg;
            cellAt(m_cy, c).bg = m_attr.bg;
        }
        break;
    }
    case 'X': {
        const int count = qMin(n, m_cols - m_cx);
        for (int c = m_cx; c < m_cx + count; ++c) {
            cellAt(m_cy, c) = Cell{};
            cellAt(m_cy, c).fg = m_attr.fg;
            cellAt(m_cy, c).bg = m_attr.bg;
        }
        break;
    }
    default:
        break;
    }
}

void TerminalWidget::feedUtf8Char(char32_t ch)
{
    putChar(ch);
}

void TerminalWidget::feedByte(unsigned char byte)
{
    switch (m_state) {
    case ParseState::Ground:
        if (byte == 0x1b) {
            m_state = ParseState::Escape;
            m_seq.clear();
            return;
        }
        if (byte == '\n') {
            newLine();
            return;
        }
        if (byte == '\r') {
            carriageReturn();
            return;
        }
        if (byte == '\b') {
            backspace();
            return;
        }
        if (byte == '\t') {
            tab();
            return;
        }
        if (byte == 0x07) {
            return;
        }
        if (byte == 0x0e) {
            m_gl = 1;
            return;
        }
        if (byte == 0x0f) {
            m_gl = 0;
            return;
        }
        if (byte < 0x20) {
            return;
        }

        if (activeCharset() == Charset::DecSpecial && byte < 0x80) {
            m_utf8Pending.clear();
            mapAndPutAscii(byte);
            return;
        }

        if (m_utf8Pending.isEmpty()) {
            const int need = utf8ExpectedLength(byte);
            if (need == 1) {
                mapAndPutAscii(byte);
                return;
            }
            if (need < 0) {
                feedUtf8Char(0xfffd);
                return;
            }
            m_utf8Pending.append(static_cast<char>(byte));
            return;
        }

        if ((byte & 0xc0) != 0x80) {
            m_utf8Pending.clear();
            feedByte(byte);
            return;
        }

        m_utf8Pending.append(static_cast<char>(byte));
        {
            const int need = utf8ExpectedLength(static_cast<unsigned char>(m_utf8Pending.at(0)));
            if (m_utf8Pending.size() >= need) {
                char32_t ch = 0xfffd;
                if (!decodeUtf8(m_utf8Pending, &ch)) {
                    ch = 0xfffd;
                }
                m_utf8Pending.clear();
                feedUtf8Char(ch);
            }
        }
        return;

    case ParseState::Escape:
        if (byte == '[') {
            m_state = ParseState::Csi;
            m_seq.clear();
            return;
        }
        if (byte == ']') {
            m_state = ParseState::Osc;
            m_seq.clear();
            return;
        }
        if (byte == '(' || byte == ')' || byte == '*' || byte == '+') {
            m_charsetSlot = byte;
            m_state = ParseState::Charset;
            m_seq.clear();
            return;
        }
        if (byte == 'c') {
            clearTerminal();
            m_state = ParseState::Ground;
            return;
        }
        if (byte == 'Z') { // DECID
            reply(QByteArray("\x1b[?1;2c"));
            m_state = ParseState::Ground;
            return;
        }
        if (byte == '=') { // DECKPAM
            m_appKeypad = true;
            m_state = ParseState::Ground;
            return;
        }
        if (byte == '>') { // DECKPNM
            m_appKeypad = false;
            m_state = ParseState::Ground;
            return;
        }
        if (byte == '7') {
            saveCursor();
            m_state = ParseState::Ground;
            return;
        }
        if (byte == '8') {
            restoreCursor();
            m_state = ParseState::Ground;
            return;
        }
        if (byte == 'D') {
            newLine();
            m_state = ParseState::Ground;
            return;
        }
        if (byte == 'E') {
            carriageReturn();
            newLine();
            m_state = ParseState::Ground;
            return;
        }
        if (byte == 'M') {
            reverseIndex();
            m_state = ParseState::Ground;
            return;
        }
        m_state = ParseState::Ground;
        return;

    case ParseState::Csi:
        if (isCsiFinal(byte)) {
            handleCsi(static_cast<char>(byte), m_seq);
            m_seq.clear();
            m_state = ParseState::Ground;
            return;
        }
        if (m_seq.size() < 256) {
            m_seq.append(static_cast<char>(byte));
        }
        return;

    case ParseState::Osc:
        if (byte == 0x07) {
            m_seq.clear();
            m_state = ParseState::Ground;
            return;
        }
        if (byte == 0x1b) {
            m_seq.append(static_cast<char>(byte));
            return;
        }
        if (!m_seq.isEmpty() && static_cast<unsigned char>(m_seq.back()) == 0x1b && byte == '\\') {
            m_seq.clear();
            m_state = ParseState::Ground;
            return;
        }
        if (m_seq.size() < 1024) {
            m_seq.append(static_cast<char>(byte));
        }
        return;

    case ParseState::Charset: {
        const int slot = (m_charsetSlot == '(') ? 0 : 1;
        designateCharset(slot, static_cast<char>(byte));
        m_state = ParseState::Ground;
        return;
    }
    }
}

void TerminalWidget::appendOutput(const QByteArray& data)
{
    // Follow new output so the live prompt stays on-screen (stick-to-bottom).
    // If the user has scrolled into history, keep their place until they return.
    const bool followOutput = (m_viewOffset == 0);

    for (unsigned char byte : data) {
        feedByte(byte);
    }

    if (followOutput || m_viewOffset == 0) {
        scrollViewToBottom();
    }
    syncScrollBar();
    update();
}

int TerminalWidget::maxViewOffset() const
{
    return m_scrollback.size();
}

void TerminalWidget::pushScrollbackLines(int fromRow, int count)
{
    for (int i = 0; i < count; ++i) {
        QVector<Cell> line(m_cols);
        for (int c = 0; c < m_cols; ++c) {
            line[c] = cellAt(fromRow + i, c);
        }
        m_scrollback.push_back(std::move(line));
    }
    trimScrollback();

    // Keep the viewport anchored when the user is scrolled up
    if (m_viewOffset > 0) {
        m_viewOffset = qMin(m_viewOffset + count, maxViewOffset());
    }
    syncScrollBar();
}

void TerminalWidget::trimScrollback()
{
    const int overflow = m_scrollback.size() - kScrollbackMax;
    if (overflow <= 0) {
        return;
    }
    m_scrollback.erase(m_scrollback.begin(), m_scrollback.begin() + overflow);
    m_viewOffset = qMin(m_viewOffset, maxViewOffset());
    shiftSelectionBy(-overflow * m_cols);
}

void TerminalWidget::clearScrollback()
{
    m_scrollback.clear();
    m_viewOffset = 0;
    syncScrollBar();
}

void TerminalWidget::setViewOffset(int offset)
{
    offset = qBound(0, offset, maxViewOffset());
    if (offset == m_viewOffset) {
        syncScrollBar();
        return;
    }
    m_viewOffset = offset;
    syncScrollBar();
    update();
}

void TerminalWidget::scrollViewBy(int deltaLines)
{
    if (deltaLines == 0 || maxViewOffset() == 0) {
        return;
    }
    setViewOffset(m_viewOffset + deltaLines);
}

void TerminalWidget::scrollViewToBottom()
{
    setViewOffset(0);
}

void TerminalWidget::syncScrollBar()
{
    if (!m_vScroll) {
        return;
    }
    const int maxOff = m_altScreen ? 0 : maxViewOffset();
    m_updatingScrollBar = true;
    if (maxOff <= 0) {
        m_vScroll->setRange(0, 0);
        m_vScroll->setValue(0);
        m_vScroll->hide();
    } else {
        m_vScroll->setRange(0, maxOff);
        m_vScroll->setPageStep(m_rows);
        m_vScroll->setSingleStep(1);
        m_vScroll->setValue(maxOff - m_viewOffset);
        m_vScroll->show();
    }
    m_updatingScrollBar = false;
    layoutScrollBar();
}

void TerminalWidget::layoutScrollBar()
{
    if (!m_vScroll) {
        return;
    }
    const int w = m_vScroll->sizeHint().width();
    m_vScroll->setGeometry(width() - w, 0, w, height());
    m_vScroll->raise();
}

const TerminalWidget::Cell& TerminalWidget::displayCell(int viewRow, int col) const
{
    static const Cell kBlank{};
    if (viewRow < 0 || viewRow >= m_rows || col < 0 || col >= m_cols) {
        return kBlank;
    }

    const int absLine = m_scrollback.size() - m_viewOffset + viewRow;
    if (absLine < 0) {
        return kBlank;
    }
    if (absLine < m_scrollback.size()) {
        const QVector<Cell>& line = m_scrollback.at(absLine);
        if (col >= line.size()) {
            return kBlank;
        }
        return line.at(col);
    }

    const int screenRow = absLine - m_scrollback.size();
    if (screenRow < 0 || screenRow >= m_rows) {
        return kBlank;
    }
    return cellAt(screenRow, col);
}

QPoint TerminalWidget::cellFromPos(const QPoint& pos) const
{
    const int cw = cellWidth();
    const int ch = cellHeight();
    const int col = qBound(0, pos.x() / cw, m_cols - 1);
    const int row = qBound(0, pos.y() / ch, m_rows - 1);
    return QPoint(col, row);
}

int TerminalWidget::absIndexFromView(int viewRow, int col) const
{
    // Stable across viewport scrolling: absolute line 0 is the oldest scrollback
    // line; the live screen sits above the newest scrollback line.
    const int absLine = m_scrollback.size() - m_viewOffset + viewRow;
    return absLine * m_cols + col;
}

int TerminalWidget::maxAbsIndex() const
{
    return (m_scrollback.size() + m_rows) * m_cols - 1;
}

const TerminalWidget::Cell& TerminalWidget::cellAtAbsLine(int absLine, int col) const
{
    static const Cell kBlank{};
    if (absLine < 0 || absLine >= m_scrollback.size() + m_rows || col < 0 || col >= m_cols) {
        return kBlank;
    }
    if (absLine < m_scrollback.size()) {
        const QVector<Cell>& line = m_scrollback.at(absLine);
        if (col >= line.size()) {
            return kBlank;
        }
        return line.at(col);
    }
    return cellAt(absLine - m_scrollback.size(), col);
}

bool TerminalWidget::hasSelection() const
{
    return m_selStart >= 0 && m_selEnd >= 0 && m_selEnd >= m_selStart;
}

bool TerminalWidget::isCellSelected(int row, int col) const
{
    if (!hasSelection()) {
        return false;
    }
    const int idx = absIndexFromView(row, col);
    return idx >= m_selStart && idx <= m_selEnd;
}

void TerminalWidget::clearSelection()
{
    if (m_selStart < 0 && m_selEnd < 0) {
        return;
    }
    m_selAnchor = -1;
    m_selStart = -1;
    m_selEnd = -1;
    update();
}

void TerminalWidget::setSelectionRange(int a, int b)
{
    if (a < 0 || b < 0) {
        clearSelection();
        return;
    }
    const int last = maxAbsIndex();
    a = qBound(0, a, last);
    b = qBound(0, b, last);
    m_selStart = qMin(a, b);
    m_selEnd = qMax(a, b);
    update();
}

void TerminalWidget::shiftSelectionBy(int cellDelta)
{
    if (m_selAnchor < 0 || m_selStart < 0 || m_selEnd < 0) {
        return;
    }
    const int last = maxAbsIndex();
    auto shift = [&](int& v) {
        if (v < 0) {
            return;
        }
        v = qBound(0, v + cellDelta, last);
    };
    shift(m_selAnchor);
    shift(m_selStart);
    shift(m_selEnd);
    if (m_selStart > m_selEnd) {
        clearSelection();
    }
}

bool TerminalWidget::isWordChar(char32_t ch) const
{
    if (ch <= 0x7f) {
        const char c = static_cast<char>(ch);
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || c == '_' || c == '-' || c == '.' || c == '/' || c == '~' || c == ':' || c == '@';
    }
    return ch != U' ';
}

QString TerminalWidget::selectedText() const
{
    if (!hasSelection()) {
        return {};
    }

    QString text;
    for (int idx = m_selStart; idx <= m_selEnd; ++idx) {
        const int r = idx / m_cols;    // absolute line
        const int c = idx % m_cols;
        if (idx > m_selStart && c == 0) {
            // Trim trailing spaces from previous line, then newline
            while (text.endsWith(QLatin1Char(' '))) {
                text.chop(1);
            }
            text += QLatin1Char('\n');
        }
        const char32_t ch = cellAtAbsLine(r, c).ch;
        text += QString::fromUcs4(&ch, 1);
    }

    while (text.endsWith(QLatin1Char(' '))) {
        text.chop(1);
    }
    return text;
}

void TerminalWidget::copySelectionToClipboard(bool clearSel)
{
    const QString text = selectedText();
    if (!text.isEmpty()) {
        QApplication::clipboard()->setText(text);
    }
    if (clearSel) {
        clearSelection();
    }
}

void TerminalWidget::pasteClipboard()
{
    if (!m_interactive) {
        return;
    }
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) {
        return;
    }

    // Raw-mode TUIs (nano, vim, less) expect the Enter key to arrive as a
    // carriage return (\r), not a line feed (\n). Normalize every newline in
    // the pasted text to \r so multi-line paste lands correctly regardless of
    // whether the clipboard uses LF, CRLF, or bare CR.
    QByteArray data = text.toUtf8();
    data.replace("\r\n", "\r");
    data.replace('\n', '\r');

    emit inputReady(data);
    // On paste, clear any active selection.
    clearSelection();
}

void TerminalWidget::injectInput(const QByteArray& data)
{
    if (!m_interactive || data.isEmpty()) {
        return;
    }
    QByteArray normalized = data;
    normalized.replace("\r\n", "\r");
    normalized.replace('\n', '\r');
    emit inputReady(normalized);
}

QString TerminalWidget::captureRecentText(int maxLines) const
{
    if (maxLines < 1) {
        maxLines = 1;
    }
    const int totalLines = m_scrollback.size() + m_rows;
    const int startAbs = qMax(0, totalLines - maxLines);
    QString out;
    out.reserve(maxLines * (m_cols + 1));
    for (int abs = startAbs; abs < totalLines; ++abs) {
        QString line;
        line.reserve(m_cols);
        for (int c = 0; c < m_cols; ++c) {
            const char32_t ch = cellAtAbsLine(abs, c).ch;
            if (ch == 0 || ch == U' ') {
                line.append(QLatin1Char(' '));
            } else if (ch > 0xFFFF) {
                line.append(QChar(QChar::ReplacementCharacter));
            } else {
                line.append(QChar(ushort(ch)));
            }
        }
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
        if (!out.isEmpty()) {
            out += QLatin1Char('\n');
        }
        out += line;
    }
    // Drop leading blank lines so the agent sees useful tail.
    while (out.startsWith(QLatin1Char('\n'))) {
        out.remove(0, 1);
    }
    return out.trimmed();
}

void TerminalWidget::showContextMenu(const QPoint& globalPos)
{
    if (!m_contextMenu) {
        m_contextMenu = new QMenu(this);
        m_contextMenu->setObjectName(QStringLiteral("termContextMenu"));

        auto* copyAction = m_contextMenu->addAction(QStringLiteral("Copy"));
        connect(copyAction, &QAction::triggered, this, [this]() {
            if (hasSelection()) {
                copySelectionToClipboard();
            } else {
                copySelectionToClipboard(false);
            }
        });

        auto* pasteAction = m_contextMenu->addAction(QStringLiteral("Paste"));
        connect(pasteAction, &QAction::triggered, this, [this]() { pasteClipboard(); });

        m_xmodemSeparator = m_contextMenu->addSeparator();
        m_xmodemSeparator->setVisible(m_xmodemAvailable);
        m_xmodemAction = m_contextMenu->addAction(QStringLiteral("Send file via XMODEM…"));
        m_xmodemAction->setVisible(m_xmodemAvailable);
        m_xmodemAction->setEnabled(m_xmodemAvailable);
        connect(m_xmodemAction, &QAction::triggered, this,
                [this]() { emit xmodemSendRequested(); });
    }
    m_contextMenu->popup(globalPos);
}

void TerminalWidget::selectWordAt(int row, int col)
{
    const int last = maxAbsIndex();
    int start = absIndexFromView(row, col);
    int end = start;
    if (!isWordChar(cellAtAbsLine(start / m_cols, start % m_cols).ch)) {
        setSelectionRange(start, end);
        return;
    }
    while (start > 0) {
        const int r = (start - 1) / m_cols;
        const int c = (start - 1) % m_cols;
        if (!isWordChar(cellAtAbsLine(r, c).ch)) {
            break;
        }
        --start;
    }
    while (end < last) {
        const int r = (end + 1) / m_cols;
        const int c = (end + 1) % m_cols;
        if (!isWordChar(cellAtAbsLine(r, c).ch)) {
            break;
        }
        ++end;
    }
    m_selAnchor = start;
    setSelectionRange(start, end);
}

void TerminalWidget::selectLineAt(int row)
{
    const int start = absIndexFromView(row, 0);
    const int end = absIndexFromView(row, m_cols - 1);
    m_selAnchor = start;
    setSelectionRange(start, end);
}

bool TerminalWidget::drawBoxDrawing(QPainter& painter, char32_t ch, const QRect& cell, const QColor& fg) const
{
    const int x = cell.x();
    const int y = cell.y();
    const int w = cell.width();
    const int h = cell.height();
    if (w <= 0 || h <= 0) {
        return false;
    }

    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const int light = qMax(1, qMin(w, h) / 6);
    const int heavy = qMax(light + 1, qMin(w, h) / 3);
    const int gap = qMax(1, light); // separation for double lines

    auto fill = [&](int rx, int ry, int rw, int rh) {
        if (rw > 0 && rh > 0) {
            painter.fillRect(rx, ry, rw, rh, fg);
        }
    };
    auto hBar = [&](int thickness, int yCenter, int x0, int x1) {
        fill(x0, yCenter - thickness / 2, x1 - x0, thickness);
    };
    auto vBar = [&](int thickness, int xCenter, int y0, int y1) {
        fill(xCenter - thickness / 2, y0, thickness, y1 - y0);
    };

    // Braille patterns U+2800–U+28FF (btop / bpytop graphs)
    // Dot layout:  1 4
    //              2 5
    //              3 6
    //              7 8
    if (ch >= 0x2800 && ch <= 0x28FF) {
        const unsigned bits = static_cast<unsigned>(ch - 0x2800);
        const int padX = qMax(1, w / 6);
        const int padY = qMax(1, h / 8);
        const int col0 = x + padX;
        const int col1 = x + w - padX;
        const int rowY[4] = {
            y + padY,
            y + h / 3,
            y + (2 * h) / 3,
            y + h - padY,
        };
        const int dot = qBound(1, qMin(w, h) / 5, 3);
        const int half = dot / 2;
        auto plot = [&](int dx, int dy) {
            fill(dx - half, dy - half, dot, dot);
        };
        // bit0=dot1 … bit7=dot8
        if (bits & 0x01) {
            plot(col0, rowY[0]);
        }
        if (bits & 0x02) {
            plot(col0, rowY[1]);
        }
        if (bits & 0x04) {
            plot(col0, rowY[2]);
        }
        if (bits & 0x08) {
            plot(col1, rowY[0]);
        }
        if (bits & 0x10) {
            plot(col1, rowY[1]);
        }
        if (bits & 0x20) {
            plot(col1, rowY[2]);
        }
        if (bits & 0x40) {
            plot(col0, rowY[3]);
        }
        if (bits & 0x80) {
            plot(col1, rowY[3]);
        }
        return true;
    }

    // Block elements U+2580–U+259F
    if (ch >= 0x2580 && ch <= 0x259F) {
        switch (ch) {
        case 0x2580: fill(x, y, w, h / 2); return true;                         // ▀
        case 0x2581: fill(x, y + h - h / 8, w, h / 8); return true;              // ▁
        case 0x2582: fill(x, y + h - h / 4, w, h / 4); return true;              // ▂
        case 0x2583: fill(x, y + h - (3 * h) / 8, w, (3 * h) / 8); return true;  // ▃
        case 0x2584: fill(x, y + h / 2, w, h - h / 2); return true;              // ▄
        case 0x2585: fill(x, y + (3 * h) / 8, w, h - (3 * h) / 8); return true;  // ▅
        case 0x2586: fill(x, y + h / 4, w, h - h / 4); return true;              // ▆
        case 0x2587: fill(x, y + h / 8, w, h - h / 8); return true;              // ▇
        case 0x2588: fill(x, y, w, h); return true;                              // █
        case 0x2589: fill(x, y, (7 * w) / 8, h); return true;                    // ▉
        case 0x258A: fill(x, y, (3 * w) / 4, h); return true;                    // ▊
        case 0x258B: fill(x, y, (5 * w) / 8, h); return true;                    // ▋
        case 0x258C: fill(x, y, w / 2, h); return true;                          // ▌
        case 0x258D: fill(x, y, (3 * w) / 8, h); return true;                    // ▍
        case 0x258E: fill(x, y, w / 4, h); return true;                          // ▎
        case 0x258F: fill(x, y, w / 8, h); return true;                          // ▏
        case 0x2590: fill(x + w / 2, y, w - w / 2, h); return true;              // ▐
        case 0x2591: { // ░ light shade
            for (int yy = y; yy < y + h; yy += 2) {
                for (int xx = x + ((yy - y) & 2); xx < x + w; xx += 4) {
                    fill(xx, yy, 1, 1);
                }
            }
            return true;
        }
        case 0x2592: { // ▒ medium shade
            for (int yy = y; yy < y + h; ++yy) {
                for (int xx = x + ((yy - y) & 1); xx < x + w; xx += 2) {
                    fill(xx, yy, 1, 1);
                }
            }
            return true;
        }
        case 0x2593: { // ▓ dark shade
            for (int yy = y; yy < y + h; ++yy) {
                for (int xx = x; xx < x + w; ++xx) {
                    if (((xx + yy) & 3) != 0) {
                        fill(xx, yy, 1, 1);
                    }
                }
            }
            return true;
        }
        case 0x2594: fill(x, y, w, qMax(1, h / 8)); return true;                 // ▔
        case 0x2595: fill(x + w - qMax(1, w / 8), y, qMax(1, w / 8), h); return true; // ▕
        case 0x2596: fill(x, y + h / 2, w / 2, h - h / 2); return true;          // ▖
        case 0x2597: fill(x + w / 2, y + h / 2, w - w / 2, h - h / 2); return true; // ▗
        case 0x2598: fill(x, y, w / 2, h / 2); return true;                      // ▘
        case 0x2599: // ▙
            fill(x, y, w / 2, h);
            fill(x + w / 2, y + h / 2, w - w / 2, h - h / 2);
            return true;
        case 0x259A: // ▚
            fill(x, y + h / 2, w / 2, h - h / 2);
            fill(x + w / 2, y, w - w / 2, h / 2);
            return true;
        case 0x259B: // ▛
            fill(x, y, w, h / 2);
            fill(x, y + h / 2, w / 2, h - h / 2);
            return true;
        case 0x259C: // ▜
            fill(x, y, w, h / 2);
            fill(x + w / 2, y + h / 2, w - w / 2, h - h / 2);
            return true;
        case 0x259D: fill(x + w / 2, y, w - w / 2, h / 2); return true;          // ▝
        case 0x259E: // ▞
            fill(x, y, w / 2, h / 2);
            fill(x + w / 2, y + h / 2, w - w / 2, h - h / 2);
            return true;
        case 0x259F: // ▟
            fill(x + w / 2, y, w - w / 2, h);
            fill(x, y + h / 2, w / 2, h - h / 2);
            return true;
        default:
            break;
        }
    }

    if (ch < 0x2500 || ch > 0x257F) {
        return false;
    }

    // Connection bits: 1=right, 2=up, 4=left, 8=down. Values: 0 none, 1 light, 2 heavy/double
    int R = 0, U = 0, L = 0, D = 0;
    bool doubles = false;

    switch (ch) {
    case 0x2500: R = L = 1; break;                         // ─
    case 0x2501: R = L = 2; break;                         // ━
    case 0x2502: U = D = 1; break;                         // │
    case 0x2503: U = D = 2; break;                         // ┃
    case 0x2504: case 0x2505: R = L = (ch == 0x2505) ? 2 : 1; break; // ┄ ┅ (approx solid)
    case 0x2506: case 0x2507: U = D = (ch == 0x2507) ? 2 : 1; break;
    case 0x2508: case 0x2509: R = L = (ch == 0x2509) ? 2 : 1; break;
    case 0x250A: case 0x250B: U = D = (ch == 0x250B) ? 2 : 1; break;
    case 0x250C: R = D = 1; break;                         // ┌
    case 0x250D: R = 2; D = 1; break;
    case 0x250E: R = 1; D = 2; break;
    case 0x250F: R = D = 2; break;
    case 0x2510: L = D = 1; break;                         // ┐
    case 0x2511: L = 2; D = 1; break;
    case 0x2512: L = 1; D = 2; break;
    case 0x2513: L = D = 2; break;
    case 0x2514: R = U = 1; break;                         // └
    case 0x2515: R = 2; U = 1; break;
    case 0x2516: R = 1; U = 2; break;
    case 0x2517: R = U = 2; break;
    case 0x2518: L = U = 1; break;                         // ┘
    case 0x2519: L = 2; U = 1; break;
    case 0x251A: L = 1; U = 2; break;
    case 0x251B: L = U = 2; break;
    case 0x251C: R = U = D = 1; break;                     // ├
    case 0x251D: R = 2; U = D = 1; break;
    case 0x251E: R = 1; U = 2; D = 1; break;
    case 0x251F: R = 1; U = 1; D = 2; break;
    case 0x2520: R = 1; U = D = 2; break;
    case 0x2521: R = 2; U = 2; D = 1; break;
    case 0x2522: R = 2; U = 1; D = 2; break;
    case 0x2523: R = U = D = 2; break;
    case 0x2524: L = U = D = 1; break;                     // ┤
    case 0x2525: L = 2; U = D = 1; break;
    case 0x2526: L = 1; U = 2; D = 1; break;
    case 0x2527: L = 1; U = 1; D = 2; break;
    case 0x2528: L = 1; U = D = 2; break;
    case 0x2529: L = 2; U = 2; D = 1; break;
    case 0x252A: L = 2; U = 1; D = 2; break;
    case 0x252B: L = U = D = 2; break;
    case 0x252C: L = R = D = 1; break;                     // ┬
    case 0x252D: L = 2; R = 1; D = 1; break;
    case 0x252E: L = 1; R = 2; D = 1; break;
    case 0x252F: L = R = 2; D = 1; break;
    case 0x2530: L = R = 1; D = 2; break;
    case 0x2531: L = 2; R = 1; D = 2; break;
    case 0x2532: L = 1; R = 2; D = 2; break;
    case 0x2533: L = R = D = 2; break;
    case 0x2534: L = R = U = 1; break;                     // ┴
    case 0x2535: L = 2; R = 1; U = 1; break;
    case 0x2536: L = 1; R = 2; U = 1; break;
    case 0x2537: L = R = 2; U = 1; break;
    case 0x2538: L = R = 1; U = 2; break;
    case 0x2539: L = 2; R = 1; U = 2; break;
    case 0x253A: L = 1; R = 2; U = 2; break;
    case 0x253B: L = R = U = 2; break;
    case 0x253C: L = R = U = D = 1; break;                 // ┼
    case 0x253D: L = 2; R = 1; U = D = 1; break;
    case 0x253E: L = 1; R = 2; U = D = 1; break;
    case 0x253F: L = R = 2; U = D = 1; break;
    case 0x2540: L = R = 1; U = 2; D = 1; break;
    case 0x2541: L = R = 1; U = 1; D = 2; break;
    case 0x2542: L = R = 1; U = D = 2; break;
    case 0x2543: L = 2; R = 1; U = 2; D = 1; break;
    case 0x2544: L = 1; R = 2; U = 2; D = 1; break;
    case 0x2545: L = 2; R = 1; U = 1; D = 2; break;
    case 0x2546: L = 1; R = 2; U = 1; D = 2; break;
    case 0x2547: L = R = 2; U = 2; D = 1; break;
    case 0x2548: L = R = 2; U = 1; D = 2; break;
    case 0x2549: L = 2; R = 1; U = D = 2; break;
    case 0x254A: L = 1; R = 2; U = D = 2; break;
    case 0x254B: L = R = U = D = 2; break;
    case 0x254C: case 0x254D: R = L = (ch == 0x254D) ? 2 : 1; break;
    case 0x254E: case 0x254F: U = D = (ch == 0x254F) ? 2 : 1; break;
    case 0x2550: R = L = 1; doubles = true; break;         // ═
    case 0x2551: U = D = 1; doubles = true; break;         // ║
    case 0x2552: R = 1; D = 1; doubles = true; break;      // ╒ mixed → treat as double corner-ish
    case 0x2553: R = 1; D = 1; doubles = true; break;      // ╓
    case 0x2554: R = D = 1; doubles = true; break;         // ╔
    case 0x2555: L = D = 1; doubles = true; break;         // ╕
    case 0x2556: L = D = 1; doubles = true; break;         // ╖
    case 0x2557: L = D = 1; doubles = true; break;         // ╗
    case 0x2558: R = U = 1; doubles = true; break;         // ╘
    case 0x2559: R = U = 1; doubles = true; break;         // ╙
    case 0x255A: R = U = 1; doubles = true; break;         // ╚
    case 0x255B: L = U = 1; doubles = true; break;         // ╛
    case 0x255C: L = U = 1; doubles = true; break;         // ╜
    case 0x255D: L = U = 1; doubles = true; break;         // ╝
    case 0x255E: case 0x255F: case 0x2560:                 // ╞ ╟ ╠
        R = U = D = 1; doubles = true; break;
    case 0x2561: case 0x2562: case 0x2563:                 // ╡ ╢ ╣
        L = U = D = 1; doubles = true; break;
    case 0x2564: case 0x2565: case 0x2566:                 // ╤ ╥ ╦
        L = R = D = 1; doubles = true; break;
    case 0x2567: case 0x2568: case 0x2569:                 // ╧ ╨ ╩
        L = R = U = 1; doubles = true; break;
    case 0x256A: case 0x256B: case 0x256C:                 // ╪ ╫ ╬
        L = R = U = D = 1; doubles = true; break;
    case 0x256D: R = D = 1; break;                         // ╭
    case 0x256E: L = D = 1; break;                         // ╮
    case 0x256F: L = U = 1; break;                         // ╯
    case 0x2570: R = U = 1; break;                         // ╰
    case 0x2571: case 0x2572: case 0x2573:                 // ╱ ╲ ╳ drawn below
        break;
    case 0x2574: L = 1; break;                             // ╴
    case 0x2575: U = 1; break;                             // ╵
    case 0x2576: R = 1; break;                             // ╶
    case 0x2577: D = 1; break;                             // ╷
    case 0x2578: L = 2; break;
    case 0x2579: U = 2; break;
    case 0x257A: R = 2; break;
    case 0x257B: D = 2; break;
    case 0x257C: L = 1; R = 2; break;
    case 0x257D: U = 1; D = 2; break;
    case 0x257E: L = 2; R = 1; break;
    case 0x257F: U = 2; D = 1; break;
    default:
        return false;
    }

    if (ch == 0x2571) { // ╱
        painter.setPen(QPen(fg, light));
        painter.drawLine(x, y + h - 1, x + w - 1, y);
        return true;
    }
    if (ch == 0x2572) { // ╲
        painter.setPen(QPen(fg, light));
        painter.drawLine(x, y, x + w - 1, y + h - 1);
        return true;
    }
    if (ch == 0x2573) { // ╳
        painter.setPen(QPen(fg, light));
        painter.drawLine(x, y, x + w - 1, y + h - 1);
        painter.drawLine(x, y + h - 1, x + w - 1, y);
        return true;
    }

    if (doubles) {
        // Double-line geometry: two parallel strokes spanning the full cell
        if (L || R) {
            const int y1 = cy - gap;
            const int y2 = cy + gap;
            if (L) {
                hBar(light, y1, x, cx + light);
                hBar(light, y2, x, cx + light);
            }
            if (R) {
                hBar(light, y1, cx - light, x + w);
                hBar(light, y2, cx - light, x + w);
            }
        }
        if (U || D) {
            const int x1 = cx - gap;
            const int x2 = cx + gap;
            if (U) {
                vBar(light, x1, y, cy + light);
                vBar(light, x2, y, cy + light);
            }
            if (D) {
                vBar(light, x1, cy - light, y + h);
                vBar(light, x2, cy - light, y + h);
            }
        }
        return true;
    }

    auto thickness = [&](int weight) { return weight >= 2 ? heavy : light; };

    if (L) {
        hBar(thickness(L), cy, x, cx + thickness(L) / 2);
    }
    if (R) {
        hBar(thickness(R), cy, cx - thickness(R) / 2, x + w);
    }
    if (U) {
        vBar(thickness(U), cx, y, cy + thickness(U) / 2);
    }
    if (D) {
        vBar(thickness(D), cx, cy - thickness(D) / 2, y + h);
    }
    return L || R || U || D;
}

QString TerminalWidget::lineTextAt(int viewRow) const
{
    QString line;
    line.reserve(m_cols);
    for (int c = 0; c < m_cols; ++c) {
        const char32_t ch = displayCell(viewRow, c).ch;
        if (ch == 0 || ch > 0xFFFF) {
            line.append(QLatin1Char(' '));
        } else {
            line.append(QChar(ushort(ch)));
        }
    }
    return line;
}

QVector<QColor> TerminalWidget::highlightColorsForRow(int viewRow) const
{
    QVector<QColor> colors(m_cols);
    const bool wantAddr = AppSettings::highlightAddresses();
    const bool wantKeys = AppSettings::highlightLogKeywords();
    const bool wantCisco = AppSettings::highlightCiscoCli();
    if (!wantAddr && !wantKeys && !wantCisco) {
        return colors;
    }

    const QString line = lineTextAt(viewRow);
    if (line.trimmed().isEmpty()) {
        return colors;
    }
    // The application inserts this standalone status line when a session
    // opens. Keep it neutral while still highlighting "connected" in actual
    // device output (for example, interface status tables).
    if (line.trimmed().compare(QStringLiteral("connected"), Qt::CaseInsensitive) == 0) {
        return colors;
    }

    static const QRegularExpression neutralUnreachableCommand(
        QStringLiteral(
            R"(^\s*(?:[A-Za-z0-9_.-]+(?:\([^)]+\))?[>#]\s*)?no\s+ip\s+icmp\s+rate-limit\s+unreachable\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const bool suppressUnreachableFault = neutralUnreachableCommand.match(line).hasMatch();
    static const QRegularExpression neutralParityDisabled(
        QStringLiteral(R"(\bparity\s+disabled\b)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch parityDisabledMatch = neutralParityDisabled.match(line);

    enum class RuleGroup { Address, Keyword, Cisco };
    struct Rule {
        QRegularExpression re;
        QColor color;
        int priority; // higher wins on overlap
        RuleGroup group;
    };

    // Built once — patterns are cheap; could cache statically
    static const QList<Rule> rules = {
        // IP & MAC first (higher priority than keywords that might appear nearby)
        {QRegularExpression(
             QStringLiteral(R"(\b(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\b)")),
         QColor(0x56, 0xb6, 0xc2), 30, RuleGroup::Address},
        {QRegularExpression(
             QStringLiteral(R"(\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b)")),
         QColor(0xc6, 0x78, 0xdd), 30, RuleGroup::Address},
        // IPv6 (compressed forms included)
        {QRegularExpression(
             QStringLiteral(R"(\b(?:[0-9A-Fa-f]{1,4}:){2,7}[0-9A-Fa-f]{0,4}\b)")),
         QColor(0x56, 0xb6, 0xc2), 28, RuleGroup::Address},

        {QRegularExpression(QStringLiteral(R"(\bERROR\b)"), QRegularExpression::CaseInsensitiveOption),
         QColor(0xf4, 0x47, 0x47), 20, RuleGroup::Keyword},
        {QRegularExpression(QStringLiteral(R"(\bWARN(?:ING)?\b)"), QRegularExpression::CaseInsensitiveOption),
         QColor(0xe5, 0xc0, 0x7b), 20, RuleGroup::Keyword},
        {QRegularExpression(QStringLiteral(R"(\bOK\b)"), QRegularExpression::CaseInsensitiveOption),
         QColor(0x23, 0xd1, 0x8b), 20, RuleGroup::Keyword},
        {QRegularExpression(QStringLiteral(R"(\bINFO\b)"), QRegularExpression::CaseInsensitiveOption),
         QColor(0x5c, 0xa1, 0xf6), 20, RuleGroup::Keyword},
        {QRegularExpression(QStringLiteral(R"(\bDEBUG\b)"), QRegularExpression::CaseInsensitiveOption),
         QColor(0x9a, 0x9a, 0xb8), 20, RuleGroup::Keyword},

        // Cisco IOS / IOS-XE / NX-OS operationally useful tokens.
        {QRegularExpression(
             QStringLiteral(R"(^\s*[A-Za-z0-9_.-]+(?:\([^)]+\))?[>#])")),
         QColor(0xe5, 0xc0, 0x7b), 24, RuleGroup::Cisco},
        {QRegularExpression(
             QStringLiteral(R"(\b(?:(?:GigabitEthernet|FastEthernet|TenGigabitEthernet|TwentyFiveGigE|FortyGigabitEthernet|HundredGigE|Ethernet|Port-channel|Loopback|Vlan|Tunnel|Serial)|(?:Gi|G|Fa|Te|Twe|Fo|Hu|Eth|Po|Lo|L|Vl|Tu|Se))\d+(?:[/.:-]\d+)*\b)"),
             QRegularExpression::CaseInsensitiveOption),
         QColor(0x56, 0xb6, 0xc2), 38, RuleGroup::Cisco},
        // Cisco's dotted MAC notation (0011.2233.4455).
        {QRegularExpression(QStringLiteral(R"(\b[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\b)")),
         QColor(0xc6, 0x78, 0xdd), 42, RuleGroup::Cisco},
        {QRegularExpression(
             QStringLiteral(R"(\b(?:OSPFv?3?|BGP|EIGRP|RIP|IS-IS|STP|RSTP|MSTP|PVST\+?|HSRP|VRRP|GLBP|CDP|LLDP|LACP|PAgP|VTP|DTP|ARP|NAT|ACL|IPsec|VRF|VLAN)\b)"),
             QRegularExpression::CaseInsensitiveOption),
         QColor(0xc6, 0x78, 0xdd), 27, RuleGroup::Cisco},
        {QRegularExpression(QStringLiteral(R"(^\s*[CLSDORBE](?:\*|\s+E[12])?(?=\s))"),
                            QRegularExpression::CaseInsensitiveOption),
         QColor(0xc6, 0x78, 0xdd), 26, RuleGroup::Cisco},
        {QRegularExpression(
             QStringLiteral(R"(\b(?:up|connected|forwarding|established|full|enabled|success(?:ful)?|permit(?:ted)?)\b)"),
             QRegularExpression::CaseInsensitiveOption),
         QColor(0x23, 0xd1, 0x8b), 34, RuleGroup::Cisco},
        {QRegularExpression(
             QStringLiteral(R"(\b(?:idle|active|init|2way|exstart|exchange|loading|listening|learning|blocking|discarding|standby|trunk(?:ing)?|routed|access)\b)"),
             QRegularExpression::CaseInsensitiveOption),
         QColor(0xe5, 0xc0, 0x7b), 32, RuleGroup::Cisco},
        {QRegularExpression(
             QStringLiteral(R"(\b(?:administratively\s+down|err-?disabled|notconnect|inactive|disabled|shutdown|down|failed|failure|timed\s+out|timeout\s+(?:while\s+)?waiting\s+for|CRC|input\s+errors?|output\s+errors?|runts?|giants?|overruns?|ignored|resets?|lost\s+carrier|no\s+carrier|late\s+collisions?|collisions?|drops?|dropped|discards?|discarded|denied?|unreachable|flapping)\b)"),
             QRegularExpression::CaseInsensitiveOption),
         QColor(0xf4, 0x47, 0x47), 55, RuleGroup::Cisco},
        // IOS syslog mnemonic with severity 0-3 = urgent/critical/error.
        {QRegularExpression(QStringLiteral(R"(%[A-Z0-9_]+-[0-3]-[A-Z0-9_]+)")),
         QColor(0xf4, 0x47, 0x47), 58, RuleGroup::Cisco},
        {QRegularExpression(QStringLiteral(R"(%[A-Z0-9_]+-4-[A-Z0-9_]+)")),
         QColor(0xe5, 0xc0, 0x7b), 57, RuleGroup::Cisco},
        {QRegularExpression(QStringLiteral(R"(%[A-Z0-9_]+-[5-7]-[A-Z0-9_]+)")),
         QColor(0x5c, 0xa1, 0xf6), 56, RuleGroup::Cisco},
    };

    QVector<int> pri(m_cols, -1);

    for (const Rule& rule : rules) {
        const bool enabled = rule.group == RuleGroup::Address ? wantAddr
            : rule.group == RuleGroup::Keyword ? wantKeys
                                               : wantCisco;
        if (!enabled) {
            continue;
        }
        auto it = rule.re.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (suppressUnreachableFault
                && m.captured().compare(QLatin1String("unreachable"), Qt::CaseInsensitive) == 0) {
                continue;
            }
            if (parityDisabledMatch.hasMatch()
                && m.captured().compare(QLatin1String("disabled"), Qt::CaseInsensitive) == 0
                && m.capturedStart() >= parityDisabledMatch.capturedStart()
                && m.capturedEnd() <= parityDisabledMatch.capturedEnd()) {
                continue;
            }
            const int a = m.capturedStart();
            const int b = m.capturedEnd(); // exclusive
            if (a < 0 || b <= a) {
                continue;
            }
            for (int i = a; i < b && i < m_cols; ++i) {
                if (rule.priority >= pri[i]) {
                    pri[i] = rule.priority;
                    colors[i] = rule.color;
                }
            }
        }
    }
    return colors;
}

void TerminalWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, false);
    p.setRenderHint(QPainter::Antialiasing, false);

    if (!m_bgImagePixmap.isNull()) {
        p.drawPixmap(rect(), m_bgImagePixmap.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        p.setOpacity(m_bgOpacity);
    }
    p.fillRect(rect(), m_defaultBg);
    p.setOpacity(1.0);

    QFont f = font();
    f.setBold(false);
    f.setKerning(false);
    f.setHintingPreference(QFont::PreferFullHinting);
    p.setFont(f);

    const QFontMetrics fm(f, this);
    const int cw = cellWidth();
    const int ch = cellHeight();
    const int baseline = fm.ascent();
    const bool light = AppSettings::isLightTheme();
    const QColor selBg = light ? QColor(0xbf, 0xbf, 0xbf) : QColor(0x4a, 0x4a, 0x4a);
    const QColor selFg = light ? QColor(0x10, 0x10, 0x10) : QColor(0xff, 0xff, 0xff);

    int termWidth = width();
    if (m_vScroll && m_vScroll->isVisible()) {
        termWidth -= m_vScroll->width();
    }

    for (int r = 0; r < m_rows; ++r) {
        const QVector<QColor> highlights = highlightColorsForRow(r);

        for (int c = 0; c < m_cols; ++c) {
            const Cell& cell = displayCell(r, c);
            QColor fg = QColor::fromRgb(cell.fg);
            QColor bg = QColor::fromRgb(cell.bg);
            if (cell.inverse) {
                qSwap(fg, bg);
            }

            const bool highlighted = c < highlights.size() && highlights[c].isValid();
            if (highlighted) {
                fg = highlights[c];
            }

            const bool selected = isCellSelected(r, c);
            if (selected) {
                bg = selBg;
                fg = selFg;
            }

            const QRect cellRect(c * cw, r * ch, cw, ch);
            if (selected || bg != m_defaultBg) {
                p.fillRect(cellRect, bg);
            }

            if (cell.ch != U' ' && cell.ch != 0) {
                if (!drawBoxDrawing(p, cell.ch, cellRect, fg)) {
                    p.setFont(f);
                    p.setPen(fg);
                    p.setClipRect(cellRect, Qt::IntersectClip);
                    const QString text = QString::fromUcs4(&cell.ch, 1);
                    const int x = cellRect.x();
                    const int y = cellRect.y() + baseline;
                    p.drawText(x, y, text);
                    if (cell.bold || highlighted) {
                        p.drawText(x + 1, y, text);
                    }
                    p.setClipping(false);
                }

                if (cell.underline) {
                    p.setPen(fg);
                    p.drawLine(c * cw, r * ch + ch - 1, c * cw + cw - 1, r * ch + ch - 1);
                }
            }
        }
    }

    if (isLiveView() && m_cursorVisible && (m_interactive || hasFocus()) && !m_selecting) {
        if (m_cx * cw + cw <= termWidth) {
            const QRect cursorRect(m_cx * cw, m_cy * ch, cw, ch);
            p.fillRect(cursorRect, QColor(0xc8, 0xc8, 0xc8, 160));
        }
    }
}

void TerminalWidget::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    const QPoint cell = cellFromPos(event->pos());

    // The explicit menu setting is a local UI choice and takes precedence over
    // xterm mouse reporting. In standard mode, right-click can still be sent to
    // a mouse-aware remote application or used for PuTTY-style paste.
    if (event->button() == Qt::RightButton && AppSettings::copyPasteMenu()) {
        showContextMenu(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (shouldReportMouse(event)) {
        clearSelection();
        const int btn = encodeMouseButton(event->button(), false);
        if (btn <= 2) {
            m_mousePressedBtn = btn;
            m_lastMouseCell = cell;
            sendMouseReport(btn, cell.x(), cell.y(), false);
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        // PuTTY: right-click pastes when the local context-menu mode is off.
        pasteClipboard();
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        // PuTTY: middle-click also pastes
        pasteClipboard();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastClickMs <= QApplication::doubleClickInterval()
            && cell == m_lastClickCell) {
            ++m_clickCount;
        } else {
            m_clickCount = 1;
        }
        m_lastClickMs = now;
        m_lastClickCell = cell;

        if (m_clickCount >= 3) {
            selectLineAt(cell.y());
            copySelectionToClipboard(false);
            m_selecting = false;
            m_clickCount = 0;
            event->accept();
            return;
        }

        m_selecting = true;
        m_selAnchor = absIndexFromView(cell.y(), cell.x());
        setSelectionRange(m_selAnchor, m_selAnchor);
        grabMouse();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint cell = cellFromPos(event->pos());

    if (shouldReportMouse(event)) {
        if (m_mouseMode == 1003) {
            // Any-event: report all motion
            const bool pressed = m_mousePressedBtn >= 0;
            const int btn = pressed ? (m_mousePressedBtn + 32) : (3 + 32); // 35 = motion no button
            if (cell != m_lastMouseCell) {
                m_lastMouseCell = cell;
                sendMouseReport(btn, cell.x(), cell.y(), false);
            }
            event->accept();
            return;
        }
        if (m_mouseMode == 1002 && m_mousePressedBtn >= 0
            && (event->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton))) {
            // Button-event: drag while held (btop scrollbar)
            if (cell != m_lastMouseCell) {
                m_lastMouseCell = cell;
                sendMouseReport(m_mousePressedBtn + 32, cell.x(), cell.y(), false);
            }
            event->accept();
            return;
        }
    }

    if (m_selecting && (event->buttons() & Qt::LeftButton)) {
        const int idx = absIndexFromView(cell.y(), cell.x());
        setSelectionRange(m_selAnchor, idx);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* event)
{
    const QPoint cell = cellFromPos(event->pos());

    if (shouldReportMouse(event) || (mouseReportingActive() && m_mousePressedBtn >= 0)) {
        // Always complete a reported press with a release if we were tracking a button,
        // even if Shift is now held mid-gesture.
        if (m_mousePressedBtn >= 0 && mouseReportingActive() && m_interactive) {
            const int btn = encodeMouseButton(event->button(), false);
            if (btn == m_mousePressedBtn || btn <= 2) {
                sendMouseReport(m_mousePressedBtn, cell.x(), cell.y(), true);
                m_mousePressedBtn = -1;
                m_lastMouseCell = QPoint(-1, -1);
                event->accept();
                return;
            }
        }
    }

    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        releaseMouse();

        const int idx = absIndexFromView(cell.y(), cell.x());
        setSelectionRange(m_selAnchor, idx);

        // PuTTY: releasing a selection copies it to the clipboard,
        // but keep the selection visible so it stays highlighted.
        if (hasSelection() && m_selStart != m_selEnd) {
            const QString text = selectedText();
            if (!text.isEmpty()) {
                QApplication::clipboard()->setText(text);
            }
        } else if (hasSelection() && m_selStart == m_selEnd) {
            // Single click: clear selection (no drag)
            clearSelection();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (shouldReportMouse(event)) {
        // Press/release already handled via normal click stream for TUIs.
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const QPoint cell = cellFromPos(event->pos());
        selectWordAt(cell.y(), cell.x());
        copySelectionToClipboard(false);
        m_selecting = false;
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TerminalWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutScrollBar();
    emitSizeIfChanged();
    positionFontZoomOverlay();
}

void TerminalWidget::wheelEvent(QWheelEvent* event)
{
    if (AppSettings::ctrlScrollFontZoom()
        && (event->modifiers() & Qt::ControlModifier)) {
        int delta = event->angleDelta().y();
        if (delta == 0) {
            delta = event->pixelDelta().y();
        }
        if (delta != 0) {
            adjustFontSize(delta > 0 ? 1 : -1);
            event->accept();
            return;
        }
    }

    if (sendWheelToApp(event)) {
        event->accept();
        return;
    }

    if (m_altScreen || maxViewOffset() <= 0) {
        QWidget::wheelEvent(event);
        return;
    }

    int delta = event->angleDelta().y();
    if (delta == 0) {
        delta = event->pixelDelta().y();
    }
    if (delta == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    // Positive wheel = scroll up into history
    const int sensitivity = AppSettings::scrollSensitivity();
    const int lines = qMax(1, qAbs(delta) / 120) * sensitivity;
    scrollViewBy(delta > 0 ? lines : -lines);
    event->accept();
}

void TerminalWidget::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (m_focusReport && m_interactive) {
        reply(QByteArray("\x1b[I"));
    }
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    if (m_focusReport && m_interactive) {
        reply(QByteArray("\x1b[O"));
    }
    update();
}

QSize TerminalWidget::sizeHint() const
{
    return QSize(cellWidth() * 80, cellHeight() * 24);
}

QSize TerminalWidget::minimumSizeHint() const
{
    return QSize(cellWidth() * 40, cellHeight() * 12);
}

void TerminalWidget::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);

    // Scrollback navigation (does not send keys to the PTY)
    if (!m_altScreen && key == Qt::Key_PageUp && mods == Qt::ShiftModifier) {
        scrollViewBy(m_rows);
        event->accept();
        return;
    }
    if (!m_altScreen && key == Qt::Key_PageDown && mods == Qt::ShiftModifier) {
        scrollViewBy(-m_rows);
        event->accept();
        return;
    }
    if (!m_altScreen && key == Qt::Key_Home && mods == (Qt::ShiftModifier | Qt::ControlModifier)) {
        setViewOffset(maxViewOffset());
        event->accept();
        return;
    }
    if (!m_altScreen && key == Qt::Key_End && mods == (Qt::ShiftModifier | Qt::ControlModifier)) {
        scrollViewToBottom();
        event->accept();
        return;
    }

    // PuTTY-style keyboard clipboard that does not steal Ctrl+C / Ctrl+V from TUIs
    if (key == Qt::Key_Insert && mods == Qt::ControlModifier) {
        if (hasSelection()) {
            copySelectionToClipboard();
        }
        event->accept();
        return;
    }
    if (key == Qt::Key_Insert && mods == Qt::ShiftModifier) {
        pasteClipboard();
        event->accept();
        return;
    }

    if (!m_interactive) {
        QWidget::keyPressEvent(event);
        return;
    }

    // Typing returns to the live prompt
    if (!isLiveView() && key != Qt::Key_Shift && key != Qt::Key_Control
        && key != Qt::Key_Alt && key != Qt::Key_Meta) {
        scrollViewToBottom();
    }

    // Ignore lone modifier presses
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_AltGr || key == Qt::Key_CapsLock || key == Qt::Key_NumLock
        || key == Qt::Key_ScrollLock) {
        event->accept();
        return;
    }

    const QByteArray payload = encodeKey(event);
    if (!payload.isEmpty()) {
        emit inputReady(payload);
        event->accept();
        return;
    }

    event->accept();
}

bool TerminalWidget::event(QEvent* event)
{
    if (m_interactive) {
        // Tab/Backtab are normally eaten for focus chaining before keyPressEvent
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
                keyPressEvent(ke);
                return true;
            }
        }
    }
    return QWidget::event(event);
}

bool TerminalWidget::focusNextPrevChild(bool next)
{
    // Keep focus in the terminal while connected so Tab reaches the TUI
    if (m_interactive) {
        return false;
    }
    return QWidget::focusNextPrevChild(next);
}

QByteArray TerminalWidget::encodeKey(QKeyEvent* event) const
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const QString text = event->text();

    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        return QByteArray("\r");
    }
    if (key == Qt::Key_Backspace) {
        return ctrl ? QByteArray("\x08") : QByteArray("\x7f");
    }
    if (key == Qt::Key_Tab) {
        return shift ? QByteArray("\x1b[Z") : QByteArray("\t");
    }
    if (key == Qt::Key_Backtab) {
        return QByteArray("\x1b[Z");
    }
    if (key == Qt::Key_Escape) {
        return QByteArray("\x1b");
    }

    // Arrows — normal CSI or application SS3
    if (key == Qt::Key_Up) {
        return m_appCursorKeys ? QByteArray("\x1bOA") : QByteArray("\x1b[A");
    }
    if (key == Qt::Key_Down) {
        return m_appCursorKeys ? QByteArray("\x1bOB") : QByteArray("\x1b[B");
    }
    if (key == Qt::Key_Right) {
        return m_appCursorKeys ? QByteArray("\x1bOC") : QByteArray("\x1b[C");
    }
    if (key == Qt::Key_Left) {
        return m_appCursorKeys ? QByteArray("\x1bOD") : QByteArray("\x1b[D");
    }
    if (key == Qt::Key_Home) {
        return m_appCursorKeys ? QByteArray("\x1bOH") : QByteArray("\x1b[H");
    }
    if (key == Qt::Key_End) {
        return m_appCursorKeys ? QByteArray("\x1bOF") : QByteArray("\x1b[F");
    }
    if (key == Qt::Key_Insert) {
        return QByteArray("\x1b[2~");
    }
    if (key == Qt::Key_Delete) {
        return QByteArray("\x1b[3~");
    }
    if (key == Qt::Key_PageUp) {
        return QByteArray("\x1b[5~");
    }
    if (key == Qt::Key_PageDown) {
        return QByteArray("\x1b[6~");
    }

    // Function keys (xterm defaults)
    if (key >= Qt::Key_F1 && key <= Qt::Key_F4 && !ctrl && !alt && !shift) {
        // VT100-style SS3 for F1-F4 is common; also accept CSI forms from apps
        static const char* fkeys[] = {"\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS"};
        return QByteArray(fkeys[key - Qt::Key_F1]);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        static const int nums[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
        int n = nums[key - Qt::Key_F1];
        int mod = 1;
        if (shift) {
            mod += 1;
        }
        if (alt) {
            mod += 2;
        }
        if (ctrl) {
            mod += 4;
        }
        if (mod == 1) {
            return QStringLiteral("\x1b[%1~").arg(n).toLatin1();
        }
        return QStringLiteral("\x1b[%1;%2~").arg(n).arg(mod).toLatin1();
    }

    // Ctrl+A..Z → ASCII control bytes (needed by TUIs: Ctrl+C, Ctrl+Z, …)
    if (ctrl && !alt && key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QByteArray(1, static_cast<char>(key - Qt::Key_A + 1));
    }
    if (ctrl && key == Qt::Key_Space) {
        return QByteArray(1, '\0');
    }
    if (ctrl && key == Qt::Key_BracketLeft) {
        return QByteArray("\x1b");
    }
    if (ctrl && key == Qt::Key_Backslash) {
        return QByteArray(1, 0x1c);
    }
    if (ctrl && key == Qt::Key_BracketRight) {
        return QByteArray(1, 0x1d);
    }
    if (ctrl && (key == Qt::Key_Minus || key == Qt::Key_Slash)) {
        // uncommon; fall through
    }

    // Alt+key → ESC prefix (meta)
    if (alt && !ctrl) {
        if (!text.isEmpty()) {
            QByteArray out("\x1b");
            out += text.toUtf8();
            return out;
        }
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            const char ch = static_cast<char>((shift ? 'A' : 'a') + (key - Qt::Key_A));
            QByteArray out("\x1b");
            out += ch;
            return out;
        }
    }

    // Normal printable text (q, Q, ?, /, …) — do not use isPrint(); it drops some keys
    if (!ctrl && !text.isEmpty()) {
        // Skip pure modifier-produced empty control texts
        bool useful = false;
        for (QChar ch : text) {
            if (ch.unicode() >= 32 || ch == QLatin1Char('\t')) {
                useful = true;
                break;
            }
        }
        if (useful) {
            return text.toUtf8();
        }
    }

    // Fallback when text() is empty (some layouts / IME / keypad)
    if (!ctrl && !alt && key >= Qt::Key_A && key <= Qt::Key_Z) {
        const char ch = static_cast<char>((shift ? 'A' : 'a') + (key - Qt::Key_A));
        return QByteArray(1, ch);
    }
    if (!ctrl && !alt && key >= Qt::Key_0 && key <= Qt::Key_9) {
        return QByteArray(1, static_cast<char>('0' + (key - Qt::Key_0)));
    }

    // Punctuation fallbacks commonly used by TUIs
    if (!ctrl && !alt && text.isEmpty()) {
        switch (key) {
        case Qt::Key_Space: return QByteArray(" ");
        case Qt::Key_Slash: return shift ? QByteArray("?") : QByteArray("/");
        case Qt::Key_Question: return QByteArray("?");
        case Qt::Key_Colon: return QByteArray(":");
        case Qt::Key_Semicolon: return shift ? QByteArray(":") : QByteArray(";");
        case Qt::Key_Comma: return QByteArray(",");
        case Qt::Key_Period: return QByteArray(".");
        case Qt::Key_Minus: return QByteArray("-");
        case Qt::Key_Equal: return QByteArray("=");
        default: break;
        }
    }

    return {};
}
