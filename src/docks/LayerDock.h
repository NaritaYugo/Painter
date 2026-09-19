#pragma once

#include <QWidget>
#include <QVector>

class GLWidget;
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

// ===========================================================================
// LayerDock  ―  レイヤー一覧パネル
//
// Photoshopのクリッピングマスクを「横に転回」して表示するUI。
// ・クリッピングされていないレイヤー(root)を1行とし、そこにクリップする
//   レイヤー群は同じ行の中で1枚だけを表示(横幅が伸びすぎないよう重ねる)
// ・行を長押し/ドラッグすると、その行のレイヤーサムネイルが横に並んだ
//   ポップアップが出て、ドラッグでスクロールしながら選べる
// ・右側に、行が多くてリストがスクロールしても全体の構成を見失わないための
//   常時表示インジケーター(行×列のドット+接続線)を置く
// ・フォルダー(星形ノード)をダブルクリックすると、パンくずリストで戻れる
//   「その中身だけを表示する」階層へ切り替わる。ルート/フォルダーの中のどちらも
//   同じ仕組み(computeRows()のscope引数、LayerDock::currentScope())で扱われ、
//   レイヤー追加/複製/結合/削除などの操作もすべて現在のスコープに対して働く。
// ===========================================================================
class LayerDock : public QWidget
{
    Q_OBJECT
public:
    explicit LayerDock(GLWidget *gl, QWidget *parent = nullptr);

    // 外部（MainWindowなど）から画面をリフレッシュするための関数
    void refresh();

    // GLWidget::layersChanged()から呼ぶ想定。ストローク確定の連打や不透明度
    // スライダーのドラッグなど、短時間に連続発火しうる場面で毎回refresh()
    // (フルキャンバス合成を伴いうる重い処理)を直接実行すると重くなるため、
    // 一定時間まとめてから最後の1回だけrefresh()を呼ぶ(デバウンス)。
    void scheduleRefresh();

    // タブ切替時に、表示対象のGLWidget(=キャンバス)を差し替える
    void setGLWidget(GLWidget *gl);

    // MainWindowのレイヤーメニューからも呼べるよう公開している、ツールバーの
    // 各ボタンと同じ操作(実体はprivateのメンバ関数と同じシグネチャ)。
    void addRow();
    void addColumn();
    void addFolder();
    void insertSolidColorLayer();
    void insertAdjustmentLayer();
    void insertFilterLayer();
    void insertTextLayer();
    void deleteActiveLayer();
    // 選択中レイヤーが一番左の列(root)かクリップ列かで、複製/結合の処理を
    // 自動的に使い分ける(詳細はLayerDock.cppのコメント参照)。
    void duplicateSelected();
    void mergeSelected();

    static constexpr int MIN_HEIGHT  = 320;
    static constexpr int MIN_WIDTH   = 260;
    static constexpr int BASE_WIDTH  = 340;
    static constexpr int BASE_HEIGHT = 420;

    QSize sizeHint() const override { return QSize(BASE_WIDTH, BASE_HEIGHT); }

private:
    GLWidget *glWidget = nullptr;

    // ---- 上部コントロール(選択中レイヤーのブレンドモード/不透明度・マスク) ----
    QComboBox *blendCombo    = nullptr;
    // 不透明度スライダーを置き換える統合コントロール。マスクの濃淡プレビューを兼ね、
    // 左右ドラッグで一律の不透明度、クリックでマスク編集モードの切り替えを行う。
    MaskOpacityPreview *opacityPreview = nullptr;
    QLabel    *opacityLabel  = nullptr;
    // blendComboの中身を何として使っているか(調整レイヤー/フィルターレイヤー
    // 選択中はコンボの用途を「種類の選択」に切り替えて使う)。
    // 初期値はコンストラクタでのpopulateBlendCombo(Blend)呼び出しを確実に
    // 中身入れ替えとして扱わせるため、あえて実際の初期状態と異なる値にしてある。
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

    // ---- レイヤーリスト ----
    QScrollArea *scrollArea    = nullptr;
    QWidget     *listContainer = nullptr;
    QVBoxLayout *listLayout    = nullptr;
    QVector<LayerRowWidget*> rows_;
    // 現在の階層(スコープ)に表示すべき行が1つも無い(=フォルダーの中身が空)
    // ときに出すプレースホルダーラベル。listLayout内、末尾のstretchの直前に
    // 常駐させ、表示/非表示だけを切り替える。
    QLabel *emptyFolderHintLabel_ = nullptr;

    // ---- 常時表示インジケーター ----
    LayerIndicatorZone *indicatorZone = nullptr;

    // ---- フォルダーの階層をたどるためのパンくずリスト ----
    // 「階層に入る/出る」表示に切り替えるための現在位置。空ならルート階層。
    // folderPath_.last()が現在開いているフォルダー(=表示スコープ)のレイヤーindex。
    // 外側→内側の順に並ぶため、そのままCanvasDocument::addLayer/removeLayerの
    // ancestorFolders引数としても使える。
    QVector<int> folderPath_;
    QWidget     *breadcrumbBar_    = nullptr;
    QHBoxLayout *breadcrumbLayout_ = nullptr;
    bool insideFolder() const { return !folderPath_.isEmpty(); }
    // 現在表示中のスコープ(-1=ルート、それ以外はフォルダーのレイヤーindex)。
    // computeRows()等のscope引数にそのまま渡せる。
    int currentScope() const { return folderPath_.isEmpty() ? -1 : folderPath_.last(); }
    void enterFolder(int folderLayerIndex);
    // keepCount: 0=ルートへ戻る、Nならフォルダー階層をN段まで残して戻る
    void goToBreadcrumbLevel(int keepCount);
    void rebuildBreadcrumb();
    // 現在のスコープ内で、アクティブレイヤーの直後に新規レイヤーを挿入する場合の
    // flat挿入位置を返す(該当行が無ければスコープの末尾)。addRow/addColumn/
    // addFolder/insertSolidColorLayer等、レイヤー追加系の操作すべてで共有する。
    int computeInsertIndexInScope() const;

    // フォルダーに対する「複製」「結合」のオーバーライド(duplicateSelected/
    // mergeSelectedのcol==0かつFolderのときに呼ばれる)。
    // sourceFolderIndexの中身(ネストも含む)を丸ごと複製し、insertAtへ挿入する。
    // 戻り値: 複製後の新しいフォルダーのインデックス(失敗時-1)。
    int duplicateFolderDeep(int sourceFolderIndex, int insertAt);
    // folderIndexの直接の中身(Normalレイヤーのみ、ネストしたフォルダーが
    // 混ざっている場合は非対応として何もしない)をすべて1枚に結合し、
    // 空になったフォルダーのマーカー自体は取り除く(結合後のレイヤーが
    // 元のフォルダーがあった位置にそのまま残る)。
    void mergeFolderContents(int folderIndex);

    // scheduleRefresh()用のデバウンスタイマー(singleShot、都度restartする)
    QTimer *refreshDebounceTimer_ = nullptr;

    void rebuildRows();
    void selectLayer(int layerIndex);
    void updateControlsFromActiveLayer();
    // rootIndexの行を、クリッピングを含めてまるごと1つ下の行へ結合する
    // (mergeSelected()がroot列選択時に呼ぶ)。
    void mergeRootRow(int rootIndex);
};
