#pragma once

#include <QString>
#include <QVector>
#include <QImage>
#include <QColor>
#include <QPointF>
#include <functional>
#include <vector>
#include <QOpenGLWidget>

// ---------------------------------------------------------------------------
// 定数
// ---------------------------------------------------------------------------
static constexpr int TILE_SIZE  = 256;
// タイルを格納するGPUテクスチャ配列(layerTexArray)は GL_MAX_ARRAY_TEXTURE_LAYERS
// (多くのGPUで2048)を超えるスライスを1つに持てない。この上限を実質的に取り除くため、
// 同サイズのテクスチャ配列を最大 MAX_TILE_BANKS 本まで「バンク」として並べ、
// グローバルなスライス番号 si を (bank = si / slicesPerBank, local = si % slicesPerBank)
// に分解して扱う。バンクは必要になった時点で遅延生成する。
// 8本 × (GPUのスライス上限) ぶんまでタイルを確保できる(2048なら16384タイル、
// 例: 16000x16000pxのキャンバスでも複数レイヤー持てる)。値を増やすとより多くの
// タイルを持てるが、フラグメント/コンピュートシェーダーが同時に使えるテクスチャ
// ユニット数(最低保証16)に収まる範囲にすること(バンク用にユニット8..8+N-1を使う)。
static constexpr int MAX_TILE_BANKS = 8;
// バンク用サンプラーを割り当てるテクスチャユニットの先頭(render.frag等が1..7を
// 使うため、その先の8から並べる)。
static constexpr int LAYER_BANK_TEXUNIT_BASE = 8;
// layerTexArrayの初期確保スライス数の下限、かつGPUドライバへの問い合わせに失敗した
// 場合のフォールバック値(OpenGL仕様上の最低保証値と同じ)。実際に使われる上限は
// GLWidget::initTextures()がGL_MAX_ARRAY_TEXTURE_LAYERSを問い合わせて決める
// (LayerSliceAllocatorがこれを上限として、必要になった時点でテクスチャ配列を
// 2倍ずつ伸長していくので、事実上「GPU/VRAMが尽きるまで」レイヤー・タイルを
// 増やせる。Photoshop同様、固定の小さい枚数上限は設けない)。
static constexpr int MAX_SLICES = 2048;

// ---------------------------------------------------------------------------
// ブレンドモード
// ---------------------------------------------------------------------------
// 数値は .tplo の既存保存データ(version 3, Normal/Multiply/Screen/Overlay=0..3)との
// 互換性を保つため、既存4種の値はそのままに、残りはPhotoshopのメニュー順で追記する。
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

// ---------------------------------------------------------------------------
// レイヤー種別
// ---------------------------------------------------------------------------
// SolidColor: 「単色レイヤー」。常に不透明な白でキャンバス全体を塗りつぶす特殊レイヤーで、
// 実際のピクセルデータ(タイル)を一切持たない(常にtilesX()==tilesY()==0)。
// 合成時はシェーダー側で手続き的に不透明白として扱われるため、キャンバスサイズが
// 変わっても常に全面が白いままになる。ペン・移動変形・ブレンドモード変更などの
// 編集機能は使用できない(タイルが無いため書き込みは自然に無視される。
// 移動変形はTransformTool/FreeTransformTool::activate()側で明示的に無効化する)。
// Adjustment: 「調整レイヤー」。実ピクセルデータを持たず(常にtilesX()==tilesY()==0)、
// 自身より下にある全レイヤーの合成結果に対して、非破壊で色調整(明るさ・コントラスト、
// 色相・彩度・明度など)を適用する。PSDの調整レイヤーと同様、種類(kind)ごとに
// 異なるパラメータを持つため、AdjustmentParamsで種類+パラメータのペアとして保持する
// (将来的なpsd入出力を見据えたデータ形式)。
// Text: 「テキストレイヤー」。文字列・フォント等のパラメータ(TextParams)をPSDの
// テキストレイヤー(Type Tool Object)に近い形で保持しつつ、TextToolでの編集のたびに
// そのパラメータからラスタライズしたグリフを実タイルへ焼き込む。合成段階では
// Normalレイヤーと全く同じ扱い(実ピクセルを持つ)なので、シェーダー側の変更は不要。
// Folder: 「フォルダー」。レイヤードックの階層表示のためだけのUIマーカーで、
// SolidColor/Adjustmentと同様に実ピクセルデータを一切持たない(常にtilesX()==
// tilesY()==0)。現時点ではクリッピング等の合成上の意味は持たず、通常のレイヤー
// と同様に合成ループを通るが、タイルが無いため常に透明として扱われ、キャンバス
// には何も描画しない(見た目上は完全な no-op)。
// Filter: 「フィルターレイヤー」。調整レイヤーと同じく実ピクセルデータを持たず
// (常にtilesX()==tilesY()==0)、自身より下にある全レイヤーの合成結果に対して
// 非破壊でフィルター(色収差など)を適用する。調整レイヤーとの決定的な違いは、
// フィルターが「近傍参照」であること: 1画素の出力に周囲の画素が要るため、
// 合成ループの中でその場で計算できない(1タップごとに下の全レイヤー合成を
// やり直すことになる)。そのため合成をこのレイヤーの位置で一度打ち切って
// オフスクリーンへ出し、そこへフィルターをかけてから続きを合成する
// (GLWidget::rebuildFilterChain 参照)。この構造上、タイル単位で合成する
// composite.comp では扱えない(タイルの外が見えないため)ので、
// フィルターレイヤーを含む文書の書き出し・プレビューはキャンバス全面の
// 経路(belowComposite.comp)を通る。
// PSDにはこれに相当するレイヤー種別が無いため、PSD書き出しでは効果を
// ラスタライズせず、単に存在しないものとして扱う(.tploでのみ保持される)。
enum class LayerType : int {
    Normal     = 0,
    SolidColor = 1,
    Adjustment = 2,
    Text       = 3,
    Folder     = 4,
    Filter     = 5,
};
Q_DECLARE_METATYPE(LayerType)

// ---------------------------------------------------------------------------
// 調整レイヤーの種類とパラメータ
// ---------------------------------------------------------------------------
// PSDの調整レイヤーは種類ごとに固有の追加情報ブロック(例: 明るさ・コントラストは
// "brit"、色相・彩度は"hue2")を持つ。それに倣い、ここでも「種類(kind)」+
// 「その種類でのみ意味を持つパラメータ」という构造にしてある。
enum class AdjustmentKind : int {
    BrightnessContrast = 0, // PSD "brit" ブロック相当
    HueSaturation      = 1, // PSD "hue2" ブロック相当
    ColorBalance        = 2, // 無料版でも使える
    ToneCurve             = 3, // 無料版でも使える
    GradientMap            = 4, // Pro限定
};
Q_DECLARE_METATYPE(AdjustmentKind)

// グラデーションマップ調整レイヤーのストップ(dialogs/GradientStripEditor::Stopと
// 同じ形だが、backend層がdialogs層へ依存しないよう独立に定義してある)。
struct AdjustmentGradientStop {
    float  pos = 0.0f; // 0..1
    QColor color;
};

struct AdjustmentParams {
    AdjustmentKind kind = AdjustmentKind::BrightnessContrast;

    // kind == BrightnessContrast のときのみ使用 (BrightnessContrastToolと同じ範囲)
    int brightness = 0; // -100..100
    int contrast   = 0; // -100..100

    // kind == HueSaturation のときのみ使用 (HueSatLightToolと同じ範囲)
    int hue        = 0; // -180..180
    int saturation = 0; // -100..100
    int lightness  = 0; // -100..100

    // kind == ColorBalance のときのみ使用 (ColorBalanceToolと同じ範囲)
    int cyan    = 0; // -100..100
    int magenta = 0; // -100..100
    int yellow  = 0; // -100..100

    // kind == ToneCurve のときのみ使用。制御点(x,yともに0..255)、x昇順、
    // 先頭x=0/末尾x=255(ToneCurveEditor/ToneCurveToolと同じ形式)。
    QVector<QPointF> curvePoints = { QPointF(0, 0), QPointF(255, 255) };

    // kind == GradientMap のときのみ使用(Pro限定)。ストップ列、pos昇順、2個以上。
    QVector<AdjustmentGradientStop> gradientStops = {
        { 0.0f, QColor(0, 0, 0) }, { 1.0f, QColor(255, 255, 255) }
    };
};

// ---------------------------------------------------------------------------
// フィルターレイヤーの種類とパラメータ
// ---------------------------------------------------------------------------
// 調整レイヤー(AdjustmentKind/AdjustmentParams)と同じ「種類(kind) + その種類でのみ
// 意味を持つパラメータ」という構造。種類が増えたらここに足す。
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

    // kind == ChromaticAberration のときのみ使用 (ChromaticAberrationToolと同じ意味)
    int   caMode       = 0;      // 0=平行, 1=円形
    float caAngleDeg   = 0.0f;   // 平行モード時のずらす向き(度、0=水平右向き)
    float caDistancePx = 8.0f;   // ずらす距離(px)
    // 円形モード時の中心。破壊的フィルター版はレイヤーローカルpx座標で持つが、
    // フィルターレイヤーは特定のレイヤーに紐付かないうえ、キャンバスサイズ変更や
    // 解像度変更をまたいで意味を保ちたいので、キャンバスに対する比率(0〜1)で持つ。
    float caCenterU    = 0.5f;
    float caCenterV    = 0.5f;

    // kind == GaussianBlur のときのみ使用 (GaussianBlurToolと同じ意味・同じ既定値)
    float blurRadiusPx = 8.0f;

    // kind == MotionBlur のときのみ使用 (MotionBlurToolと同じ意味)。円形モードの
    // 中心は caCenterU/V と同じ理由でキャンバスに対する比率(0〜1)で持つ。
    int   mbMode         = 0;      // 0=平行, 1=円形
    float mbAngleDeg     = 0.0f;   // 平行モード: ぶれる向き(度)
    float mbDistancePx   = 24.0f;  // 平行モード: ぶれる長さ(px)
    float mbCenterU      = 0.5f;   // 円形モード: 回転中心
    float mbCenterV      = 0.5f;
    float mbAngleSpanDeg = 10.0f;  // 円形モード: 振れ角(度)

    // kind == LensBlur のときのみ使用 (LensBlurToolと同じ意味・同じ既定値。Pro限定)
    float lbRadiusPx        = 16.0f;
    int   lbBlades           = 0;      // 0=円, 3以上=正多角形の辺数
    float lbBladeRotDeg     = 0.0f;
    float lbHighlightBoost = 0.6f;
    float lbThreshold        = 0.7f;

    // kind == Mosaic のときのみ使用 (MosaicToolと同じ意味・同じ既定値)
    int   mzBlockSize = 16;

    // kind == Noise のときのみ使用 (NoiseToolと同じ意味・同じ既定値)。乱数の種は
    // フィルターレイヤーの生成時に1度だけ決め、以後は固定する(NoiseToolがactivate()
    // 時に1度だけ決めるのと同じ理由: 再合成のたびに振り直すと粒がチラチラする)。
    float    nsStrength    = 0.25f;
    bool     nsMonochrome  = true;
    float    nsGrainPx     = 1.0f;
    unsigned int nsSeed    = 0;
};

// ---------------------------------------------------------------------------
// テキストレイヤーのパラメータ
// ---------------------------------------------------------------------------
// PSDのテキストレイヤー(Type Tool Object、"TySh"リソース)に倣い、実ピクセルとは
// 別に「文字列+フォント+色+配置」を独立したデータとして保持する。TextTool側は
// これを直接編集し、変更のたびにGLWidget::rasterizeTextLayer()がここから
// グリフをラスタライズしてタイルへ焼き込む(=見た目は実ピクセルだが、後から
// 再編集する際は毎回このパラメータから作り直すので実質非破壊)。
// 1つのテキストレイヤーは複数のTextParams(=テキストボックス)を持てる
// (Layer::textBoxes)。各ボックスは中心(cx,cy)・基準サイズ(width,height)・
// 回転(rotation, 度)・一様スケール(scale)を持ち、四隅ドラッグで拡縮すると
// widthとheightは変えずscaleだけを変える(=フォントサイズも含め見た目全体が
// 比例して伸縮する)。widthとheightはscale=1の時のワードラップ矩形のサイズ。
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

// ---------------------------------------------------------------------------
// Layer
// ---------------------------------------------------------------------------
// レイヤーはこの1種類のみで、Photoshop同様「下→上」の一列に並ぶ
// (旧「レーン」概念は廃止)。
//
// clipping = true のレイヤーは、自分より下にある直近の
// 「非クリッピングレイヤー(=クリッピングの土台)」にクリップされる
// (そのレイヤーのアルファ形状でマスクされる、標準的な単一チェーンのクリッピングマスク)。
// 土台自身が別のレイヤーにクリップされていても、そのままチェーンして辿る。
// ---------------------------------------------------------------------------
struct Layer {
    // tiles[ty][tx] = layerTexArray のスライス番号 (連番前提で確保される)。
    // tiles[0][0] がキャンバスタイル座標 (originTx, originTy) に対応する
    // (originTx/originTyはタイル単位。負値・キャンバス範囲外も許容し、
    //  レイヤーの矩形がキャンバスより大きい/はみ出した状態を表せる)。
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
    // layerType == Folder のときのみ意味を持つ。このフォルダーの直後に連続する
    // childCount枚(ネストしたフォルダーはその中身も含めた総数)が「このフォルダーの
    // 中身」を表す(layersはPhotoshopのグループ同様、フォルダー内の要素を物理的に
    // 連続した範囲としてフラットに保持する)。addLayer/removeLayerが増減を維持する。
    int       childCount = 0;
    // レイヤー(またはフォルダー)マスクを持つか(1枚につき最大1枚)。
    bool      hasMask = false;
    // hasMask==trueのときのみ意味を持つ。マスクはtilesと違って原点・矩形を持たず、
    // 常にキャンバス全体(doc.tilesX() x doc.tilesY())を覆う(originは常に(0,0)固定)。
    // レイヤー本体のtilesと同じ layerTexArray のスライスを共有する(単なる別グループの
    // タイル)。値はグレースケール相当(R=G=B=マスク濃度、A=255)としてRGBA8で保持し、
    // PSD書き出し時にどれか1チャンネルをそのままマスクデータとして取り出せる形にしてある。
    QVector<QVector<int>> maskTiles;
    // マスクに一度でもペンで描き込まれたか。UI(不透明度プレビュー)がマスク編集モードに
    // 入るときに白マスクを自動生成し、一度も描かれずに編集モードを抜けたら破棄する判定に使う
    // (「一律の不透明度で足りるなら数値だけ、描かれたらマスクを保持」を実現するため)。
    // GPU上にしか無いマスク画素を毎回読み戻さず安価に判定できるようにするフラグ。
    bool      maskDirty = false;

    // layerType == Adjustment かつ adjustment.kind が ToneCurve/GradientMap のときのみ
    // 使用する、LUT(256x1、layerTexArrayの1スライスの左上行だけを使う)のスライス番号
    // (無ければ-1)。tiles/maskTilesと違い、undo/複製/キャンバスリサイズ/ファイル
    // 読み込みのどの経路でも一切引き継がない(常に-1のまま新しいLayerが作られる)。
    // 理由: LUTの中身はcurvePoints/gradientStops(このLayerに含まれ、通常通り
    // コピー/永続化される)から毎回再構築できるので、GPU側の実体は単なるキャッシュ
    // でしかない。CanvasCompositor::updateLayerSSBOsが毎フレーム
    // 「今の種類がLUTを要るならensure+再構築、要らなくなっていればfree」を
    // 自己修復的に行うため、複製や元に戻す操作のたびに個別のスライス管理コードを
    // 書く必要が無い(2枚のLayerが同じスライスを共有してしまう事故も起きない)。
    int       lutSlice = -1;

    // レイヤーローカルのタイル座標(0オリジン)で指定タイルの sliceIndex を返す
    int tileSlice(int tx, int ty) const {
        if (ty < 0 || ty >= tiles.size()) return -1;
        if (tx < 0 || tx >= tiles[ty].size()) return -1;
        return tiles[ty][tx];
    }

    // キャンバスタイル座標(原点を考慮)で指定タイルの sliceIndex を返す。
    // レイヤーの矩形外なら -1 (=データなし、透明として扱う)
    int tileSliceAtCanvasTile(int canvasTx, int canvasTy) const {
        return tileSlice(canvasTx - originTx, canvasTy - originTy);
    }

    int tilesX() const { return tiles.isEmpty() ? 0 : tiles[0].size(); }
    int tilesY() const { return tiles.size(); }

    // マスクタイル版のtileSlice/tileSliceAtCanvasTile。マスクは原点が常に(0,0)固定
    // なので、キャンバスタイル座標がそのままマスクローカル座標になる(原点補正不要)。
    int maskTileSlice(int tx, int ty) const {
        if (ty < 0 || ty >= maskTiles.size()) return -1;
        if (tx < 0 || tx >= maskTiles[ty].size()) return -1;
        return maskTiles[ty][tx];
    }
    int maskTilesX() const { return maskTiles.isEmpty() ? 0 : maskTiles[0].size(); }
    int maskTilesY() const { return maskTiles.size(); }

    // 旧 texArrayIndex 互換: tile[0][0] を返す
    int texArrayIndex() const { return tileSlice(0, 0); }
};

// ---------------------------------------------------------------------------
// UndoEntry
// ---------------------------------------------------------------------------
struct TileUndo {
    int              tx, ty;
    int              sliceIndex;
    QVector<uint8_t> pixels; // RGBA8, TILE_SIZE * TILE_SIZE * 4 bytes
};

// キャンバスサイズ変更Undo用の、レイヤー1枚ぶんの全内容スナップショット
// (タイル分割前の、キャンバス全体を1枚のQImageにまとめた状態)
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
                      // (単色レイヤー/調整レイヤーの場合は実ピクセルデータを持たないので、imageは未使用/空のまま)
    int       childCount = 0; // layerType == Folder のときのみ意味を持つ(Layer::childCount参照)
    bool      hasMask = false; // Layer::hasMask参照
    QImage    maskImage; // hasMask==trueのときのみ使用。imageと同じ形式・キャンバスサイズ
};

// キャンバスサイズ変更(タイルグリッド自体が変わる)専用のUndoデータ。
// 変更前後の全レイヤー内容を丸ごと保持し、Undo/Redoどちらも同じデータから直接復元する
// (通常のTileUndoのような差分方式ではなく、構造が変わる操作のため全体スナップショット方式)。
struct CanvasResizeUndoData {
    int oldW = 0, oldH = 0;
    int newW = 0, newH = 0;
    QVector<LayerSnapshotData> beforeLayers;
    QVector<LayerSnapshotData> afterLayers;
    int activeLayerIndexBefore = 0;
    int activeLayerIndexAfter  = 0;
};

// 選択範囲の変更(作成/追加/削減/解除/全選択)専用のUndoデータ。
// CanvasResizeUndoDataと同じく変更前後を自己完結して持ち、Undo/Redoどちらも
// このデータから直接復元する。
//
// マスクはキャンバス全域のR8(1バイト/px)なので、フルHDなら1枚2MBになる。
// 履歴に何十件も積むと無視できない量になるが、選択マスクは実際には0か255が
// 大きな塊で続くだけなのでよく圧縮される。qCompress/qUncompressで圧縮して
// 保持し、通常の選択なら数KB程度に収める。
struct SelectionUndoData {
    // 変更が起きた矩形(キャンバスpx)。マスク全体ではなくこの矩形ぶんだけを持つ。
    // 全域を持つとフルHDで1枚2MB、圧縮にも毎回数十msかかっていた(選択操作の
    // 体感の重さの一因)。実際に変わるのは描いた形の外接矩形だけなので、そこに絞る。
    int  rectX = 0, rectY = 0, rectW = 0, rectH = 0;
    QByteArray beforeMask; // qCompress済み。rectW*rectH バイトのR8マスク
    QByteArray afterMask;
    bool hadSelectionBefore = false; // 「選択あり」状態(=マスクが全域255ではない)だったか
    bool hasSelectionAfter  = false;
    int  maskW = 0, maskH = 0; // 記録時のキャンバスサイズ(サイズが変わっていたら復元しない)
};

// レイヤー1枚ぶんの中身を丸ごと持つためのデータ(削除/結合で消えたレイヤー、
// および画像インポートで追加されたレイヤーのRedo用)。
// LayerSnapshotDataと違い、QImage(キャンバスサイズに切り詰められる)ではなく
// タイルの生データをそのまま持つため、キャンバス外へはみ出した領域も失わない。
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
//
// 削除・複製・結合はCanvasResize方式(全レイヤーのスナップショット2セット)を
// 流用しているが、あちらは非圧縮のQImageをレイヤー枚数×2枚ぶん持つため
// 1エントリで数十〜数百MBになる。レイヤー追加は最も頻度の高い操作な上、
// 「追加直後のレイヤーは必ず中身が空」なのでピクセルデータを持つ必要が無い。
// そこで再作成に要るパラメータだけを持つ軽量エントリにしてある
// (Undo=そのレイヤーを削除 / Redo=同じ設定で作り直す、で完全に元へ戻る)。
struct LayerAddUndoData {
    int insertIndex = -1;         // 追加されたレイヤーのlayers上の位置
    QVector<int> ancestorFolders; // 追加時のスコープ(フォルダーのchildCount増減に使う)
    int activeBefore = 0;         // 追加前のアクティブレイヤー(Undoで戻す先)

    // >= 0 なら「レイヤー複製」で作られたエントリ。Redoでは空のレイヤーを作るのではなく
    // このindexのレイヤーから複製し直す(複製内容は既存レイヤーのコピーなので、ここでも
    // ピクセルデータを持つ必要が無い)。Undo/Redoは線形なので、Redoする時点の
    // 複製元レイヤーの中身は複製した当時と必ず一致する。
    int duplicateSourceIndex = -1;

    // 画像インポートのように「中身のあるレイヤーが増える」場合に使う。Redoでは
    // 空のレイヤーを作るのではなく、ここに保存した内容ごと作り直す。増えるのは
    // 1枚だけなので、全レイヤーのスナップショット2セットを持つ必要は無い。
    bool             hasContent = false;
    RemovedLayerData content;

    // CanvasDocument::addLayer() へ渡し直すためのレイヤー設定
    QString   name;
    LayerType layerType = LayerType::Normal;
    bool      clipping  = false;
    int       originTx = 0, originTy = 0;
    int       tilesX = 0, tilesY = 0;
    // 作成後に呼び出し側が変えた可能性のある表示設定(クリップ列への複製で
    // clippingをtrueにする等)。Redoでは作り直したあとにこれらを復元する。
    float     opacity   = 1.0f;
    bool      visible   = true;
    BlendMode blendMode = BlendMode::Normal;
    // タイルを持たない種類のレイヤーの、追加直後の設定値
    QColor           solidColor = QColor(255, 255, 255, 255); // layerType == SolidColor 用
    AdjustmentParams adjustment;                              // layerType == Adjustment 用
    FilterParams     filter;                                  // layerType == Filter 用
};

// レイヤー削除専用のUndoデータ。
//
// 削除・結合はCanvasResize方式(全レイヤーのスナップショット2セット)を流用していたが、
// 全レイヤーを非圧縮でGPU→CPUへ読み戻すため、20枚のドキュメントでは1回の削除で
// 数百MBの読み戻しが発生していた。実際に復元が要るのは「消えたレイヤーだけ」なので、
// そこだけを持つ(Undo=保存したレイヤーを挿入し直す / Redo=もう一度削除する)。
struct LayerRemoveUndoData {
    int  insertIndex = -1;          // 削除された範囲の先頭index
    QVector<int> ancestorFolders;   // 削除時に渡された祖先フォルダー(childCountの増減に使う)
    bool includeContents = true;    // フォルダーを中身ごと消したか(Redoで同じ引数を使う)
    int  activeBefore = 0;
    QVector<RemovedLayerData> layers; // 削除された順(index昇順)。フォルダーなら中身も含む
};

// レイヤー結合専用のUndoデータ。結合で変わるのは「survivorのピクセル」と
// 「victimが消えること」の2つだけなので、その2枚ぶんだけを持つ
// (LayerRemoveUndoDataと同じ理由で、全レイヤーのスナップショットは要らない)。
struct LayerMergeUndoData {
    int survivorIndex = -1;      // 結合前のsurvivorのindex
    int victimIndex   = -1;      // 結合前のvictimのindex
    QVector<int> ancestorFolders;
    int   activeBefore = 0;
    float survivorOpacityBefore = 1.0f; // 結合で1.0へ焼き込まれる前の値

    // 矩形拡張(growLayerBounds)後・ピクセル結合前のsurvivorのタイル生データ。
    // 拡張ぶんのタイルは透明クリア済みの状態で入るので、これを書き戻せば
    // 矩形を縮めなくても結合前と同じ見た目に戻る。
    int survivorTilesX = 0, survivorTilesY = 0;
    QVector<QByteArray> survivorTiles;

    RemovedLayerData victim; // 消えるvictimの完全な内容
};

enum class UndoKind { LayerTiles, CanvasResize, Selection, LayerAdd, LayerRemove, LayerMerge };

struct UndoEntry {
    UndoKind kind = UndoKind::LayerTiles;

    // kind == LayerTiles で使う(通常のペイント系操作)
    int               layerIndex = 0;
    QVector<TileUndo> tiles; // 変更されたタイルのみ保存

    // kind == CanvasResize で使う
    CanvasResizeUndoData resize;

    // kind == Selection で使う
    SelectionUndoData selection;

    // kind == LayerAdd で使う
    LayerAddUndoData layerAdd;

    // kind == LayerRemove で使う
    LayerRemoveUndoData layerRemove;

    // kind == LayerMerge で使う
    LayerMergeUndoData layerMerge;
};

// ---------------------------------------------------------------------------
// CanvasDocument
// ---------------------------------------------------------------------------
class CanvasDocument
{
public:
    static constexpr int DEFAULT_MAX_UNDO = 50; // SettingsDlg::Values::undoHistoryLimitの既定値と合わせる
    // GPU側SSBOに確保する最大レイヤー数(composite.comp/render.frag と一致させる)。
    // 1レイヤーあたりのSSBO消費は数バイト~十数バイト程度なので、大きくしても
    // メモリ的にはほぼ無視できる(Photoshop同様、実用上ここが先にボトルネックに
    // なることはまず無い想定)。
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

    // ------------------------------------------------------------------
    // 変更通知
    // ------------------------------------------------------------------
    // どの粒度の変更が起きたかを表す。呼び出し側(GLWidget)はこれを見て
    // 必要な範囲だけ再描画/キャッシュ更新/シグナル発行を行う。
    enum class ChangeKind {
        CacheAndNotify,     // update(); emit layersChanged();
                            // (レイヤーopacity/blendMode/clipping/visible、構造変更。
                            //  見た目に直接影響するプロパティはすべてここに含める)
        NotifyOnly,         // emit layersChanged();
                            // (レイヤーname。見た目に影響しないメタデータのみ)
        RepaintOnly,        // update();
                            // (未使用箇所の予約。将来のプロパティ変更用)
        ActiveLayerChanged, // update();
                            // (アクティブレイヤー切り替え)
    };
    using ChangeCallback = std::function<void(ChangeKind)>;

    // 変更のたびに呼ばれるコールバック。GLWidget側で1箇所だけ購読する想定。
    ChangeCallback onChanged;

    explicit CanvasDocument(SliceAllocFn allocFn, SliceFreeFn freeFn);

    // タイルグリッドの初期化（initTextures から呼ぶ）
    void initTileGrid(int canvasW, int canvasH);
    int  tilesX() const { return tilesX_; }
    int  tilesY() const { return tilesY_; }

    // キャンバスのループ設定(横方向=X/左右、縦方向=Y/上下)。新規作成時にのみ
    // 設定され、resetToBlank()では保持される(tilesX_/tilesY_と同様、次の
    // initTileGrid呼び出しまでは古い値が残る前提だが、recreateCanvas側で
    // 必ずセットし直すので実害はない)。
    void setWrap(bool wrapX, bool wrapY) { wrapX_ = wrapX; wrapY_ = wrapY; }
    bool wrapX() const { return wrapX_; }
    bool wrapY() const { return wrapY_; }

    // ------------------------------------------------------------------
    // レイヤー操作 (下→上の順で layers に並ぶ)
    // ------------------------------------------------------------------
    // insertIndex: -1 なら一番上(最前面)に追加。clipping: 直下にクリップするか。
    // originTx/originTy/tilesXOverride/tilesYOverride: 省略時(tilesXOverride<0)は
    // キャンバス全面(原点0,0、doc全体のtilesX_/tilesY_)を確保する通常の挙動。
    // 明示的に指定すると、その矩形(キャンバスより大きい/はみ出した位置も可)で確保する
    // (duplicateLayerが複製元レイヤーの矩形をそのまま引き継ぐために使う)。
    // 戻り値: true=成功, false=失敗
    // layerType: SolidColor/Adjustment/Folderを指定すると、実際のタイルを一切確保しない
    // (originTx/originTy/tilesXOverride/tilesYOverrideは無視され、常に0x0で作成される)。
    // ancestorFolders: 新規レイヤーを挿入するフォルダーの階層チェーン(外側→内側の順、
    // どの順でもよい)。挿入位置(insertIndex)が実際にそのフォルダー(群)の中身の
    // 範囲内になるよう呼び出し側が計算していることが前提で、指定した各フォルダーの
    // Layer::childCountをここで+1する(トップレベルへの挿入なら空のまま呼ぶ)。
    bool addLayer(const QString &name, int insertIndex = -1, bool clipping = false,
                  int originTx = 0, int originTy = 0,
                  int tilesXOverride = -1, int tilesYOverride = -1,
                  LayerType layerType = LayerType::Normal,
                  const QVector<int> &ancestorFolders = {});
    // layerIndexがフォルダーの場合、その中身(childCount枚)ごとまとめて削除する
    // (フォルダーを削除するとその中身も一緒に消える)。ancestorFoldersは
    // addLayer同様、削除対象が実際に属している外側のフォルダー階層チェーンを渡し、
    // 削除された枚数ぶんchildCountを減算する。
    // includeContents=falseにすると、layerIndexがフォルダーであっても中身は
    // 巻き込まず、マーカー自体(1枚)だけを取り除く(中身はそのままの位置に残り、
    // 実質的にフォルダーの外側へ「繰り上がる」)。フォルダーの中身をすべて結合して
    // 1枚のレイヤーにまとめた後、空になったマーカーだけを消す用途に使う。
    bool removeLayer(int layerIndex, const QVector<int> &ancestorFolders = {}, bool includeContents = true);

    // レイヤー(またはフォルダー)マスクを追加する。すでに持っている場合は何もせず
    // falseを返す。マスク用のタイル(常にキャンバス全体、tilesX_ x tilesY_枚)を
    // 新規に連続確保するだけで、実際のピクセルクリア(GL側の処理)は呼び出し側
    // (GLWidget::addLayerMask)が行う。
    bool addLayerMask(int layerIndex);
    // マスクを取り除く(確保していたタイルを解放し、hasMask=falseに戻す)。
    // マスクを持っていない場合は何もせずfalseを返す。
    bool removeLayerMask(int layerIndex);

    // 調整レイヤーのLUT(ToneCurve/GradientMap用)のスライスを確保する。
    // 既に確保済み(lutSlice>=0)ならそのまま何もせずtrueを返す。中身の構築・
    // アップロードは呼び出し側(CanvasCompositor::updateLayerSSBOs)が行う
    // (ここでは確保だけ。Layer::lutSliceのコメント参照)。
    bool ensureAdjustmentLutSlice(int layerIndex);
    // 確保していたLUTスライスを解放し、lutSlice=-1に戻す。未確保なら何もしない。
    void freeAdjustmentLutSlice(int layerIndex);

    // 全レイヤーぶん一括で「そのレイヤーを含むフォルダー(祖先)のindex一覧
    // (外側→内側の順)」を返す(フォルダー自身のエントリにはそのフォルダー自身は
    // 含まず、それを囲むさらに外側のフォルダーだけが入る)。フォルダー単位の
    // 表示・不透明度・マスクをレイヤーの合成へカスケードするために
    // CanvasCompositor::updateLayerSSBOsが使う。O(layerCount)の1回のツリー
    // 走査で全レイヤーぶんまとめて求める。
    QVector<QVector<int>> computeAncestorFolders() const;

    // フォルダーのchildCountを、レイヤー配列に対して木構造として成立する値へ丸める。
    // 他形式からの取り込み(PsdCodec)や壊れたファイルで矛盾した値が入ると、
    // childCountでindexを進める全ての箇所が配列外アクセスになるため、
    // 読み込み完了時に一度必ず通す(実装のコメント参照)。
    void sanitizeFolderChildCounts();

    // レイヤーの矩形を、キャンバスタイル座標系で指定範囲[minTx,maxTxExclusive) x
    // [minTy,maxTyExclusive)を覆うように拡張する(既存の矩形との和集合。縮小はしない)。
    // 既存タイルは新しい連続スライスブロックへcopyFn経由でコピーし直し
    // (composite.comp/render.fragが「1レイヤー=1連続スライスブロック」を前提とするため)、
    // 新規タイルはclearFnで透明クリアする(実際のGL処理は呼び出し側=GLWidgetが担当する)。
    // スライス番号が再割り当てされるため、拡張が発生した場合は呼び出し側で
    // invalidateLayerTileUndoHistory()も呼ぶこと。
    // 戻り値: 実際に拡張が発生したか(falseなら既に範囲を満たしていた)。
    using CopyTileFn  = std::function<void(int srcSlice, int dstSlice)>;
    using ClearTileFn = std::function<void(int sliceIndex)>;
    bool growLayerBounds(int layerIndex, int minCanvasTx, int minCanvasTy,
                          int maxCanvasTxExclusive, int maxCanvasTyExclusive,
                          const CopyTileFn &copyFn, const ClearTileFn &clearFn);

    // レイヤーのタイルグリッドが再構築され(growLayerBounds等で)既存のsliceIndexが
    // 無効になったときに呼ぶ。そのレイヤーを指すLayerTiles系Undo/Redoエントリだけを
    // 取り除く(CanvasResize系エントリはQImageスナップショットのみを保持し
    // sliceIndexに依存しないため対象外、かつ他レイヤーの履歴も保持される)。
    void invalidateLayerTileUndoHistory(int layerIndex);

    // 既存レイヤーを並べ替える(GPUスライスの再確保は発生しない、配列内の位置移動のみ)。
    // toIndex は移動後の配列における最終的な位置。newClipping で移動後のクリッピング
    // 状態(root行に置くか、既存行へのクリップ列にするか)を設定する。
    bool moveLayer(int fromIndex, int toIndex, bool newClipping);

    // fromStartから連続するcount枚のレイヤーを、内部の並び順・クリッピング関係を
    // 保ったまままとめて移動する(クリッピング行をまるごとドラッグ移動する用)。
    // toIndex は moveLayer 同様、移動後(=ブロックを取り除いた後)の配列における
    // ブロック先頭の位置。
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

    // ------------------------------------------------------------------
    // アクティブ状態
    // ------------------------------------------------------------------
    int  activeLayerIndex() const { return activeLayer_; }
    void setActiveLayer(int layerIndex);

    Layer       &activeLayer();
    const Layer &activeLayer() const;

    // ------------------------------------------------------------------
    // Undo / Redo
    // ------------------------------------------------------------------
    void pushUndo(const UndoEntry &entry);
    void clearRedo();

    bool      canUndo() const { return !undoStack.isEmpty(); }
    bool      canRedo() const { return !redoStack.isEmpty(); }

    // スタック先頭のエントリの種類を覗き見る(undo()/redo()がLayerTiles/CanvasResizeで
    // 処理を分岐するために使う)。呼び出し前に canUndo()/canRedo() を確認すること。
    UndoKind  topUndoKind() const { return undoStack.last().kind; }
    UndoKind  topRedoKind() const { return redoStack.last().kind; }

    // スタック先頭のエントリ自体を覗き見る(pop はしない)。GLWidget::undo()/redo() が
    // 「これから復元するエントリが実際に触れるタイルだけ」を先読みして、"current"
    // (取り消す直前の状態)をレイヤー全体ではなく差分ぶんだけキャプチャするために使う。
    // 呼び出し前に canUndo()/canRedo() を確認すること。
    const UndoEntry &peekUndo() const { return undoStack.last(); }
    const UndoEntry &peekRedo() const { return redoStack.last(); }

    // current: kind==LayerTilesのときのみ使う「取り消す直前の現在の状態」。
    // kind==CanvasResizeのエントリは変更前後の内容を自己完結して持っているため
    // currentは無視される(呼び出し側はUndoEntry{}を渡してよい)。
    UndoEntry applyUndo(const UndoEntry &current);
    UndoEntry applyRedo(const UndoEntry &current);

    // CanvasResizeエントリの適用(GLWidget::rebuildCanvasFromSnapshots)はタイルグリッド自体を
    // 作り直すため内部で resetToBlank() を呼び、その副作用で undoStack/redoStack も
    // 空になってしまう。適用直後にこのエントリ自身を該当スタックへ積み直すことで、
    // このリサイズ操作自体は改めてUndo/Redoできるようにする(見た目上のクリアより前の
    // タイル差分方式の履歴は、タイルグリッドが変わった時点で無効になるため破棄されたままでよい)。
    void restoreRedoEntryAfterRebuild(const UndoEntry &entry) {
        redoStack.push_back(entry);
        if (redoStack.size() > maxUndo_) redoStack.removeFirst();
    }
    void restoreUndoEntryAfterRebuild(const UndoEntry &entry) {
        undoStack.push_back(entry);
        if (undoStack.size() > maxUndo_) undoStack.removeFirst();
    }

    // 変更検知
    bool isModified() const { return modifiedCount_ != 0; }
    void markSaved()        { modifiedCount_ = 0; }

    // 全レイヤー/Undo履歴を破棄して初期状態に戻す
    // (allocFn_/freeFn_/onChanged は保持される)
    void resetToBlank();

    // ------------------------------------------------------------------
    // データ (下→上の順)
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    // Undo/Redo履歴のlayerIndex整合(構造変更時)
    // ------------------------------------------------------------------
    // UndoEntry(kind==LayerTiles)のlayerIndexは「その時点のlayers配列でのインデックス」を
    // 指す固定値であり、レイヤーの挿入/削除/並べ替えでlayers配列の並びが変わっても
    // 自動追従しない。放置すると、削除で配列が詰まった後は別レイヤーのインデックスを
    // 指してしまい(≒無関係なレイヤーを巻き込む)、削除されたレイヤー自身のエントリは
    // 「解放され再利用されうる(LayerSliceAllocatorのフリーリスト)スライス」を指したまま
    // 残ってしまう(Undo適用時に無関係な新しいレイヤーの中身を上書きしてしまう、または
    // activeLayer_に範囲外の値が入りクラッシュする)。そのため構造変更のたびに
    // undoStack/redoStackの該当エントリを整理する。
    // CanvasResize系エントリは変更前後の全レイヤーを自己完結したスナップショットとして
    // 持つため対象外(layers配列の現在のインデックスに依存しない)。
    void reindexUndoOnInsert(int insertedAt);
    void reindexUndoOnRemove(int removedIndex);
    void reindexUndoOnMove(int fromIndex, int toIndex);
};
