#include "GP32VideoWidget.h"

extern "C" {
#include "media/gp32_media.h"
}

#include <QPainter>
#include <cstring>

GP32VideoWidget::GP32VideoWidget(QWidget *parent)
    : QWidget(parent), m_image(320, 240, QImage::Format_ARGB32), m_integerScaling(false), m_keepAspect(true),
      m_lcdPersistence(false), m_frameInterpolation(false), m_effectsReady(false) {
    setMinimumSize(320 * 2, 240 * 2);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    m_image.fill(Qt::black);
    m_pixels.resize(static_cast<size_t>(GP32_MEDIA_LCD_W) * GP32_MEDIA_LCD_H);
    m_processed.resize(static_cast<size_t>(GP32_MEDIA_LCD_W) * GP32_MEDIA_LCD_H);
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

static inline uint32_t qt_argb_from_gp32(uint32_t p) { return 0xff000000u | (p & 0x00ffffffu); }

bool GP32VideoWidget::copyFramebufferToImage(const gp32_framebuffer_desc_t &fb) {
    if (!fb.pixels_rgba8888 || !fb.width || !fb.height || fb.stride_pixels < fb.width) return false;
    const uint32_t *src = fb.pixels_rgba8888;
    const uint32_t stride = fb.stride_pixels;

    if (fb.width == GP32_MEDIA_LCD_W && fb.height == GP32_MEDIA_LCD_H) {
        for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
            uint32_t *dst = reinterpret_cast<uint32_t *>(m_image.scanLine(static_cast<int>(y)));
            const uint32_t *row = src + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) dst[x] = qt_argb_from_gp32(row[x]);
        }
        return true;
    }

    if (fb.width == 240u && fb.height == 320u) {
        for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
            const uint32_t sx = 240u - 1u - y;
            uint32_t *dst = reinterpret_cast<uint32_t *>(m_image.scanLine(static_cast<int>(y)));
            for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) dst[x] = qt_argb_from_gp32(src[static_cast<size_t>(x) * stride + sx]);
        }
        return true;
    }

    for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
        uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * fb.height) / GP32_MEDIA_LCD_H);
        if (sy >= fb.height) sy = fb.height - 1u;
        const uint32_t *row = src + static_cast<size_t>(sy) * stride;
        uint32_t *dst = reinterpret_cast<uint32_t *>(m_image.scanLine(static_cast<int>(y)));
        for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) {
            uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * fb.width) / GP32_MEDIA_LCD_W);
            if (sx >= fb.width) sx = fb.width - 1u;
            dst[x] = qt_argb_from_gp32(row[sx]);
        }
    }
    return true;
}

void GP32VideoWidget::copyRgbToImage(const uint32_t *pixels) {
    if (!pixels) return;
    for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
        uint32_t *dst = reinterpret_cast<uint32_t *>(m_image.scanLine(static_cast<int>(y)));
        const uint32_t *src = pixels + static_cast<size_t>(y) * GP32_MEDIA_LCD_W;
        for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) dst[x] = qt_argb_from_gp32(src[x]);
    }
}

void GP32VideoWidget::setFrame(const gp32_framebuffer_desc_t &fb) {
    if (m_effectsReady && (m_lcdPersistence || m_frameInterpolation)) {
        if (!gp32_media_frame_to_bgra_320x240(&fb, m_pixels.data())) return;
        const uint32_t *present = m_pixels.data();
        if (gp32_video_effects_process_320x240(&m_effects, m_pixels.data(), m_processed.data())) present = m_processed.data();
        copyRgbToImage(present);
    } else {
        if (m_effectsReady) gp32_video_effects_reset(&m_effects);
        if (!copyFramebufferToImage(fb)) return;
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
    const QRect target = imageRect();
    if (target != rect()) {
        if (target.top() > 0) p.fillRect(QRect(0, 0, width(), target.top()), Qt::black);
        if (target.bottom() + 1 < height()) p.fillRect(QRect(0, target.bottom() + 1, width(), height() - target.bottom() - 1), Qt::black);
        if (target.left() > 0) p.fillRect(QRect(0, target.top(), target.left(), target.height()), Qt::black);
        if (target.right() + 1 < width()) p.fillRect(QRect(target.right() + 1, target.top(), width() - target.right() - 1, target.height()), Qt::black);
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target, m_image);
}
