#ifndef GP32EMU_QT_MAINWINDOW_H
#define GP32EMU_QT_MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <stdint.h>

#include "GP32Engine.h"
#include "GP32VideoWidget.h"

class QAction;
class QLabel;
class QKeyEvent;

class GP32MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit GP32MainWindow(QWidget *parent = nullptr);
    GP32Engine *engine() { return &m_engine; }

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private slots:
    void openBios();
    void openSmartMedia();
    void openFxe();
    void openFpk();
    void configureBiosPath();
    void clearBiosPath();
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

private:
    void createMenus();
    void loadSettings();
    void saveSettings();
    void applyKey(QKeyEvent *event, bool down);
    void syncBootModeUi();
    GP32Engine m_engine;
    GP32VideoWidget *m_video;
    QLabel *m_status;
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
    uint32_t m_buttons;
    QString m_lastStatePath;
    QString m_lastScreenshotPath;
    QString m_lastRecordPath;
};

#endif
