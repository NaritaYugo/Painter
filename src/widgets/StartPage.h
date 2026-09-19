#pragma once

#include <QWidget>
#include <QVector>

class QGridLayout;

// ---------------------------------------------------------------------------
// StartPage  ―  キャンバスを1枚も開いていない時にcentralWidgetへ表示する画面
//
// 「新規作成」「ファイルを開く」ボタンと、最近使った.tploファイルのプレビュー
// 一覧(クリックでそのまま開ける)を表示する。プレビュー一覧はウィジェット幅に
// 応じて折り返す(ToolDockのreflowGridsと同じ考え方)。
// ---------------------------------------------------------------------------
class StartPage : public QWidget
{
    Q_OBJECT
public:
    explicit StartPage(QWidget *parent = nullptr);

    // 最近使ったファイル一覧を読み直してプレビューを再構築する
    void refresh();

    static constexpr int CARD_W = 148; // プレビューカード1枚の幅(px)
    static constexpr int CARD_H = 150; // 同高さ(px、サムネイル+ファイル名分)

signals:
    void newCanvasRequested();
    void openFileRequested();
    void openRecentFileRequested(const QString &path);
    // このタブを「透過タブ」にする(ウィンドウにこのタブぶんの穴を開け、
    // 下のウィンドウを覗けるようにする)。
    void transparentTabRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr int MAX_RECENT_SHOWN = 20; // 重くなりすぎないよう表示件数に上限を設ける

    QGridLayout *recentGrid_ = nullptr;
    QVector<QWidget *> recentCards_; // 実体はStartPage.cpp内のRecentFileCard
    int lastCols_ = -1;

    void reflowGrid(int cols);
};
