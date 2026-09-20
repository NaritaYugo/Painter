#pragma once
#include "tools/core/ToolType.h"
#include "tools/core/ToolPresetList.h"
#include "tools/core/PressureCurve.h"
#include "tools/core/PressureResponse.h"

#include <QtMinMax>
#include <QColor>
#include <QVariantMap>
#include <QSettings>

class FillToolConfig
{
public:
    void  setFillExtension(float s) { fillExtension_ = qBound(-5.0f, s, 5.0f); }
    float fillExtension() const { return fillExtension_;}

    void  setFillGapSize(int s) { fillGapSize_ = qBound(0, s, 100); }
    int  fillGapSize() { return fillGapSize_; }

    void  setProtectRayLength(int s) { protectRayLength_ = qBound(0, s, 100); }
    int  protectRayLength() { return protectRayLength_; }

// 塗りつぶしの境界判定・開始色をどこから参照するか。
    // false: 現在のレイヤーのみを参照する / true: 全レイヤーを合成したキャンバスを参照する
    // (塗る対象自体は常にアクティブレイヤーのまま変わらない)
    void setReferenceCanvas(bool v) { referenceCanvas_ = v; }
    bool referenceCanvas() const { return referenceCanvas_; }

     QVariantMap toMap() const {
        QVariantMap m;
        m["fillExtension"]    = fillExtension_;
        m["fillGapSize"]      = fillGapSize_;
        m["protectRayLength"] = protectRayLength_;
        m["referenceCanvas"]  = referenceCanvas_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        fillExtension_    = m.value("fillExtension", fillExtension_).toFloat();
        fillGapSize_      = m.value("fillGapSize", fillGapSize_).toInt();
        protectRayLength_ = m.value("protectRayLength", protectRayLength_).toInt();
        referenceCanvas_  = m.value("referenceCanvas", referenceCanvas_).toBool();
    }

private:
    float fillExtension_ = 0.5;
    int fillGapSize_ = 10;
    int protectRayLength_ = 15;
    bool referenceCanvas_ = false;
};

class PenToolConfig
{
public:
    void setSize(int s) { size_ = qMax(1, s); }
    void setOpacity(float o) { opacity_ = qBound(0.f, o, 1.f); }
    void setHardness(float h) { hardness_ = qBound(0.f, h, 1.f); }
    // スタンプ間隔。ブラシ直径に対する比率(0.05〜3.0、既定10%)。
    // PenEraserToolが「前回スタンプ位置から何px進んだら次のスタンプを打つか」を
    // これ*直径から求める(距離ベースのスタンプ方式)。
    void setSpacing(float s) { spacing_ = qBound(0.05f, s, 3.0f); }
    // 先端(スタンプ)画像。リソースパス(:/...)またはファイルシステム上のパス。
    // 実際のGLテクスチャの読み込み/差し替えはCanvasWidget::setPenTipImage()が行う
    // (このクラスはパス文字列を保持・永続化するだけ)。
    void setTipImagePath(const QString &p) { tipImagePath_ = p; }
    // このツール(プリセット)専用の筆圧カーブ。環境設定の「全体の筆圧カーブ」を
    // 通した後に適用される(PressureCurve/CanvasWidget::mapPressure参照)。
    void setPressureCurve(const PressureCurve &c) { pressureCurve_ = c; }
    // 筆圧0のときに残すサイズ/不透明度の比率(PressureResponse参照)。0で従来どおり。
    void setMinSizeRatio(float r)    { minSizeRatio_ = qBound(0.f, r, 1.f); }
    // 筆圧で不透明度も変えるか。既定はoff(従来どおりサイズだけが変わる)。
    void setPressureOpacity(bool on) { pressureOpacity_ = on; }
    void setMinOpacityRatio(float r) { minOpacityRatio_ = qBound(0.f, r, 1.f); }

    // ---- 先端の形 ----------------------------------------------------------
    // 角度[度]: 先端を回す量。真円率が1.0(真円)のままだと、手続き的な円は回しても
    //           見た目が変わらないので、先端画像か真円率と組み合わせて使う。
    // 真円率  : 1.0で真円、小さいほど短軸方向へ潰れた楕円になる(平筆・カリグラフィ)。
    // 進行方向に追従: 各スタンプの角度をストロークの進行方向に合わせる。
    void setAngleDeg(int deg)         { angleDeg_ = ((deg % 360) + 360) % 360; }
    void setRoundness(float r)        { roundness_ = qBound(0.05f, r, 1.0f); }
    void setFollowDirection(bool on)  { followDirection_ = on; }

    // ---- 傾き・ペン回転 ----------------------------------------------------
    // ペンを寝かせた量(0=垂直〜1=最大)に応じて効かせる。マウス操作時は常に0なので
    // 何を設定しても影響しない。
    // tiltSize    : 寝かせるほど太くする割合(1.0で最大時に2倍)
    // tiltOpacity : 寝かせるほど薄くする割合(1.0で最大時に消える手前まで)
    // tiltFlatten : 寝かせるほど平筆化する割合(1.0で最大時に真円率が下限まで)
    // tiltAngleFollow    : 先端の向きを「ペンを倒した方向」に合わせる
    // penRotationFollow  : 先端の向きをペン軸まわりの回転に合わせる(アートペン等)
    void setTiltSize(float v)          { tiltSize_    = qBound(0.f, v, 2.f); }
    void setTiltOpacity(float v)       { tiltOpacity_ = qBound(0.f, v, 1.f); }
    void setTiltFlatten(float v)       { tiltFlatten_ = qBound(0.f, v, 1.f); }
    void setTiltAngleFollow(bool on)   { tiltAngleFollow_ = on; }
    void setPenRotationFollow(bool on) { penRotationFollow_ = on; }

    // ---- フロー(1スタンプが乗せる量) ---------------------------------------
    // 不透明度が「1ストロークで到達できる濃さの上限」なのに対し、フローは
    // 「1スタンプがそこへどれだけ近づけるか」。100%なら1スタンプで上限に達する
    // (=従来どおり)。下げるほど、同じ場所を重ねてなぞるほど濃くなる
    // (スタンプ間隔が狭いほど重なる数が増えるので速く濃くなる)。
    void setFlow(float f) { flow_ = qBound(0.01f, f, 1.0f); }

    // ---- ブラシの合成モード ------------------------------------------------
    // ストロークをレイヤーへ焼き込むときの合成方法(レイヤーの合成モードと同じ一覧)。
    // 値は BlendMode の整数値そのままで、0(普通)が既定。
    // 型をintにしているのは、ToolConfigがdocument/CanvasDocument.hに依存しないため。
    void setBrushBlendMode(int m) { brushBlendMode_ = qBound(0, m, 26); }

    // ---- 後補正 ------------------------------------------------------------
    // ペンを離した時点で、引いた軌道そのものをなめらかに引き直す量(0で無効)。
    // 手振れ補正が「描いている最中にペン先へ遅れて追従する」のに対し、こちらは
    // 描き終わってから軌道を整えるので、追従の遅れ無しに線を整えられる。
    // 値は補正の効く長さの目安(経路上の距離)を0〜1で表したもの(実距離への換算は
    // PenEraserTool::smoothStrokePath 参照)。
    void setPostCorrection(float v) { postCorrection_ = qBound(0.f, v, 1.f); }

    // ---- 紙質テクスチャ ----------------------------------------------------
    // 紙の目をキャンバス座標に貼り付け、その明るさをブラシの濃度へ掛ける
    // (stroke.comp / PaperTexPresets 参照)。ストロークではなくキャンバスに
    // 貼り付くので、重ね描きしても目の位置が揃う。
    // paperTexPath : テクスチャ画像のパス。空文字列で「なし」(=既定・従来どおり)。
    // paperStrength: 効かせる割合。0で無効、1.0で画像の明暗をそのまま濃度に使う。
    // paperScale   : 画像を何倍に拡大して貼るか(1.0で画像1pxがキャンバス1px)。
    void setPaperTexPath(const QString &p) { paperTexPath_ = p; }
    void setPaperStrength(float v)         { paperStrength_ = qBound(0.f, v, 1.f); }
    void setPaperScale(float v)            { paperScale_ = qBound(0.25f, v, 4.0f); }

    // ---- 散布・ランダム(BrushScatter参照) ----------------------------------
    // 散布量。スタンプ位置を軌道からどれだけ散らすか(ブラシ直径に対する比率)。
    void setScatter(float s)       { scatter_ = qBound(0.f, s, 4.f); }
    // 1回のスタンプ間隔ごとに何粒打つか。散布と組み合わせて密度を上げる。
    void setParticleCount(int n)   { particleCount_ = qBound(1, n, 16); }

    // ---- 入り抜き ----------------------------------------------------------
    // ストロークの書き始め/書き終わりを、指定した距離[px]かけて細く/薄くする。
    // 「抜き」はペンを離すまで終点が分からないため、離した瞬間にストロークを
    // 引き直して適用する(PenEraserTool::rebuildStrokeWithTaper参照)。
    void setTaperInPx(int px)      { taperInPx_  = qBound(0, px, 2000); }
    void setTaperOutPx(int px)     { taperOutPx_ = qBound(0, px, 2000); }
    void setTaperSize(bool on)     { taperSize_    = on; }
    void setTaperOpacity(bool on)  { taperOpacity_ = on; }

    // ---- 下地混色 ----------------------------------------------------------
    // 筆が進みながら、描いている先のキャンバスの色を拾って混ぜる量(0で無効)。
    // 1スタンプごとに「筆の絵の具」を下地の色へこの割合だけ寄せる
    // (brushState.comp 参照)。拾う先はアクティブレイヤー(+ここまでのストローク)。
    void setMixRate(float v) { mixRate_ = qBound(0.f, v, 1.f); }

    // 筆に乗る絵の具の量。100%で「尽きない」(=従来どおり)。それ未満だと有限になり、
    // 進むほど減って、尽きると新しい絵の具を置かなくなる(線がかすれて消える)。
    // 伸びは「同じ量でどれだけ長く保つか」(消費の遅さ)。量が100%のときは効かない。
    void setPaintAmount(float v) { paintAmount_ = qBound(0.f, v, 1.f); }
    void setPaintExtend(float v) { paintExtend_ = qBound(0.f, v, 1.f); }

    // 「拾った色だけで塗る」。筆が自分の絵の具を持たなくなり、下地から拾ったぶんしか
    // 置かなくなる(=指先・色伸ばし)。何も無い所から描き始めても何も付かず、
    // 塗ってある所からなぞり始めるとその色を引き伸ばせる。
    // 拾う動作そのものは下地混色なので、これは下地混色と併用して初めて意味を持つ。
    void setPickupOnly(bool on) { pickupOnly_ = on; }

    // 色のランダム。スタンプごとに色を散らす(ストローク色バッファを使う経路。
    // stroke.comp の uUseStrokeColor 参照)。
    // hueJitter  : 色相を ±(値×180°) の範囲でずらす(色相に上限は無いので前後対称)
    // valueJitter: 明度を「最大から減らす」向きにずらす(他のランダムと同じ向き)
    void setHueJitter(float j)     { hueJitter_     = qBound(0.f, j, 1.f); }
    void setValueJitter(float j)   { valueJitter_   = qBound(0.f, j, 1.f); }

    // 以下はいずれも「最大値からどれだけランダムに減らすか/ずらすか」の割合。0で無効。
    void setSizeJitter(float j)    { sizeJitter_    = qBound(0.f, j, 1.f); }
    void setOpacityJitter(float j) { opacityJitter_ = qBound(0.f, j, 1.f); }
    void setAngleJitter(float j)   { angleJitter_   = qBound(0.f, j, 1.f); }
    void setSpacingJitter(float j) { spacingJitter_ = qBound(0.f, j, 1.f); }

    int    size()     const { return size_; }
    float  opacity()  const { return opacity_; }
    float  hardness() const { return hardness_; }
    float  spacing()  const { return spacing_; }
    QString tipImagePath() const { return tipImagePath_; }
    const PressureCurve &pressureCurve() const { return pressureCurve_; }
    float  minSizeRatio()    const { return minSizeRatio_; }
    bool   pressureOpacity() const { return pressureOpacity_; }
    float  minOpacityRatio() const { return minOpacityRatio_; }
    float  tiltSize()          const { return tiltSize_; }
    float  tiltOpacity()       const { return tiltOpacity_; }
    float  tiltFlatten()       const { return tiltFlatten_; }
    bool   tiltAngleFollow()   const { return tiltAngleFollow_; }
    bool   penRotationFollow() const { return penRotationFollow_; }
    float   postCorrection() const { return postCorrection_; }
    float   flow()           const { return flow_; }
    int     brushBlendMode() const { return brushBlendMode_; }
    QString paperTexPath()  const { return paperTexPath_; }
    float   paperStrength() const { return paperStrength_; }
    float   paperScale()    const { return paperScale_; }
    // 紙質が実際に効くか(画像が選ばれていて、かつ適用量が0より大きい)。
    bool    paperEnabled()  const { return !paperTexPath_.isEmpty() && paperStrength_ > 0.0f; }
    int    angleDeg()        const { return angleDeg_; }
    float  roundness()       const { return roundness_; }
    bool   followDirection() const { return followDirection_; }
    int    taperInPx()     const { return taperInPx_; }
    int    taperOutPx()    const { return taperOutPx_; }
    bool   taperSize()     const { return taperSize_; }
    bool   taperOpacity()  const { return taperOpacity_; }
    float  scatter()       const { return scatter_; }
    int    particleCount() const { return particleCount_; }
    float  mixRate()       const { return mixRate_; }
    float  paintAmount()   const { return paintAmount_; }
    float  paintExtend()   const { return paintExtend_; }
    bool   pickupOnly()    const { return pickupOnly_; }
    // 絵の具が尽きる設定になっているか(100%は「尽きない」なので無効扱い)。
    bool   usesPaintDepletion() const { return paintAmount_ < 1.0f; }
    // 絵の具が保つ距離[px]。sizeはブラシ直径[px]。
    // usesPaintDepletion()が真のときだけ意味がある(0は「即座に尽きる」で、
    // 「尽きない」とは別物なので、呼ぶ側は必ずusesPaintDepletion()で分岐すること)。
    float  paintCapacityPx(int size) const {
        // 伸び0%で0.25倍、100%で4倍。基準は「直径20個ぶん」。
        constexpr float kBaseDiameters = 20.0f;
        return paintAmount_ * (0.25f + 3.75f * paintExtend_) * kBaseDiameters * float(size);
    }
    float  hueJitter()     const { return hueJitter_; }
    float  valueJitter()   const { return valueJitter_; }
    // スタンプごとに色が変わるか(=ストローク色バッファが要るか)。
    bool   usesPerStampColor() const {
        return mixRate_ > 0.0f || hueJitter_ > 0.0f || valueJitter_ > 0.0f;
    }
    float  sizeJitter()    const { return sizeJitter_; }
    float  opacityJitter() const { return opacityJitter_; }
    float  angleJitter()   const { return angleJitter_; }
    float  spacingJitter() const { return spacingJitter_; }

    QVariantMap toMap() const {
        QVariantMap m;
        m["size"]         = size_;
        m["opacity"]      = opacity_;
        m["hardness"]     = hardness_;
        m["spacing"]      = spacing_;
        m["tipImagePath"] = tipImagePath_;
        m["pressureCurve"]   = pressureCurve_.toString();
        m["minSizeRatio"]    = minSizeRatio_;
        m["pressureOpacity"] = pressureOpacity_;
        m["minOpacityRatio"] = minOpacityRatio_;
        m["tiltSize"]          = tiltSize_;
        m["tiltOpacity"]       = tiltOpacity_;
        m["tiltFlatten"]       = tiltFlatten_;
        m["tiltAngleFollow"]   = tiltAngleFollow_;
        m["penRotationFollow"] = penRotationFollow_;
        m["postCorrection"] = postCorrection_;
        m["flow"]           = flow_;
        m["brushBlendMode"] = brushBlendMode_;
        m["paperTexPath"]  = paperTexPath_;
        m["paperStrength"] = paperStrength_;
        m["paperScale"]    = paperScale_;
        m["angleDeg"]        = angleDeg_;
        m["roundness"]       = roundness_;
        m["followDirection"] = followDirection_;
        m["taperInPx"]     = taperInPx_;
        m["taperOutPx"]    = taperOutPx_;
        m["taperSize"]     = taperSize_;
        m["taperOpacity"]  = taperOpacity_;
        m["scatter"]       = scatter_;
        m["particleCount"] = particleCount_;
        m["mixRate"]       = mixRate_;
        m["paintAmount"]   = paintAmount_;
        m["paintExtend"]   = paintExtend_;
        m["pickupOnly"]    = pickupOnly_;
        m["hueJitter"]     = hueJitter_;
        m["valueJitter"]   = valueJitter_;
        m["sizeJitter"]    = sizeJitter_;
        m["opacityJitter"] = opacityJitter_;
        m["angleJitter"]   = angleJitter_;
        m["spacingJitter"] = spacingJitter_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        size_         = m.value("size", size_).toInt();
        opacity_      = m.value("opacity", opacity_).toFloat();
        hardness_     = m.value("hardness", hardness_).toFloat();
        spacing_      = m.value("spacing", spacing_).toFloat();
        tipImagePath_ = m.value("tipImagePath", tipImagePath_).toString();
        pressureCurve_   = PressureCurve::fromString(m.value("pressureCurve").toString());
        minSizeRatio_    = m.value("minSizeRatio", minSizeRatio_).toFloat();
        pressureOpacity_ = m.value("pressureOpacity", pressureOpacity_).toBool();
        minOpacityRatio_ = m.value("minOpacityRatio", minOpacityRatio_).toFloat();
        tiltSize_          = m.value("tiltSize", tiltSize_).toFloat();
        tiltOpacity_       = m.value("tiltOpacity", tiltOpacity_).toFloat();
        tiltFlatten_       = m.value("tiltFlatten", tiltFlatten_).toFloat();
        tiltAngleFollow_   = m.value("tiltAngleFollow", tiltAngleFollow_).toBool();
        penRotationFollow_ = m.value("penRotationFollow", penRotationFollow_).toBool();
        postCorrection_ = m.value("postCorrection", postCorrection_).toFloat();
        flow_           = m.value("flow", flow_).toFloat();
        brushBlendMode_ = m.value("brushBlendMode", brushBlendMode_).toInt();
        paperTexPath_  = m.value("paperTexPath", paperTexPath_).toString();
        paperStrength_ = m.value("paperStrength", paperStrength_).toFloat();
        paperScale_    = m.value("paperScale", paperScale_).toFloat();
        angleDeg_        = m.value("angleDeg", angleDeg_).toInt();
        roundness_       = m.value("roundness", roundness_).toFloat();
        followDirection_ = m.value("followDirection", followDirection_).toBool();
        taperInPx_     = m.value("taperInPx", taperInPx_).toInt();
        taperOutPx_    = m.value("taperOutPx", taperOutPx_).toInt();
        taperSize_     = m.value("taperSize", taperSize_).toBool();
        taperOpacity_  = m.value("taperOpacity", taperOpacity_).toBool();
        scatter_       = m.value("scatter", scatter_).toFloat();
        particleCount_ = m.value("particleCount", particleCount_).toInt();
        mixRate_       = m.value("mixRate", mixRate_).toFloat();
        paintAmount_   = m.value("paintAmount", paintAmount_).toFloat();
        paintExtend_   = m.value("paintExtend", paintExtend_).toFloat();
        pickupOnly_    = m.value("pickupOnly", pickupOnly_).toBool();
        hueJitter_     = m.value("hueJitter", hueJitter_).toFloat();
        valueJitter_   = m.value("valueJitter", valueJitter_).toFloat();
        sizeJitter_    = m.value("sizeJitter", sizeJitter_).toFloat();
        opacityJitter_ = m.value("opacityJitter", opacityJitter_).toFloat();
        angleJitter_   = m.value("angleJitter", angleJitter_).toFloat();
        spacingJitter_ = m.value("spacingJitter", spacingJitter_).toFloat();
    }

private:
    int size_ = 5;
    float opacity_ = 1.0f;
    float hardness_ = 0.5f;
    float spacing_ = 0.1f;
    QString tipImagePath_ = ":/textures/penTip/circle.png";
    PressureCurve pressureCurve_;
    float minSizeRatio_    = 0.0f;
    bool  pressureOpacity_ = false;
    float minOpacityRatio_ = 0.0f;
    // 傾き・ペン回転は既定ですべて無効(=従来どおり)
    float tiltSize_          = 0.0f;
    float tiltOpacity_       = 0.0f;
    float tiltFlatten_       = 0.0f;
    bool  tiltAngleFollow_   = false;
    bool  penRotationFollow_ = false;
    // 後補正は既定で0(=引いたとおり。従来どおり)。
    float postCorrection_ = 0.0f;
    // フロー100% = 1スタンプで不透明度まで到達(=従来どおり)。合成モード0 = 普通。
    float flow_           = 1.0f;
    int   brushBlendMode_ = 0;
    // 紙質は既定で「なし」(=従来どおり)。適用量/拡大率は有効にしたときの初期値。
    QString paperTexPath_;
    float   paperStrength_ = 0.5f;
    float   paperScale_    = 1.0f;
    // 先端の形は既定で「回さない・真円・追従なし」(=従来どおり)
    int   angleDeg_        = 0;
    float roundness_       = 1.0f;
    bool  followDirection_ = false;
    // 入り抜きは既定で長さ0(=無効)。有効にしたときの対象は「サイズ」が既定。
    int   taperInPx_     = 0;
    int   taperOutPx_    = 0;
    bool  taperSize_     = true;
    bool  taperOpacity_  = false;
    // 散布・ランダムは既定すべて0(=1粒を軌道どおりに等間隔で打つ従来の動作)
    float scatter_       = 0.0f;
    int   particleCount_ = 1;
    float mixRate_       = 0.0f;
    // 絵の具量100% = 尽きない(=従来どおり)。伸びはそのときの初期値。
    float paintAmount_   = 1.0f;
    float paintExtend_   = 0.5f;
    bool  pickupOnly_    = false;
    float hueJitter_     = 0.0f;
    float valueJitter_   = 0.0f;
    float sizeJitter_    = 0.0f;
    float opacityJitter_ = 0.0f;
    float angleJitter_   = 0.0f;
    float spacingJitter_ = 0.0f;
};

// エアブラシ。先端画像は常に円(procedural)固定のためPenToolConfigと違いtipImagePathを
// 持たない。ペンは1ストローク分をまとめて1回だけ焼き込む(同じ場所を往復しても
// 設定した不透明度までしか濃くならない)のに対し、エアブラシはスタンプ1つごとに
// 即座に焼き込む(AirbrushTool参照)ため、重ねるほど際限なく濃くなっていく。
class AirbrushToolConfig
{
public:
    void setSize(int s) { size_ = qMax(1, s); }
    void setOpacity(float o) { opacity_ = qBound(0.f, o, 1.f); }
    void setHardness(float h) { hardness_ = qBound(0.f, h, 1.f); }
    // スタンプ間隔。ブラシ直径に対する比率(PenToolConfig::spacingと同じ考え方)。
    void setSpacing(float s) { spacing_ = qBound(0.05f, s, 3.0f); }
    // 手振れ補正の強さ(CanvasWidget::smoothingStrengthと同じ意味の値。1.0で補正なし、
    // 小さいほどペン先の動きに対して描画位置の追従が遅れる=強く補正される)。
    // ペン/消しゴムはCanvasWidget共有の1つの値を使うが、エアブラシはツールプリセットごとに
    // 独立した値を持たせる(ツール設定項目として明示的に要求されたため)。
    void setSmoothing(float s) { smoothing_ = qBound(0.01f, s, 1.0f); }
    void setPressureCurve(const PressureCurve &c) { pressureCurve_ = c; } // 以下4つ PenToolConfigと同じ
    void setMinSizeRatio(float r)    { minSizeRatio_ = qBound(0.f, r, 1.f); }
    void setPressureOpacity(bool on) { pressureOpacity_ = on; }
    void setMinOpacityRatio(float r) { minOpacityRatio_ = qBound(0.f, r, 1.f); }

    int    size()      const { return size_; }
    float  opacity()   const { return opacity_; }
    float  hardness()  const { return hardness_; }
    float  spacing()   const { return spacing_; }
    float  smoothing() const { return smoothing_; }
    const PressureCurve &pressureCurve() const { return pressureCurve_; }
    float  minSizeRatio()    const { return minSizeRatio_; }
    bool   pressureOpacity() const { return pressureOpacity_; }
    float  minOpacityRatio() const { return minOpacityRatio_; }

    QVariantMap toMap() const {
        QVariantMap m;
        m["size"]      = size_;
        m["opacity"]   = opacity_;
        m["hardness"]  = hardness_;
        m["spacing"]   = spacing_;
        m["smoothing"]       = smoothing_;
        m["pressureCurve"]   = pressureCurve_.toString();
        m["minSizeRatio"]    = minSizeRatio_;
        m["pressureOpacity"] = pressureOpacity_;
        m["minOpacityRatio"] = minOpacityRatio_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        size_      = m.value("size", size_).toInt();
        opacity_   = m.value("opacity", opacity_).toFloat();
        hardness_  = m.value("hardness", hardness_).toFloat();
        spacing_   = m.value("spacing", spacing_).toFloat();
        smoothing_ = m.value("smoothing", smoothing_).toFloat();
        pressureCurve_   = PressureCurve::fromString(m.value("pressureCurve").toString());
        minSizeRatio_    = m.value("minSizeRatio", minSizeRatio_).toFloat();
        pressureOpacity_ = m.value("pressureOpacity", pressureOpacity_).toBool();
        minOpacityRatio_ = m.value("minOpacityRatio", minOpacityRatio_).toFloat();
    }

private:
    PressureCurve pressureCurve_;
    float minSizeRatio_    = 0.0f;
    bool  pressureOpacity_ = false;
    float minOpacityRatio_ = 0.0f;
    int   size_      = 80;
    // 重ねるほど濃くなる特性上、ペン(既定100%)より低め・スプレー的な既定値にしてある。
    float opacity_   = 0.15f;
    float hardness_  = 0.3f;
    float spacing_   = 0.1f;
    float smoothing_ = 0.5f;
};

class EraserToolConfig
{
public:
    void setSize(int s) { size_ = qMax(1, s);}
    void setHardness(float h) { hardness_ = qBound(0.f, h, 1.f); }
    // 消しゴムもPenEraserToolを共有しているためスタンプ間隔を持つ(UIは無く既定値のみ)。
    void setSpacing(float s) { spacing_ = qBound(0.05f, s, 3.0f); }
    void setPressureCurve(const PressureCurve &c) { pressureCurve_ = c; } // 以下2つ PenToolConfigと同じ
    void setMinSizeRatio(float r) { minSizeRatio_ = qBound(0.f, r, 1.f); }

    int    size()     const { return size_; }
    float  hardness() const { return hardness_; }
    float  spacing()  const { return spacing_; }
    const PressureCurve &pressureCurve() const { return pressureCurve_; }
    float  minSizeRatio() const { return minSizeRatio_; }

    QVariantMap toMap() const {
        QVariantMap m;
        m["size"]     = size_;
        m["hardness"] = hardness_;
        m["spacing"]  = spacing_;
        m["pressureCurve"] = pressureCurve_.toString();
        m["minSizeRatio"]  = minSizeRatio_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        size_     = m.value("size", size_).toInt();
        hardness_ = m.value("hardness", hardness_).toFloat();
        spacing_  = m.value("spacing", spacing_).toFloat();
        pressureCurve_ = PressureCurve::fromString(m.value("pressureCurve").toString());
        minSizeRatio_  = m.value("minSizeRatio", minSizeRatio_).toFloat();
    }

private:
    int size_ = 20;
    float hardness_ = 0.5f;
    float spacing_ = 0.1f;
    PressureCurve pressureCurve_;
    float minSizeRatio_ = 0.0f;
};

// パラメータを持たないツール(移動/回転)向けの設定クラス。
// 「ツールに設定項目が無くてもツールプリセットとしては扱う」という仕様のための
// 最小限のプレースホルダー。将来これらのツールに設定を追加したくなったら
// 個別のXxxToolConfigクラスに置き換え、toMap/fromMapへメンバを足せばよい。
class EmptyToolConfig
{
public:
    QVariantMap toMap() const { return {}; }
    void fromMap(const QVariantMap &) {}
};
using MoveToolConfig    = EmptyToolConfig;
using RotateToolConfig  = EmptyToolConfig;
using TextToolConfig    = EmptyToolConfig;

class DropperToolConfig
{
public:
    // 色を拾う参照先。false: 現在のレイヤーのみを参照する / true: 全レイヤーを合成したキャンバスを参照する
    void setReferenceCanvas(bool v) { referenceCanvas_ = v; }
    bool referenceCanvas() const { return referenceCanvas_; }

    QVariantMap toMap() const {
        QVariantMap m;
        m["referenceCanvas"] = referenceCanvas_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        referenceCanvas_ = m.value("referenceCanvas", referenceCanvas_).toBool();
    }
private:
    bool referenceCanvas_ = false;
};

class BlurToolConfig
{
public:
    void setSize(int s)        { size_ = qMax(1, s); }
    void setHardness(float h)  { hardness_ = qBound(0.f, h, 1.f); }
    void setStrength(float s)  { strength_ = qBound(0.f, s, 1.f); }
    void setBlurRadius(int r)  { blurRadius_ = qBound(1, r, 32); }
    void setPressureCurve(const PressureCurve &c) { pressureCurve_ = c; } // 以下2つ PenToolConfigと同じ
    void setMinSizeRatio(float r) { minSizeRatio_ = qBound(0.f, r, 1.f); }

    int   size()       const { return size_; }
    float hardness()   const { return hardness_; }
    float strength()   const { return strength_; }   // 元画像と混ぜる量
    int   blurRadius() const { return blurRadius_; }  // ぼかしサンプル半径(px)
    const PressureCurve &pressureCurve() const { return pressureCurve_; }
    float minSizeRatio() const { return minSizeRatio_; }

    QVariantMap toMap() const {
        QVariantMap m;
        m["size"]       = size_;
        m["hardness"]   = hardness_;
        m["strength"]   = strength_;
        m["blurRadius"] = blurRadius_;
        m["pressureCurve"] = pressureCurve_.toString();
        m["minSizeRatio"]  = minSizeRatio_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        size_       = m.value("size", size_).toInt();
        hardness_   = m.value("hardness", hardness_).toFloat();
        strength_   = m.value("strength", strength_).toFloat();
        blurRadius_ = m.value("blurRadius", blurRadius_).toInt();
        pressureCurve_ = PressureCurve::fromString(m.value("pressureCurve").toString());
        minSizeRatio_  = m.value("minSizeRatio", minSizeRatio_).toFloat();
    }

private:
    PressureCurve pressureCurve_;
    float minSizeRatio_ = 0.0f;
    int   size_       = 40;
    float hardness_   = 0.5f;
    float strength_   = 0.5f;
    int   blurRadius_ = 4;
};

class WarpToolConfig
{
public:
    void setSize(int s)       { size_ = qMax(1, s); }
    void setHardness(float h) { hardness_ = qBound(0.f, h, 1.f); }
    void setStrength(float s) { strength_ = qBound(0.f, s, 1.f); }
    void setPressureCurve(const PressureCurve &c) { pressureCurve_ = c; } // 以下2つ PenToolConfigと同じ
    void setMinSizeRatio(float r) { minSizeRatio_ = qBound(0.f, r, 1.f); }

    int   size()     const { return size_; }
    float hardness() const { return hardness_; }
    float strength() const { return strength_; } // 引きずりの強さ(1で移動量そのまま追従)
    const PressureCurve &pressureCurve() const { return pressureCurve_; }
    float minSizeRatio() const { return minSizeRatio_; }

    QVariantMap toMap() const {
        QVariantMap m;
        m["size"]     = size_;
        m["hardness"] = hardness_;
        m["strength"] = strength_;
        m["pressureCurve"] = pressureCurve_.toString();
        m["minSizeRatio"]  = minSizeRatio_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        size_     = m.value("size", size_).toInt();
        hardness_ = m.value("hardness", hardness_).toFloat();
        strength_ = m.value("strength", strength_).toFloat();
        pressureCurve_ = PressureCurve::fromString(m.value("pressureCurve").toString());
        minSizeRatio_  = m.value("minSizeRatio", minSizeRatio_).toFloat();
    }

private:
    PressureCurve pressureCurve_;
    float minSizeRatio_ = 0.0f;
    int   size_     = 60;
    float hardness_ = 0.3f;
    float strength_ = 0.1f;
};

// 選択ツールの動作モード。SelectionToolConfig の1フィールドとして持たせることで、
// 「投げ縄選択」「ペン選択」を1つのSelectToolクラスに対する別々のツールプリセット
// プリセットとして表現できる(FillのreferenceCanvasと同じ考え方)。
enum class SelectionMode { Lasso, PenSelect };

class SelectionToolConfig
{
public:
    void setMode(SelectionMode m) { mode_ = m; }
    SelectionMode mode() const { return mode_; }

    // ペン選択時のブラシ設定(投げ縄では未使用)
    void  setSize(int s)      { size_ = qMax(1, s); }
    void  setHardness(float h) { hardness_ = qBound(0.f, h, 1.f); }
    int   size()     const { return size_; }
    float hardness() const { return hardness_; }

    QVariantMap toMap() const {
        QVariantMap m;
        m["mode"]     = (int)mode_;
        m["size"]     = size_;
        m["hardness"] = hardness_;
        return m;
    }
    void fromMap(const QVariantMap &m) {
        mode_     = (SelectionMode)m.value("mode", (int)mode_).toInt();
        size_     = m.value("size", size_).toInt();
        hardness_ = m.value("hardness", hardness_).toFloat();
    }

private:
    SelectionMode mode_     = SelectionMode::Lasso;
    int           size_     = 30;
    float         hardness_ = 0.8f;
};

class ColorConfig
{
public:
    void setRawRGBA(QColor rgba) { rawRGBA_ = rgba;}

    QColor rawRGBA() const { return rawRGBA_; }

    // 「透明色」。RGBを塗る代わりに、そこにあるものを消す色として扱う
    // (カラーサークル隅の市松模様のボタンで選ぶ)。
    //
    // 「不透明度0%の色」で代用しないのは、そちらは本来「何も描かない」であるべき
    // だから。以前は bake.comp が uBrushColor==vec4(0) を消しゴムの合図に使って
    // いたため、不透明度0%のペンが消しゴムとして働いてしまっていた。
    //
    // 消す強さは rawRGBA_ のアルファ(通常色のアルファと同じ位置づけ)で決まり、
    // 小さいほど消えにくくなる。eraseBrushFor() 参照。
    void setTransparent(bool on) { transparent_ = on; }
    bool isTransparent() const { return transparent_; }

private:
    QColor rawRGBA_ = QColor(0, 0, 0, 255); // 初期色は黒
    bool   transparent_ = false;
};

// ---------------------------------------------------------------------------
// 「消す」描画の判定と強さ
// ---------------------------------------------------------------------------
// 消しゴムツールと透明色は、どちらも「色を塗る」のではなく「アルファを削る」
// 動作になる。両者の違いは強さだけ:
//   ・消しゴムツール: 常に全力(1.0)。消しゴムに不透明度の概念は無い。
//   ・透明色        : ブラシの不透明度 × 色のアルファ。アルファを下げるほど
//                     消えにくくなる(薄く消せる)。
// シェーダーへは uEraseMode=1 と、この強さを uBrushColor.a として渡す
// (bake.comp / render.frag / belowComposite.comp)。
// ---------------------------------------------------------------------------
struct EraseBrush {
    bool  active   = false;
    float strength = 1.0f;
    // シェーダーへ渡す色。rgbは使われず、aだけが「消す強さ」として読まれる。
    QColor shaderColor() const { return QColor::fromRgbF(0.0f, 0.0f, 0.0f, strength); }
};

inline EraseBrush eraseBrushFor(bool isEraserTool, const ColorConfig &color, float brushOpacity)
{
    EraseBrush e;
    if (isEraserTool) {
        e.active   = true;
        e.strength = 1.0f;
    } else if (color.isTransparent()) {
        e.active   = true;
        e.strength = qBound(0.0f, brushOpacity * (float)color.rawRGBA().alphaF(), 1.0f);
    }
    return e;
}

// 表示上の見た目だけを変えるカラーモード(実データは常にRGBAのまま)。
// render.fragの最終合成結果に対して掛ける(CanvasWidget::paintGL()がuColorModeとして送る)。
// グレースケールは2種類:
//   GrayscaleLuminance: 輝度ベース(Y = 0.299R+0.587G+0.114B、人間の知覚に近い)
//   GrayscaleLightness: 明度ベース(HSLのL = (max+min)/2。「色相・彩度・明度」調整
//                        ツール(adjustHSL, common.glsl)と同じ定義)
enum class ColorMode { RGB, CMYK, GrayscaleLuminance, GrayscaleLightness };

class ColorModeConfig
{
public:
    void setMode(ColorMode m) { mode_ = m; }
    ColorMode mode() const { return mode_; }

private:
    ColorMode mode_ = ColorMode::RGB;
};

// モニターキャリブレーション。カラーモード変換後の最終出力に対して常に
// 適用される「表示調整」(実データ/カラーモードとは無関係、モニター環境の
// クセを補正するためのもの)。値域はBrightnessContrastTool/ColorBalanceTool
// と同じ-100〜100の整数(CanvasWidget::paintGL()でfloatへ変換してuniformへ送る)。
class CalibrationConfig
{
public:
    void setBrightness(int v) { brightness_ = qBound(-100, v, 100); }
    int  brightness() const { return brightness_; }

    void setContrast(int v) { contrast_ = qBound(-100, v, 100); }
    int  contrast() const { return contrast_; }

    void setCyan(int v) { cyan_ = qBound(-100, v, 100); }
    int  cyan() const { return cyan_; }

    void setMagenta(int v) { magenta_ = qBound(-100, v, 100); }
    int  magenta() const { return magenta_; }

    void setYellow(int v) { yellow_ = qBound(-100, v, 100); }
    int  yellow() const { return yellow_; }

private:
    int brightness_ = 0;
    int contrast_   = 0;
    int cyan_       = 0;
    int magenta_    = 0;
    int yellow_     = 0;
};

// キャンバスの外側(枠外)を塗る背景色。render.fragでは以前
// vec4(0.5, 0.5, 0.5, 1.0)にハードコードされていたもので、実データ
// (レイヤーのRGBA)には一切関係ない、表示上の背景色プレビュー設定。
class CanvasBackgroundConfig
{
public:
    void setColor(const QColor &c) { color_ = c; }
    QColor color() const { return color_; }

private:
    QColor color_ = QColor(128, 128, 128); // 従来のハードコード値(0.5,0.5,0.5)と同じ
};

// ===========================================================================
// ToolConfig
// ---------------------------------------------------------------------------
// 各ToolTypeごとに「ツールプリセット」(名前付き設定プリセット)の一覧を持つ。
// pen()/eraser()/fill()/move()/rotate()/dropper() は、その時点でアクティブな
// ツールプリセットの設定を返すだけなので、PenEraserTool等の既存ツールクラスは
// ツールプリセットの存在を一切意識せずに動作する。
//
// 新しいツールを追加する手順:
//   1. XxxToolConfig を作り toMap()/fromMap() を実装する
//      (パラメータが無ければ EmptyToolConfig を使い回してよい)
//   2. ToolType.h に enum値を足す
//   3. ToolRegistry.cpp のテーブルに1行足す(アイコン/ラベル/設定キー)
//   4. ここに ToolPresetList<XxxToolConfig> とアダプタを1つずつ追加し、
//      xxx() アクセサと toolPresetList() のswitchに1行足す
// ===========================================================================
class ToolConfig
{
public:
    ToolConfig()
        : pen_("ペン1"), eraser_("消しゴム1"), fill_("塗りつぶし1"),
          dropper_("スポイト1"), move_("移動1"), rotate_("回転1"), blur_("ぼかし1"),
          warp_("ゆがみ1"), selection_("投げ縄選択1"), text_("テキスト1"),
          airbrush_("エアブラシ1"),
          penAdapter_(&pen_), eraserAdapter_(&eraser_), fillAdapter_(&fill_),
          dropperAdapter_(&dropper_), moveAdapter_(&move_), rotateAdapter_(&rotate_),
          blurAdapter_(&blur_), warpAdapter_(&warp_), selectionAdapter_(&selection_),
          textAdapter_(&text_), airbrushAdapter_(&airbrush_)
    {
        // 「選択」ツールは投げ縄とペン選択という質の異なる操作を持つが、
        // どちらもSelectionToolConfig::mode()の値が違うだけの1クラスなので、
        // ツールプリセットのプリセットとして両方あらかじめ用意しておく。
        SelectionToolConfig penSelectCfg;
        penSelectCfg.setMode(SelectionMode::PenSelect);
        selection_.add("ペン選択1", penSelectCfg);
    }

    FillToolConfig &fill() { return fill_.active(); }
    const FillToolConfig &fill() const { return fill_.active(); }

    PenToolConfig &pen() { return pen_.active(); }
    const PenToolConfig &pen() const { return pen_.active(); }

    EraserToolConfig &eraser() { return eraser_.active(); }
    const EraserToolConfig &eraser() const { return eraser_.active(); }

    MoveToolConfig    &move()    { return move_.active(); }
    RotateToolConfig  &rotate()  { return rotate_.active(); }
    DropperToolConfig &dropper() { return dropper_.active(); }
    BlurToolConfig    &blur()    { return blur_.active(); }
    const BlurToolConfig &blur() const { return blur_.active(); }
    WarpToolConfig    &warp()    { return warp_.active(); }
    const WarpToolConfig &warp() const { return warp_.active(); }
    SelectionToolConfig &selection() { return selection_.active(); }
    const SelectionToolConfig &selection() const { return selection_.active(); }
    TextToolConfig &text() { return text_.active(); }
    const TextToolConfig &text() const { return text_.active(); }

    AirbrushToolConfig &airbrush() { return airbrush_.active(); }
    const AirbrushToolConfig &airbrush() const { return airbrush_.active(); }

    ColorConfig &color() { return color_; }

    ColorModeConfig &colorMode() { return colorMode_; }
    const ColorModeConfig &colorMode() const { return colorMode_; }

    CalibrationConfig &calibration() { return calibration_; }
    const CalibrationConfig &calibration() const { return calibration_; }

    CanvasBackgroundConfig &canvasBackground() { return canvasBackground_; }
    const CanvasBackgroundConfig &canvasBackground() const { return canvasBackground_; }

    // 全ツール共通の筆圧カーブ(環境設定で編集する)。ツールごとのカーブより先に
    // 適用される。ツールプリセットとは無関係のアプリ全体の設定なので、他の
    // 全体設定(カラーモード/キャリブレーション等)と同じくここに直接持つ。
    // 永続化は SettingsDlg 側(preferences/input/pressureCurve)が担当し、
    // MainWindow が起動時と設定ダイアログのOK時にここへ反映する。
    PressureCurve &globalPressureCurve() { return globalPressureCurve_; }
    const PressureCurve &globalPressureCurve() const { return globalPressureCurve_; }

    // ---- ツールプリセット一覧への型消去アクセス(ToolPresetDock/永続化用) ----
    IToolPresetList *toolPresetList(ToolType type)
    {
        switch (type) {
        case ToolType::Pen:     return &penAdapter_;
        case ToolType::Eraser:  return &eraserAdapter_;
        case ToolType::Fill:    return &fillAdapter_;
        case ToolType::Dropper: return &dropperAdapter_;
        case ToolType::Move:    return &moveAdapter_;
        case ToolType::Rotate:  return &rotateAdapter_;
        case ToolType::Blur:    return &blurAdapter_;
        case ToolType::Warp:    return &warpAdapter_;
        case ToolType::Selection: return &selectionAdapter_;
        case ToolType::Text:    return &textAdapter_;
        case ToolType::Airbrush: return &airbrushAdapter_;
        }
        return nullptr;
    }

    // ---- 永続化(QSettingsへツールプリセット一覧をまるごと保存/復元) ----
    void saveToSettings(QSettings &s);
    void loadFromSettings(QSettings &s);

private:
    ToolPresetList<PenToolConfig>     pen_;
    ToolPresetList<EraserToolConfig>  eraser_;
    ToolPresetList<FillToolConfig>    fill_;
    ToolPresetList<DropperToolConfig> dropper_;
    ToolPresetList<MoveToolConfig>    move_;
    ToolPresetList<RotateToolConfig>  rotate_;
    ToolPresetList<BlurToolConfig>    blur_;
    ToolPresetList<WarpToolConfig>    warp_;
    ToolPresetList<SelectionToolConfig> selection_;
    ToolPresetList<TextToolConfig>    text_;
    ToolPresetList<AirbrushToolConfig> airbrush_;
    ColorConfig color_;
    ColorModeConfig colorMode_;
    CalibrationConfig calibration_;
    CanvasBackgroundConfig canvasBackground_;
    PressureCurve globalPressureCurve_;

    ToolPresetListAdapter<PenToolConfig>     penAdapter_;
    ToolPresetListAdapter<EraserToolConfig>  eraserAdapter_;
    ToolPresetListAdapter<FillToolConfig>    fillAdapter_;
    ToolPresetListAdapter<DropperToolConfig> dropperAdapter_;
    ToolPresetListAdapter<MoveToolConfig>    moveAdapter_;
    ToolPresetListAdapter<RotateToolConfig>  rotateAdapter_;
    ToolPresetListAdapter<BlurToolConfig>    blurAdapter_;
    ToolPresetListAdapter<WarpToolConfig>    warpAdapter_;
    ToolPresetListAdapter<SelectionToolConfig> selectionAdapter_;
    ToolPresetListAdapter<TextToolConfig>    textAdapter_;
    ToolPresetListAdapter<AirbrushToolConfig> airbrushAdapter_;
};