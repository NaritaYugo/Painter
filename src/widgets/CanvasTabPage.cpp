#include "widgets/CanvasTabPage.h"
#include "widgets/StartPage.h"
#include "widgets/GLWidget.h"

CanvasTabPage::CanvasTabPage(QWidget *parent) : QStackedWidget(parent)
{
    startPage_ = new StartPage(this);
    addWidget(startPage_);
    setCurrentWidget(startPage_);
}

void CanvasTabPage::setCanvas(GLWidget *gl)
{
    if (!gl || canvas_) return;
    canvas_ = gl;
    // addWidget()は追加したページを隠したままにする(カレントはスタートページのまま)。
    // ここで表示へ切り替えないのが重要 ― 理由はヘッダのコメント参照。
    addWidget(gl);
}

void CanvasTabPage::showCanvas()
{
    if (canvas_) setCurrentWidget(canvas_);
}

void CanvasTabPage::makeTransparent()
{
    if (canvas_) return; // キャンバスにしたタブは透過タブにできない
    if (!holePage_) {
        holePage_ = new QWidget(this);
        addWidget(holePage_);
    }
    transparent_ = true;
    setCurrentWidget(holePage_);
    emit transparentGeometryChanged();
}

// 穴の位置・大きさ・表示状態が変わりうる場面をまとめてMainWindowへ通知する。
// ウィンドウのリサイズ、分割やスプリッターのドラッグ、タブの切り替え(表示/非表示)は
// すべてこの3つのイベントのどれかとして届く。
void CanvasTabPage::resizeEvent(QResizeEvent *event)
{
    QStackedWidget::resizeEvent(event);
    if (transparent_) emit transparentGeometryChanged();
}

void CanvasTabPage::showEvent(QShowEvent *event)
{
    QStackedWidget::showEvent(event);
    if (transparent_) emit transparentGeometryChanged();
}

void CanvasTabPage::hideEvent(QHideEvent *event)
{
    QStackedWidget::hideEvent(event);
    if (transparent_) emit transparentGeometryChanged();
}
