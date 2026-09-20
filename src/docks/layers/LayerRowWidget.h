#pragma once

#include "document/CanvasDocument.h"

#include <QFrame>
#include <QMouseEvent>
#include <QPoint>
#include <QString>
#include <QVector>

class CanvasWidget;
class LayerPickerPopup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QTimer;

// LayerDockの表示行。
class LayerRowWidget : public QFrame
{
    Q_OBJECT
public:
    LayerRowWidget(CanvasWidget *gl, QVector<int> rowLayers, QWidget *parent = nullptr);

    ~LayerRowWidget() override;

    void setSelected(bool selected);

    // 行内で現在表示している列。
    int displayColumn() const;
    int currentLayerIndex() const;

    void refreshFromDocument();

    const QVector<int> &rowLayers() const;

signals:
    void clicked(int layerIndex);
    void adjustmentLayerDoubleClicked(int layerIndex);
    void solidColorLayerDoubleClicked(int layerIndex);
    void filterLayerDoubleClicked(int layerIndex);
    void folderDoubleClicked(int layerIndex);

protected:
    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void openPicker();

    void updatePickerVisual();

    void closePicker();

    void startRename();

    void finishRename();

    // QWidget::setStyleSheet() は、同じ文字列を渡した場合でも QStyleSheetStyle のキャッシュをグローバルに捨てて全ウィジェットのスタイルを解決し直す。
    static void setStyleSheetIfChanged(QWidget *w, QString &cache, const QString &qss);

    void applyStyle();

    CanvasWidget *glWidget;
    QVector<int> rowLayers_;
    bool selected_ = false;

    QLabel    *clipArrowLabel = nullptr;
    QWidget   *blendColorBar = nullptr;
    QCheckBox *visibleCheck = nullptr;
    QLabel    *thumbLabel   = nullptr;
    QLineEdit *nameEdit     = nullptr;
    QLabel    *metaLabel    = nullptr;

    // 前回 setStyleSheet() に渡した文字列(setStyleSheetIfChanged参照)。
    QString lastRowQss_;
    QString lastBlendBarQss_;
    QString lastClipArrowQss_;

    QTimer *longPressTimer = nullptr;
    LayerPickerPopup *picker_ = nullptr;
    bool   pickerOpen_    = false;
    QPoint pressPos_;
    qreal  lastScrollOffset_ = 0; // updatePickerVisual()参照
    int    pickerBaseCol_ = 0;
    int    pickerActiveCol_ = 0;
    int    pickerContentW_ = 0; // ポップアップの全項目ぶんの論理幅(ウィンドウ幅より広い場合スクロールする)

    // サムネイルキャッシュ: getLayerPreview()はフルキャンバス合成を伴い重いので、前回生成時からこの行の見た目に関わる値が変わっていない場合は再生成をスキップする。
    bool      thumbCacheValid_ = false;
    int       cachedLi_        = -1;
    float     cachedOpacity_   = -1.0f;
    bool      cachedVisible_   = true;
    BlendMode cachedBlend_     = BlendMode::Normal;
    LayerType cachedType_      = LayerType::Normal;
    QString   cachedName_;
};

// LayerIndicatorZone ― 常時表示のクリッピング構造インジケーターリストがスクロールしても、行×列のドット配置と接続線で全体の構成を常に見渡せるようにする(幅・高さいっぱいに全行を均等割りして表示するためスクロールしない)。
