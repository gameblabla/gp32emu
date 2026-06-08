#ifndef GP32EMU_QT_ENGINE_H
#define GP32EMU_QT_ENGINE_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

extern "C" {
#include "gp32emu/gp32.h"
#include "gp32emu/platform.h"
#include "media/gp32_media.h"
}

class GP32Engine : public QObject {
    Q_OBJECT
public:
    explicit GP32Engine(QObject *parent = nullptr);
    ~GP32Engine() override;

    bool configureFromArgs(int argc, char **argv);
    bool load(const QString &programPath, const QString &biosPath = QString(), const QString &kind = QString());
    bool loadSmartMedia(const QString &path);
    bool loadFxe(const QString &path);
    bool loadFpk(const QString &path);
    bool loadBios(const QString &path);
    bool bootBios();
    void setBiosPath(const QString &path);
    void clearBiosPath();
    void setUseHle(bool enabled);
    bool useHle() const { return m_useHle; }
    bool takeScreenshot(const QString &path);
    bool startRecording(const QString &path);
    void stopRecording();
    bool isRecording() const { return m_recorder != nullptr; }
    void start();
    void stop();
    bool isRunning() const { return m_running; }
    bool hasMachine() const { return m_gp32 != nullptr; }
    void setButtons(uint32_t buttons) { m_buttons = buttons; }
    void setJitEnabled(bool enabled);
    bool jitEnabled() const { return m_jit; }
    QString biosPath() const { return m_bios; }
    QString programPath() const { return m_program; }

public slots:
    void reset();
    bool saveState(const QString &path);
    bool loadState(const QString &path);
    void frameTick();

signals:
    void frameReady(const gp32_framebuffer_desc_t &fb);
    void statusChanged(const QString &status);
    void recordingChanged(bool recording);
    void stopped();

private:
    void destroyMachine();
    bool createMachine();
    void resetAudio();
    void submitAudio();
    bool runOneFrame();
    void scheduleNextTick(qint64 nowNs = 0);
    gp32_t *m_gp32;
    gp32_audio_backend_t *m_audio;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QString m_bios;
    QString m_program;
    QString m_kind;
    uint32_t m_buttons;
    uint32_t m_cycleRemainder;
    qint64 m_nextFrameUnits;
    uint64_t m_frameIndex;
    int m_renderFrames;
    int m_emuFrames;
    qint64 m_fpsNs;
    bool m_running;
    bool m_jit;
    bool m_useHle;
    gp32_media_recorder_t *m_recorder;
};

#endif
