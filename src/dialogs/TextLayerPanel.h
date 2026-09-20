#pragma once
#include "dialogs/DraggablePanel.h"
#include "document/CanvasDocument.h"

class QPlainTextEdit;
class QFontComboBox;
class QSpinBox;
class QToolButton;
class QCheckBox;

// ---------------------------------------------------------------------------
// TextLayerPanel
// ---------------------------------------------------------------------------
// TextToolでテキストレイヤーをクリックしたときに出る、文字入力用のポップアップ
// パネル。BrightnessContrastPanel/HueSatLightPanelと同様、CanvasWidgetの子として
// 「キャンバス上に」浮かせて表示する非モーダルのQWidget。
// ---------------------------------------------------------------------------
class TextLayerPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit TextLayerPanel(QWidget *parent = nullptr);

    void setValues(const TextParams &params); // パネルを開く際、既存の値で埋める

signals:
    void valuesChanged(const TextParams &params);
    void confirmed();
    void cancelled();

protected:

private:
    QPlainTextEdit *textEdit_    = nullptr;
    QFontComboBox   *fontCombo_  = nullptr;
    QSpinBox        *sizeSpin_   = nullptr;
    QToolButton     *colorBtn_   = nullptr;
    QCheckBox       *boldCheck_  = nullptr;
    QCheckBox       *italicCheck_ = nullptr;

    QColor currentColor_ = QColor(0, 0, 0, 255);
    // 位置/サイズ/回転/スケールはこのパネルでは編集しない(TextTool側で
    // キャンバス上のドラッグ操作により変更する)ため、このパネルは保持しない。

    void updateColorButton();
    void emitValuesChanged();
};
