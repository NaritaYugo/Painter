#pragma once

#include "tools/core/ImagePresets.h"

#include <QDialog>
#include <QString>

class QListWidget;

// ===========================================================================
// ImagePresetPicker  ―  同梱画像の一覧から1枚選ぶダイアログ
// ---------------------------------------------------------------------------
// 渡された一覧をサムネイルのグリッドで並べ、その下に「ファイルから選択...」を
// 置く。ブラシ先端画像と紙質テクスチャの両方がこれを使う(以前は先端画像専用の
// TipImagePicker だった)。
//
// 用途によってサムネイルの作り方が違うので ThumbMode で切り替える:
//   Fit  … 画像全体が収まるよう縮小する。先端画像のように「1枚の絵の形」が
//          分かればよいもの向け。
//   Crop … 中央を等倍で切り出す。紙質テクスチャのように「目の細かさ」こそが
//          違いなので、縮小すると全部同じ灰色に潰れてしまうもの向け。
// ===========================================================================
class ImagePresetPicker : public QDialog
{
    Q_OBJECT
public:
    enum class ThumbMode { Fit, Crop };

    // title       : ウィンドウタイトル兼ファイル選択ダイアログの見出し
    // presets     : 並べる同梱画像
    // currentPath : いま選ばれている画像(グリッド内にあれば選択状態で開く)
    // noneLabel   : 空でなければ、先頭に「選択なし(パスは空文字列)」の項目を足す
    ImagePresetPicker(const QString &title, const ImagePresetList &presets,
                      const QString &currentPath, ThumbMode thumbMode = ThumbMode::Fit,
                      const QString &noneLabel = QString(), QWidget *parent = nullptr);

    // 選ばれた画像のパス。「なし」を選んだ場合も空になるため、
    // 「キャンセルされたか」は exec() の戻り値で判定すること。
    QString selectedPath() const { return selected_; }

private:
    void browseFile();

    QListWidget *grid_ = nullptr;
    QString      title_;
    ThumbMode    thumbMode_ = ThumbMode::Fit;
    QString      selected_;
};
