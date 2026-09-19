#pragma once

#include <QString>
#include <QByteArray>
#include <QImage>
#include <QOpenGLFunctions_4_3_Core>
#include <functional>

class GLWidget;

// ---------------------------------------------------------------------------
// CanvasSerializer
//
// .tplo ファイルへの保存・読み込みを担う。
// OpenGL 操作は GLWidget 経由で行う。
//
// フォーマット (version 6、プレビュー画像埋め込み対応):
//   [4 bytes] マジックナンバー "TPLO"
//   [4 bytes] プレビューサイズ (little-endian uint32)
//   [P bytes] プレビュー画像 (PNG、長辺 kPreviewMaxSize px 以下)
//   [4 bytes] ヘッダサイズ (little-endian uint32)
//   [N bytes] ヘッダ (UTF-8 JSON)
//   [M bytes] ピクセルデータブロック
//             各レイヤーごとに qCompress で圧縮した RGBA8 データ
//             JSON の dataOffset/dataSize はこのブロック内の位置を示す
//
// プレビュー画像をヘッダ/ピクセルデータより手前の固定位置に置くことで、
// readPreview() はファイル全体を読まずに先頭の数十~数百バイトだけを読んで
// 済ませられる(「最近使ったファイル」一覧のサムネイル表示などで、ファイル
// サイズによらず高速にプレビューだけ取り出すための設計)。
//
// 各レイヤーは originTx/originTy(タイル単位の原点)と tilesX/tilesY(自身の
// タイル数)を持ち、キャンバスより大きい/はみ出した矩形もそのまま保存できる。
// タイルのtx/tyはレイヤーローカル座標(0オリジン)。
//
// version 5以前との互換性はない。
// ---------------------------------------------------------------------------
class CanvasSerializer
{
public:
    explicit CanvasSerializer(GLWidget *gl);

    // 戻り値: 成功 true / 失敗 false
    bool save(const QString &path);
    bool load(const QString &path);

    // 直近のエラーメッセージ
    QString lastError() const { return lastError_; }

    // ファイルの先頭に埋め込まれたプレビュー画像だけを高速に読み出す(GLWidget不要、
    // ヘッダJSON/ピクセルデータには一切触れない)。失敗時は null な QImage を返す。
    static QImage readPreview(const QString &path);

    // load()呼び出し前にセットしておくと、load()内部の重い処理(検証パス+構築パス、
    // レイヤー単位)の進み具合を(現在の完了数, 全体数)で通知する。GUIスレッドから
    // 呼ばれる前提。呼び出し側でQProgressDialogの更新やprocessEvents()を行うことを
    // 想定しており、この関数自体はUIに一切触れない。
    std::function<void(int current, int total)> progressCallback;

private:
    GLWidget *gl_;
    QString   lastError_;

    void setError(const QString &msg) { lastError_ = msg; }
};