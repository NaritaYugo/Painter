#include "docks/layers/MaskOpacityPreview.h"

#include "components/ThemeColors.h"

#include <QPainter>
#include <QPolygonF>

MaskOpacityPreview::MaskOpacityPreview(QWidget *parent) : QWidget(parent)
{
        setFixedWidth(60);
        setFixedHeight(40); // 上下5pxのマーカー + 30pxのプレビュー
        // ドラッグ中もウィジェット全域を自前で塗る。
        setAttribute(Qt::WA_OpaquePaintEvent);
        setCursor(Qt::PointingHandCursor);
        setToolTip("左右ドラッグで不透明度、クリックでマスク編集の切り替え");
    }

void MaskOpacityPreview::setState(bool enabled, float opacity, bool hasMask, const QImage &maskThumb)
{
        enabled_   = enabled;
        opacity_   = opacity;
        hasMask_   = hasMask;
        maskThumb_ = maskThumb;
        update();
    }

void MaskOpacityPreview::paintEvent(QPaintEvent *)
{
        QPainter p(this);

        // WA_OpaquePaintEventでは全ピクセルを必ず塗る必要がある。
        p.fillRect(rect(), Theme::bgPanel);

        if (!enabled_) {
            p.fillRect(rect(), Theme::bgInput);
            return;
        }

        // 本体(濃淡プレビュー)の上下を三角形マーカーぶんだけ狭める。
        constexpr int kTriH = 5;
        QRect body = rect().adjusted(0, kTriH, 0, -kTriH);

        if (hasMask_ && !maskThumb_.isNull()) {
            // マスク濃淡へ一律のレイヤー不透明度も掛けて表示する。
            p.fillRect(body, Qt::black);
            p.save();
            p.setClipRect(body);
            p.setOpacity(opacity_);
            // QRectを描画先にした暗黙スケーリングでは、小数DPI環境で右端1pxが補間境界として露出することがある。
            const QImage scaledMask = maskThumb_.scaled(body.size(), Qt::IgnoreAspectRatio,
                                                        Qt::SmoothTransformation);
            p.drawImage(body.topLeft(), scaledMask);
            p.restore();
        } else {
            // 一律の不透明度に対応したグレー(100%=白, 0%=黒)。
            int g = qBound(0, int(opacity_ * 255.0f + 0.5f), 255);
            p.fillRect(body, QColor(g, g, g));
        }

        // 上下の三角形マーカー(▼▲)。
        p.setRenderHint(QPainter::Antialiasing);
        const qreal tw = 4.0; // 三角形の半幅
        const qreal cx = qBound(tw, opacity_ * width(), qreal(width()) - tw);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::accent);
        QPolygonF top;
        top << QPointF(cx - tw, 0.0) << QPointF(cx + tw, 0.0) << QPointF(cx, qreal(kTriH));
        p.drawPolygon(top);
        QPolygonF bottom;
        bottom << QPointF(cx - tw, qreal(height())) << QPointF(cx + tw, qreal(height()))
               << QPointF(cx, qreal(height() - kTriH));
        p.drawPolygon(bottom);
    }

void MaskOpacityPreview::mousePressEvent(QMouseEvent *event)
{
        if (event->button() != Qt::LeftButton || !enabled_) return;
        pressed_  = true;
        dragging_ = false;
        pressPos_ = event->pos();
    }

void MaskOpacityPreview::mouseMoveEvent(QMouseEvent *event)
{
        if (!pressed_) return;
        if (!dragging_ && qAbs(event->pos().x() - pressPos_.x()) > 3)
            dragging_ = true;
        if (dragging_) {
            const float v = (float)qBound(0.0, event->pos().x() / qreal(qMax(1, width())), 1.0);
            // ドック側への反映(LayerDock::scheduleRefresh)はサムネイル再生成を伴う重い処理を避けるため80msデバウンスされている(refresh()参照)。
            opacity_ = v;
            // ドラッグ中は1pxの描画遅延を避けるため即時更新する。
            repaint();
            emit opacityChanged(v);
        }
    }

void MaskOpacityPreview::mouseReleaseEvent(QMouseEvent *event)
{
        if (event->button() != Qt::LeftButton || !pressed_) return;
        pressed_ = false;
        if (!dragging_) emit clicked(); // 動かさずに離した=クリック
    }
