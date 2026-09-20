#pragma once

#include "actions/CanvasAction.h"
#include "document/CanvasDocument.h"

#include <QColor>

class QTimer;
class BrightnessContrastPanel;
class HueSatLightPanel;
class ColorBalancePanel;
class ToneCurvePanel;
#ifdef TIEPOLO_PRO_BUILD
class GradientMapPanel;
#endif
class SolidColorPickerPanel;
class TextLayerPanel;

// ---------------------------------------------------------------------------
// レイヤー編集系アクション(調整レイヤー / 単色レイヤー / テキストボックス)。
// いずれもレイヤー自体はLayerDock側で先に作成済みのものを使い、このアクションは
// 「既存レイヤーのパラメータをパネルでその場編集する」だけ(レイヤーの削除は行わない)。
// 対象インデックスは setTarget() で渡してから CanvasActionController::start() する。
// 確定/キャンセルの意味が色調整アクション(ピクセルへ焼き込む/破棄する)とは異なり、
// 「編集内容をそのまま残す/開始前の値に戻す」なので、専用パネルインスタンスを使う。
// ---------------------------------------------------------------------------

class AdjustmentLayerEditAction : public CanvasAction
{
public:
    AdjustmentLayerEditAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    bool blocksAllToolInput() const override { return true; }
    void setTarget(int layerIndex) { targetLayerIndex_ = layerIndex; }
    void positionPanel() override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    int targetLayerIndex_ = -1;
    AdjustmentKind pendingKind_ = AdjustmentKind::BrightnessContrast;
    AdjustmentParams backup_;
    BrightnessContrastPanel *bcPanel_  = nullptr;
    HueSatLightPanel        *hslPanel_ = nullptr;
    ColorBalancePanel       *cbPanel_  = nullptr;
    ToneCurvePanel          *tcPanel_  = nullptr;
#ifdef TIEPOLO_PRO_BUILD
    GradientMapPanel        *gmPanel_  = nullptr;
#endif
};

class SolidColorLayerEditAction : public CanvasAction
{
public:
    SolidColorLayerEditAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    bool blocksAllToolInput() const override { return true; }
    void setTarget(int layerIndex) { targetLayerIndex_ = layerIndex; }
    void positionPanel() override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    int targetLayerIndex_ = -1;
    QColor backup_;
    SolidColorPickerPanel *panel_ = nullptr;
};

class TextBoxEditAction : public CanvasAction
{
public:
    TextBoxEditAction(CanvasActionHost &h, CanvasActionController &c) : CanvasAction(h, c) {}
    bool blocksAllToolInput() const override { return true; }
    void setTarget(int layerIndex, int boxIndex) { targetLayerIndex_ = layerIndex; targetBoxIndex_ = boxIndex; }
    void positionPanel() override;
protected:
    bool canActivate() const override;
    bool onActivate() override;
    void onConfirm() override;
    void onCancel() override;
private:
    // 1文字入力ごとにラスタライズ(テクスチャ再確保を伴いうる)を連打すると
    // GPU側の処理が詰まってクラッシュしうるため、入力が止まってから一定時間後に
    // まとめてラスタライズする(デバウンス)。
    void scheduleRasterize();

    int targetLayerIndex_ = -1;
    int targetBoxIndex_   = -1;
    TextParams backup_;
    TextLayerPanel *panel_ = nullptr;
    QTimer *rasterizeTimer_ = nullptr;
};
