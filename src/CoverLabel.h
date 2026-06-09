#pragma once

#include <QLabel>
#include <QPixmap>
#include <QIcon>
#include <QPainter>
#include <QEvent>
#include <QResizeEvent>

// A QLabel that displays album art scaled to fit its current size (keeping
// aspect ratio), rescaling as the panel/splitter is resized. When there's no
// cover it shows a placeholder album-disc icon, tinted to the current theme's
// muted text colour so it reads on both light and dark backgrounds.
// Header-only; no signals/slots, so no moc needed.
class CoverLabel : public QLabel
{
public:
    using QLabel::QLabel;

    void setCover(const QPixmap &pixmap)
    {
        m_source = pixmap;
        rescale();
    }

    // A pixmap QLabel normally reports its pixmap size as the minimum, which floors how
    // far the panel/app can shrink horizontally. The art rescales to whatever space it
    // gets (see rescale()), so it needs no intrinsic minimum — let the layout shrink it.
    // (Vertical floor still comes from setMinimumHeight() on the widget.)
    QSize minimumSizeHint() const override { return QSize(0, 0); }
    QSize sizeHint() const override { return QSize(0, 0); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        rescale();
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        // Re-tint the placeholder when the theme/palette changes.
        if (m_source.isNull()
            && (event->type() == QEvent::PaletteChange
                || event->type() == QEvent::StyleChange))
            rescale();
    }

private:
    void rescale()
    {
        QLabel::setText(QString());
        if (!m_source.isNull()) {
            QLabel::setPixmap(m_source.scaled(size(), Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
            return;
        }
        QLabel::setPixmap(placeholderPixmap());
    }

    QPixmap placeholderPixmap() const
    {
        const int side = qMin(width(), height());
        if (side < 8)
            return QPixmap();
        const qreal dpr = devicePixelRatioF();
        const int px = qMax(8, int(side * 0.55));   // icon occupies ~55% of the area

        static const QIcon icon(QStringLiteral(":/icons/album_icon.svg"));
        QPixmap base = icon.pixmap(QSize(px, px), dpr);   // monochrome (black) art
        if (base.isNull())
            return QPixmap();

        // Recolour every opaque pixel to the theme's placeholder/muted colour.
        QColor tint = palette().color(QPalette::PlaceholderText);
        if (!tint.isValid() || tint.alpha() == 0)
            tint = palette().color(QPalette::WindowText);
        QPixmap tinted(base.size());
        tinted.setDevicePixelRatio(dpr);
        tinted.fill(Qt::transparent);
        QPainter p(&tinted);
        p.drawPixmap(0, 0, base);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(tinted.rect(), tint);
        p.end();
        return tinted;
    }

    QPixmap m_source;
};
