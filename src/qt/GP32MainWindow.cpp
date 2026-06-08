#include "GP32MainWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

#ifdef GP32EMU_QT_SDL3_INPUT
#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>
#endif

struct QtInputDef {
    const char *name;
    uint32_t mask;
    int defaultKey;
    const char *defaultPad;
};

static const QtInputDef kInputDefs[] = {
    {"Up",     GP32_BUTTON_UP,     Qt::Key_Up,     "axis:1:-"},
    {"Down",   GP32_BUTTON_DOWN,   Qt::Key_Down,   "axis:1:+"},
    {"Left",   GP32_BUTTON_LEFT,   Qt::Key_Left,   "axis:0:-"},
    {"Right",  GP32_BUTTON_RIGHT,  Qt::Key_Right,  "axis:0:+"},
    {"A",      GP32_BUTTON_A,      Qt::Key_Z,      "button:0"},
    {"B",      GP32_BUTTON_B,      Qt::Key_X,      "button:1"},
    {"L",      GP32_BUTTON_L,      Qt::Key_A,      "button:4"},
    {"R",      GP32_BUTTON_R,      Qt::Key_S,      "button:5"},
    {"Start",  GP32_BUTTON_START,  Qt::Key_Return, "button:7"},
    {"Select", GP32_BUTTON_SELECT, Qt::Key_Shift,  "button:6"},
};

static constexpr int kInputCount = static_cast<int>(sizeof(kInputDefs) / sizeof(kInputDefs[0]));

static QString gp32ConfigDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    QString dir = base + QStringLiteral("/gp32emu");
    QDir().mkpath(dir);
    return dir;
}

static QString gp32StateDir() {
    QString dir = gp32ConfigDir() + QStringLiteral("/sstates");
    QDir().mkpath(dir);
    return dir;
}

static QString gp32SettingsPath() { return gp32ConfigDir() + QStringLiteral("/gp32emu.conf"); }
static QString gp32DefaultStatePath() { return gp32StateDir() + QStringLiteral("/gp32_state.gp32st"); }
static QString gp32DefaultScreenshotPath() { return gp32ConfigDir() + QStringLiteral("/gp32_screenshot.bmp"); }
static QString gp32DefaultRecordPath() { return gp32ConfigDir() + QStringLiteral("/gp32_recording.mkv"); }

static QString keyName(int key) {
    switch (key) {
    case 0: return QStringLiteral("Unmapped");
    case Qt::Key_Shift: return QStringLiteral("Shift");
    case Qt::Key_Control: return QStringLiteral("Ctrl");
    case Qt::Key_Alt: return QStringLiteral("Alt");
    case Qt::Key_Meta: return QStringLiteral("Meta");
    case Qt::Key_Left: return QStringLiteral("Left");
    case Qt::Key_Right: return QStringLiteral("Right");
    case Qt::Key_Up: return QStringLiteral("Up");
    case Qt::Key_Down: return QStringLiteral("Down");
    case Qt::Key_Return: return QStringLiteral("Enter");
    case Qt::Key_Space: return QStringLiteral("Space");
    default: {
        QString s = QKeySequence(key).toString(QKeySequence::NativeText);
        return s.isEmpty() ? QStringLiteral("Key %1").arg(key) : s;
    }
    }
}

static QString padName(const QString &binding) {
    if (binding.isEmpty() || binding == QStringLiteral("none")) return QStringLiteral("Unmapped");
    const QStringList parts = binding.split(':');
    if (parts.size() >= 2 && parts[0] == QStringLiteral("button")) return QStringLiteral("Button %1").arg(parts[1]);
    if (parts.size() >= 3 && parts[0] == QStringLiteral("axis")) return QStringLiteral("Axis %1 %2").arg(parts[1], parts[2] == QStringLiteral("-") ? QStringLiteral("-") : QStringLiteral("+"));
    if (parts.size() >= 3 && parts[0] == QStringLiteral("hat")) return QStringLiteral("Hat %1 %2").arg(parts[1], parts[2]);
    return binding;
}

class KeyCaptureButton : public QPushButton {
public:
    KeyCaptureButton(int *key, QWidget *parent = nullptr) : QPushButton(parent), m_key(key), m_capturing(false) {
        refresh();
        connect(this, &QPushButton::clicked, this, [this]() {
            m_capturing = true;
            setText(QStringLiteral("Press a key..."));
            setFocus(Qt::OtherFocusReason);
        });
    }
    void refresh() { setText(keyName(m_key ? *m_key : 0)); }
protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (!m_capturing) { QPushButton::keyPressEvent(event); return; }
        int key = event->key();
        if (key == Qt::Key_Escape) key = 0;
        if (m_key) *m_key = key;
        m_capturing = false;
        refresh();
        event->accept();
    }
private:
    int *m_key;
    bool m_capturing;
};

static void populatePadCombo(QComboBox *combo, const QString &current) {
    combo->addItem(QStringLiteral("Unmapped"), QStringLiteral("none"));
    for (int i = 0; i < 32; ++i) combo->addItem(QStringLiteral("Button %1").arg(i), QStringLiteral("button:%1").arg(i));
    for (int axis = 0; axis < 8; ++axis) {
        combo->addItem(QStringLiteral("Axis %1 -").arg(axis), QStringLiteral("axis:%1:-").arg(axis));
        combo->addItem(QStringLiteral("Axis %1 +").arg(axis), QStringLiteral("axis:%1:+").arg(axis));
    }
    for (int hat = 0; hat < 4; ++hat) {
        combo->addItem(QStringLiteral("Hat %1 up").arg(hat), QStringLiteral("hat:%1:up").arg(hat));
        combo->addItem(QStringLiteral("Hat %1 down").arg(hat), QStringLiteral("hat:%1:down").arg(hat));
        combo->addItem(QStringLiteral("Hat %1 left").arg(hat), QStringLiteral("hat:%1:left").arg(hat));
        combo->addItem(QStringLiteral("Hat %1 right").arg(hat), QStringLiteral("hat:%1:right").arg(hat));
    }
    int idx = combo->findData(current);
    if (idx < 0 && !current.isEmpty()) {
        combo->addItem(padName(current), current);
        idx = combo->count() - 1;
    }
    combo->setCurrentIndex(std::max(0, idx));
}

GP32MainWindow::GP32MainWindow(QWidget *parent)
    : QMainWindow(parent), m_video(new GP32VideoWidget(this)), m_status(new QLabel(this)), m_inputTimer(new QTimer(this)),
      m_runAction(nullptr), m_jitAction(nullptr), m_scalingAction(nullptr), m_keepAspectAction(nullptr), m_fullscreenAction(nullptr),
      m_lcdPersistenceAction(nullptr), m_frameInterpolationAction(nullptr), m_recordAction(nullptr), m_stopRecordAction(nullptr),
      m_useHleAction(nullptr), m_configureControlsAction(nullptr), m_keyboardButtons(0), m_gamepadButtons(0), m_buttons(0),
      m_lastStatePath(gp32DefaultStatePath()), m_lastScreenshotPath(gp32DefaultScreenshotPath()), m_lastRecordPath(gp32DefaultRecordPath())
#ifdef GP32EMU_QT_SDL3_INPUT
      , m_joystick(nullptr), m_sdlInputReady(0), m_joyButtons(0), m_joyAxes(0), m_joyHats(0)
#endif
{
    setWindowTitle("GP32emu");
    setWindowIcon(QIcon(":/gp32emu.png"));
    resetInputBindings();
    createMenus();
    loadSettings();
    setCentralWidget(m_video);
    statusBar()->addPermanentWidget(m_status, 1);
    m_status->setText(m_engine.biosPath().isEmpty()
        ? QStringLiteral("Ready. Config is stored in %1. F5/F8 save/load state.").arg(gp32ConfigDir())
        : QStringLiteral("Ready. BIOS path configured: %1").arg(QFileInfo(m_engine.biosPath()).fileName()));
    connect(&m_engine, &GP32Engine::frameReady, m_video, &GP32VideoWidget::setFrame);
    connect(&m_engine, &GP32Engine::statusChanged, this, &GP32MainWindow::updateStatus);
    connect(&m_engine, &GP32Engine::recordingChanged, this, [this](bool recording) {
        if (m_recordAction) m_recordAction->setEnabled(!recording);
        if (m_stopRecordAction) m_stopRecordAction->setEnabled(recording);
    });
    initControllerInput();
    m_inputTimer->setTimerType(Qt::CoarseTimer);
    m_inputTimer->setInterval(16);
    connect(m_inputTimer, &QTimer::timeout, this, &GP32MainWindow::pollControllerInput);
#ifdef GP32EMU_QT_SDL3_INPUT
    if (m_sdlInputReady) m_inputTimer->start();
#endif
    resize(960, 720);
    syncBootModeUi();
    if (m_engine.biosPath().isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            QMessageBox::warning(this, "GP32emu HLE BIOS warning", "No BIOS path is configured. GP32emu will use the HLE BIOS fallback. HLE BIOS will fail to boot some games and only works with some commercial games and homebrew games.");
        });
    }
}

GP32MainWindow::~GP32MainWindow() { shutdownControllerInput(); }

void GP32MainWindow::createMenus() {
    QMenu *file = menuBar()->addMenu("File");
    file->addAction("Open BIOS...", this, &GP32MainWindow::openBios);
    file->addAction("Open SmartMedia image...", this, &GP32MainWindow::openSmartMedia);
    file->addAction("Open FXE...", this, &GP32MainWindow::openFxe);
    file->addAction("Open FPK...", this, &GP32MainWindow::openFpk);
    file->addSeparator();
    file->addAction("Save State...", this, &GP32MainWindow::saveState, QKeySequence(Qt::Key_F5));
    file->addAction("Load State...", this, &GP32MainWindow::loadState, QKeySequence(Qt::Key_F8));
    file->addSeparator();
    file->addAction("Take Screenshot...", this, &GP32MainWindow::takeScreenshot, QKeySequence(Qt::Key_F12));
    m_recordAction = file->addAction("Start Recording ZMBV MKV...");
    connect(m_recordAction, &QAction::triggered, this, &GP32MainWindow::startRecording);
    m_stopRecordAction = file->addAction("Stop Recording");
    m_stopRecordAction->setEnabled(false);
    connect(m_stopRecordAction, &QAction::triggered, this, &GP32MainWindow::stopRecording);
    file->addSeparator();
    file->addAction("Quit", this, &QWidget::close);

    QMenu *emu = menuBar()->addMenu("Emulation");
    m_runAction = emu->addAction("Run");
    m_runAction->setCheckable(true);
    connect(m_runAction, &QAction::triggered, this, &GP32MainWindow::toggleRun);
    emu->addAction("Reset", this, &GP32MainWindow::reset);
    m_jitAction = emu->addAction("Enable x64 dynarec JIT");
    m_jitAction->setCheckable(true);
    m_jitAction->setChecked(true);
    connect(m_jitAction, &QAction::toggled, this, &GP32MainWindow::toggleJit);

    QMenu *config = menuBar()->addMenu("Config");
    config->addAction("Set BIOS path...", this, &GP32MainWindow::configureBiosPath);
    config->addAction("Clear BIOS path", this, &GP32MainWindow::clearBiosPath);
    config->addSeparator();
    m_configureControlsAction = config->addAction("Configure controls...");
    connect(m_configureControlsAction, &QAction::triggered, this, &GP32MainWindow::configureControls);
    config->addSeparator();
    config->addAction("Boot BIOS now", this, &GP32MainWindow::bootBios);
    m_useHleAction = config->addAction("Use HLE BIOS fallback when no BIOS is configured");
    m_useHleAction->setCheckable(true);
    connect(m_useHleAction, &QAction::toggled, this, [this](bool checked) {
        if (!m_engine.biosPath().isEmpty()) {
            syncBootModeUi();
            return;
        }
        m_engine.setUseHle(checked);
        saveSettings();
    });

    QMenu *view = menuBar()->addMenu("View");
    m_fullscreenAction = view->addAction("Fullscreen");
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    connect(m_fullscreenAction, &QAction::triggered, this, &GP32MainWindow::toggleFullscreen);
    view->addSeparator();
    m_keepAspectAction = view->addAction("Keep aspect ratio, fill height");
    m_keepAspectAction->setCheckable(true);
    m_keepAspectAction->setChecked(true);
    connect(m_keepAspectAction, &QAction::toggled, this, &GP32MainWindow::toggleKeepAspect);
    m_scalingAction = view->addAction("Integer scaling");
    m_scalingAction->setCheckable(true);
    m_scalingAction->setChecked(false);
    connect(m_scalingAction, &QAction::toggled, this, &GP32MainWindow::toggleScaling);
    view->addSeparator();
    m_lcdPersistenceAction = view->addAction("LCD persistence / FLU ghosting");
    m_lcdPersistenceAction->setCheckable(true);
    m_lcdPersistenceAction->setChecked(false);
    connect(m_lcdPersistenceAction, &QAction::toggled, this, &GP32MainWindow::toggleLcdPersistence);
    m_frameInterpolationAction = view->addAction("Frame interpolation blend");
    m_frameInterpolationAction->setCheckable(true);
    m_frameInterpolationAction->setChecked(false);
    connect(m_frameInterpolationAction, &QAction::toggled, this, &GP32MainWindow::toggleFrameInterpolation);
}

void GP32MainWindow::resetInputBindings() {
    m_keyBindings.resize(kInputCount);
    m_gamepadBindings.clear();
    for (int i = 0; i < kInputCount; ++i) {
        m_keyBindings[i] = kInputDefs[i].defaultKey;
        m_gamepadBindings.append(QString::fromLatin1(kInputDefs[i].defaultPad));
    }
}

void GP32MainWindow::loadSettings() {
    QSettings s(gp32SettingsPath(), QSettings::IniFormat);
    QString bios = s.value(QStringLiteral("paths/bios")).toString();
    if (!bios.isEmpty()) m_engine.setBiosPath(bios);
    m_lastStatePath = s.value(QStringLiteral("paths/lastState"), gp32DefaultStatePath()).toString();
    m_lastScreenshotPath = s.value(QStringLiteral("paths/lastScreenshot"), gp32DefaultScreenshotPath()).toString();
    m_lastRecordPath = s.value(QStringLiteral("paths/lastRecord"), gp32DefaultRecordPath()).toString();
    bool jit = s.value(QStringLiteral("emulation/jit"), true).toBool();
    m_jitAction->setChecked(jit);
    m_engine.setJitEnabled(jit);
    bool keep = s.value(QStringLiteral("video/keepAspect"), true).toBool();
    bool integer = s.value(QStringLiteral("video/integerScaling"), false).toBool();
    if (keep) integer = false;
    m_keepAspectAction->setChecked(keep);
    m_video->setKeepAspect(keep);
    m_scalingAction->setChecked(integer);
    m_video->setIntegerScaling(integer);
    bool lcdPersistence = s.value(QStringLiteral("video/lcdPersistence"), false).toBool();
    bool frameInterpolation = s.value(QStringLiteral("video/frameInterpolation"), false).toBool();
    if (m_lcdPersistenceAction) m_lcdPersistenceAction->setChecked(lcdPersistence);
    if (m_frameInterpolationAction) m_frameInterpolationAction->setChecked(frameInterpolation);
    m_video->setLcdPersistence(lcdPersistence);
    m_video->setFrameInterpolation(frameInterpolation);
    bool hle = m_engine.biosPath().isEmpty() ? s.value(QStringLiteral("emulation/useHle"), true).toBool() : false;
    m_engine.setUseHle(hle);
    for (int i = 0; i < kInputCount; ++i) {
        m_keyBindings[i] = s.value(QStringLiteral("input/key/%1").arg(kInputDefs[i].name), kInputDefs[i].defaultKey).toInt();
        m_gamepadBindings[i] = s.value(QStringLiteral("input/gamepad/%1").arg(kInputDefs[i].name), QString::fromLatin1(kInputDefs[i].defaultPad)).toString();
    }
    syncBootModeUi();
}

void GP32MainWindow::saveSettings() {
    QSettings s(gp32SettingsPath(), QSettings::IniFormat);
    if (m_engine.biosPath().isEmpty()) s.remove(QStringLiteral("paths/bios"));
    else s.setValue(QStringLiteral("paths/bios"), m_engine.biosPath());
    s.setValue(QStringLiteral("paths/lastState"), m_lastStatePath);
    s.setValue(QStringLiteral("paths/lastScreenshot"), m_lastScreenshotPath);
    s.setValue(QStringLiteral("paths/lastRecord"), m_lastRecordPath);
    s.setValue(QStringLiteral("emulation/jit"), m_engine.jitEnabled());
    s.setValue(QStringLiteral("video/keepAspect"), m_keepAspectAction->isChecked());
    s.setValue(QStringLiteral("video/integerScaling"), m_scalingAction->isChecked());
    s.setValue(QStringLiteral("video/lcdPersistence"), m_lcdPersistenceAction && m_lcdPersistenceAction->isChecked());
    s.setValue(QStringLiteral("video/frameInterpolation"), m_frameInterpolationAction && m_frameInterpolationAction->isChecked());
    s.setValue(QStringLiteral("emulation/useHle"), m_engine.biosPath().isEmpty() && m_useHleAction && m_useHleAction->isChecked());
    for (int i = 0; i < kInputCount; ++i) {
        s.setValue(QStringLiteral("input/key/%1").arg(kInputDefs[i].name), m_keyBindings[i]);
        s.setValue(QStringLiteral("input/gamepad/%1").arg(kInputDefs[i].name), m_gamepadBindings[i]);
    }
    s.sync();
}

void GP32MainWindow::openBios() {
    QString path = QFileDialog::getOpenFileName(this, "Open GP32 BIOS", QString(), "GP32 BIOS (*.bin *.rom *.bios *.zip);;All files (*)");
    if (!path.isEmpty()) {
        if (m_engine.loadBios(path)) {
            syncBootModeUi();
            saveSettings();
            m_engine.start();
            m_runAction->setChecked(m_engine.isRunning());
        } else {
            QMessageBox::warning(this, "GP32emu", "Failed to boot selected BIOS. See the status bar for the emulator error.");
        }
    }
}

void GP32MainWindow::openSmartMedia() {
    QString path = QFileDialog::getOpenFileName(this, "Open GP32 SmartMedia image", QString(), "SmartMedia (*.smc *.zip);;All files (*)");
    if (!path.isEmpty()) { m_engine.loadSmartMedia(path); m_engine.start(); m_runAction->setChecked(m_engine.isRunning()); }
}

void GP32MainWindow::openFxe() {
    QString path = QFileDialog::getOpenFileName(this, "Open GP32 FXE", QString(), "GP32 executable (*.fxe *.zip);;All files (*)");
    if (!path.isEmpty()) { m_engine.loadFxe(path); m_engine.start(); m_runAction->setChecked(m_engine.isRunning()); }
}

void GP32MainWindow::openFpk() {
    QString path = QFileDialog::getOpenFileName(this, "Open GP32 FPK", QString(), "GP32 package (*.fpk *.zip);;All files (*)");
    if (!path.isEmpty()) { m_engine.loadFpk(path); m_engine.start(); m_runAction->setChecked(m_engine.isRunning()); }
}

void GP32MainWindow::configureBiosPath() {
    QString path = QFileDialog::getOpenFileName(this, "Set GP32 BIOS path", QString(), "GP32 BIOS (*.bin *.rom *.bios *.zip);;All files (*)");
    if (!path.isEmpty()) {
        m_engine.setBiosPath(path);
        syncBootModeUi();
        saveSettings();
        statusBar()->showMessage(QStringLiteral("BIOS path saved: %1").arg(QFileInfo(path).fileName()), 4000);
    }
}

void GP32MainWindow::clearBiosPath() {
    m_engine.clearBiosPath();
    syncBootModeUi();
    saveSettings();
}

void GP32MainWindow::configureControls() {
    QVector<int> keys = m_keyBindings;
    QStringList pads = m_gamepadBindings;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Configure GP32 controls"));
    QVBoxLayout *root = new QVBoxLayout(&dialog);

    QLabel *info = new QLabel(QStringLiteral("Configuration is saved to %1. Escape clears a keyboard binding while a key button is waiting for input.").arg(gp32SettingsPath()), &dialog);
    info->setWordWrap(true);
    root->addWidget(info);

#ifdef GP32EMU_QT_SDL3_INPUT
    QLabel *controller = new QLabel(QStringLiteral("Controller: %1").arg(controllerStatus()), &dialog);
#else
    QLabel *controller = new QLabel(QStringLiteral("Controller: SDL3 was not available at build time; keyboard remapping is still available."), &dialog);
#endif
    controller->setWordWrap(true);
    root->addWidget(controller);

    QGridLayout *grid = new QGridLayout();
    grid->addWidget(new QLabel(QStringLiteral("GP32 button"), &dialog), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("Keyboard"), &dialog), 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("Gamepad / joystick"), &dialog), 0, 2);
    QVector<KeyCaptureButton *> keyButtons;
    QVector<QComboBox *> padCombos;
    for (int i = 0; i < kInputCount; ++i) {
        grid->addWidget(new QLabel(QString::fromLatin1(kInputDefs[i].name), &dialog), i + 1, 0);
        KeyCaptureButton *kb = new KeyCaptureButton(&keys[i], &dialog);
        keyButtons.append(kb);
        grid->addWidget(kb, i + 1, 1);
        QComboBox *cb = new QComboBox(&dialog);
        populatePadCombo(cb, pads.value(i));
#ifndef GP32EMU_QT_SDL3_INPUT
        cb->setEnabled(false);
#endif
        padCombos.append(cb);
        grid->addWidget(cb, i + 1, 2);
    }
    root->addLayout(grid);

    QPushButton *defaults = new QPushButton(QStringLiteral("Restore defaults"), &dialog);
    connect(defaults, &QPushButton::clicked, &dialog, [&]() {
        for (int i = 0; i < kInputCount; ++i) {
            keys[i] = kInputDefs[i].defaultKey;
            pads[i] = QString::fromLatin1(kInputDefs[i].defaultPad);
            keyButtons[i]->refresh();
            int idx = padCombos[i]->findData(pads[i]);
            padCombos[i]->setCurrentIndex(std::max(0, idx));
        }
    });
    root->addWidget(defaults);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        m_keyBindings = keys;
        for (int i = 0; i < kInputCount; ++i) m_gamepadBindings[i] = padCombos[i]->currentData().toString();
        m_keyboardButtons = 0;
        m_gamepadButtons = 0;
        setCombinedButtons();
        saveSettings();
        statusBar()->showMessage(QStringLiteral("Control mappings saved"), 3000);
    }
}

void GP32MainWindow::bootBios() {
    if (m_engine.bootBios()) {
        m_engine.start();
        m_runAction->setChecked(m_engine.isRunning());
    } else {
        QMessageBox::information(this, "GP32emu", "No BIOS path is configured. Use Config > Set BIOS path first.");
    }
}

void GP32MainWindow::saveState() {
    QDir().mkpath(gp32StateDir());
    QString path = QFileDialog::getSaveFileName(this, "Save GP32 state", m_lastStatePath.isEmpty() ? gp32DefaultStatePath() : m_lastStatePath, "GP32 state (*.gp32st);;All files (*)");
    if (!path.isEmpty()) { m_lastStatePath = path; m_engine.saveState(path); saveSettings(); }
}

void GP32MainWindow::loadState() {
    QDir().mkpath(gp32StateDir());
    QString path = QFileDialog::getOpenFileName(this, "Load GP32 state", m_lastStatePath.isEmpty() ? gp32DefaultStatePath() : m_lastStatePath, "GP32 state (*.gp32st);;All files (*)");
    if (!path.isEmpty()) { m_lastStatePath = path; m_engine.loadState(path); saveSettings(); }
}

void GP32MainWindow::takeScreenshot() {
    QString path = QFileDialog::getSaveFileName(this, "Save GP32 screenshot", m_lastScreenshotPath, "Windows bitmap (*.bmp);;All files (*)");
    if (!path.isEmpty()) { m_lastScreenshotPath = path; m_engine.takeScreenshot(path); saveSettings(); }
}

void GP32MainWindow::startRecording() {
    if (m_engine.isRecording()) return;
    QString path = QFileDialog::getSaveFileName(this, "Record ZMBV/PCM MKV", m_lastRecordPath, "Matroska video (*.mkv);;All files (*)");
    if (path.isEmpty()) return;
    m_lastRecordPath = path;
    saveSettings();
    bool ok = m_engine.startRecording(path);
    if (m_recordAction) m_recordAction->setEnabled(!ok);
    if (m_stopRecordAction) m_stopRecordAction->setEnabled(ok);
}

void GP32MainWindow::stopRecording() {
    if (!m_engine.isRecording()) return;
    m_engine.stopRecording();
    if (m_recordAction) m_recordAction->setEnabled(true);
    if (m_stopRecordAction) m_stopRecordAction->setEnabled(false);
}

void GP32MainWindow::toggleRun() {
    if (m_engine.isRunning()) { m_engine.stop(); m_runAction->setChecked(false); }
    else { m_engine.start(); m_runAction->setChecked(m_engine.isRunning()); }
}

void GP32MainWindow::reset() { m_engine.reset(); }
void GP32MainWindow::toggleJit(bool checked) {
    if (m_engine.hasMachine()) {
        QString target = checked ? QStringLiteral("x64 JIT") : QStringLiteral("interpreter");
        if (QMessageBox::question(this, "GP32emu", QStringLiteral("Changing CPU core to %1 requires resetting the emulation core. Continue?").arg(target), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            m_jitAction->blockSignals(true);
            m_jitAction->setChecked(!checked);
            m_jitAction->blockSignals(false);
            return;
        }
    }
    m_engine.setJitEnabled(checked);
    saveSettings();
}
void GP32MainWindow::toggleScaling(bool checked) {
    if (checked && m_keepAspectAction && m_keepAspectAction->isChecked()) {
        m_keepAspectAction->blockSignals(true);
        m_keepAspectAction->setChecked(false);
        m_keepAspectAction->blockSignals(false);
        m_video->setKeepAspect(false);
    }
    m_video->setIntegerScaling(checked);
    saveSettings();
}
void GP32MainWindow::toggleKeepAspect(bool checked) {
    if (checked && m_scalingAction && m_scalingAction->isChecked()) {
        m_scalingAction->blockSignals(true);
        m_scalingAction->setChecked(false);
        m_scalingAction->blockSignals(false);
        m_video->setIntegerScaling(false);
    }
    m_video->setKeepAspect(checked);
    saveSettings();
}
void GP32MainWindow::toggleLcdPersistence(bool checked) {
    m_video->setLcdPersistence(checked);
    saveSettings();
}
void GP32MainWindow::toggleFrameInterpolation(bool checked) {
    m_video->setFrameInterpolation(checked);
    saveSettings();
}
void GP32MainWindow::toggleFullscreen() {
    if (isFullScreen()) {
        menuBar()->show();
        if (m_fullscreenAction) m_fullscreenAction->setChecked(false);
        showNormal();
    } else {
        if (m_fullscreenAction) m_fullscreenAction->setChecked(true);
        menuBar()->hide();
        showFullScreen();
    }
}

void GP32MainWindow::syncBootModeUi() {
    const bool haveBios = !m_engine.biosPath().isEmpty();
    if (haveBios && m_engine.useHle()) m_engine.setUseHle(false);
    if (!haveBios && !m_engine.useHle()) m_engine.setUseHle(true);
    if (m_useHleAction) {
        m_useHleAction->blockSignals(true);
        m_useHleAction->setChecked(!haveBios && m_engine.useHle());
        m_useHleAction->setEnabled(!haveBios);
        m_useHleAction->blockSignals(false);
    }
}

void GP32MainWindow::updateStatus(const QString &status) { m_status->setText(status); }

uint32_t GP32MainWindow::keyMappedButton(int key) const {
    for (int i = 0; i < kInputCount; ++i) if (m_keyBindings.value(i) == key) return kInputDefs[i].mask;

    /* Stable convenience aliases for arcade-style homebrew: AKA NOID requires
       a coin/select press followed by start.  Keep these aliases independent of
       the configurable primary bindings so old user settings cannot make the
       default title-screen controls appear dead. */
    if (key == Qt::Key_Space) return GP32_BUTTON_START;
    if (key == Qt::Key_C) return GP32_BUTTON_SELECT;
    return 0u;
}

uint32_t GP32MainWindow::bindingMask(int bindingIndex) const {
    return (bindingIndex >= 0 && bindingIndex < kInputCount) ? kInputDefs[bindingIndex].mask : 0u;
}

void GP32MainWindow::setCombinedButtons() {
    m_buttons = m_keyboardButtons | m_gamepadButtons;
    m_engine.setButtons(m_buttons);
}

void GP32MainWindow::applyKey(QKeyEvent *event, bool down) {
    if (event->isAutoRepeat()) return;
    if (event->key() == Qt::Key_Escape && down) { if (isFullScreen()) toggleFullscreen(); else close(); return; }
    if ((event->key() == Qt::Key_F11 || (event->key() == Qt::Key_Return && (event->modifiers() & Qt::AltModifier))) && down) { toggleFullscreen(); return; }
    if (event->key() == Qt::Key_F5 && down) { saveState(); return; }
    if (event->key() == Qt::Key_F8 && down) { loadState(); return; }
    if (event->key() == Qt::Key_F12 && down) { takeScreenshot(); return; }
    uint32_t b = keyMappedButton(event->key());
    if (!b) { event->ignore(); return; }
    if (down) m_keyboardButtons |= b;
    else m_keyboardButtons &= ~b;
    setCombinedButtons();
    event->accept();
}

void GP32MainWindow::keyPressEvent(QKeyEvent *event) { applyKey(event, true); }
void GP32MainWindow::keyReleaseEvent(QKeyEvent *event) { applyKey(event, false); }

void GP32MainWindow::closeEvent(QCloseEvent *event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

void GP32MainWindow::initControllerInput() {
#ifdef GP32EMU_QT_SDL3_INPUT
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS)) {
        m_sdlInputReady = 1;
        openFirstController();
    } else {
        m_sdlInputReady = 0;
    }
#endif
}

void GP32MainWindow::shutdownControllerInput() {
#ifdef GP32EMU_QT_SDL3_INPUT
    if (m_joystick) { SDL_CloseJoystick(m_joystick); m_joystick = nullptr; }
    if (m_sdlInputReady) SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS);
    m_sdlInputReady = 0;
#endif
}

void GP32MainWindow::openFirstController() {
#ifdef GP32EMU_QT_SDL3_INPUT
    if (!m_sdlInputReady || m_joystick) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids && count > 0) {
        m_joystick = SDL_OpenJoystick(ids[0]);
        if (m_joystick) {
            SDL_UpdateJoysticks();
            m_joyButtons = SDL_GetNumJoystickButtons(m_joystick);
            m_joyAxes = SDL_GetNumJoystickAxes(m_joystick);
            m_joyHats = SDL_GetNumJoystickHats(m_joystick);
        }
    }
    if (ids) SDL_free(ids);
#endif
}

QString GP32MainWindow::controllerStatus() const {
#ifdef GP32EMU_QT_SDL3_INPUT
    if (!m_sdlInputReady) return QStringLiteral("SDL3 input initialization failed: %1").arg(QString::fromUtf8(SDL_GetError()));
    if (!m_joystick) return QStringLiteral("No SDL3 joystick/gamepad detected");
    const char *name = SDL_GetJoystickName(m_joystick);
    return QStringLiteral("%1 (%2 buttons, %3 axes, %4 hats)").arg(QString::fromUtf8(name && name[0] ? name : "SDL3 joystick")).arg(m_joyButtons).arg(m_joyAxes).arg(m_joyHats);
#else
    return QStringLiteral("SDL3 input unavailable");
#endif
}

void GP32MainWindow::pollControllerInput() {
#ifdef GP32EMU_QT_SDL3_INPUT
    if (!m_sdlInputReady) return;
    SDL_PumpEvents();
    if (!m_joystick) openFirstController();
    uint32_t mask = 0;
    if (m_joystick) {
        SDL_UpdateJoysticks();
        for (int i = 0; i < kInputCount; ++i) {
            const QString binding = m_gamepadBindings.value(i);
            if (binding.isEmpty() || binding == QStringLiteral("none")) continue;
            const QStringList parts = binding.split(':');
            bool pressed = false;
            if (parts.size() >= 2 && parts[0] == QStringLiteral("button")) {
                bool ok = false;
                int b = parts[1].toInt(&ok);
                pressed = ok && b >= 0 && b < m_joyButtons && SDL_GetJoystickButton(m_joystick, b);
            } else if (parts.size() >= 3 && parts[0] == QStringLiteral("axis")) {
                bool ok = false;
                int axis = parts[1].toInt(&ok);
                if (ok && axis >= 0 && axis < m_joyAxes) {
                    int v = static_cast<int>(SDL_GetJoystickAxis(m_joystick, axis));
                    pressed = (parts[2] == QStringLiteral("-")) ? (v < -18000) : (v > 18000);
                }
            } else if (parts.size() >= 3 && parts[0] == QStringLiteral("hat")) {
                bool ok = false;
                int hatIndex = parts[1].toInt(&ok);
                if (ok && hatIndex >= 0 && hatIndex < m_joyHats) {
                    Uint8 h = SDL_GetJoystickHat(m_joystick, hatIndex);
                    if (parts[2] == QStringLiteral("up")) pressed = (h & SDL_HAT_UP) != 0;
                    else if (parts[2] == QStringLiteral("down")) pressed = (h & SDL_HAT_DOWN) != 0;
                    else if (parts[2] == QStringLiteral("left")) pressed = (h & SDL_HAT_LEFT) != 0;
                    else if (parts[2] == QStringLiteral("right")) pressed = (h & SDL_HAT_RIGHT) != 0;
                }
            }
            if (pressed) mask |= bindingMask(i);
        }
    }
    if (mask != m_gamepadButtons) {
        m_gamepadButtons = mask;
        setCombinedButtons();
    }
#endif
}
