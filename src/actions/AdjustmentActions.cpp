#include "actions/AdjustmentActions.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "tools/core/ToolContext.h"
#include "document/CanvasDocument.h"

#include "dialogs/HueSatLightPanel.h"
#include "dialogs/BrightnessContrastPanel.h"
#include "dialogs/ColorBalancePanel.h"
#include "dialogs/ToneCurvePanel.h"

#include <QOpenGLShaderProgram>
#include <QWidget>
#include <QVector>
#include <QPointF>

// ===========================================================================
// 色相・彩度・明度
// ===========================================================================
bool HueSatLightAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    return doc && !doc->layers.isEmpty();
}

bool HueSatLightAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new HueSatLightPanel(host);
        QObject::connect(panel_, &HueSatLightPanel::valuesChanged, host, [this](int h, int s, int l) {
            tool_.setHue(h);
            tool_.setSaturation(s);
            tool_.setLightness(l);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &HueSatLightPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &HueSatLightPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->resetValues();
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void HueSatLightAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void HueSatLightAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void HueSatLightAction::positionPanel() { centerPanelTop(panel_); }

void HueSatLightAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsHueSatLightTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uHueShift",   (float)tool_.hue());
        p->setUniformValue("uSatShift",   tool_.saturation() / 100.0f);
        p->setUniformValue("uLightShift", tool_.lightness() / 100.0f);
    }
}

// ===========================================================================
// 明るさ・コントラスト
// ===========================================================================
bool BrightnessContrastAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    return doc && !doc->layers.isEmpty();
}

bool BrightnessContrastAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new BrightnessContrastPanel(host);
        QObject::connect(panel_, &BrightnessContrastPanel::valuesChanged, host, [this](int b, int c) {
            tool_.setBrightness(b);
            tool_.setContrast(c);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &BrightnessContrastPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &BrightnessContrastPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->resetValues();
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void BrightnessContrastAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void BrightnessContrastAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void BrightnessContrastAction::positionPanel() { centerPanelTop(panel_); }

void BrightnessContrastAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsBrightnessContrastTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uBrightnessShift", tool_.brightness() / 200.0f);
        p->setUniformValue("uContrastFactor",  1.0f + tool_.contrast() / 100.0f);
    }
}

// ===========================================================================
// カラーバランス
// ===========================================================================
bool ColorBalanceAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    return doc && !doc->layers.isEmpty();
}

bool ColorBalanceAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new ColorBalancePanel(host);
        QObject::connect(panel_, &ColorBalancePanel::valuesChanged, host, [this](int c, int m, int y) {
            tool_.setCyan(c);
            tool_.setMagenta(m);
            tool_.setYellow(y);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &ColorBalancePanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &ColorBalancePanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->resetValues();
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void ColorBalanceAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void ColorBalanceAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void ColorBalanceAction::positionPanel() { centerPanelTop(panel_); }

void ColorBalanceAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsColorBalanceTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uCyanShift",    tool_.cyan()    / 100.0f);
        p->setUniformValue("uMagentaShift", tool_.magenta() / 100.0f);
        p->setUniformValue("uYellowShift",  tool_.yellow()  / 100.0f);
    }
}

// ===========================================================================
// トーンカーブ
// ===========================================================================
bool ToneCurveAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || doc->layers.isEmpty()) return false;
    const LayerType lt = doc->activeLayer().layerType;
    return lt == LayerType::Normal || lt == LayerType::Text;
}

bool ToneCurveAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new ToneCurvePanel(host);
        QObject::connect(panel_, &ToneCurvePanel::pointsChanged, host, [this](const QVector<QPointF> &points) {
            host_.hostMakeCurrent();
            tool_.setControlPoints(host_.hostToolContext(), points);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &ToneCurvePanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &ToneCurvePanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->resetValues();
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void ToneCurveAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void ToneCurveAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void ToneCurveAction::positionPanel() { centerPanelTop(panel_); }

void ToneCurveAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsToneCurveTool", on ? 1 : 0);
}
