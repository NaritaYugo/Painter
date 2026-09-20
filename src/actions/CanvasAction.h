#pragma once

#include <QtGlobal>
#include <QVector2D>

// ---------------------------------------------------------------------------
// CanvasAction
// ---------------------------------------------------------------------------
// 色調整/フィルター/変形/キャンバスサイズ/レイヤー編集など、「メニューやショートカット
// から開始し、専用パネルやキャンバス上のハンドルで操作して、確定/キャンセルで終わる」
// 一回限りのアクションの抽象基底。以前は CanvasWidget に startXxx/confirmXxx/cancelXxx/
// positionXxxPanel として直書きされ、相互排他・描画uniform・マウス入力ブロックが
// あちこちに散らばっていたものを、この基底 + CanvasActionController に集約する。
//
// ライフサイクルはテンプレートメソッドで共通化する:
//   begin()   … canActivate() ガード → makeCurrent → onActivate()(engaged判定)→ active化
//   confirm() … onConfirm()(タイルへ焼き込む等)→ active解除
//   cancel()  … onCancel()(レイヤー本体に触れず破棄)→ active解除
// 具体アクションは onActivate/onConfirm/onCancel と、必要な結合点フック
// (applyRenderState/paintOverlay/handleMouseXxx/positionPanel/blocksToolInput)だけを
// 実装する。パネルとツールは各アクションが所有する。
// ---------------------------------------------------------------------------

#include <QPointF>

class QOpenGLContext;
class QOpenGLShaderProgram;
class QMouseEvent;
class QWidget;
class QPainter;
struct ToolContext;
class CanvasActionHost;
class CanvasActionController;

class CanvasAction
{
public:
    CanvasAction(CanvasActionHost &host, CanvasActionController &ctrl)
        : host_(host), ctrl_(ctrl) {}
    virtual ~CanvasAction() = default;

    bool isActive() const { return active_; }

    // コントローラから呼ばれるライフサイクル。
    bool begin();   // 成功して active になったら true
    void confirm();
    void cancel();

    // ---- 結合点フック(既定は何もしない) --------------------------------
    // GL 初期化時に一度だけ呼ばれる(所有するツールの initialize 用)。
    virtual void initialize(QOpenGLContext * /*ctx*/) {}
    // CanvasWidget デストラクタから、GLコンテキストがまだ有効なうちに一度だけ呼ばれる。
    // 動的コンパイル済みシェーダー等、tool 側で明示的な解放が必要な場合に使う
    // (CustomShaderTool::releaseGL 等。ほとんどのアクションは何もしなくてよい)。
    virtual void releaseGL() {}
    // ウィジェットのリサイズ時、アクティブなアクションに対してだけ呼ばれる。
    virtual void positionPanel() {}
    // このアクションが表示中、通常のペン等ツール入力をブロックするか
    // (色調整/フィルター/レイヤー編集パネルは true、変形系は false)。
    virtual bool blocksToolInput() const { return true; }
    // blocksToolInput()==true のアクションのうち、移動/回転ツールへの切り替えだけは
    // 素通しする(パネルを開いたままキャンバスを見る位置調整ができる)か。
    // 色調整/フィルター系パネルは true(既定)。画像解像度変更/レイヤー編集系パネル
    // (対象がキャンバス実データそのものの構造変更/直接編集のため中途半端な移動・回転
    // 操作を許すと混乱するもの)は false でオーバーライドする。
    virtual bool blocksAllToolInput() const { return false; }
    // render.frag 用の per-tool uniform(uIsXxxTool + origin/size 等)を設定する。
    // 【重要】アクティブか否かに関わらず毎フレーム全アクションに対して呼ばれる。
    // 各アクションは自分の uIsXxxTool を isActive() に応じて 1/0 に設定すること
    // (uniform はプログラムに保持され続けるため、非アクティブ時に 0 へ戻さないと
    // 直前まで効いていたフィルターが残ってしまう)。
    virtual void applyRenderState(QOpenGLShaderProgram * /*renderProg*/) {}
    // GL 描画後の QPainter オーバーレイ(変形枠・ハンドル等)。実際に描いたら true を返す。
    // 変形/キャンバスサイズ/色収差ハンドルのみ描画し、色調整/フィルターパネル系は
    // 何も描かない(false を返し、CanvasWidget 側で通常ツールのオーバーレイを描かせる)。
    virtual bool paintOverlay(QPainter & /*painter*/, const ToolContext & /*ctx*/) { return false; }

    // ---- マウス横取り(変形/キャンバスサイズ/色収差ハンドル用) -----------
    // 消費したら true を返す(その場合 CanvasWidget は通常のツール処理へ進まない)。
    virtual bool handleMousePress      (QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }
    virtual bool handleMouseMove       (QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }
    virtual bool handleMouseRelease    (QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }
    virtual bool handleMouseDoubleClick(QMouseEvent * /*e*/, ToolContext & /*ctx*/) { return false; }

    // 選択範囲の点線(マーチングアンツ)を、このアクションのプレビュー変換へ追従させる。
    // 変形中は selectionMaskTex 自体はまだ動いておらず(確定時に transform.comp が
    // 動かす)、点線はそのマスクから作った輪郭キャッシュを描いているため、何もしないと
    // ドラッグ中だけ点線が元の位置に取り残される。変形系アクションはここで、輪郭の各点
    // (キャンバスpx座標)にプレビューと同じ変換を掛けて返す。
    // 対応しないアクションは false を返す(既定。呼び出し側は点をそのまま使う)。
    virtual bool mapSelectionOutlinePoint(QPointF & /*canvasPx*/) const { return false; }

    // Shift+WASD 等のキーボードによる平行移動(変形/自由変形アクションのみ対応)。
    // 対応していない/対応していても何もしなかった場合は false を返し、CanvasWidget 側の
    // 通常のレイヤー内容ナッジ(nudgeActiveLayer)へフォールスルーさせる。
    virtual bool nudge(const QVector2D & /*delta*/) { return false; }

protected:
    // 多くのパネルで共通の「ウィジェット上端中央(y=20)に sizeHint で配置」処理。
    // 各アクションの positionPanel() から呼ぶ。
    void centerPanelTop(QWidget *panel);

    // レイヤー種別などの事前ガード(既定は常に許可)。
    virtual bool canActivate() const { return true; }
    // ツールを activate しパネルを生成/配線/表示する。engaged できたら true。
    virtual bool onActivate() = 0;
    // 確定(タイルへ焼き込む等)。
    virtual void onConfirm() = 0;
    // キャンセル(レイヤー本体には触れず破棄)。
    virtual void onCancel() = 0;

    CanvasActionHost       &host_;
    CanvasActionController  &ctrl_;
    bool active_ = false;
};
