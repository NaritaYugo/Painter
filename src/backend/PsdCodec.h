#pragma once

#include <QString>
#include <functional>

class GLWidget;

// ---------------------------------------------------------------------------
// PsdCodec
//
// Adobe Photoshop (.psd) 形式でのファイル書き出し/読み込みを担う。
// CanvasSerializer(.tplo)と同じくGLWidget経由でOpenGL操作を行う。
//
// 書き出し:
//   8BPS ヘッダ + 空のカラーモードデータ + 空のイメージリソース +
//   レイヤー&マスク情報セクション(各レイヤーをチャンネル画像データとして非圧縮(RAW)で
//   書き出す) + 合成プレビュー画像(イメージデータセクション、参考用)。
//   単色/調整/テキストレイヤーは、他アプリでの見た目を優先しつつ
//   (単色→"SoCo", 明るさ・コントラスト→"brit" の標準ブロックを書く)、
//   Tiepolo自身での完全な再現性のために非標準の追加情報キー
//   "tpAJ"(調整パラメータ)/"tpTx"(テキストパラメータ)にJSONをそのまま格納する
//   (他アプリからは未知のキーとして単純に無視されるだけなので害はない)。
//
// 読み込み:
//   一般的なPSD(Photoshop/GIMP/Krita等が書き出したもの)を許容範囲で解釈する。
//   ・レイヤーフォルダ(セクション区切り "lsct")→ 中身だけ展開してフラットに配置
//   ・レイヤーマスク(チャンネルID -2)→ アルファに乗算して普通のレイヤーとして配置(破壊的)
//   ・塗りつぶし/調整レイヤー→ "tpAJ"があれば完全復元、無ければ "SoCo"(単色)/
//     "brit"(明るさ・コントラスト)/"hue2"(色相・彩度・明度)を認識、それ以外の
//     種類(レベル補正・トーンカーブ等)は透明な通常レイヤーとして配置
//   ・テキストレイヤー→ "tpTx"があれば完全復元、無ければ"TySh"内の簡易Descriptor
//     解析で文字列(+可能ならAdobeのfont/size)を取り出してテキストレイヤー化、
//     解析に失敗した場合はラスタライズ済みピクセルをそのまま通常レイヤーとして配置
//   ・色空間→ RGBのみ対応(CMYK/グレースケール/Lab等は非対応としてエラーにする)
// ---------------------------------------------------------------------------
class PsdCodec
{
public:
    explicit PsdCodec(GLWidget *gl);

    bool save(const QString &path);
    bool load(const QString &path);

    QString lastError() const { return lastError_; }

    // load()呼び出し前にセットしておくと、レイヤー単位の重い処理(チャンネル
    // デコード+ドキュメント構築)の進み具合を(現在の完了数, 全体数)で通知する。
    // GUIスレッドから呼ばれる前提。CanvasSerializer::progressCallback参照。
    std::function<void(int current, int total)> progressCallback;

private:
    GLWidget *gl_;
    QString   lastError_;

    void setError(const QString &msg) { lastError_ = msg; }
};
