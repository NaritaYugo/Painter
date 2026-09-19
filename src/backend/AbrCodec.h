#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

// ===========================================================================
// AbrCodec  ―  Photoshopのブラシファイル(.abr)の読み書き
// ---------------------------------------------------------------------------
// 【対応範囲】
//   読み込み: version 1/2 (サンプルされた先端画像だけの古い形式) と
//             version 6.1/6.2 (8BIMセクション列)。
//             先端画像は 'samp' セクション、設定は 'desc' セクションの記述子ツリー。
//   書き出し: version 6.1 ('samp' + 'patt' + 'desc')。
//
// 【なぜ書き出しは 6.1 なのか】
//   6.1 と 6.2 で違うのは 'samp' の各レコードの前置きの長さで、
//     6.1 = 47バイト … 内訳が分かっている(名前37 + short境界8 + 深度2)
//     6.2 = 301バイト … 大半が 0 で、先頭に用途の不明な数値がいくつか入る
//   6.2 のその数値を推測で埋めるより、全バイトの意味が分かっている 6.1 で出す方が
//   安全なため、書き出しは 6.1 に固定している(Photoshopは両方読める)。
//
// 【設定のキー名について】
//   ABRのコンテナ構造と記述子のバイト表現(PsdDescriptor)は仕様が公開されているが、
//   ブラシ設定のキー名(Dmtr/Hrdn/Spcn…)には公開仕様が無い。ここでの対応付けは
//   resources/brushes/ の実ファイル(Photoshopが書いた 6.1 と 6.2)を解析して
//   確かめたものだが、そこに出てこなかった値(特に「進行方向追従」を表す
//   angleDynamics.bVTy の番号)は推定のままなので、
//     ・読み込みは「知っているキーだけ拾い、未知のキーは無視する」寛容な作りにし、
//       取りこぼしても既定値のままになるだけで壊れないようにしてある
//     ・調査用に、読んだ記述子ツリー全体を dumpLines として返す
//   という方針は維持している。
//
// 【このアプリとPhotoshopで意味が対応しない設定】
//   Photoshop側にしか無いもの(デュアルブラシ・ウェットエッジ・ノイズ・
//   カラー動的など)と、このアプリ側にしか無いもの(紙質・傾き・入り抜き・
//   後補正・混色など)は、どちらの向きでも移らない。呼び出し側が利用者へ
//   伝えられるよう、読み書きの結果に注記を返す。
// ===========================================================================

// .abr 1本ぶんのうち、このアプリが扱える範囲。
// 「ファイルに入っていなかった」と「値が0だった」を区別するため、
// 数値は has* フラグとセットで持つ。
struct AbrBrush {
    QString name;

    struct OptD { bool has = false; double v = 0.0; void set(double x) { has = true; v = x; } };
    struct OptI { bool has = false; int    v = 0;   void set(int x)    { has = true; v = x; } };
    struct OptB { bool has = false; bool   v = false; void set(bool x) { has = true; v = x; } };

    OptD diameterPx;      // Brsh/Dmtr : 直径[px]
    OptD hardness;        // Brsh/Hrdn : 0〜1 (computedBrush=手続き的な丸ブラシのみ)
    OptD spacing;         // Brsh/Spcn : 直径に対する比(Photoshopは1000%まで)
    OptD angleDeg;        // Brsh/Angl : 度
    OptD roundness;       // Brsh/Rndn : 0〜1
    OptD minDiameter;     // minimumDiameter      : 0〜1
    OptD sizeJitter;      // szVr/jitter          : 0〜1
    OptD angleJitter;     // angleDynamics/jitter : 0〜1
    OptD scatter;         // scatterDynamics/jitter : 0〜1+
    OptI count;           // Cnt  : 1点あたりの粒子数
    OptD opacityJitter;   // opVr/jitter          : 0〜1
    OptB followDirection; // angleDynamics/bVTy が「進行方向」か

    // サンプルされた先端。8bitの濃さをアルファに入れたRGBA画像
    // (このアプリの先端画像はアルファだけを見る。stroke.compを参照)。
    QImage tip;
};

struct AbrReadResult {
    bool ok = false;
    QString error;             // ok==false のときの理由(利用者向け)
    int version = 0;           // 1 / 2 / 6
    int subversion = 0;
    QVector<AbrBrush> brushes;
    QStringList notes;         // 「移せなかったもの」等の注記
    QStringList dumpLines;     // 記述子ツリーの内容(調査用。ログへ出す)
};

namespace AbrCodec {

// .abr のバイト列を解析する。壊れていても例外は投げず、ok=false と理由を返す。
AbrReadResult read(const QByteArray &data);

// ブラシ1本を version 6.1 の .abr として書き出す。
// brush.tip が空でなければ、そのアルファを先端画像として 'samp' に入れる。
QByteArray write(const AbrBrush &brush);

} // namespace AbrCodec
