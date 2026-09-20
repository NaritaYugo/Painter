#pragma once

#include <QString>
#include <QVector>
#include <QImage>
#include <QColor>
#include <QPointF>
#include <QMetaType>
#include <functional>
#include <vector>
#include <QtGui/qopengl.h> // GLuint only; the document model does not depend on widgets.

// 定数。
static constexpr int TILE_SIZE  = 256;
// タイルを格納するGPUテクスチャ配列(layerTexArray)は GL_MAX_ARRAY_TEXTURE_LAYERS(多くのGPUで2048)を超えるスライスを1つに持てない。
static constexpr int MAX_TILE_BANKS = 8;
// バンク用サンプラーを割り当てるテクスチャユニットの先頭(render.frag等が1..7を使うため、その先の8から並べる)。
static constexpr int LAYER_BANK_TEXUNIT_BASE = 8;
// layerTexArrayの初期確保スライス数の下限、かつGPUドライバへの問い合わせに失敗した場合のフォールバック値(OpenGL仕様上の最低保証値と同じ)。
static constexpr int MAX_SLICES = 2048;

// ブレンドモード
enum class BlendMode : int {
    Normal        = 0,
    Multiply      = 1,
    Screen        = 2,
    Overlay       = 3,
    Dissolve      = 4,
    Darken        = 5,
    ColorBurn     = 6,
    LinearBurn    = 7,
    DarkerColor   = 8,
    Lighten       = 9,
    ColorDodge    = 10,
    LinearDodge   = 11, // 加算(Add)
    LighterColor  = 12,
    SoftLight     = 13,
    HardLight     = 14,
    VividLight    = 15,
    LinearLight   = 16,
    PinLight      = 17,
    HardMix       = 18,
    Difference    = 19,
    Exclusion     = 20,
    Subtract      = 21,
    Divide        = 22,
    Hue           = 23,
    Saturation    = 24,
    Color         = 25,
    Luminosity    = 26,
};
Q_DECLARE_METATYPE(BlendMode)

// レイヤー種別
enum class LayerType : int {
    Normal     = 0,
    SolidColor = 1,
    Adjustment = 2,
    Text       = 3,
    Folder     = 4,
    Filter     = 5,
};
Q_DECLARE_METATYPE(LayerType)

// 調整レイヤーの種類とパラメータ
enum class AdjustmentKind : int {
    BrightnessContrast = 0, // PSD "brit" ブロック相当
    HueSaturation      = 1, // PSD "hue2" ブロック相当
    ColorBalance        = 2, // 無料版でも使える
    ToneCurve             = 3, // 無料版でも使える
    GradientMap            = 4, // Pro限定
};
Q_DECLARE_METATYPE(AdjustmentKind)

// グラデーションマップ調整レイヤーのストップ(dialogs/GradientStripEditor::Stopと同じ形だが、backend層がdialogs層へ依存しないよう独立に定義してある)。
struct AdjustmentGradientStop {
    float  pos = 0.0f; // 0..1
    QColor color;
};

struct AdjustmentParams {
    AdjustmentKind kind = AdjustmentKind::BrightnessContrast;

    // kind == BrightnessContrast のときのみ使用 (BrightnessContrastToolと同じ範囲)。
    int brightness = 0; // -100..100
    int contrast   = 0; // -100..100

    // kind == HueSaturation のときのみ使用 (HueSatLightToolと同じ範囲)。
    int hue        = 0; // -180..180
    int saturation = 0; // -100..100
    int lightness  = 0; // -100..100

    // kind == ColorBalance のときのみ使用 (ColorBalanceToolと同じ範囲)。
    int cyan    = 0; // -100..100
    int magenta = 0; // -100..100
    int yellow  = 0; // -100..100

    // kind == ToneCurve のときのみ使用。
    QVector<QPointF> curvePoints = { QPointF(0, 0), QPointF(255, 255) };

    // kind == GradientMap のときのみ使用(Pro限定)。
    QVector<AdjustmentGradientStop> gradientStops = {
        { 0.0f, QColor(0, 0, 0) }, { 1.0f, QColor(255, 255, 255) }
    };
};

// フィルターレイヤーの種類とパラメータ
enum class FilterKind : int {
    ChromaticAberration = 0, // 色収差(Pro限定)
    GaussianBlur        = 1, // ガウスぼかし(無料版でも使える)
    MotionBlur           = 2, // 移動ぼかし(無料版でも使える)
    LensBlur              = 3, // レンズぼかし(Pro限定)
    Mosaic                 = 4, // モザイク(無料版でも使える)
    Noise                   = 5, // ノイズ(無料版でも使える)
};
Q_DECLARE_METATYPE(FilterKind)

struct FilterParams {
    FilterKind kind = FilterKind::ChromaticAberration;

    // kind == ChromaticAberration のときのみ使用 (ChromaticAberrationToolと同じ意味)。
    int   caMode       = 0;      // 0=平行, 1=円形
    float caAngleDeg   = 0.0f;   // 平行モード時のずらす向き(度、0=水平右向き)
    float caDistancePx = 8.0f;   // ずらす距離(px)
    // 円形モード時の中心。
    float caCenterU    = 0.5f;
    float caCenterV    = 0.5f;

    // kind == GaussianBlur のときのみ使用 (GaussianBlurToolと同じ意味・同じ既定値)。
    float blurRadiusPx = 8.0f;

    // kind == MotionBlur のときのみ使用 (MotionBlurToolと同じ意味)。
    int   mbMode         = 0;      // 0=平行, 1=円形
    float mbAngleDeg     = 0.0f;   // 平行モード: ぶれる向き(度)
    float mbDistancePx   = 24.0f;  // 平行モード: ぶれる長さ(px)
    float mbCenterU      = 0.5f;   // 円形モード: 回転中心
    float mbCenterV      = 0.5f;
    float mbAngleSpanDeg = 10.0f;  // 円形モード: 振れ角(度)

    // kind == LensBlur のときのみ使用 (LensBlurToolと同じ意味・同じ既定値。Pro限定)。
    float lbRadiusPx        = 16.0f;
    int   lbBlades           = 0;      // 0=円, 3以上=正多角形の辺数
    float lbBladeRotDeg     = 0.0f;
    float lbHighlightBoost = 0.6f;
    float lbThreshold        = 0.7f;

    // kind == Mosaic のときのみ使用 (MosaicToolと同じ意味・同じ既定値)。
    int   mzBlockSize = 16;

    // kind == Noise のときのみ使用 (NoiseToolと同じ意味・同じ既定値)。
    float    nsStrength    = 0.25f;
    bool     nsMonochrome  = true;
    float    nsGrainPx     = 1.0f;
    unsigned int nsSeed    = 0;
};

// テキストレイヤーのパラメータ
struct TextParams {
    QString text;
    QString fontFamily = QStringLiteral("Yu Gothic UI");
    int     fontSize    = 48;                  // px (scale=1のときの基準サイズ)
    QColor  color        = QColor(0, 0, 0, 255);
    bool    bold         = false;
    bool    italic        = false;
    float   cx = 0, cy = 0;         // キャンバスピクセル座標(ボックス中心)
    float   width = 300, height = 100; // scale=1のときの基準サイズ(ワードラップ矩形)
    float   rotation = 0;           // 度
    float   scale = 1.0f;           // 一様スケール
};

// Layer
struct Layer {
    // tiles[ty][tx] = layerTexArray のスライス番号 (連番前提で確保される)。
    QVector<QVector<int>> tiles;
    int originTx = 0, originTy = 0;

    QString   name;
    float     opacity   = 1.0f;
    bool      visible   = true;
    BlendMode blendMode = BlendMode::Normal;
    bool      clipping  = false; // true: 直下の非クリッピングレイヤーにクリップする
    LayerType layerType = LayerType::Normal;
    AdjustmentParams adjustment; // layerType == Adjustment のときのみ意味を持つ
    FilterParams     filter;     // layerType == Filter のときのみ意味を持つ
    std::vector<TextParams> textBoxes; // layerType == Text のときのみ意味を持つ(複数可)
    QColor    solidColor = QColor(255, 255, 255, 255); // layerType == SolidColor のときのみ意味を持つ
    // layerType == Folder のときのみ意味を持つ。
    int       childCount = 0;
    // レイヤー(またはフォルダー)マスクを持つか(1枚につき最大1枚)。
    bool      hasMask = false;
    // hasMask==trueのときのみ意味を持つ。
    QVector<QVector<int>> maskTiles;
    // マスクに一度でもペンで描き込まれたか。
    bool      maskDirty = false;

    // layerType == Adjustment かつ adjustment.kind が ToneCurve/GradientMap のときのみ使用する、LUT(256x1、
    // layerTexArrayの1スライスの左上行だけを使う)のスライス番号(無ければ-1)。
    int       lutSlice = -1;

    // レイヤーローカルのタイル座標(0オリジン)で指定タイルの sliceIndex を返す。
    int tileSlice(int tx, int ty) const {
        if (ty < 0 || ty >= tiles.size()) return -1;
        if (tx < 0 || tx >= tiles[ty].size()) return -1;
        return tiles[ty][tx];
    }

    // キャンバスタイル座標(原点を考慮)で指定タイルの sliceIndex を返す。
    int tileSliceAtCanvasTile(int canvasTx, int canvasTy) const {
        return tileSlice(canvasTx - originTx, canvasTy - originTy);
    }

    int tilesX() const { return tiles.isEmpty() ? 0 : tiles[0].size(); }
    int tilesY() const { return tiles.size(); }

    // マスクタイル版のtileSlice/tileSliceAtCanvasTile。
    int maskTileSlice(int tx, int ty) const {
        if (ty < 0 || ty >= maskTiles.size()) return -1;
        if (tx < 0 || tx >= maskTiles[ty].size()) return -1;
        return maskTiles[ty][tx];
    }
    int maskTilesX() const { return maskTiles.isEmpty() ? 0 : maskTiles[0].size(); }
    int maskTilesY() const { return maskTiles.size(); }

    // 旧 texArrayIndex 互換: tile[0][0] を返す。
    int texArrayIndex() const { return tileSlice(0, 0); }
};

// UndoEntry
struct TileUndo {
    int              tx, ty;
    int              sliceIndex;
    QVector<uint8_t> pixels; // RGBA8, TILE_SIZE * TILE_SIZE * 4 bytes
};

// キャンバスサイズ変更Undo用の、レイヤー1枚ぶんの全内容スナップショット。
struct LayerSnapshotData {
    QString   name;
    float     opacity   = 1.0f;
    bool      visible   = true;
    BlendMode blendMode = BlendMode::Normal;
    bool      clipping  = false;
    LayerType layerType = LayerType::Normal;
    AdjustmentParams adjustment; // layerType == Adjustment のときのみ意味を持つ
    FilterParams     filter;     // layerType == Filter のときのみ意味を持つ
    std::vector<TextParams> textBoxes; // layerType == Text のときのみ意味を持つ(複数可)
    QColor    solidColor = QColor(255, 255, 255, 255); // layerType == SolidColor のときのみ意味を持つ
    QImage    image; // Format_RGBA8888_Premultiplied, そのスナップショット時点のキャンバスサイズ
                      // (単色レイヤー/調整レイヤーの場合は実ピクセルデータを持たないので、imageは未使用/空のまま)。
    int       childCount = 0; // layerType == Folder のときのみ意味を持つ(Layer::childCount参照)
    bool      hasMask = false; // Layer::hasMask参照
    QImage    maskImage; // hasMask==trueのときのみ使用。imageと同じ形式・キャンバスサイズ
};

// キャンバスサイズ変更(タイルグリッド自体が変わる)専用のUndoデータ。
struct CanvasResizeUndoData {
    int oldW = 0, oldH = 0;
    int newW = 0, newH = 0;
    QVector<LayerSnapshotData> beforeLayers;
    QVector<LayerSnapshotData> afterLayers;
    int activeLayerIndexBefore = 0;
    int activeLayerIndexAfter  = 0;
};

// 選択範囲の変更(作成/追加/削減/解除/全選択)専用のUndoデータ。
struct SelectionUndoData {
    // 変更が起きた矩形(キャンバスpx)。
    int  rectX = 0, rectY = 0, rectW = 0, rectH = 0;
    QByteArray beforeMask; // qCompress済み。rectW*rectH バイトのR8マスク
    QByteArray afterMask;
    bool hadSelectionBefore = false; // 「選択あり」状態(=マスクが全域255ではない)だったか
    bool hasSelectionAfter  = false;
    int  maskW = 0, maskH = 0; // 記録時のキャンバスサイズ(サイズが変わっていたら復元しない)
};

// レイヤー1枚ぶんの中身を丸ごと持つためのデータ(削除/結合で消えたレイヤー、および画像インポートで追加されたレイヤーのRedo用)。
struct RemovedLayerData {
    QString   name;
    LayerType layerType = LayerType::Normal;
    bool      clipping  = false;
    bool      visible   = true;
    float     opacity   = 1.0f;
    BlendMode blendMode = BlendMode::Normal;
    int       originTx = 0, originTy = 0;
    int       tilesX = 0, tilesY = 0;
    int       childCount = 0; // layerType == Folder のときのみ意味を持つ
    QColor           solidColor = QColor(255, 255, 255, 255);
    AdjustmentParams adjustment;
    FilterParams     filter;
    std::vector<TextParams> textBoxes;
    bool      hasMask = false;
    QVector<QByteArray> tiles;     // tilesY*tilesX 個(row-major)。1枚 = TILE_SIZE^2 * 4 バイト
    QVector<QByteArray> maskTiles; // hasMask のときのみ。キャンバス全体のタイル数ぶん
};

// レイヤー追加(新規/クリップ/フォルダー/単色/テキスト/調整)専用のUndoデータ。
struct LayerAddUndoData {
    int insertIndex = -1;         // 追加されたレイヤーのlayers上の位置
    QVector<int> ancestorFolders; // 追加時のスコープ(フォルダーのchildCount増減に使う)
    int activeBefore = 0;         // 追加前のアクティブレイヤー(Undoで戻す先)

    // >= 0 なら「レイヤー複製」で作られたエントリ。
    int duplicateSourceIndex = -1;

    // 画像インポートのように「中身のあるレイヤーが増える」場合に使う。
    bool             hasContent = false;
    RemovedLayerData content;

    // CanvasDocument::addLayer() へ渡し直すためのレイヤー設定。
    QString   name;
    LayerType layerType = LayerType::Normal;
    bool      clipping  = false;
    int       originTx = 0, originTy = 0;
    int       tilesX = 0, tilesY = 0;
    // 作成後に呼び出し側が変えた可能性のある表示設定(クリップ列への複製でclippingをtrueにする等)。
    float     opacity   = 1.0f;
    bool      visible   = true;
    BlendMode blendMode = BlendMode::Normal;
    // タイルを持たない種類のレイヤーの、追加直後の設定値。
    QColor           solidColor = QColor(255, 255, 255, 255); // layerType == SolidColor 用
    AdjustmentParams adjustment;                              // layerType == Adjustment 用
    FilterParams     filter;                                  // layerType == Filter 用
};

// レイヤー削除専用のUndoデータ。
struct LayerRemoveUndoData {
    int  insertIndex = -1;          // 削除された範囲の先頭index
    QVector<int> ancestorFolders;   // 削除時に渡された祖先フォルダー(childCountの増減に使う)
    bool includeContents = true;    // フォルダーを中身ごと消したか(Redoで同じ引数を使う)
    int  activeBefore = 0;
    QVector<RemovedLayerData> layers; // 削除された順(index昇順)。フォルダーなら中身も含む
};

// レイヤー結合専用のUndoデータ。
struct LayerMergeUndoData {
    int survivorIndex = -1;      // 結合前のsurvivorのindex
    int victimIndex   = -1;      // 結合前のvictimのindex
    QVector<int> ancestorFolders;
    int   activeBefore = 0;
    float survivorOpacityBefore = 1.0f; // 結合で1.0へ焼き込まれる前の値

    // 矩形拡張(growLayerBounds)後・ピクセル結合前のsurvivorのタイル生データ。
    int survivorTilesX = 0, survivorTilesY = 0;
    QVector<QByteArray> survivorTiles;

    RemovedLayerData victim; // 消えるvictimの完全な内容
};

enum class UndoKind { LayerTiles, CanvasResize, Selection, LayerAdd, LayerRemove, LayerMerge };

struct UndoEntry {
    UndoKind kind = UndoKind::LayerTiles;

    // kind == LayerTiles で使う(通常のペイント系操作)。
    int               layerIndex = 0;
    QVector<TileUndo> tiles; // 変更されたタイルのみ保存

    // kind == CanvasResize で使う。
    CanvasResizeUndoData resize;

    // kind == Selection で使う。
    SelectionUndoData selection;

    // kind == LayerAdd で使う。
    LayerAddUndoData layerAdd;

    // kind == LayerRemove で使う。
    LayerRemoveUndoData layerRemove;

    // kind == LayerMerge で使う。
    LayerMergeUndoData layerMerge;
};

// CanvasDocument
class CanvasDocument
{
public:
    static constexpr int DEFAULT_MAX_UNDO = 50; // SettingsDialog::Values::undoHistoryLimitの既定値と合わせる
    // GPU側SSBOに確保する最大レイヤー数(composite.comp/render.frag と一致させる)。
    static constexpr int MAX_LAYERS = 8192;

    // Undo履歴の最大保持数(設定ダイアログから変更可能)。
    int  maxUndo() const { return maxUndo_; }
    void setMaxUndo(int n) {
        maxUndo_ = qMax(1, n);
        while (undoStack.size() > maxUndo_) undoStack.removeFirst();
        while (redoStack.size() > maxUndo_) redoStack.removeFirst();
    }

    using SliceAllocFn = std::function<int(int count)>; // count枚を連番確保してベースを返す
    using SliceFreeFn  = std::function<void(int)>;

    // 変更通知
    enum class ChangeKind {
        CacheAndNotify,     // 表示内容または構造の変更
        NotifyOnly,         // 名前などのメタデータ変更
        RepaintOnly,
        ActiveLayerChanged,
    };
    using ChangeCallback = std::function<void(ChangeKind)>;

    // 変更のたびに呼ばれるコールバック。
    ChangeCallback onChanged;

    explicit CanvasDocument(SliceAllocFn allocFn, SliceFreeFn freeFn);

    // タイルグリッドの初期化（initTextures から呼ぶ）。
    void initTileGrid(int canvasW, int canvasH);
    int  tilesX() const { return tilesX_; }
    int  tilesY() const { return tilesY_; }

    // キャンバスのループ設定(横方向=X/左右、縦方向=Y/上下)。
    void setWrap(bool wrapX, bool wrapY) { wrapX_ = wrapX; wrapY_ = wrapY; }
    bool wrapX() const { return wrapX_; }
    bool wrapY() const { return wrapY_; }

    // レイヤー操作 (下→上の順で layers に並ぶ)
    bool addLayer(const QString &name, int insertIndex = -1, bool clipping = false,
                  int originTx = 0, int originTy = 0,
                  int tilesXOverride = -1, int tilesYOverride = -1,
                  LayerType layerType = LayerType::Normal,
                  const QVector<int> &ancestorFolders = {});
    // layerIndexがフォルダーの場合、その中身(childCount枚)ごとまとめて削除する(フォルダーを削除するとその中身も一緒に消える)。
    bool removeLayer(int layerIndex, const QVector<int> &ancestorFolders = {}, bool includeContents = true);

    // レイヤー(またはフォルダー)マスクを追加する。
    bool addLayerMask(int layerIndex);
    // マスクを取り除く(確保していたタイルを解放し、hasMask=falseに戻す)。
    bool removeLayerMask(int layerIndex);

    // 調整レイヤーのLUT(ToneCurve/GradientMap用)のスライスを確保する。
    bool ensureAdjustmentLutSlice(int layerIndex);
    // 確保していたLUTスライスを解放し、lutSlice=-1に戻す。
    void freeAdjustmentLutSlice(int layerIndex);

    // 全レイヤーぶん一括で「そのレイヤーを含むフォルダー(祖先)のindex一覧(外側→内側の順)」を返す(フォルダー自身のエントリにはそのフォルダー自身は含まず、それを囲むさらに外側のフォルダーだけが入る)。
    QVector<QVector<int>> computeAncestorFolders() const;

    // フォルダーのchildCountを、レイヤー配列に対して木構造として成立する値へ丸める。
    void sanitizeFolderChildCounts();

    // レイヤーの矩形を、キャンバスタイル座標系で指定範囲[minTx,maxTxExclusive) x[minTy,maxTyExclusive)を覆うように拡張する(既存の矩形との和集合。縮小はしない)。
    using CopyTileFn  = std::function<void(int srcSlice, int dstSlice)>;
    using ClearTileFn = std::function<void(int sliceIndex)>;
    bool growLayerBounds(int layerIndex, int minCanvasTx, int minCanvasTy,
                          int maxCanvasTxExclusive, int maxCanvasTyExclusive,
                          const CopyTileFn &copyFn, const ClearTileFn &clearFn);

    // レイヤーのタイルグリッドが再構築され(growLayerBounds等で)既存のsliceIndexが無効になったときに呼ぶ。
    void invalidateLayerTileUndoHistory(int layerIndex);

    // 既存レイヤーを並べ替える(GPUスライスの再確保は発生しない、配列内の位置移動のみ)。
    bool moveLayer(int fromIndex, int toIndex, bool newClipping);

    // fromStartから連続するcount枚のレイヤーを、内部の並び順・クリッピング関係を保ったまままとめて移動する(クリッピング行をまるごとドラッグ移動する用)。
    bool moveLayerBlock(int fromStart, int count, int toIndex);

    int       layerCount()                    const { return layers.size(); }
    Layer     layerAt(int layerIndex)         const;
    Layer    &layerRef(int layerIndex);
    QString   layerName(int layerIndex)       const;
    float     layerOpacity(int layerIndex)    const;
    bool      layerVisible(int layerIndex)    const;
    BlendMode layerBlendMode(int layerIndex)  const;
    bool      layerClipping(int layerIndex)   const;

    void setLayerOpacity(int layerIndex, float opacity);
    void setLayerVisible(int layerIndex, bool visible);
    void setLayerName(int layerIndex, const QString &name);
    void setLayerBlendMode(int layerIndex, BlendMode mode);
    void setLayerClipping(int layerIndex, bool clipping);

    // アクティブ状態。
    int  activeLayerIndex() const { return activeLayer_; }
    void setActiveLayer(int layerIndex);

    Layer       &activeLayer();
    const Layer &activeLayer() const;

    // Undo / Redo
    void pushUndo(const UndoEntry &entry);
    void clearRedo();

    bool      canUndo() const { return !undoStack.isEmpty(); }
    bool      canRedo() const { return !redoStack.isEmpty(); }

    // スタック先頭のエントリの種類を覗き見る(undo()/redo()がLayerTiles/CanvasResizeで処理を分岐するために使う)。
    UndoKind  topUndoKind() const { return undoStack.last().kind; }
    UndoKind  topRedoKind() const { return redoStack.last().kind; }

    // スタック先頭のエントリ自体を覗き見る(pop はしない)。
    const UndoEntry &peekUndo() const { return undoStack.last(); }
    const UndoEntry &peekRedo() const { return redoStack.last(); }

    // current: kind==LayerTilesのときのみ使う「取り消す直前の現在の状態」。
    UndoEntry applyUndo(const UndoEntry &current);
    UndoEntry applyRedo(const UndoEntry &current);

    // CanvasResizeエントリの適用(CanvasWidget::rebuildCanvasFromSnapshots)はタイルグリッド自体を作り直すため内部で resetToBlank() を呼び、
    // その副作用で undoStack/redoStack も空になってしまう。
    void restoreRedoEntryAfterRebuild(const UndoEntry &entry) {
        redoStack.push_back(entry);
        if (redoStack.size() > maxUndo_) redoStack.removeFirst();
    }
    void restoreUndoEntryAfterRebuild(const UndoEntry &entry) {
        undoStack.push_back(entry);
        if (undoStack.size() > maxUndo_) undoStack.removeFirst();
    }

    // 変更検知。
    bool isModified() const { return modifiedCount_ != 0; }
    void markSaved()        { modifiedCount_ = 0; }

    // 全レイヤー/Undo履歴を破棄して初期状態に戻す
    void resetToBlank();

    // データ (下→上の順)。
    QVector<Layer> layers;

private:
    int tilesX_ = 0;
    int tilesY_ = 0;
    bool wrapX_ = false;
    bool wrapY_ = false;

    int activeLayer_ = 0;

    int modifiedCount_ = 0;

    QVector<UndoEntry> undoStack;
    QVector<UndoEntry> redoStack;
    int maxUndo_ = DEFAULT_MAX_UNDO;

    SliceAllocFn allocFn_;
    SliceFreeFn  freeFn_;

    bool layerIndexValid(int i) const { return i >= 0 && i < layers.size(); }

    void notify(ChangeKind kind) { if (onChanged) onChanged(kind); }

    // Undo/Redo履歴のlayerIndex整合(構造変更時)
    void reindexUndoOnInsert(int insertedAt);
    void reindexUndoOnRemove(int removedIndex);
    void reindexUndoOnMove(int fromIndex, int toIndex);
};
