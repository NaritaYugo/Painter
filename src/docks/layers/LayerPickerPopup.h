#pragma once

#include <QPixmap>
#include <QString>
#include <QVector>
#include <QWidget>

// 行を長押し/ドラッグしたとき、クリッピング列を横並びで表示する選択UI。
class LayerPickerPopup : public QWidget
{
public:
    static constexpr int ItemW    = 64;
    static constexpr int ThumbH   = 64;
    static constexpr int TextH    = 32;
    static constexpr int ItemH    = ThumbH + 4 + TextH;
    static constexpr int Spacing  = 16;
    static constexpr int Step     = ItemW + Spacing;
    static constexpr int Padding  = 14;

    struct ItemInfo { QPixmap thumb; QString name; QString meta; };

    explicit LayerPickerPopup(QWidget *parent = nullptr);

    void setItems(const QVector<ItemInfo> &items);

    // ドラッグ開始時点で表示していた(=元居た)列。
    void setOriginIndex(int index);

    static qreal itemCenterX(int index);

    // frameCenterX: 選択枠の中心のローカルX座標(ドラッグに連続して追従する、スクロールオフセットを含まない論理座標)。
    void setFrame(qreal frameCenterX, int activeIndex);

    // ウィンドウ幅がコンテンツ全幅より狭い(=画面に収まりきらない)ときに、表示中の項目群をこの分だけ横にずらす(常に <= 0)。
    void setScrollOffset(qreal offset);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QVector<ItemInfo> items_;
    qreal frameCenterX_ = 0;
    int   active_ = 0;
    qreal scrollOffset_ = 0;
    int   origin_ = -1;
};

// LayerRowWidget ― クリッピングの1行ぶんの表示
