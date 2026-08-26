#pragma once

#include "core/SessionProfile.h"
#include "core/sync/SyncController.h"
#include "core/addons/AddonHost.h"
#include "core/addons/AddonStore.h"

#include <QColor>
#include <QHash>
#include <QStringList>
#include <QUrl>
#include <QWidget>
#include <QVector>

class SessionManager;
class QStackedWidget;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QToolButton;
class QTableWidget;
class QTreeWidget;
class QMenu;
class QListWidget;
class QPlainTextEdit;
class QButtonGroup;
class QComboBox;
class QSlider;
class QAbstractButton;
class QKeySequenceEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QVBoxLayout;

class DashboardPage : public QWidget
{
    Q_OBJECT

public:
    explicit DashboardPage(SessionManager* sessions, QWidget* parent = nullptr);

    void refresh();
    void showNewSessionForm();
    void showSettings();
    void showHome();
    void appendLog(const QString& line);
    void syncTerminalFontSizeUi(int points);
    /** Persist a currently open terminal/SFTP profile if it is not saved yet. */
    void saveSessionProfile(const SessionProfile& profile);

signals:
    void openProfile(const SessionProfile& profile);
    void openLiveSession(const QString& id);
    void openSftpForProfile(const SessionProfile& profile);
    void openSftpForSession(const QString& sessionId);
    void closeLiveSession(const QString& sessionId);
    void settingsApplied();

private:
    enum class NavPage {
        Hosts = 0,
        Active, // deprecated/unused: live sessions are surfaced via the "N live" badge
        Keychain,
        Logs,
        Settings,
        Form
    };

    enum class SettingsCategory {
        General = 0,
        Appearance,
        Performance,
        Ssh,
        Sftp,
        Shortcuts,
        Sync,
        Addons,
        About
    };

    void rebuildSavedList();
    void rebuildActiveList();
    void rebuildKeychainList();
    void applySavedFilter();
    // ---- Host tags ------------------------------------------------------
    void showHostContextMenu(const QPoint& globalPos, const QString& profileId);
    void showPageContextMenu(const QPoint& globalPos);
    void showTagContextMenu(const QPoint& globalPos, const QString& tagName);
    void addTagDialog();
    void renameTagDialog(const QString& tagName);
    void deleteTag(const QString& tagName);
    void moveProfileToTag(const QString& profileId, const QString& tagName);
    bool moveProfileToTagAt(const QString& profileId, const QString& tagName,
                            const QString& beforeProfileId);
    void removeProfileFromTag(const QString& profileId);
    QStringList currentTags() const;
    void persistTagCollapseState();
    void clearForm();
    void loadProfileIntoForm(const SessionProfile& profile);
    void showEditSessionForm(const QString& profileId);
    void openSavedProfile(const QString& profileId);
    void sftpSavedProfile(const QString& profileId);
    void deleteSavedProfile(const QString& profileId);
    void saveLiveSession(const QString& sessionId);
    void showLiveSessionContextMenu(const QPoint& globalPos, const QString& sessionId);
    bool isProfileSaved(const SessionProfile& profile) const;
    void setProfileSystem(const QString& profileId, const QString& system);
    void saveCurrentFormAsProfile();
    void connectFromForm();
    void loadSettingsUi();
    void saveSettingsUi();
    void setSettingsCategory(int index);
    void populateFontCombos();
    void ensureSelectedFonts();
    void refreshFontPreviews();
    void persistAppearanceLive();
    void persistPrefsLive();
    void persistHighlightSettings();
    void notifyHighlightSettingsChanged();
    void syncColorSwatch(QAbstractButton* btn, const QColor& color);
    void pickTerminalColor(bool foreground);
    void persistShortcutsLive();
    void resetShortcutsToDefaults();
    void setNavPage(NavPage page);
    void browsePrivateKey();
    void reloadKeyringCombo();
    void importKeyIntoKeyring();
    void renameSelectedStoredKey();
    void editSelectedStoredKeyPassphrase();
    void removeSelectedKeyringKey();
    void updateStoredKeyActions();
    void onKeyringSelectionChanged(int index);
    void updateAuthMethodUi();
    void updateConnectionModeUi();
    void fillProfileFromForm(SessionProfile* profile) const;
    int profileIndexById(const QString& id) const;
    QToolButton* makeNavButton(const QString& iconPath, const QString& text, QWidget* parent);
    QToolButton* makeRowAction(const QString& iconPath, const QString& tip, QWidget* parent);
    void updateTopBar();
    QWidget* buildSettingsSection(const QString& title, QWidget* parent);
    void addSettingsField(QWidget* section, const QString& label, QWidget* field);

    SessionManager* m_sessions = nullptr;

    QWidget* m_sidebar = nullptr;
    QButtonGroup* m_navGroup = nullptr;
    QToolButton* m_navHosts = nullptr;
    QToolButton* m_navKeys = nullptr;
    QToolButton* m_navLogs = nullptr;
    QToolButton* m_navSettings = nullptr;
    QLabel* m_activeBadge = nullptr;

    QLabel* m_pageTitle = nullptr;
    QLabel* m_pageSub = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QPushButton* m_newSessionBtn = nullptr;
    QWidget* m_topBar = nullptr;

    QStackedWidget* m_stack = nullptr;
    QWidget* m_hostsPage = nullptr;
    QWidget* m_keysPage = nullptr;
    QWidget* m_logsPage = nullptr;
    QWidget* m_settingsPage = nullptr;
    QWidget* m_formPage = nullptr;

    QTreeWidget* m_savedTree = nullptr;
    QLabel* m_savedEmpty = nullptr;
    QTableWidget* m_keysTable = nullptr;
    QLabel* m_keysEmpty = nullptr;
    QLabel* m_keysStatus = nullptr;
    QLabel* m_agentStatus = nullptr;
    QPlainTextEdit* m_logsView = nullptr;

    QLabel* m_formTitle = nullptr;
    QLabel* m_formSub = nullptr;
    QLineEdit* m_nameEdit = nullptr;
    QComboBox* m_connectionModeCombo = nullptr;
    QLineEdit* m_hostEdit = nullptr;
    QSpinBox* m_portSpin = nullptr;
    QLineEdit* m_userEdit = nullptr;
    QWidget* m_networkFieldsPanel = nullptr;
    QWidget* m_serialFieldsPanel = nullptr;
    QComboBox* m_serialPortCombo = nullptr;
    QComboBox* m_serialBaudCombo = nullptr;
    QComboBox* m_serialDataBitsCombo = nullptr;
    QComboBox* m_serialParityCombo = nullptr;
    QComboBox* m_serialStopBitsCombo = nullptr;
    QComboBox* m_serialFlowCombo = nullptr;
    QComboBox* m_authMethodCombo = nullptr;
    QWidget* m_authSectionTitle = nullptr;
    QWidget* m_authSectionRule = nullptr;
    QWidget* m_authMethodLabel = nullptr;
    QWidget* m_authPasswordPanel = nullptr;
    QWidget* m_authKeyringPanel = nullptr;
    QWidget* m_authKeyFilePanel = nullptr;
    QWidget* m_authPassphrasePanel = nullptr;
    QLineEdit* m_passEdit = nullptr;
    QCheckBox* m_savePass = nullptr;
    QLineEdit* m_keyPathEdit = nullptr;
    QComboBox* m_keyringCombo = nullptr;
    QPushButton* m_importKeyBtn = nullptr;
    QPushButton* m_removeKeyBtn = nullptr;
    QPushButton* m_renameKeyBtn = nullptr;
    QPushButton* m_passphraseKeyBtn = nullptr;
    QPushButton* m_manageKeysBtn = nullptr;
    QLineEdit* m_keyPassEdit = nullptr;
    QCheckBox* m_saveKeyPass = nullptr;
    QPushButton* m_browseKeyBtn = nullptr;
    QPushButton* m_saveProfileBtn = nullptr;
    QPushButton* m_connectProfileBtn = nullptr;

    QListWidget* m_settingsNav = nullptr;
    QStackedWidget* m_settingsStack = nullptr;

    QCheckBox* m_settingsSavePassDefault = nullptr;
    QLineEdit* m_settingsDefaultHost = nullptr;
    QLineEdit* m_settingsDefaultUser = nullptr;

    QComboBox* m_settingsTheme = nullptr;
    QComboBox* m_settingsUiFontFamily = nullptr;
    QSpinBox* m_settingsUiFontSize = nullptr;
    QComboBox* m_settingsFontFamily = nullptr;
    QSpinBox* m_settingsFontSize = nullptr;
    QLabel* m_settingsFontStatus = nullptr;
    QToolButton* m_settingsTermFgBtn = nullptr;
    QToolButton* m_settingsTermBgBtn = nullptr;
    QToolButton* m_settingsTermBgImageBtn = nullptr;
    QLabel* m_settingsTermBgImage = nullptr;
    QSpinBox* m_settingsTermBgOpacity = nullptr;
    QSpinBox* m_settingsTermBgBlur = nullptr;
    QLabel* m_settingsTermPreview = nullptr;
    QColor m_termFg;
    QColor m_termBg;

    QCheckBox* m_settingsAnimations = nullptr;
    QCheckBox* m_settingsShowStats = nullptr;
    QSpinBox* m_settingsStatsInterval = nullptr;

    QSpinBox* m_settingsDefaultPort = nullptr;

    QCheckBox* m_settingsHideDotfiles = nullptr;
    QComboBox* m_settingsSftpView = nullptr;
    QCheckBox* m_settingsSftpVerbose = nullptr;
    QCheckBox* m_settingsHighlightAddresses = nullptr;
    QCheckBox* m_settingsHighlightKeywords = nullptr;
    QCheckBox* m_settingsHighlightCiscoCli = nullptr;
    bool m_applyingAppearance = false;

    QCheckBox* m_settingsCtrlScrollZoom = nullptr;
    QSlider* m_settingsScrollSensitivity = nullptr;
    QLabel* m_settingsScrollSensitivityValue = nullptr;
    QComboBox* m_settingsCopyPaste = nullptr;
    QKeySequenceEdit* m_shortcutNewSession = nullptr;
    QKeySequenceEdit* m_shortcutSettings = nullptr;
    QKeySequenceEdit* m_shortcutDashboard = nullptr;
    QKeySequenceEdit* m_shortcutClosePanel = nullptr;
    QKeySequenceEdit* m_shortcutOpenSftp = nullptr;
    QKeySequenceEdit* m_shortcutClearTerminal = nullptr;
    QKeySequenceEdit* m_shortcutFontLarger = nullptr;
    QKeySequenceEdit* m_shortcutFontSmaller = nullptr;
    QKeySequenceEdit* m_shortcutFontReset = nullptr;
    QCheckBox* m_enableNewSession = nullptr;
    QCheckBox* m_enableSettings = nullptr;
    QCheckBox* m_enableDashboard = nullptr;
    QCheckBox* m_enableClosePanel = nullptr;
    QCheckBox* m_enableOpenSftp = nullptr;
    QCheckBox* m_enableClearTerminal = nullptr;
    QCheckBox* m_enableFontLarger = nullptr;
    QCheckBox* m_enableFontSmaller = nullptr;
    QCheckBox* m_enableFontReset = nullptr;

    // ---- Sync (GitHub Gist) -------------------------------------------------
    SyncController* m_sync = nullptr;
    QLabel* m_syncEnabledHint = nullptr;
    QCheckBox* m_syncEnabledCheck = nullptr;
    QLineEdit* m_syncTokenEdit = nullptr;
    QPushButton* m_syncTokenClearBtn = nullptr;
    QLabel* m_syncTokenStatusLabel = nullptr;
    QPushButton* m_syncTestBtn = nullptr;
    QPushButton* m_syncCreateBtn = nullptr;
    QPushButton* m_syncJoinBtn = nullptr;
    QPushButton* m_syncDisableBtn = nullptr;
    QPushButton* m_syncSyncNowBtn = nullptr;
    QLineEdit* m_syncKeyEdit = nullptr;      // paste sync key (Computer 2)
    QLineEdit* m_syncKeyDisplay = nullptr;   // read-only current sync key
    QPushButton* m_syncCopyKeyBtn = nullptr;
    QPushButton* m_syncCopyAgainBtn = nullptr;
    QSpinBox* m_syncPollInterval = nullptr;
    QLabel* m_syncStatus = nullptr;
    QLabel* m_syncGistIdLabel = nullptr;
    // Live-only token gate for setup
    QLineEdit* m_syncCreateTokenEdit = nullptr;
    QPushButton* m_syncJoinTokenEye = nullptr;
    // Debounce support
    QTimer* m_syncSaveDebounce = nullptr;

    // ---- Addons (marketplace; plugin load comes later) ----------------------
    AddonStore* m_addonStore = nullptr;
    AddonHost* m_addonHost = nullptr;
    QLineEdit* m_addonsRepoEdit = nullptr;
    QPushButton* m_addonsRefreshBtn = nullptr;
    QLabel* m_addonsAbiLabel = nullptr;
    QLabel* m_addonsStatus = nullptr;
    QWidget* m_addonsListHost = nullptr;
    QVBoxLayout* m_addonsListLay = nullptr;

    void syncCreateSetup();
    void syncJoinFromInput();
    void syncDisable();
    void syncTestToken();
    void syncPushNow();
    void syncPullNow();
    void syncRefreshUiFromSyncState();
    void syncOnStateChanged(SyncController::State state);
    void syncOnStatus(const QString& message);
    void syncOnError(const QString& message);
    void syncOnDataUpdated();

    void rebuildAddonsList();
    void persistAddonsRepoUrl();

    void persistSyncLive();
    void applyStoredSyncState();

    void checkForUpdates();
    void handleUpdateCheckReply(QNetworkReply* reply);

    QLabel* m_hint = nullptr;

    QLabel* m_aboutCurrentVersion = nullptr;
    QLabel* m_aboutUpdateStatus = nullptr;
    QPushButton* m_aboutDownloadBtn = nullptr;
    QNetworkAccessManager* m_updateNam = nullptr;
    QUrl m_aboutReleaseUrl;
    bool m_updateCheckInFlight = false;

    QVector<SessionProfile> m_profiles;
    QStringList m_tags;
    QHash<QString, QStringList> m_tagAssignments; // tagName → profile IDs
    QStringList m_tagCollapsed;                   // stable keys of collapsed host folders
    QString m_editingId;
    NavPage m_currentNav = NavPage::Hosts;
};
