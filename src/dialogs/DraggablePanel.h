#pragma once
#include "components/ThemeColors.h"

#include <QWidget>
#include <QPoint>
#include <QMouseEvent>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

// ---------------------------------------------------------------------------
// DraggablePanel
// ---------------------------------------------------------------------------
// キャンバス上に浮かぶ各種アクションパネル(色相・彩度・明度/明るさ・コントラスト/
// カラーバランス/トーンカーブ/ガウスぼかし/モザイク/カスタムシェーダー/
// キャンバスサイズ変更/画像解像度変更/テキストレイヤー編集)の共通基底クラス。
//
// CanvasWidgetの子ウィジェットとしてではなく、Qt::Tool(枠なし・タスクバーに出ない・
// メインウィンドウと最小化等が連動する「フローティングツールウィンドウ」)として
// 独立したトップレベルウィンドウにすることで、CanvasWidget(タブ)の表示範囲を超えて
// 画面上の好きな位置へ移動できるようにする(コンストラクタにparentを渡す点は
// 従来通りで、所有・破棄のライフタイムはそのまま親に従う)。
// パネル自身の背景(スライダーやボタン等の子ウィジェットが乗っていない領域、
// タイトルラベル部分を含む)をドラッグすると、そのままパネルを画面上の任意の
// 位置へ移動できる。各パネルはこのクラスを継承するだけでよく、個々の
// paintEvent()等の見た目は一切変更しない。
// ---------------------------------------------------------------------------
class DraggablePanel : public QWidget
{
public:
    explicit DraggablePanel(QWidget *parent = nullptr) : QWidget(parent)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    }

protected:
    // 全パネルで完全に同一だった半透明の角丸背景描画をここに集約する
    // (色はTheme::overlayPanelBgの1箇所からのみ変更できる)。個々のパネルは
    // 見た目の異なる描画を追加したくなった場合にのみこれをoverrideすればよい。
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(rect(), 8, 8);
        painter.fillPath(path, Theme::overlayPanelBg);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            dragStartMousePos_ = event->globalPosition().toPoint();
            dragStartPanelPos_ = pos();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            const QPoint delta = event->globalPosition().toPoint() - dragStartMousePos_;
            QPoint newPos = dragStartPanelPos_ + delta;
            // 画面外へ完全に出てしまわない範囲に留める(トップレベルウィンドウに
            // なったため、座標は画面全体のグローバル座標系で扱う)。
            if (QScreen *scr = screen()) {
                const QRect avail = scr->availableGeometry();
                const int maxX = qMax(avail.left(), avail.right()  - width()  + 1);
                const int maxY = qMax(avail.top(),  avail.bottom() - height() + 1);
                newPos.setX(qBound(avail.left(), newPos.x(), maxX));
                newPos.setY(qBound(avail.top(),  newPos.y(), maxY));
            }
            move(newPos);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && dragging_) {
            dragging_ = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    bool   dragging_ = false;
    QPoint dragStartMousePos_;
    QPoint dragStartPanelPos_;
};
