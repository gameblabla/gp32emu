#include "GP32MainWindow.h"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

static uint32_t keyButton(int key) {
    switch (key) {
    case Qt::Key_Left: return GP32_BUTTON_LEFT;
    case Qt::Key_Right: return GP32_BUTTON_RIGHT;
    case Qt::Key_Up: return GP32_BUTTON_UP;
    case Qt::Key_Down: return GP32_BUTTON_DOWN;
    case Qt::Key_Z: return GP32_BUTTON_A;
    case Qt::Key_X: return GP32_BUTTON_B;
    case Qt::Key_A: return GP32_BUTTON_L;
    case Qt::Key_S: return GP32_BUTTON_R;
    case Qt::Key_Return:
    case Qt::Key_Enter: return GP32_BUTTON_START;
    case Qt::Key_Shift: return GP32_BUTTON_SELECT;
    default: return 0u;
    }
}

GP32MainWindow::GP32MainWindow(QWidget *parent)
    : QMainWindow(parent), m_video(new GP32VideoWidget(this)), m_status(new QLabel(this)),
      m_runAction(nullptr), m_jitAction(nullptr), m_scalingAction(nullptr), m_keepAspectAction(nullptr), m_fullscreenAction(nullptr), m_lcdPersistenceAction(nullptr), m_frameInterpolationAction(nullptr), m_recordAction(nullptr), m_stopRecordAction(nullptr), m_useHleAction(nullptr), m_buttons(0),
      m_lastStatePath(QStringLiteral("gp32_state.gp32st")), m_lastScreenshotPath(QStringLiteral("gp32_screenshot.bmp")), m_lastRecordPath(QStringLiteral("gp32_recording.mkv")) {
    setWindowTitle("GP32emu");
    setWindowIcon(QIcon(":/gp32emu.png"));
    createMenus();
    loadSettings();
    setCentralWidget(m_video);
    statusBar()->addPermanentWidget(m_status, 1);
    m_status->setText(m_engine.biosPath().isEmpty()
        ? QStringLiteral("Ready. Keyboard: arrows, Z/X, A/S, Enter, Shift. F5/F8 save/load state.")
        : QStringLiteral("Ready. BIOS path configured: %1").arg(QFileInfo(m_engine.biosPath()).fileName()));
    connect(&m_engine, &GP32Engine::frameReady, m_video, &GP32VideoWidget::setFrame);
    connect(&m_engine, &GP32Engine::statusChanged, this, &GP32MainWindow::updateStatus);
    connect(&m_engine, &GP32Engine::recordingChanged, this, [this](bool recording) {
        if (m_recordAction) m_recordAction->setEnabled(!recording);
        if (m_stopRecordAction) m_stopRecordAction->setEnabled(recording);
    });
    resize(960, 720);
    syncBootModeUi();
    if (m_engine.biosPath().isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            QMessageBox::warning(this, "GP32emu HLE BIOS warning", "No BIOS path is configured. GP32emu will use the HLE BIOS fallback. HLE BIOS will fail to boot some games and only works with some commercial games and homebrew games.");
        });
    }
}


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

void GP32MainWindow::loadSettings() {
    QSettings s;
    QString bios = s.value(QStringLiteral("paths/bios")).toString();
    if (!bios.isEmpty()) m_engine.setBiosPath(bios);
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
    syncBootModeUi();
}

void GP32MainWindow::saveSettings() {
    QSettings s;
    if (m_engine.biosPath().isEmpty()) s.remove(QStringLiteral("paths/bios"));
    else s.setValue(QStringLiteral("paths/bios"), m_engine.biosPath());
    s.setValue(QStringLiteral("emulation/jit"), m_engine.jitEnabled());
    s.setValue(QStringLiteral("video/keepAspect"), m_keepAspectAction->isChecked());
    s.setValue(QStringLiteral("video/integerScaling"), m_scalingAction->isChecked());
    s.setValue(QStringLiteral("video/lcdPersistence"), m_lcdPersistenceAction && m_lcdPersistenceAction->isChecked());
    s.setValue(QStringLiteral("video/frameInterpolation"), m_frameInterpolationAction && m_frameInterpolationAction->isChecked());
    s.setValue(QStringLiteral("emulation/useHle"), m_engine.biosPath().isEmpty() && m_useHleAction && m_useHleAction->isChecked());
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
    QString path = QFileDialog::getOpenFileName(this, "Open GP32 SmartMedia image", QString(), "SmartMedia (*.smc);;All files (*)");
    if (!path.isEmpty()) { m_engine.loadSmartMedia(path); m_engine.start(); m_runAction->setChecked(m_engine.isRunning()); }
}

void GP32MainWindow::openFxe() {
    QString path = QFileDialog::getOpenFileName(this, "Open GP32 FXE", QString(), "GP32 executable (*.fxe);;All files (*)");
    if (!path.isEmpty()) { m_engine.loadFxe(path); m_engine.start(); m_runAction->setChecked(m_engine.isRunning()); }
}

void GP32MainWindow::openFpk() {
    QString path = QFileDialog::getOpenFileName(this, "Open GP32 FPK", QString(), "GP32 package (*.fpk);;All files (*)");
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

void GP32MainWindow::bootBios() {
    if (m_engine.bootBios()) {
        m_engine.start();
        m_runAction->setChecked(m_engine.isRunning());
    } else {
        QMessageBox::information(this, "GP32emu", "No BIOS path is configured. Use Config > Set BIOS path first.");
    }
}

void GP32MainWindow::saveState() {
    QString path = QFileDialog::getSaveFileName(this, "Save GP32 state", m_lastStatePath, "GP32 state (*.gp32st);;All files (*)");
    if (!path.isEmpty()) { m_lastStatePath = path; m_engine.saveState(path); }
}

void GP32MainWindow::loadState() {
    QString path = QFileDialog::getOpenFileName(this, "Load GP32 state", m_lastStatePath, "GP32 state (*.gp32st);;All files (*)");
    if (!path.isEmpty()) { m_lastStatePath = path; m_engine.loadState(path); }
}

void GP32MainWindow::takeScreenshot() {
    QString path = QFileDialog::getSaveFileName(this, "Save GP32 screenshot", m_lastScreenshotPath, "Windows bitmap (*.bmp);;All files (*)");
    if (!path.isEmpty()) { m_lastScreenshotPath = path; m_engine.takeScreenshot(path); }
}

void GP32MainWindow::startRecording() {
    if (m_engine.isRecording()) return;
    QString path = QFileDialog::getSaveFileName(this, "Record ZMBV/PCM MKV", m_lastRecordPath, "Matroska video (*.mkv);;All files (*)");
    if (path.isEmpty()) return;
    m_lastRecordPath = path;
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

void GP32MainWindow::applyKey(QKeyEvent *event, bool down) {
    if (event->isAutoRepeat()) return;
    if (event->key() == Qt::Key_Escape && down) { if (isFullScreen()) toggleFullscreen(); else close(); return; }
    if ((event->key() == Qt::Key_F11 || (event->key() == Qt::Key_Return && (event->modifiers() & Qt::AltModifier))) && down) { toggleFullscreen(); return; }
    if (event->key() == Qt::Key_F5 && down) { saveState(); return; }
    if (event->key() == Qt::Key_F8 && down) { loadState(); return; }
    if (event->key() == Qt::Key_F12 && down) { takeScreenshot(); return; }
    uint32_t b = keyButton(event->key());
    if (!b) { event->ignore(); return; }
    if (down) m_buttons |= b;
    else m_buttons &= ~b;
    m_engine.setButtons(m_buttons);
    event->accept();
}

void GP32MainWindow::keyPressEvent(QKeyEvent *event) { applyKey(event, true); }
void GP32MainWindow::keyReleaseEvent(QKeyEvent *event) { applyKey(event, false); }
