#pragma once

#include <QStringList>

// ---------------------------------------------------------------------------
// RecentFiles
//
// 「最近使った.tploファイル」のパス一覧をQSettingsへ永続化する。
// スタート画面(StartPage)のプレビュー一覧の元データとして使う。
// プレビュー画像はCanvasSerializer::readPreview()で.tplo自体から高速に
// 読み出せるため、ここではパスの管理(MRU順・重複除去・上限)だけを行う。
// ---------------------------------------------------------------------------
class RecentFiles
{
public:
    // 実在するファイルのみ、最近使った順(先頭が最新)で返す
    static QStringList list();

    // 先頭に追加する(既に含まれていれば一旦除いてから追加=先頭へ移動)。
    // 上限件数を超えた分は古い方から切り捨てる。
    static void touch(const QString &path);

    static constexpr int kMaxStored = 30;
};
