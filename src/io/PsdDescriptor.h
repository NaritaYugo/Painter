#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QPair>

// ===========================================================================
// PsdDescriptor  ―  Photoshopの「記述子(Descriptor)」の読み書き
// ---------------------------------------------------------------------------
// PSD/ABR/PSBが共通で使う、入れ子のキー・値ツリー。バイト表現はAdobeの
// 「Photoshop File Formats」仕様に載っている部分で、型は4文字コードで表される
// (Objc=入れ子, VlLs=配列, doub=倍精度, UntF=単位付き実数, long=整数,
//  bool=真偽, TEXT=UTF-16文字列, enum=列挙, tdta=生データ …)。
//
// PsdCodec.cpp にも記述子を辿るコードがあるが、あちらはテキストレイヤーの本文を
// 取り出すためだけの「値を捨てる読み飛ばし」なので、値を保持する必要がある
// ABR(AbrCodec)ではこちらを使う。
//
// 値の型は多いがどれも小さいので、variantを使わず全フィールドを持つ素直な構造体に
// してある(読み書きの対応が目で追えることを優先)。
// ===========================================================================

struct DescValue;
using DescField  = QPair<QByteArray, DescValue>;  // (キー, 値)。順序は保持する
using DescFields = QVector<DescField>;

struct DescValue {
    QByteArray type;        // "long" / "doub" / "UntF" / "bool" / "TEXT" / "enum" /
                            // "Objc" / "VlLs" / "tdta" / "alis" / "obj " など

    qint32     i    = 0;    // long
    double     d    = 0.0;  // doub / UntF
    QByteArray unit;        // UntF の単位 ("#Prc" 百分率 / "#Pxl" ピクセル / "#Ang" 角度)
    bool       b    = false;// bool
    QString    text;        // TEXT
    QByteArray enumType;    // enum の型ID
    QByteArray enumValue;   // enum の値ID

    QString    objName;     // Objc の名前(通常は空)
    QByteArray classId;     // Objc のクラスID
    DescFields fields;      // Objc の中身

    QVector<DescValue> items; // VlLs の要素

    QByteArray raw;         // tdta / alis / 未対応型の生バイト

    // --- 組み立て用ヘルパー -------------------------------------------------
    static DescValue makeLong(qint32 v)          { DescValue x; x.type = "long"; x.i = v; return x; }
    static DescValue makeDouble(double v)        { DescValue x; x.type = "doub"; x.d = v; return x; }
    static DescValue makeBool(bool v)            { DescValue x; x.type = "bool"; x.b = v; return x; }
    static DescValue makeText(const QString &v)  { DescValue x; x.type = "TEXT"; x.text = v; return x; }
    static DescValue makeUnit(const QByteArray &unit, double v) {
        DescValue x; x.type = "UntF"; x.unit = unit; x.d = v; return x;
    }
    static DescValue makeEnum(const QByteArray &t, const QByteArray &v) {
        DescValue x; x.type = "enum"; x.enumType = t; x.enumValue = v; return x;
    }
    static DescValue makeObject(const QByteArray &classId, const DescFields &f = {}) {
        DescValue x; x.type = "Objc"; x.classId = classId; x.fields = f; return x;
    }
    static DescValue makeList(const QVector<DescValue> &v) {
        DescValue x; x.type = "VlLs"; x.items = v; return x;
    }

    // --- 参照用ヘルパー -----------------------------------------------------
    // Objc の中から key を探す(見つからなければ nullptr)。
    const DescValue *field(const QByteArray &key) const;
    // long / doub / UntF のいずれでも数値として取り出す。型が違えば false。
    bool asNumber(double &out) const;
};

namespace PsdDescriptor {

// 記述子1つを読む。cursor は「名前(Unicode文字列)」の先頭を指していること
// (先頭の descriptor version 4バイトは呼び出し側が読むこと)。
// 戻り値は成功可否、pos は読み終わった位置へ進む。
bool read(const QByteArray &data, int &pos, DescValue &out);

// 記述子1つを書く(名前+クラスID+フィールド。version は書かない)。
void write(QByteArray &out, const DescValue &objc);

// 読めた内容を人が読める1行ずつのテキストにする(未知キーの調査用)。
QStringList dump(const DescValue &v, int indent = 0);

} // namespace PsdDescriptor
