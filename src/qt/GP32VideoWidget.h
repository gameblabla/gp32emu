#ifndef GP32EMU_QT_VIDEOWIDGET_H
#define GP32EMU_QT_VIDEOWIDGET_H

#include <QImage>
#include <QWidget>

#include <vector>

extern "C" {
#include "gp32emu/gp32.h"
#include "gp32emu/video_effects.h"
}

class GP32VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit GP32VideoWidget(QWidget *parent = nullptr);
    ~GP32VideoWidget() override;
    void setFrame(const gp32_framebuffer_desc_t &fb);
    void setIntegerScaling(bool enabled);
    bool integerScaling() const { return m_integerScaling; }
    void setKeepAspect(bool enabled);
    bool keepAspect() const { return m_keepAspect; }
    void setLcdPersistence(bool enabled);
    bool lcdPersistence() const { return m_lcdPersistence; }
    void setFrameInterpolation(bool enabled);
    bool frameInterpolation() const { return m_frameInterpolation; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRect imageRect() const;
    bool copyFramebufferToImage(const gp32_framebuffer_desc_t &fb);
    void copyRgbToImage(const uint32_t *pixels);
    QImage m_image;
    bool m_integerScaling;
    bool m_keepAspect;
    bool m_lcdPersistence;
    bool m_frameInterpolation;
    bool m_effectsReady;
    gp32_video_effects_t m_effects;
    std::vector<uint32_t> m_pixels;
    std::vector<uint32_t> m_processed;
};

#endif
