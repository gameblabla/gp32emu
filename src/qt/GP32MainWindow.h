#ifndef GP32EMU_QT_MAINWINDOW_H
#define GP32EMU_QT_MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVector>
#include <stdint.h>

#include "GP32Engine.h"
#include "GP32VideoWidget.h"

class QAction;
class QLabel;
class QKeyEvent;
class QCloseEvent;
class QTimer;

#ifdef GP32EMU_QT_SDL3_INPUT
struct SDL_Joystick;
#endif

class GP32MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit GP32MainWindow(QWidget *parent = nullptr);
    ~GP32MainWindow() override;
    GP32Engine *engine() { return &m_engine; }

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openBios();
    void openSmartMedia();
    void openFxe();
    void openFpk();
    void configureBiosPath();
    void clearBiosPath();
    void configureControls();
    void bootBios();
    void saveState();
    void loadState();
    void takeScreenshot();
    void startRecording();
    void stopRecording();
    void toggleRun();
    void reset();
    void toggleJit(bool checked);
    void toggleScaling(bool checked);
    void toggleKeepAspect(bool checked);
    void toggleFullscreen();
    void toggleLcdPersistence(bool checked);
    void toggleFrameInterpolation(bool checked);
    void updateStatus(const QString &status);
    void pollControllerInput();

private:
    void createMenus();
    void loadSettings();
    void saveSettings();
    void applyKey(QKeyEvent *event, bool down);
    void syncBootModeUi();
    void setCombinedButtons();
    void resetInputBindings();
    uint32_t keyMappedButton(int key) const;
    uint32_t bindingMask(int bindingIndex) const;
    void initControllerInput();
    void shutdownControllerInput();
    void openFirstController();
    QString controllerStatus() const;
    GP32Engine m_engine;
    GP32VideoWidget *m_video;
    QLabel *m_status;
    QTimer *m_inputTimer;
    QAction *m_runAction;
    QAction *m_jitAction;
    QAction *m_scalingAction;
    QAction *m_keepAspectAction;
    QAction *m_fullscreenAction;
    QAction *m_lcdPersistenceAction;
    QAction *m_frameInterpolationAction;
    QAction *m_recordAction;
    QAction *m_stopRecordAction;
    QAction *m_useHleAction;
    QAction *m_configureControlsAction;
    QVector<int> m_keyBindings;
    QStringList m_gamepadBindings;
    uint32_t m_keyboardButtons;
    uint32_t m_gamepadButtons;
    uint32_t m_buttons;
    QString m_lastStatePath;
    QString m_lastScreenshotPath;
    QString m_lastRecordPath;
#ifdef GP32EMU_QT_SDL3_INPUT
    SDL_Joystick *m_joystick;
    int m_sdlInputReady;
    int m_joyButtons;
    int m_joyAxes;
    int m_joyHats;
#endif
};

#endif
