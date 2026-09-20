#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>

#include "canvas/CanvasWidget.h"
#include "app/NativeWindowLog.h" // 診断ログ(TIEPOLO_WINLOG)。初期化時間の計測に使う
#include "rendering/ShaderCache.h"
#include "tools/core/ToolRegistry.h"
#include "tools/core/CursorUtils.h"
#include "tools/core/MaskBrush.h"
#include "dialogs/ProFeatureDialog.h"
#include "components/ThemeColors.h"
#include "actions/AdjustmentActions.h"
#include "actions/FilterActions.h"
#include "actions/MotionBlurAction.h"
#include "actions/TransformActions.h"
#include "actions/CanvasSizeActions.h"
#include "actions/LayerEditActions.h"
#include "actions/FilterLayerEditAction.h"
#ifdef TIEPOLO_PRO_BUILD
#include "licensing/LicenseManager.h"
#include "actions/ChromaticAberrationAction.h"
#include "actions/LensBlurAction.h"
#include "actions/GradientMapAction.h"
#endif
#include <QTimer>
#include <QMouseEvent>
#include <QDebug>
#include <QFile>
#include <QVector3D>
#include <QQueue>
#include <QStack>
#include <QPainter>
#include <QPainterPath>
#include <QBitmap>
#include <QGuiApplication>
#include <QSurfaceFormat>
#include <QWindow>
#include <QClipboard>
#include <QMimeData>
#include <cmath>

// Canvas-action registration and public action entry points.

void CanvasWidget::startTransformAction()
{
    cancelNonControllerActions();
    actions_.start(transformAction_);
}

void CanvasWidget::startFreeTransformAction()
{
    cancelNonControllerActions();
    actions_.start(freeTransformAction_);
}

// ===========================================================================
// 色相・彩度・明度(アクション)
// ---------------------------------------------------------------------------
// Transform/FreeTransformと違いキャンバス上のドラッグ操作は無く、CanvasWidgetの
// 子ウィジェットとして浮かせたHueSatLightPanel(実際のQSlider3本)から値を受け取る。
// パネル自体がクリックを受け取るため、実行中はCanvasWidgetのマウスイベントを
// (パネル外へのクリックも)すべて無視し、activeToolへの委譲も止める。
// ===========================================================================
// ===========================================================================
// 色調整/フィルター系アクションの登録と入口(フォワーダ)
// ---------------------------------------------------------------------------
// 各アクションの中身(ツール activate/confirm、パネル生成・配線、描画 uniform 等)は
// src/actions/ の CanvasAction サブクラスへ移動した。ここに残るのは:
//   - registerCanvasActions(): controller へアクションを登録(コンストラクタから)
//   - cancelNonControllerActions(): まだ CanvasWidget 側に残る変形/キャンバス/レイヤー編集
//     アクションを cancel する(controller 管理アクションとの相互排他のため)
//   - startXxxAction(): メニュー/ショートカットの入口。上記で他アクションを畳んでから
//     controller.start() するだけ。
// ===========================================================================
void CanvasWidget::registerCanvasActions()
{
    hueSatLightAction_        = actions_.add<HueSatLightAction>();
    brightnessContrastAction_ = actions_.add<BrightnessContrastAction>();
    colorBalanceAction_       = actions_.add<ColorBalanceAction>();
    toneCurveAction_          = actions_.add<ToneCurveAction>();
    gaussianBlurAction_       = actions_.add<GaussianBlurAction>();
    customShaderAction_       = actions_.add<CustomShaderAction>();
    mosaicAction_             = actions_.add<MosaicAction>();
    motionBlurAction_         = actions_.add<MotionBlurAction>();
    noiseAction_              = actions_.add<NoiseAction>();
    transformAction_          = actions_.add<TransformAction>();
    freeTransformAction_      = actions_.add<FreeTransformAction>();
    canvasSizeAction_         = actions_.add<CanvasSizeAction>();
    imageResolutionAction_    = actions_.add<ImageResolutionAction>();
    adjustmentLayerEditAction_ = actions_.add<AdjustmentLayerEditAction>();
    solidColorLayerEditAction_ = actions_.add<SolidColorLayerEditAction>();
    textBoxEditAction_         = actions_.add<TextBoxEditAction>();
    // ガウスぼかしフィルターレイヤーは無料版でも使えるため、FilterLayerEditAction
    // 自体は常に登録する(色収差はPro限定だが、editFilterLayer側が種類ごとに
    // ライセンス確認を行うので、アクションの登録可否では分岐しない)。
    filterLayerEditAction_     = actions_.add<FilterLayerEditAction>();
#ifdef TIEPOLO_PRO_BUILD
    chromaticAberrationAction_ = actions_.add<ChromaticAberrationAction>();
    lensBlurAction_            = actions_.add<LensBlurAction>();
    gradientMapAction_         = actions_.add<GradientMapAction>();
#endif
}

void CanvasWidget::cancelNonControllerActions()
{
    // 変形/キャンバス/レイヤー編集系を含め、全14アクションが controller 管理下に
    // 移行したため、相互排他は actions_.start()/cancelActive() だけで完結するように
    // なった。呼び出し側(startXxxAction群)を変更せずに済むよう、この関数自体は
    // 呼び出し互換のために残してあるだけの no-op。
}

void CanvasWidget::startHueSatLightAction()        { cancelNonControllerActions(); actions_.start(hueSatLightAction_); }
void CanvasWidget::startBrightnessContrastAction() { cancelNonControllerActions(); actions_.start(brightnessContrastAction_); }

// ===========================================================================
// 調整レイヤー編集(アクション)
// ---------------------------------------------------------------------------
// レイヤー自体はLayerDockの「新規調整レイヤー」で先に作成済み(色相・彩度・明度、
// 全パラメータ0)のものを使う。LayerDockでそのレイヤーのプレビューをダブルクリック
// すると開始し、パネルの値変更のたびにlayer.adjustmentを更新してその場で下の
// レイヤーへ非破壊にプレビューする。キャンセル時は編集開始時点のAdjustmentParams
// へ戻すだけで、テキストレイヤーと同様にレイヤー自体の削除は行わない
// (何度でも開き直して編集できる)。
// ===========================================================================
void CanvasWidget::editAdjustmentLayer(int layerIndex)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return;

    // トーンカーブ・グラデーションマップはPro限定(破壊的な各ツールと同じ扱い。
    // startToneCurveAction/editFilterLayer参照)。カラーバランス・色相・彩度・明度・
    // 明るさ・コントラストは無料版でも常に開ける。
    const AdjustmentKind editKind = doc_->layers[layerIndex].adjustment.kind;
    if (editKind == AdjustmentKind::ToneCurve || editKind == AdjustmentKind::GradientMap) {
        const QString featureName = (editKind == AdjustmentKind::ToneCurve)
            ? QStringLiteral("トーンカーブ") : QStringLiteral("グラデーションマップ");
#ifdef TIEPOLO_PRO_BUILD
        if (!LicenseManager::instance().isUnlocked()) {
            ProFeatureDialog::show(this, featureName);
            return;
        }
#else
        ProFeatureDialog::show(this, featureName);
        return;
#endif
    }

    adjustmentLayerEditAction_->setTarget(layerIndex);
    actions_.start(adjustmentLayerEditAction_);
}

void CanvasWidget::editFilterLayer(int layerIndex)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return;

    // Pro限定なのは種類ごとに決まる(色収差・レンズぼかし)。ガウスぼかし・移動ぼかしは
    // 無料版でも常に開ける。ここで種類を見て、Pro限定の種類のときだけライセンス確認する。
    const FilterKind editKind = doc_->layers[layerIndex].filter.kind;
    if (editKind == FilterKind::ChromaticAberration || editKind == FilterKind::LensBlur) {
        const QString featureName = (editKind == FilterKind::ChromaticAberration)
            ? QStringLiteral("色収差") : QStringLiteral("レンズぼかし");
#ifdef TIEPOLO_PRO_BUILD
        if (!LicenseManager::instance().isUnlocked()) {
            ProFeatureDialog::show(this, featureName);
            return;
        }
#else
        ProFeatureDialog::show(this, featureName);
        return;
#endif
    }

    filterLayerEditAction_->setTarget(layerIndex);
    actions_.start(filterLayerEditAction_);
}

// ===========================================================================
// 単色レイヤー編集(アクション)
// ---------------------------------------------------------------------------
// レイヤー自体はLayerDockの「新規単色レイヤー」で先に作成済みのものを使う。
// LayerDockでそのレイヤーのプレビューをダブルクリックすると開始し、パネル
// (OKLCHベースのカラーサークル、ColorCircleDockと共通のColorWheelWidgetを使う)の
// 値変更のたびにlayer.solidColorを更新してその場でプレビューする。キャンセル時は
// 編集開始時点の色へ戻すだけで、テキスト/調整レイヤーと同様にレイヤー自体の
// 削除は行わない(何度でも開き直して編集できる)。
// ===========================================================================
void CanvasWidget::editSolidColorLayer(int layerIndex)
{
    solidColorLayerEditAction_->setTarget(layerIndex);
    actions_.start(solidColorLayerEditAction_);
}

// ===========================================================================
// テキストレイヤー編集(アクション)
// ---------------------------------------------------------------------------
// レイヤー自体はLayerDockの「新規テキストレイヤー」で先に作成済み(空のテキスト
// レイヤー)のものを使う。1レイヤーは複数のテキストボックス(layer.textBoxes)を
// 持てる。TextToolで新規ボックスをtextBoxesへ追加/既存ボックスを選択した後、
// これを呼んで編集パネルを開く。TextLayerPanelの値変更のたびに対象ボックスを
// 更新してrasterizeTextLayer()で実タイルへ焼き込む(=キャンバス上にリアル
// タイムでプレビューされる)。キャンセル時は編集開始時点のTextParamsへ戻す。
// 確定/キャンセルいずれでも、結果として文字列が空になったボックスは
// textBoxesから削除する(空ボックスを残さない)。
// ===========================================================================
void CanvasWidget::startOrEditTextBox(int boxIndex)
{
    if (!doc_ || doc_->layers.isEmpty()) return;
    const int idx = doc_->activeLayerIndex();
    textBoxEditAction_->setTarget(idx, boxIndex);
    actions_.start(textBoxEditAction_);
}

// TextToolでのドラッグ操作(移動/拡縮/回転)中に呼ばれる。1文字入力ごと/1ドラッグ
// フレームごとにrasterizeTextLayer(テクスチャ再確保を伴いうる)を連打すると
// GPU側の処理が詰まってクラッシュしうるため、入力/操作が止まってから一定時間後に
// まとめてラスタライズする(デバウンス。TextBoxEditActionが編集パネル用に持つ
// 別のデバウンスタイマーとは独立)。
void CanvasWidget::scheduleTextRasterize(int layerIndex)
{
    textRasterizeLayerIndex_ = layerIndex;
    if (!textRasterizeTimer_) {
        textRasterizeTimer_ = new QTimer(this);
        textRasterizeTimer_->setSingleShot(true);
        connect(textRasterizeTimer_, &QTimer::timeout, this, [this]() {
            if (textRasterizeLayerIndex_ < 0) return;
            rasterizeTextLayer(textRasterizeLayerIndex_);
            update();
        });
    }
    textRasterizeTimer_->start(150);
}

// layer.textBoxesの内容をそれぞれQImageへラスタライズして1枚に合成し、レイヤーの
// 実タイルへ焼き込む。ボックスが1つも無い場合は既存タイルを透明クリアするだけ
// (縮小はしない)。
void CanvasWidget::rasterizeTextLayer(int layerIndex)
{
    if (!doc_ || layerIndex < 0 || layerIndex >= doc_->layerCount()) return;
    if (doc_->layerRef(layerIndex).layerType != LayerType::Text) return;

    makeCurrent();
    // grow中の再確保に影響されないようコピー
    const std::vector<TextParams> boxes = doc_->layerRef(layerIndex).textBoxes;

    // 全ボックスの回転後AABBの和集合を画像サイズとする(+余白)。
    QRect unionRect;
    for (const TextParams &box : boxes) {
        if (box.text.isEmpty()) continue;
        const float hw = box.width  * box.scale / 2.0f;
        const float hh = box.height * box.scale / 2.0f;
        const float diag = std::sqrt(hw * hw + hh * hh) + 2.0f; // 回転を考慮した安全マージン
        const QRect r(qFloor(box.cx - diag), qFloor(box.cy - diag), qCeil(diag * 2), qCeil(diag * 2));
        unionRect = unionRect.isNull() ? r : unionRect.united(r);
    }

    QImage img;
    int imgX = 0, imgY = 0, w = 0, h = 0;
    if (!unionRect.isNull()) {
        imgX = unionRect.x();
        imgY = unionRect.y();
        w = qMax(1, unionRect.width());
        h = qMax(1, unionRect.height());

        img = QImage(w, h, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        for (const TextParams &box : boxes) {
            if (box.text.isEmpty()) continue;
            QFont font(box.fontFamily, box.fontSize);
            font.setBold(box.bold);
            font.setItalic(box.italic);
            p.save();
            // 後で画像全体を1回だけ上下反転する(下記)ため、反転後にbox.cyへ
            // 正しく戻るよう、あらかじめ反転を見込んだY座標(imgY + h - box.cy)へ
            // 描画しておく。単純にbox.cy - imgYを使うと、そのボックスが合成画像
            // 全体の垂直中心にある場合(=ボックスが1つだけの場合)のみ正しく、
            // 複数ボックスが同じ画像に収まる場合は他ボックスの位置に応じてズレる
            // (1つのボックスを動かすと他のボックスも動いて見えるバグの原因だった)。
            p.translate(box.cx - imgX, imgY + h - box.cy);
            p.rotate(box.rotation);
            p.scale(box.scale, box.scale);
            p.setFont(font);
            p.setPen(box.color);
            p.drawText(QRectF(-box.width / 2.0, -box.height / 2.0, box.width, box.height),
                       Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignVCenter, box.text);
            p.restore();
        }
        p.end();

        // QPainterはY下向き(通常の画像座標系)で描画するが、キャンバスピクセル座標は
        // Y上向き(widgetToCanvas参照)なので、タイルへ書き込む前に上下反転させる。
        img = img.mirrored(false, true);

        const int minTx   = qFloor((double)imgX / TILE_SIZE);
        const int minTy   = qFloor((double)imgY / TILE_SIZE);
        const int maxTxEx = qCeil((double)(imgX + w) / TILE_SIZE);
        const int maxTyEx = qCeil((double)(imgY + h) / TILE_SIZE);
        growLayerBoundsToCoverCanvasTiles(layerIndex, minTx, minTy, maxTxEx, maxTyEx);
    }

    Layer &layer = doc_->layerRef(layerIndex);
    static const QByteArray zeroTile(TILE_SIZE * TILE_SIZE * 4, 0);
    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            const int canvasTx = layer.originTx + tx;
            const int canvasTy = layer.originTy + ty;
            const int tilePxX  = canvasTx * TILE_SIZE;
            const int tilePxY  = canvasTy * TILE_SIZE;
            const int slice    = layer.tiles[ty][tx];

            if (img.isNull()) { writeSlicePixels(slice, zeroTile); continue; }

            const int ox0 = qMax(tilePxX, imgX);
            const int oy0 = qMax(tilePxY, imgY);
            const int ox1 = qMin(tilePxX + TILE_SIZE, imgX + w);
            const int oy1 = qMin(tilePxY + TILE_SIZE, imgY + h);
            if (ox0 >= ox1 || oy0 >= oy1) { writeSlicePixels(slice, zeroTile); continue; }

            QByteArray buf = zeroTile;
            for (int y = oy0; y < oy1; y++) {
                const uchar *srcRow = img.constScanLine(y - imgY) + (ox0 - imgX) * 4;
                uchar *dstRow = reinterpret_cast<uchar*>(buf.data()) + ((y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                memcpy(dstRow, srcRow, (ox1 - ox0) * 4);
            }
            writeSlicePixels(slice, buf);
        }
    }

    // 1文字入力/1ドラッグフレームごとに呼ばれるため、連打するとgrowLayerBounds
    // (テクスチャ再確保を伴いうる)~書き込みが連続で走る。GPU側の処理が完了しない
    // うちに次の呼び出しでまたテクスチャを確保し直す(rebuildCanvasFromSnapshotsと
    // 同種のタイミング依存クラッシュ)のを防ぐため、ここで明示的に完了を待つ。
    glFinish();

    updateCompositedTex();
}

// ===========================================================================
// カラーバランス(アクション)
// ---------------------------------------------------------------------------
// HueSatLightアクションと全く同じ構造。CanvasWidgetの子ウィジェットとして浮かせた
// ColorBalancePanel(C/M/Yの3本のQSlider)から値を受け取る。
// ===========================================================================
// ===========================================================================
// 色調整/フィルター系アクションの入口(フォワーダ)続き。実装は src/actions/ 側。
// ===========================================================================
void CanvasWidget::startColorBalanceAction() { cancelNonControllerActions(); actions_.start(colorBalanceAction_); }
void CanvasWidget::startGaussianBlurAction() { cancelNonControllerActions(); actions_.start(gaussianBlurAction_); }
void CanvasWidget::startCustomShaderAction() { cancelNonControllerActions(); actions_.start(customShaderAction_); }
void CanvasWidget::startMosaicAction()       { cancelNonControllerActions(); actions_.start(mosaicAction_); }
void CanvasWidget::startMotionBlurAction()   { cancelNonControllerActions(); actions_.start(motionBlurAction_); }
void CanvasWidget::startNoiseAction()        { cancelNonControllerActions(); actions_.start(noiseAction_); }

// トーンカーブはPro限定機能(色収差と同じ案内ダイアログパターン)。ツール自体は
// 無料版にも含まれる(CMakeLists上はPro専用に分離していない)ため、ここで
// ライセンス確認だけを行う。
void CanvasWidget::startToneCurveAction()
{
#ifdef TIEPOLO_PRO_BUILD
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("トーンカーブ"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(toneCurveAction_);
#else
    ProFeatureDialog::show(this, QStringLiteral("トーンカーブ"));
#endif
}

void CanvasWidget::startChromaticAberrationAction()
{
#ifdef TIEPOLO_PRO_BUILD
    // Pro版でもライセンス未認証なら実処理せず案内ダイアログを出す。
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("色収差"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(chromaticAberrationAction_);
#else
    // 無料版ビルドでは色収差アクション自体を持たない。案内ダイアログのみ。
    ProFeatureDialog::show(this, QStringLiteral("色収差"));
#endif
}

void CanvasWidget::startLensBlurAction()
{
#ifdef TIEPOLO_PRO_BUILD
    // Pro版でもライセンス未認証なら実処理せず案内ダイアログを出す(色収差と同じパターン)。
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("レンズぼかし"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(lensBlurAction_);
#else
    // 無料版ビルドではレンズぼかしアクション自体を持たない。案内ダイアログのみ。
    ProFeatureDialog::show(this, QStringLiteral("レンズぼかし"));
#endif
}

void CanvasWidget::startGradientMapAction()
{
#ifdef TIEPOLO_PRO_BUILD
    // Pro版でもライセンス未認証なら実処理せず案内ダイアログを出す(色収差と同じパターン)。
    if (!LicenseManager::instance().isUnlocked()) {
        ProFeatureDialog::show(this, QStringLiteral("グラデーションマップ"));
        return;
    }
    cancelNonControllerActions();
    actions_.start(gradientMapAction_);
#else
    // 無料版ビルドではグラデーションマップアクション自体を持たない。案内ダイアログのみ。
    ProFeatureDialog::show(this, QStringLiteral("グラデーションマップ"));
#endif
}

// ===========================================================================
// キャンバスサイズ変更 / 画像解像度変更(アクション)
// ---------------------------------------------------------------------------
// 実体は src/actions/CanvasSizeActions.h の CanvasSizeAction/ImageResolutionAction
// へ移動した。ここに残るのはメニュー/ショートカットの入口だけ。
// ===========================================================================
void CanvasWidget::startCanvasSizeAction()
{
    cancelNonControllerActions();
    actions_.start(canvasSizeAction_);
}

void CanvasWidget::startImageResolutionAction()
{
    cancelNonControllerActions();
    actions_.start(imageResolutionAction_);
}

// ===========================================================================
// 初期化
// ===========================================================================
