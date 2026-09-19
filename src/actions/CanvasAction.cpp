#include "actions/CanvasAction.h"
#include "actions/CanvasActionHost.h"

#include <QWidget>

void CanvasAction::centerPanelTop(QWidget *panel)
{
    if (!panel) return;
    QWidget *host = host_.hostWidget();
    const QSize sz = panel->sizeHint();
    const QPoint gp = host->mapToGlobal(QPoint((host->width() - sz.width()) / 2, 20));
    panel->setGeometry(gp.x(), gp.y(), sz.width(), sz.height());
}

bool CanvasAction::begin()
{
    if (active_) return false;
    if (!canActivate()) return false;
    host_.hostMakeCurrent();
    if (!onActivate()) return false; // ツールが engage できなかった(0x0レイヤー等)
    active_ = true;
    host_.hostUpdateCursor();
    host_.hostUpdate();
    return true;
}

void CanvasAction::confirm()
{
    if (!active_) return;
    host_.hostMakeCurrent(); // 確定はタイルへ焼き込む(GL 書き込み)ため必須
    onConfirm();
    active_ = false;
    host_.hostUpdateCursor();
    host_.hostUpdate();
}

void CanvasAction::cancel()
{
    if (!active_) return;
    onCancel();
    active_ = false;
    host_.hostUpdateCursor();
    host_.hostUpdate();
}
