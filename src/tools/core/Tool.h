#pragma once

#include "tools/core/ToolContext.h"
#include <QMouseEvent>
#include <QCursor>
#include <optional>

class QPainter;

// ---------------------------------------------------------------------------
// Tool
// ---------------------------------------------------------------------------
// 各描画ツール(ペン/消しゴム/塗りつぶし/移動/回転/スポイト)の共通インターフェース。
// GLWidget はイベントを activeTool_ に委譲するだけになる。
// ---------------------------------------------------------------------------
class Tool
{
public:
    virtual ~Tool() = default;

    virtual void onMousePress(QMouseEvent *event, ToolContext &ctx)   {}
    virtual void onMouseMove(QMouseEvent *event, ToolContext &ctx)    {}
    virtual void onMouseRelease(QMouseEvent *event, ToolContext &ctx) {}
    // ダブルクリック(テキストボックスの再編集など、対応するツールだけが使う)
    virtual void onMouseDoubleClick(QMouseEvent *event, ToolContext &ctx) { Q_UNUSED(event); Q_UNUSED(ctx); }

    // 現在ストローク中か(GLWidget::mouseMoveEvent/mouseReleaseEventのガードに使う)
    virtual bool isActive() const { return false; }

    // ドラッグ中、GLWidget側の「一定間隔で同期repaintする」処理を回す必要があるか。
    // ペン/消しゴム/エアブラシは引いた線をその場で見せる必要があるので true(既定)。
    // 一方、ペン選択のようにドラッグ中キャンバスの見た目が全く変わらないツールは
    // false を返すことで、無駄な全レイヤー再合成を完全に止められる
    // (SelectTool::needsCanvasRepaintWhileActive のコメント参照)。
    virtual bool needsCanvasRepaintWhileActive() const { return true; }

    // タブレット筆圧など、ツールによっては使わないので空実装をデフォルトに
    virtual void setPressure(float pressure) { pressure_ = pressure; }

    // タブレットの傾きとペン軸まわりの回転。筆圧と同じく、GLWidget が
    // マウス/タブレットのイベントごとに設定する(マウス操作中はすべて0)。
    //   amount   : 0(垂直に立てた状態)〜1(最大まで寝かせた状態)
    //   angle    : 傾けた方向[rad]。キャンバス座標(Y上向き)での向き
    //   rotation : ペン軸まわりの回転[rad](アートペン等。非対応機は常に0)
    virtual void setTilt(float amount, float angle, float rotation)
    {
        tiltAmount_  = amount;
        tiltAngle_   = angle;
        penRotation_ = rotation;
    }

    // マウスカーソルの見た目。std::nullopt を返すと、呼び出し側(GLWidget)が
    // ToolRegistry に登録されたそのツールのアイコンをカーソルとして使う。
    // 新しいツールを追加したとき、特別なカーソル(ペンの円やスポイトの色など)が
    // 不要ならこれをオーバーライドしなくてよい(アイコンカーソルが自動で使われる)。
    virtual std::optional<QCursor> cursor(const ToolContext &ctx) const { return std::nullopt; }

    // GLの描画が終わった後にQPainterで重ねて描く追加のオーバーレイ(投げ縄選択の
    // 軌跡プレビューなど)。ほとんどのツールは不要なのでデフォルトは何もしない。
    virtual void paintOverlay(QPainter &painter, const ToolContext &ctx) const { Q_UNUSED(painter); Q_UNUSED(ctx); }

    // フレームレート律速バッチ用のフック。GLWidget::paintGL()が毎フレームの描画
    // 直前に必ず1回呼ぶ。ペンタブは物理サンプル頻度がマウスよりずっと高く、
    // onMouseMove()のたびに即座にGPUディスパッチしていると表示フレーム数よりも
    // 多くの回数GPU処理が走ってしまい重くなる。onMouseMove()側で入力(位置・筆圧)
    // だけを貯めておき、実際のGPUディスパッチはこちらへ遅延させることで、
    // GPU処理回数を表示フレーム数まで抑えられる(位置・筆圧のサンプリング精度自体は
    // 落ちない。onMouseMove()は引き続き実イベントごとに正しい順序で呼ばれるため)。
    // 未対応のツールはデフォルトの空実装のままでよい。
    virtual void flushPendingInput(ToolContext &ctx) { Q_UNUSED(ctx); }

    // 「まだ画面に出していない入力が溜まっているか」。
    //
    // GLWidget の取りこぼし用タイマー(inputFlushTimer_)は、これが true のときだけ
    // 画面を更新する。溜まっていないのに update() を積むと、直前に出したのと同じ絵を
    // もう1枚描き直すことになり、そのたびにウィンドウ全体の再合成とpresentが走る
    // (実測: この環境では1回 15〜65ms)。
    //
    // 特に移動/回転ツールはドラッグ中に入力を溜めない(その場でビューへ反映する)ため、
    // 既定の false のままでよい。以前はここを見ずに毎回 update() を積んでいたので、
    // ドラッグ中は「本物のフレーム1枚につき無駄なフレーム1枚」が交互に入り、
    // ボタンを押したまま止めている間もずっと再合成が続いていた。
    // これがビュー変換がカクついて見える主因だった。
    virtual bool hasPendingInput() const { return false; }

    // ドラッグ中に「キャンバスの中身ではなくビュー(表示位置・角度・倍率)だけ」を
    // 動かすツールか。移動ツールと回転ツールだけが true。
    //
    // GLWidget はこの間、描画中にしか意味の無い処理を省いて1フレームを軽くする
    // (GLWidget::paintGL のオーバーレイのコメント参照)。
    virtual bool transformsViewWhileActive() const { return false; }

    // フレームレート律速バッチの「入力を貯めてから実際にGPUへ描くまで」許容する最大
    // 遅延(ミリ秒)。GLWidget::mouseMoveEvent はこの間隔を超えたときだけ
    // flushPendingInput() する。0 なら毎マウスイベントで即 flush =
    // 遅延最小・dispatch回数最大(=描いた線がペン先に最も速く追従する)。
    //
    // ペン/消しゴム/エアブラシは1回のdispatchのコストがブラシ面積(∝size^2)に比例して
    // 増えるため、細いブラシは0(追従性最優先)、太いブラシほど間隔を空けてdispatch回数を
    // 抑える、という形でオーバーライドする。バッチしないツールはデフォルトの0でよい。
    virtual int flushIntervalMs() const { return 0; }

protected:
    float pressure_ = 1.0f;
    // setTilt() で入る値(既定はマウス相当の「傾き・回転なし」)
    float tiltAmount_  = 0.0f;
    float tiltAngle_   = 0.0f;
    float penRotation_ = 0.0f;
};