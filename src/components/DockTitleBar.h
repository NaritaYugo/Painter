#pragma once

#include <QWidget>

class QToolButton;

// ===========================================================================
// DockTitleBar  ―  幅に応じて表示を切り替えるカスタムドックタイトルバー
//
//  狭い時 : グリップ( ⋮⋮ ) ＋ 閉じるボタン
//  広い時 : グリップ ＋ タイトル文字列 ＋ 閉じるボタン
//
//  ヘッダー全体をドラッグするとフローティング／移動できる
//  (QDockWidget の標準ドラッグ機構をそのまま使う)
// ===========================================================================
class DockTitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit DockTitleBar(const QString &title, QWidget *parent = nullptr);

    QSize sizeHint()        const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event)   override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr int BAR_HEIGHT    = 20; // タイトルバーの高さ (px)
    static constexpr int GRIP_WIDTH    = 12; // グリップ部分の幅 (px)
    static constexpr int CLOSE_SIZE    = 14; // 閉じるボタンのサイズ (px)
    static constexpr int TITLE_MARGIN  =  4; // グリップ〜タイトル間マージン
    // この幅以上になったらタイトルを表示
    static constexpr int TITLE_SHOW_THRESHOLD = 60;

    QString      m_title;
    QToolButton *m_closeBtn = nullptr;

    void updateLayout();      // リサイズ時にボタン位置を更新
    void drawGrip(QPainter &p, const QRect &gripRect);
};