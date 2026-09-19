#pragma once

#include <QStackedWidget>

class StartPage;
class GLWidget;

// ---------------------------------------------------------------------------
// CanvasTabPage  ―  タブ1枚ぶんの中身。「スタートページ」か「キャンバス」のどちらかを表示する。
//
// タブは必ずスタートページとして生まれ(=「タブを追加」した直後の状態)、そこで
// 最近使ったファイルを選ぶ・新規作成する・ファイルを開くと、そのタブがキャンバスへ
// 切り替わる。一度キャンバスになったタブがスタートページへ戻ることはない。
//
// これにより、中央ウィジェットの構造が
//     中央 ├ スタートページ / └ タブ(キャンバスのみ)
// から
//     中央 └ タブ(それぞれスタートページ or キャンバス)
// へ変わり、2枚目以降のタブでも「最近の作品」のサムネイルを見ながら選べるようになる。
//
// GLWidgetはここが親になる。所属タブはgl->parentWidget()をqobject_castすれば引ける
// (MainWindow側でGLWidget->タブの対応表を別に持つ必要はない)。
// ---------------------------------------------------------------------------
class CanvasTabPage : public QStackedWidget
{
    Q_OBJECT
public:
    explicit CanvasTabPage(QWidget *parent = nullptr);

    StartPage *startPage() const { return startPage_; }
    // まだスタートページ状態(キャンバス未作成)ならnullptr。
    GLWidget  *canvas()    const { return canvas_; }
    bool       isCanvas()  const { return canvas_ != nullptr; }

    // GLWidgetをこのタブに載せる。1タブにつき1回だけ呼ぶ(2回目以降・nullptrは無視)。
    //
    // 重要: ここでは表示は切り替えず、スタートページを出したままにする。
    // このタブが既に画面に出ている状態でGLWidgetをカレントにすると、その場で
    // 同期的にshow()→resizeEvent→initializeGL()まで走り、GLWidget::initializedが
    // 呼び出し元へ戻る前に発行され切ってしまう。ファイル読み込み/新規キャンバス作成は
    // そのシグナルを待って行うため、接続を張る前に発行されると何も起きない
    // (=初期状態の空キャンバスのまま)。表示への切り替えはshowCanvas()で明示的に行う。
    void setCanvas(GLWidget *gl);

    // 実際にキャンバス表示へ切り替える(GLWidget::initializedへの接続を済ませてから呼ぶ)。
    void showCanvas();

    // このタブを「透過タブ」にする。中身を何も描かない空のページへ切り替え、
    // MainWindow側でこのページの矩形ぶんだけウィンドウに穴を開ける
    // (MainWindow::updateWindowMask()参照)。下のウィンドウがそのまま見え、
    // クリックもそちらへ抜ける。キャンバスにしたタブには適用できない。
    void makeTransparent();
    bool isTransparent() const { return transparent_ && currentWidget() == holePage_; }

signals:
    // 透過タブの穴の位置・大きさ・表示状態が変わったことをMainWindowへ知らせる
    // (ウィンドウのマスクを貼り直してもらうため)。
    void transparentGeometryChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    StartPage *startPage_  = nullptr;
    GLWidget  *canvas_     = nullptr;
    QWidget   *holePage_   = nullptr; // 透過タブ用の空ページ(何も描かない)
    bool       transparent_ = false;
};
