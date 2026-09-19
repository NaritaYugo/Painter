#include "actions/FilterLayerEditAction.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "dialogs/GaussianBlurPanel.h"
#include "dialogs/MotionBlurPanel.h"
#include "dialogs/MosaicPanel.h"
#include "dialogs/NoisePanel.h"
#ifdef TIEPOLO_PRO_BUILD
#include "dialogs/ChromaticAberrationPanel.h"
#include "dialogs/LensBlurPanel.h"
#endif
#include "tools/core/ToolContext.h"

#include <QWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QLineF>

bool FilterLayerEditAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return false;
    return doc->layerRef(targetLayerIndex_).layerType == LayerType::Filter;
}

bool FilterLayerEditAction::onActivate()
{
    CanvasDocument *doc = host_.hostDocument();
    FilterParams &params = doc->layerRef(targetLayerIndex_).filter;
    backup_ = params;
    draggingCenter_ = false;

    QWidget *host = host_.hostWidget();
    const int layerIndex = targetLayerIndex_; // パネルはアクションが所有するので *this の生存を仮定してよい

    if (params.kind == FilterKind::GaussianBlur) {
        if (!blurPanel_) {
            blurPanel_ = new GaussianBlurPanel(host);
            QObject::connect(blurPanel_, &GaussianBlurPanel::radiusChanged, host,
                [this, layerIndex](int radius) {
                    host_.hostDocument()->layerRef(layerIndex).filter.blurRadiusPx = (float)radius;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(blurPanel_, &GaussianBlurPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(blurPanel_, &GaussianBlurPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        blurPanel_->setRadius((int)qRound(params.blurRadiusPx));
        positionPanel();
        blurPanel_->show();
        blurPanel_->raise();
        return true;
    }

    if (params.kind == FilterKind::MotionBlur) {
        if (!motionBlurPanel_) {
            motionBlurPanel_ = new MotionBlurPanel(host);
            QObject::connect(motionBlurPanel_, &MotionBlurPanel::modeChanged, host,
                [this, layerIndex](int mode) {
                    host_.hostDocument()->layerRef(layerIndex).filter.mbMode = mode;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(motionBlurPanel_, &MotionBlurPanel::angleChanged, host,
                [this, layerIndex](float deg) {
                    host_.hostDocument()->layerRef(layerIndex).filter.mbAngleDeg = deg;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(motionBlurPanel_, &MotionBlurPanel::distanceChanged, host,
                [this, layerIndex](float px) {
                    host_.hostDocument()->layerRef(layerIndex).filter.mbDistancePx = px;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(motionBlurPanel_, &MotionBlurPanel::angleSpanChanged, host,
                [this, layerIndex](float deg) {
                    host_.hostDocument()->layerRef(layerIndex).filter.mbAngleSpanDeg = deg;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(motionBlurPanel_, &MotionBlurPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(motionBlurPanel_, &MotionBlurPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        motionBlurPanel_->setMode(params.mbMode);
        motionBlurPanel_->setAngleDeg(params.mbAngleDeg);
        motionBlurPanel_->setDistancePx(params.mbDistancePx);
        motionBlurPanel_->setAngleSpanDeg(params.mbAngleSpanDeg);
        positionPanel();
        motionBlurPanel_->show();
        motionBlurPanel_->raise();
        return true;
    }

    if (params.kind == FilterKind::Mosaic) {
        if (!mosaicPanel_) {
            mosaicPanel_ = new MosaicPanel(host);
            QObject::connect(mosaicPanel_, &MosaicPanel::blockSizeChanged, host,
                [this, layerIndex](int blockSize) {
                    host_.hostDocument()->layerRef(layerIndex).filter.mzBlockSize = blockSize;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(mosaicPanel_, &MosaicPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(mosaicPanel_, &MosaicPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        mosaicPanel_->setBlockSize(params.mzBlockSize);
        positionPanel();
        mosaicPanel_->show();
        mosaicPanel_->raise();
        return true;
    }

    if (params.kind == FilterKind::Noise) {
        if (!noisePanel_) {
            noisePanel_ = new NoisePanel(host);
            QObject::connect(noisePanel_, &NoisePanel::strengthChanged, host,
                [this, layerIndex](float strength01) {
                    host_.hostDocument()->layerRef(layerIndex).filter.nsStrength = strength01;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(noisePanel_, &NoisePanel::monochromeChanged, host,
                [this, layerIndex](bool mono) {
                    host_.hostDocument()->layerRef(layerIndex).filter.nsMonochrome = mono;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(noisePanel_, &NoisePanel::grainChanged, host,
                [this, layerIndex](float grainPx) {
                    host_.hostDocument()->layerRef(layerIndex).filter.nsGrainPx = grainPx;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(noisePanel_, &NoisePanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(noisePanel_, &NoisePanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        noisePanel_->setStrength(params.nsStrength);
        noisePanel_->setMonochrome(params.nsMonochrome);
        noisePanel_->setGrainPx(params.nsGrainPx);
        positionPanel();
        noisePanel_->show();
        noisePanel_->raise();
        return true;
    }

#ifdef TIEPOLO_PRO_BUILD
    if (params.kind == FilterKind::ChromaticAberration) {
        if (!caPanel_) {
            caPanel_ = new ChromaticAberrationPanel(host);
            QObject::connect(caPanel_, &ChromaticAberrationPanel::modeChanged, host,
                [this, layerIndex](int mode) {
                    host_.hostDocument()->layerRef(layerIndex).filter.caMode = mode;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(caPanel_, &ChromaticAberrationPanel::angleChanged, host,
                [this, layerIndex](float deg) {
                    host_.hostDocument()->layerRef(layerIndex).filter.caAngleDeg = deg;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(caPanel_, &ChromaticAberrationPanel::distanceChanged, host,
                [this, layerIndex](float px) {
                    host_.hostDocument()->layerRef(layerIndex).filter.caDistancePx = px;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(caPanel_, &ChromaticAberrationPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(caPanel_, &ChromaticAberrationPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        caPanel_->setMode(params.caMode);
        caPanel_->setAngleDeg(params.caAngleDeg);
        caPanel_->setDistancePx(params.caDistancePx);
        positionPanel();
        caPanel_->show();
        caPanel_->raise();
        return true;
    }

    if (params.kind == FilterKind::LensBlur) {
        if (!lensBlurPanel_) {
            lensBlurPanel_ = new LensBlurPanel(host);
            QObject::connect(lensBlurPanel_, &LensBlurPanel::radiusChanged, host,
                [this, layerIndex](float px) {
                    host_.hostDocument()->layerRef(layerIndex).filter.lbRadiusPx = px;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(lensBlurPanel_, &LensBlurPanel::bladesChanged, host,
                [this, layerIndex](int blades) {
                    host_.hostDocument()->layerRef(layerIndex).filter.lbBlades = blades;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(lensBlurPanel_, &LensBlurPanel::bladeRotChanged, host,
                [this, layerIndex](float deg) {
                    host_.hostDocument()->layerRef(layerIndex).filter.lbBladeRotDeg = deg;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(lensBlurPanel_, &LensBlurPanel::highlightBoostChanged, host,
                [this, layerIndex](float boost) {
                    host_.hostDocument()->layerRef(layerIndex).filter.lbHighlightBoost = boost;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(lensBlurPanel_, &LensBlurPanel::thresholdChanged, host,
                [this, layerIndex](float threshold) {
                    host_.hostDocument()->layerRef(layerIndex).filter.lbThreshold = threshold;
                    host_.hostInvalidateFilterChain();
                    host_.hostUpdate();
                });
            QObject::connect(lensBlurPanel_, &LensBlurPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(lensBlurPanel_, &LensBlurPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        lensBlurPanel_->setRadiusPx(params.lbRadiusPx);
        lensBlurPanel_->setBlades(params.lbBlades);
        lensBlurPanel_->setBladeRotDeg(params.lbBladeRotDeg);
        lensBlurPanel_->setHighlightBoost(params.lbHighlightBoost);
        lensBlurPanel_->setThreshold(params.lbThreshold);
        positionPanel();
        lensBlurPanel_->show();
        lensBlurPanel_->raise();
        return true;
    }
#endif

    // 未知の種類(将来追加される可能性)、またはPro限定の種類を無料版ビルドで
    // 開こうとした場合。呼び出し側(GLWidget::editFilterLayer)が事前に弾いている
    // はずなので、ここに来るのは想定外のケースだけ。
    return false;
}

void FilterLayerEditAction::onConfirm()
{
    if (blurPanel_) blurPanel_->hide();
    if (motionBlurPanel_) motionBlurPanel_->hide();
    if (mosaicPanel_) mosaicPanel_->hide();
    if (noisePanel_) noisePanel_->hide();
#ifdef TIEPOLO_PRO_BUILD
    if (caPanel_) caPanel_->hide();
    if (lensBlurPanel_) lensBlurPanel_->hide();
#endif
    targetLayerIndex_ = -1;
    draggingCenter_ = false;
    host_.hostInvalidateFilterChain();
    host_.hostNotifyLayersChanged();
}

void FilterLayerEditAction::onCancel()
{
    if (targetLayerIndex_ >= 0)
        host_.hostDocument()->layerRef(targetLayerIndex_).filter = backup_; // 編集開始前の値に戻す
    if (blurPanel_) blurPanel_->hide();
    if (motionBlurPanel_) motionBlurPanel_->hide();
    if (mosaicPanel_) mosaicPanel_->hide();
    if (noisePanel_) noisePanel_->hide();
#ifdef TIEPOLO_PRO_BUILD
    if (caPanel_) caPanel_->hide();
    if (lensBlurPanel_) lensBlurPanel_->hide();
#endif
    targetLayerIndex_ = -1;
    draggingCenter_ = false;
    host_.hostInvalidateFilterChain();
    host_.hostNotifyLayersChanged();
}

void FilterLayerEditAction::positionPanel()
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return;
    const FilterKind kind = doc->layerRef(targetLayerIndex_).filter.kind;
    if (kind == FilterKind::GaussianBlur) {
        centerPanelTop(blurPanel_);
        return;
    }
    if (kind == FilterKind::MotionBlur) {
        centerPanelTop(motionBlurPanel_);
        return;
    }
    if (kind == FilterKind::Mosaic) {
        centerPanelTop(mosaicPanel_);
        return;
    }
    if (kind == FilterKind::Noise) {
        centerPanelTop(noisePanel_);
        return;
    }
#ifdef TIEPOLO_PRO_BUILD
    if (kind == FilterKind::ChromaticAberration) {
        centerPanelTop(caPanel_);
        return;
    }
    if (kind == FilterKind::LensBlur) {
        centerPanelTop(lensBlurPanel_);
        return;
    }
#endif
}

// 円形/放射モードの中心ハンドルを持つ種類かどうか(色収差の円形モード、
// 移動ぼかしの円形モード)。円板半径・振れ角の類はパネル側のスライダーで
// 済むので、ハンドルが要るのは「中心座標」を持つこの2種類だけ。
static bool filterKindHasCenterHandle(const FilterParams &p)
{
#ifdef TIEPOLO_PRO_BUILD
    if (p.kind == FilterKind::ChromaticAberration && p.caMode == 1) return true;
#endif
    if (p.kind == FilterKind::MotionBlur && p.mbMode == 1) return true;
    return false;
}

QVector2D FilterLayerEditAction::centerCanvasPx(const ToolContext &ctx) const
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount())
        return QVector2D(0.0f, 0.0f);
    const FilterParams &p = doc->layerRef(targetLayerIndex_).filter;
#ifdef TIEPOLO_PRO_BUILD
    if (p.kind == FilterKind::ChromaticAberration)
        return QVector2D(p.caCenterU * ctx.canvasW, p.caCenterV * ctx.canvasH);
#endif
    if (p.kind == FilterKind::MotionBlur)
        return QVector2D(p.mbCenterU * ctx.canvasW, p.mbCenterV * ctx.canvasH);
    return QVector2D(0.0f, 0.0f);
}

bool FilterLayerEditAction::handleMousePress(QMouseEvent *e, ToolContext &ctx)
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return false;
    if (!filterKindHasCenterHandle(doc->layerRef(targetLayerIndex_).filter)) return false;
    if (e->button() != Qt::LeftButton || !ctx.pixelToWidget) return false;

    const QPointF handleWidget = ctx.pixelToWidget(centerCanvasPx(ctx));
    constexpr double kHitRadiusWidget = 12.0;
    if (QLineF(handleWidget, e->position()).length() > kHitRadiusWidget) return false;

    draggingCenter_ = true;
    return true;
}

bool FilterLayerEditAction::handleMouseMove(QMouseEvent *e, ToolContext &ctx)
{
    if (!draggingCenter_ || !ctx.widgetToPixel) return false;
    CanvasDocument *doc = host_.hostDocument();
    if (targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return false;

    const QVector2D canvasPx = ctx.widgetToPixel(e->position());
    FilterParams &p = doc->layerRef(targetLayerIndex_).filter;
    // キャンバスに対する比率で持つ(キャンバスサイズ変更をまたいでも相対位置が保たれる)
    const float u = (ctx.canvasW > 0) ? float(canvasPx.x()) / ctx.canvasW : 0.5f;
    const float v = (ctx.canvasH > 0) ? float(canvasPx.y()) / ctx.canvasH : 0.5f;
#ifdef TIEPOLO_PRO_BUILD
    if (p.kind == FilterKind::ChromaticAberration) { p.caCenterU = u; p.caCenterV = v; }
#endif
    if (p.kind == FilterKind::MotionBlur) { p.mbCenterU = u; p.mbCenterV = v; }
    host_.hostInvalidateFilterChain();
    host_.hostUpdate();
    return true;
}

bool FilterLayerEditAction::handleMouseRelease(QMouseEvent *e, ToolContext &ctx)
{
    Q_UNUSED(e); Q_UNUSED(ctx);
    if (!draggingCenter_) return false;
    draggingCenter_ = false;
    return true;
}

bool FilterLayerEditAction::paintOverlay(QPainter &painter, const ToolContext &ctx)
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return false;
    if (!filterKindHasCenterHandle(doc->layerRef(targetLayerIndex_).filter)) return false;
    if (!ctx.pixelToWidget) return false;

    // 破壊的フィルター版(ChromaticAberrationTool/MotionBlurTool::paintOverlay)と
    // 同じ見た目のハンドル
    const QPointF p = ctx.pixelToWidget(centerCanvasPx(ctx));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(255, 170, 40, 230), 1.5));
    painter.setBrush(QColor(255, 255, 255, 230));
    painter.drawEllipse(p, 7, 7);
    painter.drawLine(QPointF(p.x() - 11, p.y()), QPointF(p.x() + 11, p.y()));
    painter.drawLine(QPointF(p.x(), p.y() - 11), QPointF(p.x(), p.y() + 11));
    return true;
}
