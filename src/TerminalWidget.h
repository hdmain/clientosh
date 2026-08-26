#pragma once

#include <QColor>
#include <QPoint>
#include <QVector>
#include <QWidget>

class QPainter;
class QScrollBar;
class QLabel;
class QTimer;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QInputEvent;
class QWheelEvent;
class QMenu;
class QAction;

class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);

    void setInteractive(bool enabled);
    void setXmodemAvailable(bool enabled);
    void appendOutput(const QByteArray& data);
    void clearTerminal();
    void estimatePtySize(int* cols, int* rows) const;
    void applyAppearanceFromSettings();
    /** Adjust terminal font size by delta (persists to settings). Returns new size. */
    int adjustFontSize(int delta);
    /** Set absolute terminal font size (persists). */
    void setFontSizePt(int points);
    /** Recalculate cols/rows from the current widget size; always emit if force. */
    void syncPtySize(bool forceEmit = false);
    void showFontSizeOverlay(int points);
    void scrollViewToBottom();
    /** Inject keystrokes into the remote PTY (used by AI agent confirmed commands). */
    void injectInput(const QByteArray& data);
    /** Plain-text snapshot of the newest scrollback + live screen (for AI observation). */
    QString captureRecentText(int maxLines = 80) const;

signals:
    void inputReady(const QByteArray& data);
    void ptySizeChanged(int cols, int rows);
    void terminalFontSizeChanged(int points);
    void xmodemSendRequested();

protected:
    bool event(QEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    struct Cell {
        char32_t ch = U' ';
        QRgb fg = qRgb(0xc8, 0xc8, 0xc8);
        QRgb bg = qRgb(0x1a, 0x1a, 0x1a);
        bool bold = false;
        bool underline = false;
        bool inverse = false;
    };

    enum class ParseState {
        Ground,
        Escape,
        Csi,
        Osc,
        Charset,
    };

    enum class Charset {
        Ascii,
        DecSpecial,
    };

    void ensureGrid(int cols, int rows);
    void resizeGrid(int cols, int rows);
    Cell& cellAt(int row, int col);
    const Cell& cellAt(int row, int col) const;
    void putChar(char32_t ch);
    int charDisplayWidth(char32_t ch) const;
    void newLine();
    void reverseIndex();
    void carriageReturn();
    void backspace();
    void tab();
    void scrollUp(int n = 1);
    void scrollDown(int n = 1);
    void insertLines(int n);
    void deleteLines(int n);
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void applySgr(const QVector<int>& params);
    void handleCsi(char finalByte, const QByteArray& params);
    void feedByte(unsigned char byte);
    void feedUtf8Char(char32_t ch);
    void mapAndPutAscii(unsigned char byte);
    char32_t mapDecSpecial(unsigned char byte) const;
    void designateCharset(int slot, char name);
    Charset activeCharset() const;
    void saveCursor();
    void restoreCursor();
    void resetTerminalState();
    void enterAltScreen(bool clear);
    void leaveAltScreen(bool clear);
    void reply(const QByteArray& data);
    QByteArray encodeKey(QKeyEvent* event) const;
    void emitSizeIfChanged();
    void ensureFontZoomOverlay();
    void positionFontZoomOverlay();
    void fadeOutFontZoomOverlay();
    bool mouseReportingActive() const;
    bool shouldReportMouse(const QInputEvent* event) const;
    void applyMouseMode(int mode, bool set);
    void updateQtMouseTracking();
    int encodeMouseButton(Qt::MouseButton button, bool motion) const;
    int encodeMouseModifiers(Qt::KeyboardModifiers mods) const;
    void sendMouseReport(int buttonCode, int col0, int row0, bool release);
    bool sendWheelToApp(QWheelEvent* event);
    QColor ansiColor(int index, bool bright) const;
    QColor xterm256(int index) const;
    void resetAttributes();
    Cell makeBlankCell() const;
    void remapDefaultColors(const QColor& oldFg, const QColor& oldBg, const QColor& newFg,
                            const QColor& newBg);
    int cellWidth() const;
    int cellHeight() const;
    int scrollTop() const { return m_scrollTop; }
    int scrollBottom() const { return m_scrollBottom; }
    void clampCursor();

    QPoint cellFromPos(const QPoint& pos) const;
    // Selection uses absolute cell indices (0 = oldest scrollback line) so that
    // the selection survives viewport scrolling and can span scrollback history.
    int absIndexFromView(int viewRow, int col) const;
    int maxAbsIndex() const;
    const Cell& cellAtAbsLine(int absLine, int col) const;
    bool hasSelection() const;
    bool isCellSelected(int row, int col) const;
    void clearSelection();
    void setSelectionRange(int a, int b);
    void shiftSelectionBy(int cellDelta);
    QString selectedText() const;
    void copySelectionToClipboard(bool clearSel = true);
    void pasteClipboard();
    void showContextMenu(const QPoint& globalPos);
    void selectWordAt(int row, int col);
    void selectLineAt(int row);
    bool isWordChar(char32_t ch) const;
    bool drawBoxDrawing(QPainter& painter, char32_t ch, const QRect& cell, const QColor& fg) const;
    QString lineTextAt(int viewRow) const;
    QVector<QColor> highlightColorsForRow(int viewRow) const;

    void pushScrollbackLines(int fromRow, int count);
    void trimScrollback();
    void clearScrollback();
    void setViewOffset(int offset);
    void scrollViewBy(int deltaLines);
    void syncScrollBar();
    void layoutScrollBar();
    const Cell& displayCell(int viewRow, int col) const;
    int maxViewOffset() const;
    bool isLiveView() const { return m_viewOffset == 0; }

    bool m_interactive = false;
    int m_cols = 80;
    int m_rows = 24;
    int m_cx = 0;
    int m_cy = 0;
    bool m_cursorVisible = true;
    bool m_originMode = false;
    bool m_autoWrap = true;
    bool m_pendingWrap = false;
    bool m_altScreen = false;
    bool m_appCursorKeys = false; // DECCKM
    bool m_appKeypad = false;     // DECKPAM
    // xterm mouse: 0=off, 1000=click, 1002=drag, 1003=any-motion
    int m_mouseMode = 0;
    bool m_mouseSgr = false;    // DECSET 1006
    bool m_mouseUrxvt = false;  // DECSET 1015
    bool m_mouseUtf8 = false;   // DECSET 1005
    bool m_altScroll = true;    // DECSET 1007 — default on (xterm alternateScroll)
    bool m_focusReport = false; // DECSET 1004
    int m_mousePressedBtn = -1; // last pressed button code (0/1/2), -1 if none
    QPoint m_lastMouseCell{-1, -1};
    char32_t m_lastChar = U' ';

    int m_scrollTop = 0;
    int m_scrollBottom = 23;

    Charset m_g0 = Charset::Ascii;
    Charset m_g1 = Charset::Ascii;
    int m_gl = 0; // 0 = G0, 1 = G1
    unsigned char m_charsetSlot = '(';

    int m_savedCx = 0;
    int m_savedCy = 0;
    Cell m_savedAttr;
    int m_savedGl = 0;

    // Saved cursor for DEC private modes (separate from ESC 7)
    int m_decSavedCx = 0;
    int m_decSavedCy = 0;
    Cell m_decSavedAttr;

    Cell m_attr;
    QColor m_defaultFg{0xc8, 0xc8, 0xc8};
    QColor m_defaultBg{0x1a, 0x1a, 0x1a};
    QVector<Cell> m_cells;
    QVector<Cell> m_altCells;
    QVector<Cell> m_mainCells;
    int m_mainCx = 0;
    int m_mainCy = 0;
    int m_mainScrollTop = 0;
    int m_mainScrollBottom = 23;

    ParseState m_state = ParseState::Ground;
    QByteArray m_seq;
    QByteArray m_utf8Pending;

    int m_selAnchor = -1;
    int m_selStart = -1;
    int m_selEnd = -1;
    bool m_selecting = false;
    int m_clickCount = 0;
    qint64 m_lastClickMs = 0;
    QPoint m_lastClickCell;

    QPixmap m_bgImagePixmap; // Cached blurred background image.
    int m_bgBlurRadius = 0;
    qreal m_bgOpacity = 0.5;

    static constexpr int kScrollbackMax = 10000;
    QVector<QVector<Cell>> m_scrollback;
    int m_viewOffset = 0; // lines above the live screen currently shown
    QScrollBar* m_vScroll = nullptr;
    bool m_updatingScrollBar = false;

    QLabel* m_fontZoomLabel = nullptr;
    QGraphicsOpacityEffect* m_fontZoomOpacity = nullptr;
    QPropertyAnimation* m_fontZoomFade = nullptr;
    QTimer* m_fontZoomHideTimer = nullptr;
    QMenu* m_contextMenu = nullptr;
    QAction* m_xmodemSeparator = nullptr;
    QAction* m_xmodemAction = nullptr;
    bool m_xmodemAvailable = false;
};
