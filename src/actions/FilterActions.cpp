#include "actions/FilterActions.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "tools/core/ToolContext.h"
#include "document/CanvasDocument.h"

#include "dialogs/GaussianBlurPanel.h"
#include "dialogs/MosaicPanel.h"
#include "dialogs/NoisePanel.h"
#include "dialogs/CustomShaderPanel.h"

#include <QOpenGLShaderProgram>
#include <QWidget>
#include <QString>

namespace {
// フィルターは実ピクセルを持つレイヤー(Normal/Text)のみ対象。
bool activeLayerIsPixelLayer(CanvasDocument *doc)
{
    if (!doc || doc->layers.isEmpty()) return false;
    const LayerType lt = doc->activeLayer().layerType;
    return lt == LayerType::Normal || lt == LayerType::Text;
}
} // namespace

// ===========================================================================
// ガウスぼかし
// ===========================================================================
bool GaussianBlurAction::canActivate() const { return activeLayerIsPixelLayer(host_.hostDocument()); }

bool GaussianBlurAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false; // レイヤーが0x0など(通常起こらない)
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new GaussianBlurPanel(host);
        QObject::connect(panel_, &GaussianBlurPanel::radiusChanged, host, [this](int r) {
            tool_.setRadius(host_.hostToolContext(), r);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &GaussianBlurPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &GaussianBlurPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setRadius(tool_.radius());
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void GaussianBlurAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void GaussianBlurAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void GaussianBlurAction::positionPanel() { centerPanelTop(panel_); }

void GaussianBlurAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsGaussianBlurTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uGaussianBlurOriginPx", tool_.previewOriginPx());
        p->setUniformValue("uGaussianBlurSizePx",   tool_.previewSizePx());
    }
}

// ===========================================================================
// モザイク
// ===========================================================================
bool MosaicAction::canActivate() const { return activeLayerIsPixelLayer(host_.hostDocument()); }

bool MosaicAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false;
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new MosaicPanel(host);
        QObject::connect(panel_, &MosaicPanel::blockSizeChanged, host, [this](int s) {
            tool_.setBlockSize(host_.hostToolContext(), s);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &MosaicPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &MosaicPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setBlockSize(tool_.blockSize());
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void MosaicAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void MosaicAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void MosaicAction::positionPanel() { centerPanelTop(panel_); }

void MosaicAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsMosaicTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uMosaicOriginPx", tool_.previewOriginPx());
        p->setUniformValue("uMosaicSizePx",   tool_.previewSizePx());
    }
}

// ===========================================================================
// ノイズ
// ===========================================================================
bool NoiseAction::canActivate() const { return activeLayerIsPixelLayer(host_.hostDocument()); }

bool NoiseAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false;
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new NoisePanel(host);
        QObject::connect(panel_, &NoisePanel::strengthChanged, host, [this](float v) {
            tool_.setStrength(host_.hostToolContext(), v);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &NoisePanel::monochromeChanged, host, [this](bool mono) {
            tool_.setMonochrome(host_.hostToolContext(), mono);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &NoisePanel::grainChanged, host, [this](float g) {
            tool_.setGrainPx(host_.hostToolContext(), g);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &NoisePanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &NoisePanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setStrength(tool_.strength());
    panel_->setMonochrome(tool_.monochrome());
    panel_->setGrainPx(tool_.grainPx());
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void NoiseAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void NoiseAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void NoiseAction::positionPanel() { centerPanelTop(panel_); }

void NoiseAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsNoiseTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uNoiseOriginPx", tool_.previewOriginPx());
        p->setUniformValue("uNoiseSizePx",   tool_.previewSizePx());
    }
}

// ===========================================================================
// カスタムシェーダー
// ===========================================================================
bool CustomShaderAction::canActivate() const { return activeLayerIsPixelLayer(host_.hostDocument()); }

bool CustomShaderAction::onActivate()
{
    ToolContext &ctx = host_.hostToolContext();
    tool_.activate(ctx);
    if (!tool_.engaged()) return false;
    if (!panel_) {
        QWidget *host = host_.hostWidget();
        panel_ = new CustomShaderPanel(host);
        QObject::connect(panel_, &CustomShaderPanel::sourceChanged, host, [this](const QString &src) {
            host_.hostMakeCurrent();
            const QString err = tool_.setSource(host_.hostToolContext(), src);
            if (panel_) panel_->setError(err);
            host_.hostUpdate();
        });
        QObject::connect(panel_, &CustomShaderPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &CustomShaderPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setSource(tool_.source());
    panel_->setError(QString());
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void CustomShaderAction::onConfirm()
{
    tool_.confirm(host_.hostToolContext());
    if (panel_) panel_->hide();
}

void CustomShaderAction::onCancel()
{
    tool_.deactivate();
    if (panel_) panel_->hide();
}

void CustomShaderAction::positionPanel() { centerPanelTop(panel_); }

void CustomShaderAction::applyRenderState(QOpenGLShaderProgram *p)
{
    const bool on = isActive() && tool_.engaged();
    p->setUniformValue("uIsCustomShaderTool", on ? 1 : 0);
    if (on) {
        p->setUniformValue("uCustomShaderOriginPx", tool_.previewOriginPx());
        p->setUniformValue("uCustomShaderSizePx",   tool_.previewSizePx());
    }
}
