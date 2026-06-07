#include "GP32Engine.h"

#include <QFile>
#include <QFileInfo>

#include <cstring>

static QByteArray toLocal(const QString &s) { return QFile::encodeName(s); }

GP32Engine::GP32Engine(QObject *parent)
    : QObject(parent), m_gp32(nullptr), m_audio(nullptr), m_buttons(0), m_cycleRemainder(0),
      m_accumUnits(1000000ull), m_frameIndex(0), m_renderFrames(0), m_emuFrames(0),
      m_lastNs(0), m_fpsNs(0), m_running(false), m_jit(true), m_useHle(false), m_recorder(nullptr) {
    connect(&m_timer, &QTimer::timeout, this, &GP32Engine::frameTick);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(0);
    m_clock.start();
}

GP32Engine::~GP32Engine() { stopRecording(); destroyMachine(); }

void GP32Engine::destroyMachine() {
    if (m_audio) { gp32_audio_destroy(m_audio); m_audio = nullptr; }
    if (m_gp32) { gp32_destroy(m_gp32); m_gp32 = nullptr; }
    m_cycleRemainder = 0;
    m_accumUnits = 1000000ull;
    m_frameIndex = 0;
}

bool GP32Engine::createMachine() {
    if (m_bios.isEmpty() && m_program.isEmpty()) {
        emit statusChanged("No BIOS or GP32 program selected. Use Config > Set BIOS path or File > Open SmartMedia/FXE/FPK.");
        return false;
    }
    destroyMachine();
    QByteArray bios = toLocal(m_bios);
    QByteArray program = toLocal(m_program);
    gp32_options_t opt;
    std::memset(&opt, 0, sizeof(opt));
    m_gp32 = gp32_create(&opt);
    if (!m_gp32) { emit statusChanged("gp32_create failed"); return false; }
    gp32_set_jit(m_gp32, m_jit ? 1 : 0);
    gp32_status_t st = GP32_OK;
    const bool haveProgram = !m_program.isEmpty();
    const bool useRealBios = !m_bios.isEmpty();
    const bool useHleBoot = haveProgram && !useRealBios;
    if (useHleBoot && !m_useHle) {
        emit statusChanged("No BIOS path is configured and HLE BIOS fallback is disabled.");
        return false;
    }
    if (useRealBios) {
        st = gp32_load_bios(m_gp32, bios.constData());
        if (st == GP32_OK && !m_program.isEmpty() && m_kind == "smc") st = gp32_load_smartmedia(m_gp32, program.constData());
        if (st == GP32_OK) st = gp32_reset(m_gp32);
    }
    if (!m_program.isEmpty()) {
        if (m_kind == "smc" && useHleBoot) st = gp32_load_smartmedia_direct(m_gp32, program.constData());
        else if (st == GP32_OK && m_kind == "fxe") st = gp32_load_fxe(m_gp32, program.constData());
        else if (st == GP32_OK && m_kind == "fpk") st = gp32_load_fpk(m_gp32, program.constData());
    }
    if (st != GP32_OK) { emit statusChanged(QStringLiteral("Load failed: %1").arg(QString::fromUtf8(gp32_get_error(m_gp32)))); return false; }
#ifdef GP32EMU_QT_SDL3_AUDIO
    gp32_audio_options_t aopt;
    std::memset(&aopt, 0, sizeof(aopt));
    aopt.sample_rate_hz = 44100u;
    aopt.buffer_frames = 2048u;
    m_audio = gp32_audio_sdl3_create(&aopt);
    if (!m_audio) emit statusChanged("SDL3 audio unavailable");
#endif
    m_accumUnits = 1000000ull;
    m_lastNs = m_clock.nsecsElapsed();
    m_fpsNs = m_lastNs;
    m_running = true;
    if (m_program.isEmpty()) emit statusChanged(QStringLiteral("Booting BIOS: %1").arg(QFileInfo(m_bios).fileName()));
    else emit statusChanged(QStringLiteral("Loaded %1 using %2").arg(QFileInfo(m_program).fileName(), useRealBios ? QStringLiteral("BIOS") : QStringLiteral("HLE BIOS")));
    return true;
}

bool GP32Engine::load(const QString &programPath, const QString &biosPath, const QString &kind) {
    if (!biosPath.isEmpty()) { m_bios = biosPath; m_useHle = false; }
    m_program = programPath;
    m_kind = kind;
    if (m_kind.isEmpty()) {
        QString suffix = QFileInfo(programPath).suffix().toLower();
        if (suffix == "fxe") m_kind = "fxe";
        else if (suffix == "fpk") m_kind = "fpk";
        else m_kind = "smc";
    }
    return createMachine();
}

bool GP32Engine::loadSmartMedia(const QString &path) { return load(path, QString(), "smc"); }
bool GP32Engine::loadFxe(const QString &path) { return load(path, QString(), "fxe"); }
bool GP32Engine::loadFpk(const QString &path) { return load(path, QString(), "fpk"); }

bool GP32Engine::loadBios(const QString &path) {
    m_bios = path;
    if (!m_bios.isEmpty()) m_useHle = false;
    if (m_program.isEmpty()) return bootBios();
    return createMachine();
}

bool GP32Engine::bootBios() {
    if (m_bios.isEmpty()) {
        emit statusChanged("No BIOS path configured. Use Config > Set BIOS path or File > Open BIOS.");
        return false;
    }
    m_program.clear();
    m_kind.clear();
    return createMachine();
}

void GP32Engine::setBiosPath(const QString &path) {
    m_bios = path;
    m_useHle = m_bios.isEmpty();
    emit statusChanged(path.isEmpty() ? QStringLiteral("BIOS path cleared; HLE BIOS fallback will be used for game launches") : QStringLiteral("BIOS path set: %1; HLE BIOS fallback disabled").arg(QFileInfo(path).fileName()));
}

void GP32Engine::clearBiosPath() {
    m_bios.clear();
    m_useHle = true;
    emit statusChanged("BIOS path cleared. HLE BIOS fallback will be used for game launches.");
}

void GP32Engine::setUseHle(bool enabled) {
    m_useHle = enabled;
    emit statusChanged(enabled ? QStringLiteral("HLE BIOS selected for game launches") : QStringLiteral("Configured BIOS selected for game launches"));
}

bool GP32Engine::configureFromArgs(int argc, char **argv) {
    QString bios, program, kind;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--bios" && i + 1 < argc) bios = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--smc" && i + 1 < argc) { program = QString::fromLocal8Bit(argv[++i]); kind = "smc"; }
        else if (a == "--fxe" && i + 1 < argc) { program = QString::fromLocal8Bit(argv[++i]); kind = "fxe"; }
        else if (a == "--fpk" && i + 1 < argc) { program = QString::fromLocal8Bit(argv[++i]); kind = "fpk"; }
        else if (a == "--no-jit") m_jit = false;
        else if (a == "--jit") m_jit = true;
        else if (!a.startsWith('-')) program = a;
    }
    if (!bios.isEmpty()) { m_bios = bios; m_useHle = false; }
    if (!program.isEmpty()) return load(program, QString(), kind);
    return true;
}

void GP32Engine::start() {
    if (!m_gp32 && (!m_program.isEmpty() || !m_bios.isEmpty())) {
        if (!createMachine()) { m_running = false; return; }
    }
    if (!m_gp32) {
        emit statusChanged("No BIOS or GP32 program loaded.");
        m_running = false;
        return;
    }
    m_running = true;
    m_lastNs = m_clock.nsecsElapsed();
    m_timer.start();
}

void GP32Engine::stop() {
    m_running = false;
    m_timer.stop();
    emit stopped();
}

void GP32Engine::setJitEnabled(bool enabled) {
    m_jit = enabled;
    if (m_gp32) createMachine();
}

void GP32Engine::reset() {
    if (!m_gp32) return;
    gp32_status_t st = gp32_reset(m_gp32);
    if (st == GP32_OK) { m_cycleRemainder = 0; m_accumUnits = 1000000ull; emit statusChanged("Reset"); }
    else emit statusChanged(QString::fromUtf8(gp32_get_error(m_gp32)));
}

bool GP32Engine::saveState(const QString &path) {
    if (!m_gp32) return false;
    QByteArray p = toLocal(path);
    gp32_status_t st = gp32_save_state(m_gp32, p.constData());
    emit statusChanged(st == GP32_OK ? QStringLiteral("State saved") : QString::fromUtf8(gp32_get_error(m_gp32)));
    return st == GP32_OK;
}

bool GP32Engine::loadState(const QString &path) {
    if (!m_gp32) return false;
    QByteArray p = toLocal(path);
    gp32_status_t st = gp32_load_state(m_gp32, p.constData());
    if (st == GP32_OK) { m_accumUnits = 1000000ull; emit statusChanged("State loaded"); }
    else emit statusChanged(QString::fromUtf8(gp32_get_error(m_gp32)));
    return st == GP32_OK;
}

bool GP32Engine::takeScreenshot(const QString &path) {
    if (!m_gp32 || path.isEmpty()) return false;
    gp32_framebuffer_desc_t fb;
    char err[256] = {0};
    QByteArray p = toLocal(path);
    bool ok = gp32_get_framebuffer(m_gp32, &fb) == GP32_OK && gp32_media_write_bmp_320x240(p.constData(), &fb, err, sizeof(err));
    emit statusChanged(ok ? QStringLiteral("Screenshot saved") : QStringLiteral("Screenshot failed: %1").arg(QString::fromUtf8(err[0] ? err : "unknown error")));
    return ok;
}

bool GP32Engine::startRecording(const QString &path) {
    if (path.isEmpty()) return false;
    stopRecording();
    char err[256] = {0};
    QByteArray p = toLocal(path);
    m_recorder = gp32_media_recorder_open(p.constData(), 44100u, err, sizeof(err));
    if (!m_recorder) { emit statusChanged(QStringLiteral("Recording open failed: %1").arg(QString::fromUtf8(err[0] ? err : "unknown error"))); return false; }
    emit statusChanged("Recording started: ZMBV video + PCM audio in MKV");
    emit recordingChanged(true);
    return true;
}

void GP32Engine::stopRecording() {
    if (!m_recorder) return;
    gp32_media_recorder_close(m_recorder);
    m_recorder = nullptr;
    emit statusChanged("Recording stopped");
    emit recordingChanged(false);
}

void GP32Engine::submitAudio() {
    if (!m_gp32) return;
    gp32_audio_desc_t aud;
    if (gp32_get_audio(m_gp32, &aud) == GP32_OK && aud.frame_count > 0) {
        if (m_audio) gp32_audio_submit(m_audio, &aud);
        if (m_recorder && !gp32_media_recorder_add_audio(m_recorder, &aud)) {
            emit statusChanged(QStringLiteral("Recording audio failed: %1").arg(QString::fromUtf8(gp32_media_recorder_error(m_recorder))));
            stopRecording();
        }
        gp32_clear_audio(m_gp32);
    }
}

bool GP32Engine::runOneFrame() {
    if (!m_gp32) return false;
    gp32_set_buttons(m_gp32, m_buttons);
    uint32_t hz = gp32_get_run_clock_hz(m_gp32);
    if (!hz) hz = 66000000u;
    uint32_t cycles = hz / 60u;
    m_cycleRemainder += hz % 60u;
    if (m_cycleRemainder >= 60u) { cycles++; m_cycleRemainder -= 60u; }
    gp32_status_t st = gp32_run_cycles(m_gp32, cycles);
    if (st != GP32_OK) { emit statusChanged(QString::fromUtf8(gp32_get_error(m_gp32))); stop(); return false; }
    submitAudio();
    m_frameIndex++;
    m_emuFrames++;
    return true;
}

void GP32Engine::frameTick() {
    if (!m_running || !m_gp32) return;
    qint64 now = m_clock.nsecsElapsed();
    if (!m_lastNs) m_lastNs = now;
    qint64 elapsedNs = now - m_lastNs;
    m_lastNs = now;
    if (elapsedNs < 0) elapsedNs = 0;
    if (elapsedNs > 250000000) elapsedNs = 250000000;
    m_accumUnits += static_cast<uint64_t>(elapsedNs / 1000) * 60ull;
    int ran = 0;
    for (int i = 0; i < 5 && m_accumUnits >= 1000000ull; ++i) {
        if (!runOneFrame()) break;
        m_accumUnits -= 1000000ull;
        ran = 1;
    }
    if (m_accumUnits >= 1000000ull) m_accumUnits = 1000000ull;
    if (ran) {
        gp32_framebuffer_desc_t fb;
        if (gp32_get_framebuffer(m_gp32, &fb) == GP32_OK) { emit frameReady(fb); if (m_recorder && !gp32_media_recorder_add_frame(m_recorder, &fb, m_frameIndex ? m_frameIndex - 1ull : 0ull)) { emit statusChanged(QStringLiteral("Recording video failed: %1").arg(QString::fromUtf8(gp32_media_recorder_error(m_recorder)))); stopRecording(); } m_renderFrames++; }
    }
    if (!m_fpsNs) m_fpsNs = now;
    if (now - m_fpsNs >= 1000000000ll) {
        emit statusChanged(QStringLiteral("%1 render / %2 emu FPS | JIT %3").arg(m_renderFrames).arg(m_emuFrames).arg(m_jit ? "on" : "off"));
        m_renderFrames = 0;
        m_emuFrames = 0;
        m_fpsNs = now;
    }
}
