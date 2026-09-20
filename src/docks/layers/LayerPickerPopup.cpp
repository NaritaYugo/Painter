#include "docks/layers/LayerPickerPopup.h"

#include "components/ThemeColors.h"

#include <QPainter>

LayerPickerPopup::LayerPickerPopup(QWidget *parent) : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint)
{
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
    }

void LayerPickerPopup::setItems(const QVector<ItemInfo> &items)
{ items_ = items; }

void LayerPickerPopup::setOriginIndex(int index)
{
        origin_ = index;
        update();
    }

qreal LayerPickerPopup::itemCenterX(int index)
{ return Padding + index * Step + ItemW / 2.0; }

void LayerPickerPopup::setFrame(qreal frameCenterX, int activeIndex)
{
        frameCenterX_ = frameCenterX;
        active_ = activeIndex;
        update();
    }

void LayerPickerPopup::setScrollOffset(qreal offset)
{
        scrollOffset_ = offset;
        update();
    }

void LayerPickerPopup::paintEvent(QPaintEvent *)
{
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Theme::overlayPanelBg);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(rect(), 8, 8);

        const qreal rowTop = 0;

        for (int i = 0; i < items_.size(); i++) {
            bool active = (i == active_);
            qreal cx = itemCenterX(i) + scrollOffset_;
            QRectF thumbRect(cx - ItemW / 2.0, rowTop + Padding, ItemW, ThumbH);

            p.setPen(QPen(Theme::bgButton, 1));
            p.setBrush(Theme::bgInput);
            p.drawRoundedRect(thumbRect, 5, 5);
            p.setOpacity(active ? 1.0 : 0.55);
            p.drawPixmap(thumbRect.adjusted(2, 2, -2, -2).toRect(), items_[i].thumb);
            p.setOpacity(1.0);

            QFont f = p.font();
            f.setPointSizeF(8.0);
            p.setFont(f);
            QRectF nameRect(cx - ItemW / 2.0 - 4, rowTop + Padding + ThumbH + 3, ItemW + 8, 14);
            p.setPen(active ? Theme::textBright : Theme::textDisabled);
            p.drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(items_[i].name, Qt::ElideRight, ItemW + 8));

            QRectF metaRect(cx - ItemW / 2.0 - 4, rowTop + Padding + ThumbH + 17, ItemW + 8, 14);
            p.setPen(active ? Theme::panelText : Theme::textDisabled);
            p.drawText(metaRect, Qt::AlignHCenter | Qt::AlignVCenter, items_[i].meta);
        }

        // 元居た列(ドラッグ開始時点でアクティブだった列)を、サムネイルだけを囲む点線の枠で常に示す(選択枠と重なっていても線種・大きさが違うので見分けが付く)。
        if (origin_ >= 0 && origin_ < items_.size()) {
            qreal ocx = itemCenterX(origin_) + scrollOffset_;
            QRectF originRect(ocx - ItemW / 2.0 - 3, rowTop + Padding - 3, ItemW + 6, ThumbH + 6);
            p.setPen(QPen(Theme::textBright, 2, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(originRect, 6, 6);
        }

        // サムネイルは固定。
        qreal frameW = ItemW + 10;
        qreal frameH = ItemH + 6;
        QRectF frame(frameCenterX_ + scrollOffset_ - frameW / 2.0, rowTop + Padding - 5, frameW, frameH);
        p.setPen(QPen(Theme::bgButton, 2.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 7, 7);
    }
