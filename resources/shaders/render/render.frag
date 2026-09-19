#version 430 core
//COMMON_INCLUDE

in vec2 uv;
out vec4 fragColor;

// タイル配列は GL_MAX_ARRAY_TEXTURE_LAYERS の上限を超えるため複数の「バンク」
// (それぞれ sampler2DArray)へ分割されている。グローバルなスライス番号 si は
// (bank = si / uSlicesPerBank, local = si % uSlicesPerBank) に分解して読む。
// MAX_BANKS は C++ 側 CanvasDocument.h の MAX_TILE_BANKS と一致させること。
#define MAX_BANKS 8
uniform sampler2DArray layerTexBanks[MAX_BANKS];
uniform int uSlicesPerBank;
// フラグメントシェーダーでも、バンク配列を「for + 定数index(b)」で引くのは
// dynamically uniform で安全。ミップ無しなので textureLod(...,0.0) を使う
// (非一様な制御フロー内でも微分不要で確実に動く)。
vec4 sampleTileArray(int si, vec2 uv) {
    int bank = si / uSlicesPerBank;
    float local = float(si - bank * uSlicesPerBank);
    for (int b = 0; b < MAX_BANKS; b++)
        if (b == bank) return textureLod(layerTexBanks[b], vec3(uv, local), 0.0);
    return vec4(0.0);
}
// 調整レイヤーの色調整本体(composite.compのadjustLayerColorと同じ。詳細はそちらの
// コメント参照)。
vec3 adjustLayerColor(int kind, vec3 rgb, vec3 p, int baseSlice) {
    if (kind == 1) return adjustBrightnessContrast(rgb, p.x, p.y);
    if (kind == 2) return adjustHSL(rgb, p.x, p.y, p.z);
    if (kind == 4) return adjustColorBalance(rgb, p.x, p.y, p.z);
    if (kind == 5) {
        if (baseSlice < 0) return rgb;
        return vec3(
            sampleTileArray(baseSlice, vec2(clamp(rgb.r, 0.0, 1.0), 0.5 / 256.0)).r,
            sampleTileArray(baseSlice, vec2(clamp(rgb.g, 0.0, 1.0), 0.5 / 256.0)).r,
            sampleTileArray(baseSlice, vec2(clamp(rgb.b, 0.0, 1.0), 0.5 / 256.0)).r
        );
    }
    if (kind == 6) {
        if (baseSlice < 0) return rgb;
        return sampleTileArray(baseSlice, vec2(clamp(blendLuma(rgb), 0.0, 1.0), 0.5 / 256.0)).rgb;
    }
    return rgb;
}
// グラデーションマップ(Pro限定)の256x1 RGBA8 LUT。輝度を横位置として引く
// (toneCurveLUTTexと同じ考え方。テクスチャユニット0は他で使っていないのでここに置く)。
layout(binding = 0) uniform sampler2D      gradientMapLUTTex;
layout(binding = 2) uniform sampler2D      maskTex;
layout(binding = 3) uniform sampler2D      selectionMaskTex;
layout(binding = 4) uniform sampler2D      gaussianBlurPreviewTex;
layout(binding = 5) uniform sampler2D      toneCurveLUTTex;
// item4: 「アクティブレイヤーより下」の合成結果キャッシュ(GLWidget::updateBelowCompositeCache
// 参照)。uUseBelowCompositeCache!=0のときだけ使う。ストローク中は自レイヤー以外の
// ピクセルが変わらないことを利用し、z=アクティブレイヤーより下の再合成を省略する。
layout(binding = 6) uniform sampler2D      belowCompositeTex;
layout(binding = 7) uniform sampler2D      belowCompositeClipBaseTex;
uniform int uUseBelowCompositeCache;
// belowCompositeTex が「どこまでを合成済みか」。uUseBelowCompositeCache!=0 のとき
// ループはこのzから始まる。ストローク中のキャッシュならアクティブレイヤーのindex、
// フィルターレイヤーがある文書なら一番上のフィルターレイヤーの1つ上。
uniform int uCompositeStartZ;
// 「アクティブレイヤーより上」の事前合成キャッシュ(GLWidget::updateAboveCompositeCache
// 参照)。アクティブより上のレイヤーが全て通常ブレンド・非クリッピング・非調整のときだけ
// 有効化され、それらを1枚に事前合成したもの。有効時はループをアクティブレイヤーで
// 打ち切り、最後にこれを1回normalBlendで重ねる(上に多数レイヤーがあっても
// ストローク中の合成コストが一定になる)。
layout(binding = 1) uniform sampler2D      aboveCompositeTex;
uniform int uUseAboveCompositeCache;

// 表示上の見た目だけを変えるカラーモード(実データは常にRGBAのまま)。
// 0=RGB(無変換) 1=CMYK(擬似変換) 2=グレースケール(輝度ベース) 3=グレースケール(明度ベース)。
uniform int uColorMode;

// モニターキャリブレーション。カラーモード変換後の最終出力に対して常に適用する
// 「表示調整」(モニター環境のクセを補正するためのもの)。adjustBrightnessContrast/
// adjustColorBalance(共にcommon.glsl)と同じ引数の意味。
uniform float uCalBrightness;
uniform float uCalContrast;
uniform float uCalCyan;
uniform float uCalMagenta;
uniform float uCalYellow;

// キャンバス外側(枠外)を塗る背景色。実データ(レイヤーのRGBA)には無関係の
// 表示上の設定(以前はvec4(0.5,0.5,0.5,1.0)にハードコードされていた)。
uniform vec3 uCanvasOutsideBg;
uniform vec3 uCheckerColorA; // 市松模様(暗い方)。ThemeColors::checkerDark由来
uniform vec3 uCheckerColorB; // 市松模様(明るい方)。ThemeColors::checkerLight由来

// 描画設定
uniform vec4  uBrushColor;
uniform vec2  uCanvasSize;
// 表示倍率(キャンバスpx→デバイスpx)。1.0以上=拡大表示のときだけサンプル位置を
// 画素中心へ吸着させる(下のmain()のコメント参照)。
uniform float uViewScale;
// 縮小表示時に1画素あたり何点サンプルするか(1辺の数。1なら等倍/拡大扱い)。
// C++側が 1/倍率 から決める(GLWidget::paintGL)。
uniform int   uMinifySamples;
uniform vec2  uWindowSize;
// 「消す」モード(消しゴムツール、または透明色を選んでいるとき)。bake.compの
// uEraseModeと同じ意味で、削る強さも同じくuBrushColor.aから読む。
uniform int   uEraseMode;
// ブラシの合成モード(bake.compのuBrushBlendModeと同じ)。ライブプレビューを
// 焼き込み後と同じ見た目にするために使う。
uniform int   uBrushBlendMode;
// ストローク色バッファ(stroke.comp参照)。スタンプごとに色が変わるとき(色のランダム、
// 将来の混色)だけ使う。テクスチャユニットは、レイヤーバンク(8〜15)より後ろの
// 空きが要るため 16 を使う。GPUの上限が足りない環境ではC++側が
// uUseStrokeColor=0 のまま固定するので、ここは参照されない。
layout(binding = 16) uniform sampler2D strokeColorTex;
uniform int   uUseStrokeColor;
// アクティブレイヤーのマスクを編集中か(LayerDockのマスクサムネイルクリックで
// 切り替わる)。trueの間は、通常の色ペイントのライブプレビュー(brushMaskを
// uBrushColorとして重ねる処理)を止める。マスクへのストローク自体はmaskTexに
// 溜まっているが、これは色ではなく濃淡なので、そのままcへ重ねると実際の効果とは
// 異なる色のプレビューになってしまうため。
// 代わりに previewMaskAlphaOf() が、bake.compと同じ式で「焼き込んだらこうなる」
// マスク値を先に反映して表示する(ライブプレビュー)。
uniform int   uIsEditingMaskLayer;
// マスク編集中のブラシ色(rgb=寄せ先の濃度、a=寄せる強さ)。焼き込み側が
// bake.compへ渡すのと同じ値をC++側(src/tools/core/MaskBrush.h)から受け取る。
uniform vec4  uMaskBrushColor;
uniform int   uHasSelection;    // 選択範囲が有効か
uniform int   uIsSelectionTool; // 選択ツールがアクティブか(maskTexを塗り色ではなく選択プレビューとして描く)

// 変形ツール(拡大・縮小・回転)のプレビュー用。ドラッグ中は実データを一切書き換えず、
// このパラメータだけを使ってアクティブレイヤーの表示をその場で合成する
// (確定時にtransform.compで同じ計算を行い実際に焼き込む)。
uniform int   uIsTransformTool;
uniform vec2  uTransformPivot;
uniform vec2  uTransformRotCosSin;
uniform vec2  uTransformScale;
uniform vec2  uTransformC0;
uniform vec2  uTransformHalfSize;
uniform int   uTransformWrapX; // キャンバスの左右ループが有効か
uniform int   uTransformWrapY; // キャンバスの上下ループが有効か

// 自由変形(シアー変形)のプレビュー用。考え方はuIsTransformToolと同じだが、
// アフィン変換ではなく4頂点による双一次補間(invBilinear)で逆変換する。
uniform int   uIsFreeTransformTool;
uniform vec2  uFreeP0; // TL
uniform vec2  uFreeP1; // TR
uniform vec2  uFreeP2; // BR
uniform vec2  uFreeP3; // BL
uniform vec2  uFreeC0;
uniform vec2  uFreeHalfSize;

// 色相・彩度・明度調整のプレビュー用。ピクセル位置は動かさず、その場で
// selectionMaskTexに応じて(未選択時は全域)色だけを変える。
uniform int   uIsHueSatLightTool;
uniform float uHueShift;
uniform float uSatShift;
uniform float uLightShift;

// 明るさ・コントラスト調整のプレビュー用。考え方はuIsHueSatLightToolと同じ。
uniform int   uIsBrightnessContrastTool;
uniform float uBrightnessShift;
uniform float uContrastFactor;

// カラーバランス(C/M/Y)調整のプレビュー用。考え方はuIsHueSatLightToolと同じ。
uniform int   uIsColorBalanceTool;
uniform float uCyanShift;
uniform float uMagentaShift;
uniform float uYellowShift;

// トーンカーブ調整のプレビュー用。考え方はuIsHueSatLightToolと同じだが、
// ToneCurveTool側が制御点からCPUで構築した256x1のLUTテクスチャ(toneCurveLUTTex)を
// そのままサンプルするだけなので、曲線の形状に関わらず毎フレームのコストは一定。
uniform int   uIsToneCurveTool;

// グラデーションマップ(Pro限定)のプレビュー用。考え方はuIsToneCurveToolと同じで、
// GradientMapTool側がCPUで組んだ256x1のLUT(gradientMapLUTTex)を、輝度を横位置として
// 1回引くだけ。グラデーションの複雑さに関わらず毎フレームのコストは一定。
uniform int   uIsGradientMapTool;

// ガウスぼかしのプレビュー用。近傍サンプリングが必要で毎フレームここで再計算すると
// 重すぎるため、GaussianBlurTool側が半径変更のたびに1回だけコンピュートシェーダーで
// gaussianBlurPreviewTexへ計算済みの結果を焼いており、ここでは単にそれをサンプルする
// だけになる。uGaussianBlurOriginPx/SizePxは、そのテクスチャがキャンバス座標系の
// どこ(レイヤーの矩形)に対応するかを表す。
uniform int   uIsGaussianBlurTool;
uniform vec2  uGaussianBlurOriginPx;
uniform vec2  uGaussianBlurSizePx;

// モザイクのプレビュー用。考え方・仕組みはuIsGaussianBlurToolと全く同じで、
// 同じgaussianBlurPreviewTex(=transformSrcTex)を再利用する(両アクションは
// 互いに排他なので同時にアクティブになることはない)。
uniform int   uIsMosaicTool;
uniform vec2  uMosaicOriginPx;
uniform vec2  uMosaicSizePx;

// 色収差(Pro限定)のプレビュー用。考え方・仕組みはuIsMosaicToolと全く同じで、
// 同じgaussianBlurPreviewTex(=transformSrcTex)を再利用する(互いに排他なので
// 同時にアクティブになることはない)。
uniform int   uIsChromaticAberrationTool;
uniform vec2  uChromaticAberrationOriginPx;
uniform vec2  uChromaticAberrationSizePx;

// 移動ぼかしのプレビュー用。考え方・仕組みはuIsMosaicToolと全く同じで、
// 同じgaussianBlurPreviewTex(=transformSrcTex)を再利用する(互いに排他なので
// 同時にアクティブになることはない)。
uniform int   uIsMotionBlurTool;
uniform vec2  uMotionBlurOriginPx;
uniform vec2  uMotionBlurSizePx;

// レンズぼかし(Pro限定)のプレビュー用。上と同じ。
uniform int   uIsLensBlurTool;
uniform vec2  uLensBlurOriginPx;
uniform vec2  uLensBlurSizePx;

// ノイズのプレビュー用。上と同じ。
uniform int   uIsNoiseTool;
uniform vec2  uNoiseOriginPx;
uniform vec2  uNoiseSizePx;

// カスタムシェーダー(ユーザー定義GLSL)のプレビュー用。考え方・仕組みは
// uIsGaussianBlurTool/uIsMosaicToolと全く同じで、同じgaussianBlurPreviewTex
// (=transformSrcTex)を再利用する(3アクションは互いに排他なので同時にアクティブにならない)。
uniform int   uIsCustomShaderTool;
uniform vec2  uCustomShaderOriginPx;
uniform vec2  uCustomShaderSizePx;

// タイル設定
uniform int   uTileSize;  // = TILE_SIZE (256)
uniform int   uCanvasTilesX; // キャンバス全体のタイル数(横)。マスクのタイル参照に使う

// アクティブレイヤー(ブラシプレビュー用)
uniform int   uActiveLayerIndex;

uniform int   uLayerCount;

layout(std430, binding = 4) readonly buffer LayerOpacityBuf   { float uLayerOpacity[]; };
layout(std430, binding = 5) readonly buffer LayerVisibleBuf   { int   uLayerVisible[]; };
layout(std430, binding = 6) readonly buffer LayerBaseSliceBuf { int   uLayerBaseSlice[]; };
// レイヤーマスクのベーススライス(-1なら無し)。composite.comp/belowComposite.compと同じ。
layout(std430, binding = 1) readonly buffer LayerMaskBaseSliceBuf { int uLayerMaskBaseSlice[]; };
// レイヤーを含むフォルダー(祖先、最大4階層ぶん)のマスクベーススライス。
// composite.comp/belowComposite.compと同じ(-1なら該当階層は無し/マスク無し)。
layout(std430, binding = 2) readonly buffer LayerAncestorMaskSlicesBuf { ivec4 uLayerAncestorMaskSlices[]; };
layout(std430, binding = 7) readonly buffer LayerBlendModeBuf { int   uLayerBlendMode[]; };
layout(std430, binding = 8) readonly buffer LayerClippingBuf  { int   uLayerClipping[]; };
layout(std430, binding = 9)  readonly buffer LayerOriginTxBuf { int   uLayerOriginTx[]; };
layout(std430, binding = 10) readonly buffer LayerOriginTyBuf { int   uLayerOriginTy[]; };
layout(std430, binding = 11) readonly buffer LayerTilesXBuf   { int   uLayerTilesX[]; };
layout(std430, binding = 12) readonly buffer LayerTilesYBuf   { int   uLayerTilesY[]; };
layout(std430, binding = 13) readonly buffer LayerIsSolidColorBuf { int   uLayerIsSolidColor[]; };
// 単色レイヤーの色(straight alpha, 0〜1)。binding 4〜15が埋まっているため、
// テクスチャ/画像とは別名前空間で空いているbinding 0を使う。
layout(std430, binding = 0)  readonly buffer LayerSolidColorBuf { vec4 uLayerSolidColor[]; };
// 調整レイヤー関連。SSBOバインディング数の上限(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS、
// 環境によっては16しかない)に収めるため、種類は1本のintに(0=調整レイヤーでない、
// 1=明るさ・コントラスト、2=色相・彩度・明度)、パラメータはvec3配列1本にまとめてある。
layout(std430, binding = 14) readonly buffer LayerAdjKindBuf   { int  uLayerAdjKind[]; };
layout(std430, binding = 15) readonly buffer LayerAdjParamsBuf { vec3 uLayerAdjParams[]; };

uniform mat3 uViewMatrix;
uniform mat4 uViewMatrixInverse;

// キャンバスUV座標が[0,1]の範囲外かどうか。呼び出し側は「キャンバスの外」として
// 扱う(main()ではグレー背景、sampleSelMask()では選択なし=0として扱う)。
// 将来のキャンバスループ機能では、ここを呼ぶ側の判定をfract()による周回に
// 置き換えることになる(この関数自体は単純化のためそのまま残る想定)。
bool outsideCanvasUV(vec2 uv) {
    return uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;
}

// キャンバスピクセル座標からタイル座標(tx,ty)を求める。
// 将来のキャンバスループ機能では、ここにtilesX/tilesYに対するmoduloを
// 足すだけで対応できる想定の choke point。
ivec2 canvasPxToTileCoord(vec2 canvasPx, float tileSize) {
    return ivec2(floor(canvasPx.x / tileSize), floor(canvasPx.y / tileSize));
}

// キャンバスUV座標(レイヤーzの矩形外、またはキャンバス[0,1]の外でも構わない) →
// layerTexArrayのスライスとタイル内UVを返す。レイヤーzの矩形外なら false を返す
// (=データなし、呼び出し側は透明として扱う)。
bool canvasUVToTile(vec2 sampleUV, int z, out int outSi, out vec2 outUV)
{
    vec2 canvasPx = sampleUV * uCanvasSize;

    ivec2 t  = canvasPxToTileCoord(canvasPx, float(uTileSize));
    int tx = t.x;
    int ty = t.y;

    int localTx = tx - uLayerOriginTx[z];
    int localTy = ty - uLayerOriginTy[z];
    if (localTx < 0 || localTx >= uLayerTilesX[z] || localTy < 0 || localTy >= uLayerTilesY[z])
        return false;

    vec2 tileOrigin = vec2(tx * uTileSize, ty * uTileSize);
    outUV = (canvasPx - tileOrigin) / float(uTileSize);
    outSi = uLayerBaseSlice[z] + localTy * uLayerTilesX[z] + localTx;
    return true;
}

// レイヤーzのマスク値(0〜1、マスクを持たなければ1=フルオープン)をキャンバスUV
// 座標で読む。マスクは原点(0,0)固定・キャンバス全体サイズなので(composite.comp
// の同名関数と同じ考え方)、レイヤー本体と違い矩形外判定は不要。
float sampleMaskSlice(int base, vec2 sampleUV) {
    if (base < 0) return 1.0;
    vec2 canvasPx = sampleUV * uCanvasSize;
    ivec2 t = canvasPxToTileCoord(canvasPx, float(uTileSize));
    vec2 tileOrigin = vec2(t.x * uTileSize, t.y * uTileSize);
    vec2 localUV = (canvasPx - tileOrigin) / float(uTileSize);
    int si = base + t.y * uCanvasTilesX + t.x;
    return sampleTileArray(si, localUV).r;
}

// 選択マスクを読む。未選択(uHasSelection==0)なら常に「全域選択扱い」(=1.0)、
// 選択ありならキャンバス範囲外は0(選択範囲はキャンバス内にしか存在しない)。
float sampleSelMask(vec2 uv) {
    if (uHasSelection == 0) return 1.0;
    if (outsideCanvasUV(uv)) return 0.0;
    return texture(selectionMaskTex, uv).r;
}

// マスク値へ「マスク編集中の未確定ストローク」を乗せる。
//
// マスクへのストロークは maskTex に溜まるだけで、実際のマスクタイルへ書き込まれるのは
// bake時(ペン/消しゴム: mouseRelease、エアブラシ: スタンプごと)。そのままだとペンを
// 離すまで画面が一切変わらず「どこを塗ったか分からない」ため、ここで bake.comp と
// 同じ式を先回りで適用して「焼き込んだらこうなる」マスク値を表示する。
//
// bake.comp (maskMode) の該当処理:
//     mask = maskTex.r * selMask.r
//     dst  = normalBlend(dst, uBrushColor * mask)   // normalBlend(bg,fg) = fg + bg*(1-fg.a)
// の .r 成分だけを取り出したものが下の式(マスクとして使われるのは .r のみ)。
// uMaskBrushColorも事前乗算済み(.r = 寄せ先の濃度 × 強さ、.a = 強さ。MaskBrush.h参照)
// なので、結果は「own を寄せ先へ 強さ×mask のぶんだけ寄せる」になる。
// 焼き込み先はRGBA8なので値は0〜1にクランプされる。ここでも同じくクランプする。
float applyMaskStroke(float own, vec2 sampleUV, float brushMask) {
    float m = brushMask * sampleSelMask(sampleUV);
    return clamp(uMaskBrushColor.r * m + own * (1.0 - uMaskBrushColor.a * m), 0.0, 1.0);
}

// sampleMaskSlice の、編集中マスクだけ未確定ストロークを乗せる版。
// 「編集中のマスクか」はスライス番号がアクティブレイヤーのマスクと一致するかで判定する。
// これはレイヤー自身のマスク(z==uActiveLayerIndex)だけでなく、フォルダーのマスクを
// 編集している場合(そのフォルダーが祖先である子レイヤー側から見た anc.x〜w)にも
// 同じ判定で当たるので、フォルダーマスクを塗っている最中も中身へライブ反映される。
float sampleMaskSlicePreview(int base, vec2 sampleUV, float brushMask, bool editing) {
    float m = sampleMaskSlice(base, sampleUV);
    if (editing && base >= 0 && base == uLayerMaskBaseSlice[uActiveLayerIndex])
        m = applyMaskStroke(m, sampleUV, brushMask);
    return m;
}

// レイヤーzに効くマスク値(自身のマスク×祖先フォルダーのマスク、最大4階層)。
// マスクを持たなければ sampleMaskSlice が1.0を返すので、その場合は無害に1.0になる。
// 編集中のマスクにだけ、上記の未確定ストロークが上乗せされる。
//
// uIsSelectionTool中を除外しているのは、選択ツール(ペン選択)も同じmaskTexを
// 作業用に使っているため。マスク編集レイヤーを選んだまま選択範囲を描くと、
// 選択のストロークがマスクの濃淡として誤ってプレビューされてしまう。
float previewMaskAlphaOf(int z, vec2 sampleUV, float brushMask) {
    bool editing = (uIsEditingMaskLayer != 0 && uIsSelectionTool == 0);
    ivec4 anc = uLayerAncestorMaskSlices[z];
    return sampleMaskSlicePreview(uLayerMaskBaseSlice[z], sampleUV, brushMask, editing)
         * sampleMaskSlicePreview(anc.x, sampleUV, brushMask, editing)
         * sampleMaskSlicePreview(anc.y, sampleUV, brushMask, editing)
         * sampleMaskSlicePreview(anc.z, sampleUV, brushMask, editing)
         * sampleMaskSlicePreview(anc.w, sampleUV, brushMask, editing);
}

// キャンバス1サンプル分の合成結果(premultiplied)を返す。
//
// main()から切り出したのは、縮小表示のときにここを複数回呼んで平均するため。
// 1画素が覆うキャンバス範囲を1点だけで代表すると、アンチエイリアスのために
// 入っている淡い画素が拾われたり飛ばされたりして、斜め線が階段状に見える
// (中間の縮小率で特に目立つ)。範囲内を格子状に取って平均すれば、面積比に
// 応じた正しい濃さになる。
//
// 呼び出し回数はuMinifySamples^2に増えるが、縮小するほどキャンバスが占める
// 画面画素数は倍率の二乗で減るので、画面全体の処理量はほぼ一定に保たれる。
vec4 composeCanvasAt(vec2 sampleUV, vec2 canvasPxXY, out float outBrushMask) {
    float brushMask = texture(maskTex, sampleUV).r;

    // item4: キャッシュが有効なら、z=アクティブレイヤーより下はキャッシュ済みの
    // 合成結果から始める(その範囲は今フレームで再計算しない)。無効なら従来通り
    // z=0、result/clipBaseとも空から始める。
    vec4 result   = uUseBelowCompositeCache != 0 ? texture(belowCompositeTex, sampleUV) : vec4(0.0);
    vec4 clipBase = uUseBelowCompositeCache != 0 ? texture(belowCompositeClipBaseTex, sampleUV) : vec4(0.0); // 直近の非クリッピングレイヤーの(不透明度適用後の)色
    // 事前合成済みの範囲 [0, uCompositeStartZ) を飛ばして、その続きから合成する。
    // ストローク中のキャッシュではアクティブレイヤーのindex、フィルターレイヤーが
    // ある文書では「一番上のフィルターレイヤーの1つ上」が入る(GLWidget側が決める)。
    int startZ = uUseBelowCompositeCache != 0 ? uCompositeStartZ : 0;

    // Photoshop準拠のクリッピングマスク合成(composite.compと同一アルゴリズム)。
    // 全レイヤーを layerTexArray から直接合成する。上レイヤー事前合成キャッシュが
    // 有効なら、アクティブレイヤーでループを打ち切る(残りは下でまとめて重ねる)。
    int endZExclusive = (uUseAboveCompositeCache != 0) ? (uActiveLayerIndex + 1) : uLayerCount;
    for (int z = startZ; z < endZExclusive; z++) {
        // フィルターレイヤー(近傍参照なのでこのループでは計算できない)。効果は
        // オフスクリーンの連鎖(GLWidget::rebuildFilterChain)でかかっており、その
        // 結果が startZ 未満の初期値として渡ってくる。ここに現れるのは非表示などで
        // 連鎖の対象外になったものだけなので、何もせず読み飛ばす(調整レイヤーと
        // 同じくクリップの区切りにもならない)。
        if (uLayerAdjKind[z] == 3) continue;

        if (uLayerVisible[z] == 0) {
            if (uLayerClipping[z] == 0) clipBase = vec4(0.0);
            continue;
        }

        // 調整レイヤーはタイルを持たず、自身は色を持たない。代わりにここまでの
        // 合成結果(result)へ非破壊で色調整をかけてから、通常のブレンド/クリップ
        // 処理をスキップして次のレイヤーへ進む(不透明度は効果の強さとして使う)。
        if (uLayerAdjKind[z] != 0) {
            vec3 rgb = safeUnpremul(result.rgb, result.a);
            vec3 adjusted = adjustLayerColor(uLayerAdjKind[z], rgb, uLayerAdjParams[z], uLayerBaseSlice[z]);
            float adjMaskA = previewMaskAlphaOf(z, sampleUV, brushMask);
            result.rgb = mix(rgb, adjusted, uLayerOpacity[z] * adjMaskA) * result.a;
            continue;
        }

        // 単色レイヤーはタイルを持たず、選択された色で手続き的に合成する
        vec4 c;
        if (uLayerIsSolidColor[z] != 0) {
            vec4 sc = uLayerSolidColor[z];
            c = vec4(sc.rgb * sc.a, sc.a); // テクスチャ側と同じ事前乗算アルファ形式に揃える
        } else {
            int si; vec2 uv;
            bool inLayer = canvasUVToTile(sampleUV, z, si, uv);
            c = inLayer ? sampleTileArray(si, uv) : vec4(0.0);
        }
        c.rgb *= uLayerOpacity[z];
        c.a   *= uLayerOpacity[z];
        // レイヤーマスク(マスクを持たなければ1.0を返すので無害)。マスク編集中の
        // アクティブレイヤーだけは、未確定ストロークを乗せたプレビュー値になる。
        float layerMaskA = previewMaskAlphaOf(z, sampleUV, brushMask);
        c.rgb *= layerMaskA;
        c.a   *= layerMaskA;

        // アクティブレイヤーのみブラシプレビュー(選択ツール中・単色/調整レイヤーは編集不可のため対象外)
        //
        // ただし変形/自由変形のプレビューだけは「選択ツール中」でも必ず表示する。
        // これらはツールではなくアクションで、blocksToolInput()==false のため開始しても
        // activeTool が変わらない(色調整/フィルター系パネルは blocksToolInput()==true で、
        // 開いた瞬間に GLWidget::hostNotifyToolBlockingActionStarted() が移動ツールへ
        // 切り替えるので uIsSelectionTool は必ず0になる)。
        // つまり「選択範囲を作った直後(=選択ツールがアクティブなまま)に変形を始める」と、
        // uIsSelectionTool==1 のせいでこのブロック全体が飛ばされ、変形中だけプレビューが
        // 出ない(確定時の焼き込みは transform.comp 側なので結果は正しい)という症状になっていた。
        if (z == uActiveLayerIndex && uLayerIsSolidColor[z] == 0
            && (uIsSelectionTool == 0 || uIsTransformTool != 0 || uIsFreeTransformTool != 0)) {
            if (uIsTransformTool != 0) {
                // 逆変換: 今表示しようとしている位置canvasPxXYに映るべき
                // 「変形前のキャンバス座標」を求め、そこから色を拾ってくる
                vec2 d     = canvasPxXY - uTransformPivot;
                vec2 local = vec2(uTransformRotCosSin.x * d.x + uTransformRotCosSin.y * d.y,
                                  -uTransformRotCosSin.y * d.x + uTransformRotCosSin.x * d.y);
                vec2 srcLocal    = local / uTransformScale;
                // ラップ有効な軸は、元のバウンディングボックス(±uTransformHalfSize)の
                // 外側もそのbboxサイズで周回させて取得する(transform.compの焼き込み
                // 処理と同じロジック。プレビューと確定後の見た目を一致させる)。
                if (uTransformWrapX != 0 && uTransformHalfSize.x > 0.0001)
                    srcLocal.x = mod(srcLocal.x + uTransformHalfSize.x, 2.0 * uTransformHalfSize.x) - uTransformHalfSize.x;
                if (uTransformWrapY != 0 && uTransformHalfSize.y > 0.0001)
                    srcLocal.y = mod(srcLocal.y + uTransformHalfSize.y, 2.0 * uTransformHalfSize.y) - uTransformHalfSize.y;
                vec2 srcCanvasPx = srcLocal + uTransformC0;
                vec2 srcUV       = srcCanvasPx / uCanvasSize;

                // 「この画面位置がもともと選択されていた(=持ち去られる)場所」なら穴を開ける
                float ownSel = sampleSelMask(sampleUV);
                vec4  base   = ownSel > 0.0 ? vec4(0.0) : c;

                // 移動元は「レイヤー自身の矩形内(キャンバス範囲外でも、以前拡張された
                // 領域なら可)」かどうかで判定する(キャンバス[0,1]範囲には limitしない)。
                vec4 moved = vec4(0.0);
                bool inHalfSize = abs(srcLocal.x) <= uTransformHalfSize.x && abs(srcLocal.y) <= uTransformHalfSize.y;
                if (inHalfSize) {
                    float srcSel = sampleSelMask(srcUV);
                    if (srcSel > 0.0) {
                        int srcSi; vec2 srcUVt;
                        if (canvasUVToTile(srcUV, z, srcSi, srcUVt)) {
                            moved = sampleTileArray(srcSi, srcUVt);
                            moved.rgb *= uLayerOpacity[z];
                            moved.a   *= uLayerOpacity[z];
                        }
                    }
                }
                c = normalBlend(base, moved);
            } else if (uIsFreeTransformTool != 0) {
                // 逆双一次補間で、今表示しようとしている位置canvasPxXYに映るべき
                // 「変形前のキャンバス座標」を求める
                vec2 uvFree      = invBilinear(canvasPxXY, uFreeP0, uFreeP1, uFreeP2, uFreeP3);
                vec2 srcCanvasPx = uFreeC0 + vec2((uvFree.x * 2.0 - 1.0) * uFreeHalfSize.x,
                                                   (uvFree.y * 2.0 - 1.0) * uFreeHalfSize.y);
                vec2 srcUV       = srcCanvasPx / uCanvasSize;

                float ownSel = sampleSelMask(sampleUV);
                vec4  base   = ownSel > 0.0 ? vec4(0.0) : c;

                vec4 moved = vec4(0.0);
                bool inQuad = uvFree.x >= 0.0 && uvFree.x <= 1.0 && uvFree.y >= 0.0 && uvFree.y <= 1.0;
                if (inQuad) {
                    float srcSel = sampleSelMask(srcUV);
                    if (srcSel > 0.0) {
                        int srcSi; vec2 srcUVt;
                        if (canvasUVToTile(srcUV, z, srcSi, srcUVt)) {
                            moved = sampleTileArray(srcSi, srcUVt);
                            moved.rgb *= uLayerOpacity[z];
                            moved.a   *= uLayerOpacity[z];
                        }
                    }
                }
                c = normalBlend(base, moved);
            } else if (uIsHueSatLightTool != 0) {
                float sel = texture(selectionMaskTex, sampleUV).r;
                if (sel > 0.0 && c.a > 0.0001) {
                    vec3 rgb = c.rgb / c.a;
                    rgb = adjustHSL(rgb, uHueShift, uSatShift, uLightShift);
                    c.rgb = rgb * c.a;
                }
            } else if (uIsBrightnessContrastTool != 0) {
                float sel = texture(selectionMaskTex, sampleUV).r;
                if (sel > 0.0 && c.a > 0.0001) {
                    vec3 rgb = c.rgb / c.a;
                    rgb = adjustBrightnessContrast(rgb, uBrightnessShift, uContrastFactor);
                    c.rgb = rgb * c.a;
                }
            } else if (uIsColorBalanceTool != 0) {
                float sel = texture(selectionMaskTex, sampleUV).r;
                if (sel > 0.0 && c.a > 0.0001) {
                    vec3 rgb = c.rgb / c.a;
                    rgb = adjustColorBalance(rgb, uCyanShift, uMagentaShift, uYellowShift);
                    c.rgb = rgb * c.a;
                }
            } else if (uIsToneCurveTool != 0) {
                float sel = texture(selectionMaskTex, sampleUV).r;
                if (sel > 0.0 && c.a > 0.0001) {
                    vec3 rgb = c.rgb / c.a;
                    rgb.r = texture(toneCurveLUTTex, vec2(rgb.r, 0.5)).r;
                    rgb.g = texture(toneCurveLUTTex, vec2(rgb.g, 0.5)).r;
                    rgb.b = texture(toneCurveLUTTex, vec2(rgb.b, 0.5)).r;
                    c.rgb = rgb * c.a;
                }
            } else if (uIsGradientMapTool != 0) {
                float sel = texture(selectionMaskTex, sampleUV).r;
                if (sel > 0.0 && c.a > 0.0001) {
                    vec3 rgb = c.rgb / c.a;
                    vec3 mapped = texture(gradientMapLUTTex, vec2(clamp(blendLuma(rgb), 0.0, 1.0), 0.5)).rgb;
                    c.rgb = mapped * c.a;
                }
            } else if (uIsGaussianBlurTool != 0) {
                vec2 localPx = canvasPxXY - uGaussianBlurOriginPx;
                if (localPx.x >= 0.0 && localPx.y >= 0.0 &&
                    localPx.x < uGaussianBlurSizePx.x && localPx.y < uGaussianBlurSizePx.y) {
                    vec2 previewUV = localPx / uGaussianBlurSizePx;
                    c = texture(gaussianBlurPreviewTex, previewUV);
                    c.rgb *= uLayerOpacity[z];
                    c.a   *= uLayerOpacity[z];
                }
            } else if (uIsMosaicTool != 0) {
                vec2 localPx = canvasPxXY - uMosaicOriginPx;
                if (localPx.x >= 0.0 && localPx.y >= 0.0 &&
                    localPx.x < uMosaicSizePx.x && localPx.y < uMosaicSizePx.y) {
                    vec2 previewUV = localPx / uMosaicSizePx;
                    c = texture(gaussianBlurPreviewTex, previewUV);
                    c.rgb *= uLayerOpacity[z];
                    c.a   *= uLayerOpacity[z];
                }
            } else if (uIsChromaticAberrationTool != 0) {
                vec2 localPx = canvasPxXY - uChromaticAberrationOriginPx;
                if (localPx.x >= 0.0 && localPx.y >= 0.0 &&
                    localPx.x < uChromaticAberrationSizePx.x && localPx.y < uChromaticAberrationSizePx.y) {
                    vec2 previewUV = localPx / uChromaticAberrationSizePx;
                    c = texture(gaussianBlurPreviewTex, previewUV);
                    c.rgb *= uLayerOpacity[z];
                    c.a   *= uLayerOpacity[z];
                }
            } else if (uIsMotionBlurTool != 0) {
                vec2 localPx = canvasPxXY - uMotionBlurOriginPx;
                if (localPx.x >= 0.0 && localPx.y >= 0.0 &&
                    localPx.x < uMotionBlurSizePx.x && localPx.y < uMotionBlurSizePx.y) {
                    vec2 previewUV = localPx / uMotionBlurSizePx;
                    c = texture(gaussianBlurPreviewTex, previewUV);
                    c.rgb *= uLayerOpacity[z];
                    c.a   *= uLayerOpacity[z];
                }
            } else if (uIsLensBlurTool != 0) {
                vec2 localPx = canvasPxXY - uLensBlurOriginPx;
                if (localPx.x >= 0.0 && localPx.y >= 0.0 &&
                    localPx.x < uLensBlurSizePx.x && localPx.y < uLensBlurSizePx.y) {
                    vec2 previewUV = localPx / uLensBlurSizePx;
                    c = texture(gaussianBlurPreviewTex, previewUV);
                    c.rgb *= uLayerOpacity[z];
                    c.a   *= uLayerOpacity[z];
                }
            } else if (uIsNoiseTool != 0) {
                vec2 localPx = canvasPxXY - uNoiseOriginPx;
                if (localPx.x >= 0.0 && localPx.y >= 0.0 &&
                    localPx.x < uNoiseSizePx.x && localPx.y < uNoiseSizePx.y) {
                    vec2 previewUV = localPx / uNoiseSizePx;
                    c = texture(gaussianBlurPreviewTex, previewUV);
                    c.rgb *= uLayerOpacity[z];
                    c.a   *= uLayerOpacity[z];
                }
            } else if (uIsCustomShaderTool != 0) {
                vec2 localPx = canvasPxXY - uCustomShaderOriginPx;
                if (localPx.x >= 0.0 && localPx.y >= 0.0 &&
                    localPx.x < uCustomShaderSizePx.x && localPx.y < uCustomShaderSizePx.y) {
                    vec2 previewUV = localPx / uCustomShaderSizePx;
                    c = texture(gaussianBlurPreviewTex, previewUV);
                    c.rgb *= uLayerOpacity[z];
                    c.a   *= uLayerOpacity[z];
                }
            } else if (uIsEditingMaskLayer != 0) {
                // マスク編集中は色のライブプレビューをしない(brushMaskは色ではなく
                // マスク濃度なので、ここでcへ重ねると別物になる。ペン/消しゴムいずれの
                // サブツールでも同じ)。代わりに上の previewMaskAlphaOf() が、同じ
                // brushMask を「マスク値」として先に反映済み。
            } else {
                // 焼き込み(bake.comp)と同じ関数を使う(common.glsl)。
                //
                // ・選択範囲マスクを掛けるのは焼き込み側が
                //     mask = maskTex.r * selMask.r
                //   としているため。掛けないとプレビューだけ選択範囲の外へはみ出して
                //   描かれ、マウスを離した瞬間に切り取られる(選択が無いときは
                //   sampleSelMask()が1.0を返すので影響しない)。
                // ・レイヤー不透明度を渡すのは、cには既にそれが掛かっているため。
                //   渡さないとストローク中だけ「不透明度100%」相当の濃さで表示され、
                //   離した瞬間に急に薄くなって見える。
                //   なお焼き込みはレイヤーの生の画素に対して行うのに対し、ここでは
                //   レイヤー不透明度とレイヤーマスクを掛けた後のcへ重ねるため、
                //   どちらも100%でなければ厳密には一致しない(通常合成のときからある近似)。
                float sel = sampleSelMask(sampleUV);
                vec3 strokeRGB = (uUseStrokeColor != 0)
                               ? texture(strokeColorTex, sampleUV).rgb * sel : vec3(0.0);
                c = applyStrokeToPixel(c, uBrushColor, brushMask * sel,
                                       strokeRGB, uUseStrokeColor,
                                       uEraseMode, uBrushBlendMode, uLayerOpacity[z],
                                       canvasPxXY);
            }
        }

        if (uLayerClipping[z] != 0) {
            c.rgb *= clipBase.a;
            c.a   *= clipBase.a;
        } else {
            clipBase = c;
        }

        result = applyBlend(result, c, uLayerBlendMode[z], canvasPxXY);
    }

    // 上レイヤー事前合成キャッシュ: アクティブより上を1枚に事前合成したものを
    // 最後に1回だけ通常ブレンドで重ねる(有効化条件により上は全て通常ブレンド・
    // 非クリッピング・非調整なので、これで通常経路と同じ結果になる)。
    if (uUseAboveCompositeCache != 0) {
        result = normalBlend(result, texture(aboveCompositeTex, sampleUV));
    }
    outBrushMask = brushMask;
    return result;
}

void main() {
    vec2 screenPx = uv * uWindowSize;
    vec4 canvasPx4 = uViewMatrixInverse * vec4(screenPx, 0.0, 1.0);
    vec2 sampleUV  = canvasPx4.xy / uCanvasSize;

    if (outsideCanvasUV(sampleUV)) {
        fragColor = vec4(uCanvasOutsideBg, 1.0);
        return;
    }

    // 表示倍率に応じてサンプリングの仕方を変える。
    //
    // 【拡大時(uMinifySamples==1)】読み取り位置をキャンバス画素の中心へ吸着させる
    // (=ニアレスト相当)。これをしないとバイリニア補間で画素と画素の境目まで溶け、
    // 「アンチエイリアスが効いている」のではなく「ただボケている」見え方になる。
    // 本来アンチエイリアスは画素データ側の淡い色として入っているので、表示は
    // 画素を四角いまま拡大するのが正しい。タイル境界に出ていた筋も、画素中心を
    // 突けば補間自体が起きないので同時に消える。
    //
    // 【縮小時(uMinifySamples>1)】1画素が覆うキャンバス範囲を格子状に取って平均する。
    // 1点だけで代表すると、アンチエイリアスのために入っている淡い画素が拾われたり
    // 飛ばされたりして斜め線が階段状に見える(中間の縮小率で特に目立つ)。
    // 面積平均にすれば、覆っている範囲の比率どおりの濃さになる。
    vec4  result;
    float brushMask;
    if (uMinifySamples <= 1) {
        // 画素の「中」は中心の色をそのまま使い、「境目」だけを画面1画素ぶんの幅で
        // ならす(アンチエイリアス付きニアレスト)。
        //
        // 単純なニアレストだと画素の境界が完全な段差になり、斜めの輪郭では
        // その段差自体がジャギーとして見える。逆に素のバイリニアだと画素の中まで
        // 溶けてボケる。境目だけを画面1画素ぶんならせば、画素は四角いまま
        // 輪郭のギザギザだけが取れる。
        //
        // 遷移幅を「画面1画素ぶん」にしているのが要点で、これにより倍率1.0のとき
        // 素のバイリニアと完全に一致し、縮小側(面積平均)と連続に繋がる。
        // 以前は倍率1.0を境にニアレストと面積平均が不連続に切り替わっており、
        // ズームしていくとある一点で見え方が急に変わっていた。
        vec2 p  = canvasPx4.xy - vec2(0.5);   // テクセル中心が整数になる座標系
        vec2 i  = floor(p);
        vec2 f  = p - i;                      // 0〜1(画素内の位置)
        // 中央付近は0か1へ潰し、境目付近だけ線形に繋ぐ。倍率が上がるほど急峻になる。
        vec2 fw = clamp((f - vec2(0.5)) * uViewScale + vec2(0.5), 0.0, 1.0);
        vec2 px = clamp(i + fw + vec2(0.5), vec2(0.5), uCanvasSize - vec2(0.5));
        result = composeCanvasAt(px / uCanvasSize, px, brushMask);
    } else {
        result    = vec4(0.0);
        brushMask = 0.0;
        // 【試して駄目だったこと】ここを1.5倍幅の三角形(テント)の重みにしてみたが、
        // 縁の輪郭はほとんど変わらず(実測 226/35/24/213 -> 217/43/29/203)、
        // わずかに鮮鋭さを落とすだけだった。高品質バイキュービックで縮小した
        // 参照画像の輪郭(230/27/11/213)とも既にほぼ一致しており、箱型で足りている。
        //
        // 画面1画素が覆うキャンバスpx(回転は無視して正方形で近似する)
        float footprint = 1.0 / max(uViewScale, 0.0001);
        float inv       = 1.0 / float(uMinifySamples);
        for (int sy = 0; sy < uMinifySamples; sy++) {
            for (int sx = 0; sx < uMinifySamples; sx++) {
                // 画素の中を等間隔に刻む(-0.5〜+0.5の範囲へ写す)
                vec2 off = (vec2(float(sx), float(sy)) + vec2(0.5)) * inv - vec2(0.5);
                // キャンバスの外まではみ出すとマスク等のサンプルが回り込むので内側へ寄せる
                vec2 px  = clamp(canvasPx4.xy + off * footprint,
                                  vec2(0.5), uCanvasSize - vec2(0.5));
                float bm;
                result    += composeCanvasAt(px / uCanvasSize, px, bm);
                brushMask += bm;
            }
        }
        float n = float(uMinifySamples * uMinifySamples);
        result    /= n;
        brushMask /= n;
    }
    vec2 sampleUVCenter = clamp(canvasPx4.xy, vec2(0.5), uCanvasSize - vec2(0.5)) / uCanvasSize;

    // カラーモードによる見た目だけのソフトプルーフ変換。resultはこの時点で
    // premultiplied(rgbにaが掛かっている)なので、一旦アンプリマルチプライして
    // から変換し、戻す。
    if (result.a > 0.0001 && uColorMode != 0) {
        vec3 rgb = result.rgb / result.a;
        if (uColorMode == 2) {
            // 輝度ベース(人間の知覚に近い重み付け)
            float y = dot(rgb, vec3(0.299, 0.587, 0.114));
            rgb = vec3(y);
        } else if (uColorMode == 3) {
            // 明度ベース(HSLのL = (max+min)/2。「色相・彩度・明度」調整ツールと
            // 同じ定義、common.glslのadjustHSL参照)
            float maxc = max(max(rgb.r, rgb.g), rgb.b);
            float minc = min(min(rgb.r, rgb.g), rgb.b);
            rgb = vec3((maxc + minc) * 0.5);
        } else if (uColorMode == 1) {
            // 単純なC=1-R,M=1-G,Y=1-Bの往復変換は、原色に近い彩度の高い色
            // (いずれかのチャンネルが0または1に近い色)がほぼ無変化になって
            // しまう(RGBとCMYKで見分けがつかない)ため、実際のインクが
            // 理想的でない吸収特性を持つこと(にじみ・不純物)を簡易的な
            // 3x3の吸収行列として表現し、彩度の高い色でも視覚的に違いが
            // 出るようにする。各行の合計は1.0にしてあるため、無彩色
            // (白・黒・グレー)は変換前後で変化しない。
            vec3 c = clamp(1.0 - rgb, 0.0, 1.0);
            vec3 absorbed;
            absorbed.r = 0.87 * c.r + 0.10 * c.g + 0.03 * c.b;
            absorbed.g = 0.09 * c.r + 0.86 * c.g + 0.05 * c.b;
            absorbed.b = 0.03 * c.r + 0.18 * c.g + 0.79 * c.b;
            rgb = clamp(1.0 - absorbed, 0.0, 1.0);
        }
        result.rgb = rgb * result.a;
    }

    // 最終出力(市松模様の2色はuCheckerColorA/B。ThemeColors::checkerDark/Light由来で
    // 先端画像プレビュー・レイヤープレビューと共通)
    vec2 p = floor(canvasPx4.xy * 0.05);
    float checker = mod(p.x + p.y, 2.0);
    vec4 transparent = vec4(mix(uCheckerColorA, uCheckerColorB, checker), 1.0);

    fragColor = normalBlend(transparent, result);

    // 選択範囲外を少し暗くして視覚的に分かるようにする
    if (uHasSelection != 0) {
        float sel = texture(selectionMaskTex, sampleUVCenter).r;
        fragColor.rgb *= mix(0.55, 1.0, sel);
    }

    // ペン選択でドラッグ中のストロークを、実際の塗り色ではなく半透明の水色で重ねる
    if (uIsSelectionTool != 0 && brushMask > 0.0) {
        vec3 selPreviewColor = vec3(0.2, 0.6, 1.0);
        fragColor.rgb = mix(fragColor.rgb, selPreviewColor, brushMask * 0.6);
    }

    // モニターキャリブレーション: 内部データ→カラーモード変換後の色に対し、
    // 画面に出す一番最後にモニター環境の補正として適用する。
    fragColor.rgb = adjustBrightnessContrast(fragColor.rgb, uCalBrightness, uCalContrast);
    fragColor.rgb = adjustColorBalance(fragColor.rgb, uCalCyan, uCalMagenta, uCalYellow);
}
