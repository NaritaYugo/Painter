#include "actions/LayerEditActions.h"
#include "actions/CanvasActionHost.h"
#include "actions/CanvasActionController.h"
#include "dialogs/BrightnessContrastPanel.h"
#include "dialogs/HueSatLightPanel.h"
#include "dialogs/ColorBalancePanel.h"
#include "dialogs/ToneCurvePanel.h"
#ifdef TIEPOLO_PRO_BUILD
#include "dialogs/GradientMapPanel.h"
#endif
#include "dialogs/SolidColorPickerPanel.h"
#include "dialogs/TextLayerPanel.h"
#include "tools/core/ToolContext.h"

#include <QWidget>
#include <QTimer>
#include <QVector2D>

// ===========================================================================
// AdjustmentLayerEditAction
// ===========================================================================
bool AdjustmentLayerEditAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return false;
    return doc->layerRef(targetLayerIndex_).layerType == LayerType::Adjustment;
}

bool AdjustmentLayerEditAction::onActivate()
{
    CanvasDocument *doc = host_.hostDocument();
    AdjustmentParams &params = doc->layerRef(targetLayerIndex_).adjustment;
    pendingKind_ = params.kind;
    backup_ = params;

    QWidget *host = host_.hostWidget();
    const int layerIndex = targetLayerIndex_; // ラムダは *this の生存を仮定してよい(パネルはアクションが所有)

    if (pendingKind_ == AdjustmentKind::BrightnessContrast) {
        if (!bcPanel_) {
            bcPanel_ = new BrightnessContrastPanel(host);
            QObject::connect(bcPanel_, &BrightnessContrastPanel::valuesChanged, host,
                [this, layerIndex](int b, int c) {
                    CanvasDocument *d = host_.hostDocument();
                    AdjustmentParams &p = d->layerRef(layerIndex).adjustment;
                    p.brightness = b;
                    p.contrast   = c;
                    host_.hostUpdate();
                });
            QObject::connect(bcPanel_, &BrightnessContrastPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(bcPanel_, &BrightnessContrastPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        bcPanel_->setValues(params.brightness, params.contrast);
        positionPanel();
        bcPanel_->show();
        bcPanel_->raise();
    } else if (pendingKind_ == AdjustmentKind::HueSaturation) {
        if (!hslPanel_) {
            hslPanel_ = new HueSatLightPanel(host);
            QObject::connect(hslPanel_, &HueSatLightPanel::valuesChanged, host,
                [this, layerIndex](int h, int s, int l) {
                    CanvasDocument *d = host_.hostDocument();
                    AdjustmentParams &p = d->layerRef(layerIndex).adjustment;
                    p.hue        = h;
                    p.saturation = s;
                    p.lightness  = l;
                    host_.hostUpdate();
                });
            QObject::connect(hslPanel_, &HueSatLightPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(hslPanel_, &HueSatLightPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        hslPanel_->setValues(params.hue, params.saturation, params.lightness);
        positionPanel();
        hslPanel_->show();
        hslPanel_->raise();
    } else if (pendingKind_ == AdjustmentKind::ColorBalance) {
        if (!cbPanel_) {
            cbPanel_ = new ColorBalancePanel(host);
            QObject::connect(cbPanel_, &ColorBalancePanel::valuesChanged, host,
                [this, layerIndex](int c, int m, int y) {
                    CanvasDocument *d = host_.hostDocument();
                    AdjustmentParams &p = d->layerRef(layerIndex).adjustment;
                    p.cyan    = c;
                    p.magenta = m;
                    p.yellow  = y;
                    host_.hostUpdate();
                });
            QObject::connect(cbPanel_, &ColorBalancePanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(cbPanel_, &ColorBalancePanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        cbPanel_->setValues(params.cyan, params.magenta, params.yellow);
        positionPanel();
        cbPanel_->show();
        cbPanel_->raise();
    } else if (pendingKind_ == AdjustmentKind::ToneCurve) {
        // トーンカーブはPro限定(host_側のeditAdjustmentLayer()で開く前にライセンス
        // 確認済み。このアクションはPro/無料どちらのビルドでも常に持つ。
        // ToneCurveEditor/ToneCurveToolのCMake配置と同じ扱い)。
        if (!tcPanel_) {
            tcPanel_ = new ToneCurvePanel(host);
            QObject::connect(tcPanel_, &ToneCurvePanel::pointsChanged, host,
                [this, layerIndex](const QVector<QPointF> &pts) {
                    CanvasDocument *d = host_.hostDocument();
                    d->layerRef(layerIndex).adjustment.curvePoints = pts;
                    host_.hostUpdate();
                });
            QObject::connect(tcPanel_, &ToneCurvePanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(tcPanel_, &ToneCurvePanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        tcPanel_->setPoints(params.curvePoints);
        positionPanel();
        tcPanel_->show();
        tcPanel_->raise();
    }
#ifdef TIEPOLO_PRO_BUILD
    else if (pendingKind_ == AdjustmentKind::GradientMap) {
        if (!gmPanel_) {
            gmPanel_ = new GradientMapPanel(host);
            QObject::connect(gmPanel_, &GradientMapPanel::stopsChanged, host,
                [this, layerIndex](const QVector<GradientStripEditor::Stop> &stops) {
                    CanvasDocument *d = host_.hostDocument();
                    QVector<AdjustmentGradientStop> converted;
                    converted.reserve(stops.size());
                    for (const auto &s : stops)
                        converted.push_back({ s.pos, s.color });
                    d->layerRef(layerIndex).adjustment.gradientStops = converted;
                    host_.hostUpdate();
                });
            QObject::connect(gmPanel_, &GradientMapPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
            QObject::connect(gmPanel_, &GradientMapPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
        }
        QVector<GradientStripEditor::Stop> stops;
        stops.reserve(params.gradientStops.size());
        for (const auto &s : params.gradientStops)
            stops.push_back({ s.pos, s.color });
        gmPanel_->setStops(stops);
        positionPanel();
        gmPanel_->show();
        gmPanel_->raise();
    }
#endif
    return true;
}

void AdjustmentLayerEditAction::onConfirm()
{
    if (bcPanel_)  bcPanel_->hide();
    if (hslPanel_) hslPanel_->hide();
    if (cbPanel_)  cbPanel_->hide();
    if (tcPanel_)  tcPanel_->hide();
#ifdef TIEPOLO_PRO_BUILD
    if (gmPanel_)  gmPanel_->hide();
#endif
    targetLayerIndex_ = -1;
    host_.hostNotifyLayersChanged(); // 最終状態でLayerDockのサムネイル等を確定させる
}

void AdjustmentLayerEditAction::onCancel()
{
    if (targetLayerIndex_ >= 0)
        host_.hostDocument()->layerRef(targetLayerIndex_).adjustment = backup_; // 編集開始前の値に戻す
    if (bcPanel_)  bcPanel_->hide();
    if (hslPanel_) hslPanel_->hide();
    if (cbPanel_)  cbPanel_->hide();
    if (tcPanel_)  tcPanel_->hide();
#ifdef TIEPOLO_PRO_BUILD
    if (gmPanel_)  gmPanel_->hide();
#endif
    targetLayerIndex_ = -1;
    host_.hostNotifyLayersChanged();
}

void AdjustmentLayerEditAction::positionPanel()
{
    QWidget *panel = nullptr;
    switch (pendingKind_) {
    case AdjustmentKind::BrightnessContrast: panel = bcPanel_;  break;
    case AdjustmentKind::HueSaturation:      panel = hslPanel_; break;
    case AdjustmentKind::ColorBalance:       panel = cbPanel_;  break;
    case AdjustmentKind::ToneCurve:          panel = tcPanel_;  break;
    case AdjustmentKind::GradientMap:
#ifdef TIEPOLO_PRO_BUILD
        panel = gmPanel_;
#endif
        break;
    }
    centerPanelTop(panel);
}

// ===========================================================================
// SolidColorLayerEditAction
// ===========================================================================
bool SolidColorLayerEditAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return false;
    return doc->layerRef(targetLayerIndex_).layerType == LayerType::SolidColor;
}

bool SolidColorLayerEditAction::onActivate()
{
    CanvasDocument *doc = host_.hostDocument();
    backup_ = doc->layerRef(targetLayerIndex_).solidColor;

    QWidget *host = host_.hostWidget();
    const int layerIndex = targetLayerIndex_;

    if (!panel_) {
        panel_ = new SolidColorPickerPanel(host);
        QObject::connect(panel_, &SolidColorPickerPanel::colorChanged, host,
            [this, layerIndex](const QColor &c) {
                host_.hostDocument()->layerRef(layerIndex).solidColor = c;
                host_.hostUpdate();
            });
        QObject::connect(panel_, &SolidColorPickerPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &SolidColorPickerPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setColor(backup_);
    positionPanel();
    panel_->show();
    panel_->raise();
    return true;
}

void SolidColorLayerEditAction::onConfirm()
{
    if (panel_) panel_->hide();
    targetLayerIndex_ = -1;
    host_.hostNotifyLayersChanged();
}

void SolidColorLayerEditAction::onCancel()
{
    if (targetLayerIndex_ >= 0)
        host_.hostDocument()->layerRef(targetLayerIndex_).solidColor = backup_; // 編集開始前の色に戻す
    if (panel_) panel_->hide();
    targetLayerIndex_ = -1;
    host_.hostNotifyLayersChanged();
}

void SolidColorLayerEditAction::positionPanel()
{
    centerPanelTop(panel_);
}

// ===========================================================================
// TextBoxEditAction
// ===========================================================================
// 確定/キャンセルいずれの後処理でも共通: 文字列が空のボックスは削除する。
static void pruneTextBoxIfEmpty(CanvasDocument *doc, int layerIndex, int boxIndex)
{
    if (!doc || layerIndex < 0 || layerIndex >= doc->layerCount()) return;
    Layer &layer = doc->layerRef(layerIndex);
    if (boxIndex < 0 || boxIndex >= (int)layer.textBoxes.size()) return;
    if (layer.textBoxes[boxIndex].text.trimmed().isEmpty())
        layer.textBoxes.erase(layer.textBoxes.begin() + boxIndex);
}

bool TextBoxEditAction::canActivate() const
{
    CanvasDocument *doc = host_.hostDocument();
    if (!doc || targetLayerIndex_ < 0 || targetLayerIndex_ >= doc->layerCount()) return false;
    const Layer &layer = doc->layerRef(targetLayerIndex_);
    if (layer.layerType != LayerType::Text) return false;
    return targetBoxIndex_ >= 0 && targetBoxIndex_ < (int)layer.textBoxes.size();
}

bool TextBoxEditAction::onActivate()
{
    CanvasDocument *doc = host_.hostDocument();
    Layer &layer = doc->layerRef(targetLayerIndex_);
    backup_ = layer.textBoxes[targetBoxIndex_];

    QWidget *host = host_.hostWidget();
    if (!panel_) {
        panel_ = new TextLayerPanel(host);
        QObject::connect(panel_, &TextLayerPanel::valuesChanged, host,
            [this](const TextParams &p) {
                CanvasDocument *d = host_.hostDocument();
                if (targetLayerIndex_ < 0 || targetBoxIndex_ < 0) return;
                Layer &l = d->layerRef(targetLayerIndex_);
                if (targetBoxIndex_ >= (int)l.textBoxes.size()) return;
                TextParams &box = l.textBoxes[targetBoxIndex_];
                // 位置/サイズ/回転/スケールはパネルが扱わないフィールドなので保持する
                const float cx = box.cx, cy = box.cy, w = box.width, h = box.height;
                const float rot = box.rotation, sc = box.scale;
                box = p;
                box.cx = cx; box.cy = cy; box.width = w; box.height = h;
                box.rotation = rot; box.scale = sc;
                scheduleRasterize();
            });
        QObject::connect(panel_, &TextLayerPanel::confirmed, host, [this] { ctrl_.confirmActive(); });
        QObject::connect(panel_, &TextLayerPanel::cancelled, host, [this] { ctrl_.cancelActive(); });
    }
    panel_->setValues(layer.textBoxes[targetBoxIndex_]);
    positionPanel();
    panel_->show();
    panel_->raise();
    panel_->setFocus();
    return true;
}

void TextBoxEditAction::onConfirm()
{
    if (rasterizeTimer_ && rasterizeTimer_->isActive()) {
        // デバウンス待ち(=最後のキー入力がまだ焼き込まれていない)状態のまま
        // 確定された場合は、ここで確実に最新の内容を焼き込む。
        rasterizeTimer_->stop();
        if (targetLayerIndex_ >= 0) host_.hostRasterizeTextLayer(targetLayerIndex_);
    }
    pruneTextBoxIfEmpty(host_.hostDocument(), targetLayerIndex_, targetBoxIndex_);
    if (panel_) panel_->hide();
    targetLayerIndex_ = -1;
    targetBoxIndex_   = -1;
    host_.hostNotifyLayersChanged();
}

void TextBoxEditAction::onCancel()
{
    if (rasterizeTimer_) rasterizeTimer_->stop();
    CanvasDocument *doc = host_.hostDocument();
    if (targetLayerIndex_ >= 0 && targetBoxIndex_ >= 0) {
        Layer &l = doc->layerRef(targetLayerIndex_);
        if (targetBoxIndex_ < (int)l.textBoxes.size())
            l.textBoxes[targetBoxIndex_] = backup_;
        host_.hostRasterizeTextLayer(targetLayerIndex_);
    }
    pruneTextBoxIfEmpty(doc, targetLayerIndex_, targetBoxIndex_);
    if (panel_) panel_->hide();
    targetLayerIndex_ = -1;
    targetBoxIndex_   = -1;
}

void TextBoxEditAction::positionPanel()
{
    if (!panel_) return;
    QWidget *host = host_.hostWidget();
    const QSize sz = panel_->sizeHint();
    QPoint anchor((host->width() - sz.width()) / 2, 20);
    CanvasDocument *doc = host_.hostDocument();
    if (doc && targetLayerIndex_ >= 0 && targetLayerIndex_ < doc->layerCount() && targetBoxIndex_ >= 0) {
        const Layer &layer = doc->layerRef(targetLayerIndex_);
        if (targetBoxIndex_ < (int)layer.textBoxes.size()) {
            const TextParams &box = layer.textBoxes[targetBoxIndex_];
            const ToolContext &ctx = host_.hostToolContext();
            const QPointF topLeft = ctx.pixelToWidget(QVector2D(box.cx - box.width * box.scale / 2.0f,
                                                                   box.cy - box.height * box.scale / 2.0f));
            anchor = QPoint(qRound(topLeft.x()), qRound(topLeft.y()) - sz.height() - 8);
        }
    }
    const QPoint gp = host->mapToGlobal(anchor);
    panel_->setGeometry(gp.x(), gp.y(), sz.width(), sz.height());
}

void TextBoxEditAction::scheduleRasterize()
{
    if (!rasterizeTimer_) {
        rasterizeTimer_ = new QTimer(host_.hostWidget());
        rasterizeTimer_->setSingleShot(true);
        QObject::connect(rasterizeTimer_, &QTimer::timeout, host_.hostWidget(), [this]() {
            if (targetLayerIndex_ < 0) return;
            host_.hostRasterizeTextLayer(targetLayerIndex_);
            host_.hostUpdate();
        });
    }
    rasterizeTimer_->start(150);
}
