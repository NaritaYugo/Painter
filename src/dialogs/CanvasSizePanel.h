#pragma once
#include "dialogs/DraggablePanel.h"

class QSpinBox;
class QPushButton;
class QButtonGroup;

// ---------------------------------------------------------------------------
// CanvasSizePanel
// ---------------------------------------------------------------------------
// 編集メニュー「キャンバスサイズ変更」アクション用のポップアップパネル。
// 9箇所のアンカー選択(3x3ボタン)と幅/高さのQSpinBoxを持つ。CanvasWidgetの子として
// 「キャンバス上に」浮かせて表示する非モーダルのQWidget。
// キャンバス上でのハンドルドラッグとも数値欄が双方向に同期する
// (ドラッグ時はCanvasWidgetがsetSizeFields()で数値欄だけを更新する)。
// ---------------------------------------------------------------------------
class CanvasSizePanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit CanvasSizePanel(QWidget *parent = nullptr);

    // アクション開始時: 現在のキャンバスサイズ・アンカー(中央)にリセットする
    void resetTo(int w, int h);

    // ハンドルドラッグ後、数値欄だけをドラッグ結果に同期する(アンカー選択は変えない)
    void setSizeFields(int w, int h);

signals:
    void settingsChanged(int anchorIndex, int w, int h); // アンカー変更 or 数値変更のたび
    void confirmed();
    void cancelled();

protected:

private:
    QSpinBox *widthSpin_  = nullptr;
    QSpinBox *heightSpin_ = nullptr;
    QButtonGroup *anchorGroup_ = nullptr;

    void emitSettingsChanged();
};
