#include "GP32VideoWidget.h"

extern "C" {
#include "media/gp32_media.h"
}

#include <QPainter>
#include <cstring>
#include <vector>

GP32VideoWidget::GP32VideoWidget(QWidget *parent)
    : QWidget(parent), m_image(320, 240, QImage::Format_ARGB32), m_integerScaling(false), m_keepAspect(true),
      m_lcdPersistence(false), m_frameInterpolation(false), m_effectsReady(false) {
    setMinimumSize(320 * 2, 240 * 2);
    setFocusPolicy(Qt::StrongFocus);
    m_image.fill(Qt::black);
    std::memset(&m_effects, 0, sizeof(m_effects));
    m_effectsReady = gp32_video_effects_init(&m_effects) != 0;
}

GP32VideoWidget::~GP32VideoWidget() { gp32_video_effects_shutdown(&m_effects); }

void GP32VideoWidget::setIntegerScaling(bool enabled) {
    m_integerScaling = enabled;
    update();
}

void GP32VideoWidget::setKeepAspect(bool enabled) {
    m_keepAspect = enabled;
    update();
}

void GP32VideoWidget::setLcdPersistence(bool enabled) {
    m_lcdPersistence = enabled;
    if (m_effectsReady) gp32_video_effects_set(&m_effects, m_lcdPersistence, m_frameInterpolation);
    update();
}

void GP32VideoWidget::setFrameInterpolation(bool enabled) {
    m_frameInterpolation = enabled;
    if (m_effectsReady) gp32_video_effects_set(&m_effects, m_lcdPersistence, m_frameInterpolation);
    update();
}

void GP32VideoWidget::setFrame(const gp32_framebuffer_desc_t &fb) {
    std::vector<uint32_t> pixels(static_cast<size_t>(GP32_MEDIA_LCD_W) * GP32_MEDIA_LCD_H);
    if (!gp32_media_frame_to_bgra_320x240(&fb, pixels.data())) return;
    if (m_effectsReady && (m_lcdPersistence || m_frameInterpolation)) {
        std::vector<uint32_t> processed(static_cast<size_t>(GP32_MEDIA_LCD_W) * GP32_MEDIA_LCD_H);
        if (gp32_video_effects_process_320x240(&m_effects, pixels.data(), processed.data())) pixels.swap(processed);
    } else if (m_effectsReady) {
        gp32_video_effects_reset(&m_effects);
    }
    for (int y = 0; y < static_cast<int>(GP32_MEDIA_LCD_H); ++y) {
        uint32_t *dst = reinterpret_cast<uint32_t *>(m_image.scanLine(y));
        const uint32_t *src = pixels.data() + static_cast<size_t>(y) * GP32_MEDIA_LCD_W;
        for (int x = 0; x < static_cast<int>(GP32_MEDIA_LCD_W); ++x) dst[x] = 0xff000000u | (src[x] & 0x00ffffffu);
    }
    update();
}

QSize GP32VideoWidget::sizeHint() const { return QSize(320 * 3, 240 * 3); }

QRect GP32VideoWidget::imageRect() const {
    QRect r = rect();
    if (r.isEmpty()) return r;
    if (m_integerScaling) {
        int sx = r.width() / 320;
        int sy = r.height() / 240;
        int s = qMax(1, qMin(sx, sy));
        QSize sz(320 * s, 240 * s);
        return QRect(QPoint((r.width() - sz.width()) / 2, (r.height() - sz.height()) / 2), sz);
    }
    if (m_keepAspect) {
        int w = (r.height() * 320 + 120) / 240;
        int h = r.height();
        if (w > r.width()) {
            w = r.width();
            h = (r.width() * 240 + 160) / 320;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return QRect(QPoint((r.width() - w) / 2, (r.height() - h) / 2), QSize(w, h));
    }
    return r;
}

void GP32VideoWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(imageRect(), m_image);
}
