#pragma once

#include <QWidget>
#include <QVector>

class CanvasWidget;
class QScrollArea;
class QVBoxLayout;
class QHBoxLayout;
class QPushButton;
class QComboBox;
class QLabel;
class MaskOpacityPreview;
class LayerRowWidget;
class LayerIndicatorZone;
class QTimer;

class LayerDock : public QWidget
{
    Q_OBJECT
public:
    explicit LayerDock(CanvasWidget *gl, QWidget *parent = nullptr);

    void refresh();
    void scheduleRefresh();
    void setCanvasWidget(CanvasWidget *gl);

    // ツールバーとMainWindowのレイヤーメニューで共有する操作。
    void addRow();
    void addColumn();
    void addFolder();
    void insertSolidColorLayer();
    void insertAdjustmentLayer();
    void insertFilterLayer();
    void insertTextLayer();
    void deleteActiveLayer();
    void duplicateSelected();
    void mergeSelected();

    static constexpr int MIN_HEIGHT  = 320;
    static constexpr int MIN_WIDTH   = 260;
    static constexpr int BASE_WIDTH  = 340;
    static constexpr int BASE_HEIGHT = 420;

    QSize sizeHint() const override { return QSize(BASE_WIDTH, BASE_HEIGHT); }

private:
    CanvasWidget *glWidget = nullptr;

    // 選択中レイヤーの設定
    QComboBox *blendCombo    = nullptr;
    MaskOpacityPreview *opacityPreview = nullptr;
    QLabel    *opacityLabel  = nullptr;
    enum class BlendComboMode { Blend, Adjustment, Filter };
    BlendComboMode blendComboMode_ = BlendComboMode::Adjustment;
    void populateBlendCombo(BlendComboMode mode);
    void openAdjustmentLayerEditor(int layerIndex);
    void openSolidColorLayerEditor(int layerIndex);
    void openFilterLayerEditor(int layerIndex);
    QPushButton *addRowBtn    = nullptr;
    QPushButton *addColBtn    = nullptr;
    QPushButton *duplicateBtn = nullptr;
    QPushButton *mergeBtn     = nullptr;
    QPushButton *deleteBtn    = nullptr;
    QPushButton *addSpecialLayerBtn = nullptr;

    // レイヤーリスト
    QScrollArea *scrollArea    = nullptr;
    QWidget     *listContainer = nullptr;
    QVBoxLayout *listLayout    = nullptr;
    QVector<LayerRowWidget*> rows_;
    QLabel *emptyFolderHintLabel_ = nullptr;

    LayerIndicatorZone *indicatorZone = nullptr;

    // フォルダー階層
    QVector<int> folderPath_;
    QWidget     *breadcrumbBar_    = nullptr;
    QHBoxLayout *breadcrumbLayout_ = nullptr;
    bool insideFolder() const { return !folderPath_.isEmpty(); }
    int currentScope() const { return folderPath_.isEmpty() ? -1 : folderPath_.last(); }
    void enterFolder(int folderLayerIndex);
    void goToBreadcrumbLevel(int keepCount);
    void rebuildBreadcrumb();
    int computeInsertIndexInScope() const;

    int duplicateFolderDeep(int sourceFolderIndex, int insertAt);
    // 直下の通常レイヤーを結合し、フォルダーマーカーを取り除く。
    void mergeFolderContents(int folderIndex);

    QTimer *refreshDebounceTimer_ = nullptr;

    void rebuildRows();
    void selectLayer(int layerIndex);
    void updateControlsFromActiveLayer();
    void mergeRootRow(int rootIndex);
};
