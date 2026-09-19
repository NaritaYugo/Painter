#include "widgets/GLWidget.h"
#include "backend/PsdCodec.h"
#include "backend/CanvasDocument.h"

#include <QFile>
#include <QSaveFile>
#include <QImage>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFont>
#include <QFontMetrics>
#include <QSet>
#include <QtEndian>
#include <QtMath>
#include <QtConcurrent>
#include <QDebug>

#include <functional>

namespace {

// ===========================================================================
// バイト列組み立てヘルパー(書き出し用、ビッグエンディアン)
// ===========================================================================
void putU8(QByteArray &b, quint8 v)  { b.append(char(v)); }
void putU16(QByteArray &b, quint16 v){ quint16 be = qToBigEndian(v); b.append((const char*)&be, 2); }
void putU32(QByteArray &b, quint32 v){ quint32 be = qToBigEndian(v); b.append((const char*)&be, 4); }
void putI16(QByteArray &b, qint16 v) { putU16(b, (quint16)v); }
void putI32(QByteArray &b, qint32 v) { putU32(b, (quint32)v); }
void putTag(QByteArray &b, const char *tag) { b.append(tag, 4); }
void padToEven(QByteArray &b) { if (b.size() % 2 != 0) b.append(char(0)); }

// 4バイトキー(標準4文字コード、または非標準の"tpAJ"/"tpTx")の追加情報ブロックを1つ追加する
void appendAdditionalInfo(QByteArray &out, const char *key, const QByteArray &data) {
    putTag(out, "8BIM");
    putTag(out, key);
    putU32(out, (quint32)data.size());
    out.append(data);
    padToEven(out);
}

// ===========================================================================
// 読み出し用カーソル(範囲チェック付き)
// ===========================================================================
struct Cursor {
    const uchar *base = nullptr;
    qint64 size = 0;
    qint64 pos  = 0;
    bool   ok   = true;

    explicit Cursor(const QByteArray &ba)
        : base(reinterpret_cast<const uchar*>(ba.constData())), size(ba.size()) {}
    Cursor(const uchar *b, qint64 s) : base(b), size(s) {}

    bool require(qint64 n) {
        if (!ok || n < 0 || pos + n > size) { ok = false; return false; }
        return true;
    }
    quint8  u8()  { if (!require(1)) return 0; return base[pos++]; }
    quint16 u16() { if (!require(2)) return 0; quint16 v = qFromBigEndian<quint16>(base + pos); pos += 2; return v; }
    quint32 u32() { if (!require(4)) return 0; quint32 v = qFromBigEndian<quint32>(base + pos); pos += 4; return v; }
    qint16  i16() { return (qint16)u16(); }
    qint32  i32() { return (qint32)u32(); }
    QByteArray bytes(qint64 n) {
        if (!require(n)) return {};
        QByteArray r(reinterpret_cast<const char*>(base + pos), n);
        pos += n;
        return r;
    }
    QByteArray tag4() { return bytes(4); }
    void skip(qint64 n) {
        if (n <= 0) return;
        if (!require(n)) { pos = size; return; }
        pos += n;
    }
    // 既知のブロック境界(宣言済みの長さから計算した位置)へ強制的に巻き戻し/巻き進みする。
    // その直前で個々の値の解析に失敗していても、ここで明示的に既知の安全な位置へ
    // 復帰するので ok を立て直す(1レイヤー/1ブロック内の解析ミスがファイル全体の
    // 読み込み失敗へ連鎖しないようにするため)。範囲外だけは致命的エラーとして扱う。
    void seekAbs(qint64 p) {
        if (p < 0 || p > size) { pos = size; ok = false; return; }
        pos = p;
        ok = true;
    }
    qint64 remaining() const { return size - pos; }
};

// ===========================================================================
// PackBits (RLE) デコード
// ===========================================================================
// PackBitsの1行を dst[0..width) へ直接展開する。
// 以前は QByteArray を戻り値にし、さらにラン(連続)1つごとに一時 QByteArray を
// 確保して append していたため、レイヤー数×チャンネル数×行数×ラン数ぶんの
// メモリ確保が発生し、PSD読み込み時間の大半をここで消費していた
// (実測: 激重ファイルで73秒中の大半)。dstへ直接memcpy/memsetする。
// dstは必ずwidthバイトすべてが埋まる(データが尽きた場合は0で埋める)。
void packBitsDecodeRowInto(const uchar *src, qint64 srcLen, char *dst, int width) {
    qint64 sp = 0;
    int written = 0;
    while (written < width && sp < srcLen) {
        const qint8 n = (qint8)src[sp++];
        if (n >= 0) {
            // リテラル: 続くn+1バイトをそのままコピー
            qint64 count = n + 1;
            if (sp + count > srcLen) count = srcLen - sp; // 壊れた行は読める分だけ
            const qint64 w = qMin<qint64>(count, width - written);
            if (w <= 0) break;
            memcpy(dst + written, src + sp, (size_t)w);
            sp      += count;
            written += (int)w;
        } else if (n != -128) {
            // ラン: 続く1バイトを1-n回繰り返す
            if (sp >= srcLen) break;
            const char v = (char)src[sp++];
            const qint64 w = qMin<qint64>((qint64)(1 - n), (qint64)(width - written));
            memset(dst + written, v, (size_t)w);
            written += (int)w;
        }
        // n == -128 は PackBits の規定によりノーオペレーション
    }
    if (written < width) memset(dst + written, 0, (size_t)(width - written));
}

// channelDataLen: このチャンネルのために宣言済みのバイト数(圧縮方式2バイトを含まない、
// つまり実際のピクセルデータ本体のバイト数)。デコード内容に関わらず、必ずこの分だけ
// カーソルを進めてから返す(以降のレイヤー解析がずれないようにするための安全策)。
QByteArray decodeChannelPlane(Cursor &c, quint16 compression, int width, int height, qint64 channelDataLen) {
    const qint64 startPos = c.pos;
    QByteArray plane;
    if (width <= 0 || height <= 0) {
        c.skip(channelDataLen);
        return plane;
    }
    if (compression == 0) {
        plane = c.bytes((qint64)width * height);
    } else if (compression == 1) {
        QVector<quint16> rowLens(height);
        for (int y = 0; y < height; y++) rowLens[y] = c.u16();
        // 出力先を先に1回だけ確保し、各行をその中へ直接展開する
        // (行ごとの一時バッファ確保と、その append によるコピーをなくす)。
        plane.resize((qint64)width * height);
        char *dst = plane.data();
        int y = 0;
        for (; y < height && c.ok; y++) {
            const qint64 rowLen = rowLens[y];
            if (!c.require(rowLen)) break;
            packBitsDecodeRowInto(c.base + c.pos, rowLen, dst + (qint64)y * width, width);
            c.pos += rowLen;
        }
        // 途中で尽きた行は透明(0)にしておく(resizeは中身を初期化しないため)
        if (y < height)
            memset(dst + (qint64)y * width, 0, (size_t)((qint64)(height - y) * width));
    } else {
        // ZIP / ZIP+予測符号化は非対応。空データ(透明)として扱う。
        plane = QByteArray((qint64)width * height, char(0));
    }
    if (plane.size() < width * (qint64)height)
        plane.append(QByteArray(width * (qint64)height - plane.size(), char(0)));

    const qint64 consumed = c.pos - startPos;
    if (consumed < channelDataLen) c.skip(channelDataLen - consumed);
    else if (consumed > channelDataLen) { c.ok = false; c.seekAbs(startPos + channelDataLen); c.ok = true; }
    return plane;
}

// depth==16のとき、2バイト/サンプルの上位バイトだけを取り出して8bit相当に間引く
QByteArray decodeChannelPlane16(Cursor &c, quint16 compression, int width, int height, qint64 channelDataLen) {
    QByteArray wide = decodeChannelPlane(c, compression, width * 2, height, channelDataLen);
    QByteArray out((qint64)width * height, char(0));
    for (int i = 0; i < width * height; i++)
        out[i] = wide[i * 2];
    return out;
}

// ===========================================================================
// 二値/未associatedアルファ ⇔ 事前乗算アルファ 変換
// ===========================================================================
inline void premulPixel(quint8 r, quint8 g, quint8 b, quint8 a, uchar *dst) {
    dst[0] = (uchar)(((int)r * a + 127) / 255);
    dst[1] = (uchar)(((int)g * a + 127) / 255);
    dst[2] = (uchar)(((int)b * a + 127) / 255);
    dst[3] = a;
}
inline void unpremulPixel(const uchar *src, quint8 &r, quint8 &g, quint8 &b, quint8 &a) {
    a = src[3];
    if (a == 0) { r = g = b = 0; return; }
    r = (quint8)qMin(255, (int)(((int)src[0] * 255 + a / 2) / a));
    g = (quint8)qMin(255, (int)(((int)src[1] * 255 + a / 2) / a));
    b = (quint8)qMin(255, (int)(((int)src[2] * 255 + a / 2) / a));
}

// GLの内部タイル(readSlicePixels/writeSlicePixels)はOpenGLのテクスチャ行順
// (原点が左下、Y上向き)で格納されている。CanvasCompositor::renderExport等の
// 既存の書き出し経路は読み出し後に必ず mirrored(false,true) で上下反転して
// 標準的な画像(原点が左上、Y下向き)に直しているが、PsdCodecのタイル組み立ては
// それを行っていなかったため、PSDの行順(標準画像と同じくY下向き)との間で
// 上下が逆になっていた。RGBA8のバッファをレイヤー全体単位で反転するためのヘルパー。
void flipRowsVertically(QByteArray &buf, int w, int h) {
    const int rowBytes = w * 4;
    QByteArray tmp(rowBytes, char(0));
    for (int y = 0; y < h / 2; y++) {
        char *top = buf.data() + (qint64)y * rowBytes;
        char *bottom = buf.data() + (qint64)(h - 1 - y) * rowBytes;
        memcpy(tmp.data(), top, rowBytes);
        memcpy(top, bottom, rowBytes);
        memcpy(bottom, tmp.data(), rowBytes);
    }
}

// flipRowsVertically の1チャンネル版(レイヤーマスクのグレースケール平面用)。
void flipRowsVertically1ch(QByteArray &buf, int w, int h) {
    QByteArray tmp(w, char(0));
    for (int y = 0; y < h / 2; y++) {
        char *top = buf.data() + (qint64)y * w;
        char *bottom = buf.data() + (qint64)(h - 1 - y) * w;
        memcpy(tmp.data(), top, w);
        memcpy(top, bottom, w);
        memcpy(bottom, tmp.data(), w);
    }
}

// ===========================================================================
// ブレンドモード ⇔ PSDブレンドキー
// (BlendMode enumはPhotoshopのメニュー順に合わせて宣言済みなので単純な配列で足りる)
// ===========================================================================
QByteArray blendModeToKey(BlendMode m) {
    static const QByteArray table[] = {
        "norm","mul ","scrn","over","diss","dark","idiv","lbrn","dkCl",
        "lite","div ","lddg","lgCl","over","sLit","hLit","vLit","lLit",
        "pLit","hMix","diff","smud","fsub","fdiv","hue ","sat ","colr","lum ",
    };
    int i = (int)m;
    if (i < 0 || i >= (int)(sizeof(table)/sizeof(table[0]))) return "norm";
    // インデックス3(Overlay)とインデックス13(SoftLight)の間、tableは実enumと1:1対応させてある
    return table[i];
}
BlendMode blendKeyToMode(const QByteArray &key) {
    static const QByteArray table[] = {
        "norm","mul ","scrn","over","diss","dark","idiv","lbrn","dkCl",
        "lite","div ","lddg","lgCl","sLit","hLit","vLit","lLit",
        "pLit","hMix","diff","smud","fsub","fdiv","hue ","sat ","colr","lum ",
    };
    for (int i = 0; i < (int)(sizeof(table)/sizeof(table[0])); i++)
        if (table[i] == key) return (BlendMode)i;
    return BlendMode::Normal;
}

// ===========================================================================
// Descriptor(記述子)簡易パーサ ―― TySh(テキストレイヤー)から
// "Txt "(本文文字列)キーだけを取り出すための、寛容な最小実装。
// 未知の型/参照は構造だけ辿って読み飛ばす(値は捨てる)。
// ===========================================================================
struct DescAccum { QString text; bool gotText = false; };

bool descReadIdField(Cursor &c, QByteArray &outId) {
    quint32 len = c.u32();
    if (!c.ok) return false;
    outId = (len == 0) ? c.tag4() : c.bytes(len);
    return c.ok;
}
bool descReadUnicodeString(Cursor &c, QString &out) {
    quint32 len = c.u32();
    if (!c.ok) return false;
    QByteArray raw = c.bytes((qint64)len * 2);
    if (!c.ok) return false;
    QVector<ushort> u16(len);
    for (quint32 i = 0; i < len; i++)
        u16[i] = (ushort)(((uchar)raw[i*2] << 8) | (uchar)raw[i*2+1]);
    out = QString::fromUtf16(u16.constData(), (int)len);
    while (!out.isEmpty() && out.back() == QChar(0)) out.chop(1);
    return true;
}

bool descReadDescriptorBody(Cursor &c, DescAccum &acc);

// value本体を読み飛ばす(型に応じて)。value自身がTEXT型ならoutTextIfAnyへ文字列を返す。
bool descReadValue(Cursor &c, const QByteArray &type, DescAccum &acc, QString *outTextIfAny) {
    if (type == "obj ") { // Reference
        quint32 count = c.u32();
        if (!c.ok) return false;
        for (quint32 i = 0; i < count && c.ok; i++) {
            QByteArray reftype = c.tag4();
            if (!c.ok) return false;
            if (reftype == "Clss" || reftype == "type" || reftype == "GlbC") {
                QByteArray id; if (!descReadIdField(c, id)) return false;
            } else if (reftype == "Enmr") {
                QByteArray a, b;
                if (!descReadIdField(c, a) || !descReadIdField(c, b)) return false;
            } else if (reftype == "Idnt" || reftype == "indx") {
                c.u32(); if (!c.ok) return false;
            } else if (reftype == "name") {
                QString s; if (!descReadUnicodeString(c, s)) return false;
                QByteArray cid; if (!descReadIdField(c, cid)) return false;
            } else if (reftype == "prop") {
                QByteArray cid; if (!descReadIdField(c, cid)) return false;
                QString nm; if (!descReadUnicodeString(c, nm)) return false;
                QByteArray kid; if (!descReadIdField(c, kid)) return false;
            } else {
                return false; // 未知の参照型は諦める
            }
        }
        return c.ok;
    }
    if (type == "Objc" || type == "GlbO") return descReadDescriptorBody(c, acc);
    if (type == "VlLs") {
        quint32 count = c.u32();
        if (!c.ok) return false;
        for (quint32 i = 0; i < count && c.ok; i++) {
            QByteArray itemType = c.tag4();
            if (!c.ok) return false;
            if (!descReadValue(c, itemType, acc, nullptr)) return false;
        }
        return c.ok;
    }
    if (type == "doub") { c.bytes(8); return c.ok; }
    if (type == "UntF") { c.tag4(); c.bytes(8); return c.ok; }
    if (type == "TEXT") {
        QString s; if (!descReadUnicodeString(c, s)) return false;
        if (outTextIfAny) *outTextIfAny = s;
        return true;
    }
    if (type == "enum") { QByteArray a, b; return descReadIdField(c, a) && descReadIdField(c, b); }
    if (type == "long") { c.i32(); return c.ok; }
    if (type == "comp") { c.bytes(8); return c.ok; }
    if (type == "bool") { c.u8(); return c.ok; }
    if (type == "type" || type == "GlbC") { QByteArray id; return descReadIdField(c, id); }
    if (type == "alis" || type == "tdta") {
        quint32 len = c.u32(); if (!c.ok) return false;
        c.bytes(len); return c.ok;
    }
    return false; // 未知の型
}

bool descReadDescriptorBody(Cursor &c, DescAccum &acc) {
    QString name; if (!descReadUnicodeString(c, name)) return false;
    QByteArray classId; if (!descReadIdField(c, classId)) return false;
    quint32 count = c.u32();
    if (!c.ok) return false;
    for (quint32 i = 0; i < count && c.ok; i++) {
        QByteArray key; if (!descReadIdField(c, key)) return false;
        QByteArray type = c.tag4(); if (!c.ok) return false;
        QString textVal;
        if (!descReadValue(c, type, acc, &textVal)) return false;
        if (type == "TEXT" && key == "Txt " && !acc.gotText) {
            acc.text = textVal;
            acc.gotText = true;
        }
    }
    return c.ok;
}

// TySh追加情報ブロック全体から、本文文字列だけを取り出す(フォント/サイズ/色は
// EngineData(独自テキスト形式)側にしかなく解析コストが高いため対象外。
// 位置はレイヤー矩形から求めるので、ここでは文字列だけで十分)。
bool parseTyShText(const QByteArray &data, QString &outText) {
    Cursor c(data);
    c.i16();                 // version
    for (int i = 0; i < 6; i++) c.bytes(8); // transform (xx,xy,yx,yy,tx,ty)
    c.i16();                 // text version
    c.i32();                 // descriptor version
    if (!c.ok) return false;
    DescAccum acc;
    if (!descReadDescriptorBody(c, acc)) return false;
    if (!acc.gotText) return false;
    outText = acc.text;
    return true;
}

// "brit" (明るさ・コントラスト) の簡易パース
bool parseBrit(const QByteArray &d, int &brightness, int &contrast) {
    if (d.size() < 4) return false;
    Cursor c(d);
    brightness = c.i16();
    contrast   = c.i16();
    return c.ok;
}

// "hue2" (色相・彩度・明度) のベストエフォートパース。
// colorization(色付け)は未対応、マスター(全体)のH/S/Lのみ取り出す。
bool parseHue2(const QByteArray &d, int &hue, int &sat, int &light) {
    if (d.size() < 16) return false;
    Cursor c(d);
    c.i16();       // version
    c.i16();       // enable colorization
    c.i16(); c.i16(); c.i16(); // colorization H/S/L (未対応)
    hue   = c.i16();
    sat   = c.i16();
    light = c.i16();
    return c.ok;
}

// 「塗りつぶし/調整レイヤーだが、Tiepoloが対応していない種類」を示す標準キー集合。
// (levl=レベル補正, curv=トーンカーブ, expA=露光量, vibA=自然な彩度, blwh=白黒,
//  blnc=カラーバランス, selc=特定色域, thrs=しきい値, post=ポスタリゼーション,
//  nvrt=階調の反転, mixr=チャンネルミキサー, grdm=グラデーションマップ,
//  GdFl=グラデーションで塗りつぶし, PtFl=パターンで塗りつぶし)
const QSet<QByteArray> &unsupportedAdjustmentKeys() {
    static const QSet<QByteArray> s = {
        "levl", "curv", "expA", "vibA", "blwh", "blnc", "selc", "thrs",
        "post", "nvrt", "mixr", "grdm", "GdFl", "PtFl",
    };
    return s;
}

struct AddlInfo {
    QHash<QByteArray, QByteArray> blocks; // key(4文字, 空白パディング) -> data
    bool has(const char *k) const { return blocks.contains(QByteArray(k)); }
    QByteArray get(const char *k) const { return blocks.value(QByteArray(k)); }
};

} // namespace

// ===========================================================================
// PsdCodec
// ===========================================================================
PsdCodec::PsdCodec(GLWidget *gl) : gl_(gl) {}

// ---------------------------------------------------------------------------
// 書き出し
// ---------------------------------------------------------------------------
bool PsdCodec::save(const QString &path)
{
    gl_->makeCurrent();
    const CanvasDocument &doc = gl_->document();
    const int canvasW = gl_->canvasWidth();
    const int canvasH = gl_->canvasHeight();

    // ------------------------------------------------------------------
    // 1. ヘッダ
    // ------------------------------------------------------------------
    QByteArray out;
    out.append("8BPS", 4);
    putU16(out, 1);                 // version = 1
    for (int i = 0; i < 6; i++) putU8(out, 0); // reserved
    putU16(out, 4);                 // channels (合成イメージ用、RGBA)
    putU32(out, (quint32)canvasH);
    putU32(out, (quint32)canvasW);
    putU16(out, 8);                 // depth
    putU16(out, 3);                 // mode = RGB

    // ------------------------------------------------------------------
    // 2. カラーモードデータ(空)
    // ------------------------------------------------------------------
    putU32(out, 0);

    // ------------------------------------------------------------------
    // 3. イメージリソース(空)
    // ------------------------------------------------------------------
    putU32(out, 0);

    // ------------------------------------------------------------------
    // 4. レイヤー & マスク情報セクション
    //
    // フォルダー(グループ)はPSDの仕様上、実データを持たない3つの要素の組み合わせ
    // として表現する: 「区切りマーカー(lsct type=3、グループの開始、ファイル順=
    // ボトムアップで中身より先)」+「中身(子レイヤー、そのまま)」+「グループ
    // ヘッダー本体(lsct type=1、実際の名前・表示状態・マスクはここに乗る、中身の
    // 直後=ファイル順で最後)」。Tiepolo内部の[フォルダー, 子1..子N]という連続配置
    // (祖先が子より前=childCountぶん直後に連続)をそのままファイル順として使えるので、
    // 区切りマーカーの挿入・グループヘッダーの並べ替えだけで変換できる。
    // ------------------------------------------------------------------
    QByteArray layerRecords;
    QByteArray channelImageData;

    int folderCount = 0;
    for (int li = 0; li < doc.layerCount(); li++)
        if (doc.layers[li].layerType == LayerType::Folder) folderCount++;
    putI16(layerRecords, (qint16)(doc.layerCount() + folderCount));

    // レイヤーマスクのグレースケール平面(キャンバス全体、PSDの行順(原点左上)に
    // 変換済み)を1枚組み立てる。マスクは常にキャンバス全体を覆う連続タイルなので、
    // 通常レイヤーの矩形のような原点補正は不要(左上は常にorigin(0,0)扱い)。
    auto extractMaskGrayFlipped = [&](const Layer &layer) -> QByteArray {
        const int mw = layer.maskTilesX() * TILE_SIZE, mh = layer.maskTilesY() * TILE_SIZE;
        QByteArray full((qint64)mw * mh, char(-1));
        for (int ty = 0; ty < layer.maskTilesY(); ty++) {
            for (int tx = 0; tx < layer.maskTilesX(); tx++) {
                QByteArray tile = gl_->readSlicePixels(layer.maskTiles[ty][tx]); // RGBA8, R=G=B=濃淡
                for (int y = 0; y < TILE_SIZE; y++) {
                    const char *src = tile.constData() + y * TILE_SIZE * 4;
                    char *dst = full.data() + ((qint64)(ty * TILE_SIZE + y) * mw + tx * TILE_SIZE);
                    for (int x = 0; x < TILE_SIZE; x++) dst[x] = src[x * 4];
                }
            }
        }
        flipRowsVertically1ch(full, mw, mh);
        return full;
    };

    // 単色/調整/テキストレイヤーそれぞれの非標準・標準追加情報ブロック(SoCo/brit/
    // tpAJ/tpTx)を組み立てる(フォルダーの区切りマーカー・グループヘッダーには
    // 呼ばない、通常の実体レイヤーだけが対象)。
    auto buildLeafExtraInfo = [&](const Layer &layer) -> QByteArray {
        QByteArray extra;
        if (layer.layerType == LayerType::SolidColor) {
            // 単色レイヤー(常に不透明白)。他アプリでの表示用に標準の"SoCo"を書く。
            QByteArray soco;
            putU32(soco, 16); // descriptor version
            putU32(soco, 0);  // name (unicode string length=0)
            putU32(soco, 0);  // classID length=0
            putTag(soco, "nlaC"); // classID (適当な4文字コード。実際の値は他アプリでは重要でない)
            putU32(soco, 1);  // item count = 1
            putU32(soco, 0);  putTag(soco, "Clr "); // key = "Clr "
            putTag(soco, "Objc");
            putU32(soco, 0); putU32(soco, 0); putTag(soco, "RGBC");
            putU32(soco, 3);
            auto writeDoubleColor = [&](const char *key, double v) {
                putU32(soco, 0); putTag(soco, key);
                putTag(soco, "doub");
                quint64 bits; memcpy(&bits, &v, 8);
                quint64 be = qToBigEndian(bits);
                soco.append((const char*)&be, 8);
            };
            writeDoubleColor("Rd  ", 255.0);
            writeDoubleColor("Grn ", 255.0);
            writeDoubleColor("Bl  ", 255.0);
            appendAdditionalInfo(extra, "SoCo", soco);
        } else if (layer.layerType == LayerType::Adjustment) {
            if (layer.adjustment.kind == AdjustmentKind::BrightnessContrast) {
                QByteArray brit;
                putI16(brit, (qint16)layer.adjustment.brightness);
                putI16(brit, (qint16)layer.adjustment.contrast);
                putI16(brit, 0); // mean (未使用)
                putU8(brit, 0);  // lab only
                putU8(brit, 0);  // padding
                appendAdditionalInfo(extra, "brit", brit);
            }
            // 色相・彩度・明度の公式"hue2"ブロックは正確な仕様検証ができないため書き出さない
            // (他アプリでは空の調整レイヤーとして扱われる)。Tiepolo自身での完全な
            // 再現性は下の非標準"tpAJ"ブロックで保証する。

            QJsonObject adjObj;
            adjObj["kind"] = (int)layer.adjustment.kind;
            adjObj["brightness"] = layer.adjustment.brightness;
            adjObj["contrast"]   = layer.adjustment.contrast;
            adjObj["hue"]        = layer.adjustment.hue;
            adjObj["saturation"] = layer.adjustment.saturation;
            adjObj["lightness"]  = layer.adjustment.lightness;
            appendAdditionalInfo(extra, "tpAJ", QJsonDocument(adjObj).toJson(QJsonDocument::Compact));
        } else if (layer.layerType == LayerType::Text) {
            // 公式"TySh"は書式データ(EngineData)の仕様検証ができないため書き出さない
            // (他アプリでは既にラスタライズ済みの通常ピクセルレイヤーとして正しく表示される)。
            // Tiepolo自身での編集可能な再読み込みは非標準"tpTx"ブロックで保証する。
            QJsonArray boxesArr;
            for (const TextParams &box : layer.textBoxes) {
                QJsonObject boxObj;
                boxObj["text"]       = box.text;
                boxObj["fontFamily"] = box.fontFamily;
                boxObj["fontSize"]   = box.fontSize;
                boxObj["color"]      = (qint64)box.color.rgba();
                boxObj["bold"]       = box.bold;
                boxObj["italic"]     = box.italic;
                boxObj["cx"]         = box.cx;
                boxObj["cy"]         = box.cy;
                boxObj["width"]      = box.width;
                boxObj["height"]     = box.height;
                boxObj["rotation"]   = box.rotation;
                boxObj["scale"]      = box.scale;
                boxesArr.append(boxObj);
            }
            QJsonObject textObj;
            textObj["boxes"] = boxesArr;
            appendAdditionalInfo(extra, "tpTx", QJsonDocument(textObj).toJson(QJsonDocument::Compact));
        }
        return extra;
    };

    // 1レコード分を書き出す共通処理。pixelLayer!=nullptrなら実ピクセル(R,G,B,A)を
    // 持つ矩形として書き、nullptrなら単色/調整/フォルダー用の既存の挙動と同じく
    // 0x0の空チャンネルにする。maskLayer!=nullptrならレイヤーマスクのチャンネル
    // (id=-2)と対応するマスクデータブロックも追加で書く。
    auto appendRecord = [&](BlendMode blend, quint8 opacity255, bool clipping, bool visible,
                             const QString &name, const Layer *pixelLayer,
                             const Layer *maskLayer, const QByteArray &extraTypeInfo)
    {
        const bool hasPixels = (pixelLayer != nullptr);
        int left = 0, top = 0, right = 0, bottom = 0;
        QByteArray rgba; // premultiplied RGBA8, w*h*4
        int w = 0, h = 0;
        if (hasPixels) {
            w = pixelLayer->tilesX() * TILE_SIZE;
            h = pixelLayer->tilesY() * TILE_SIZE;
            left = pixelLayer->originTx * TILE_SIZE;
            right = left + w;
            // X軸(左右)はTiepolo/PSDで向きが同じなのでそのままでよいが、Y軸は
            // Tiepolo内部がY上向き(原点が左下)なのに対し、PSDは標準画像と同じ
            // Y下向き(原点が左上)。レイヤーの矩形もキャンバス全体の高さを基準に
            // 上下反転して変換する必要がある(ピクセル内容だけでなく原点も)。
            const int tiepoloYMin = pixelLayer->originTy * TILE_SIZE;
            const int tiepoloYMax = tiepoloYMin + h;
            top    = canvasH - tiepoloYMax;
            bottom = canvasH - tiepoloYMin;

            rgba.resize((qint64)w * h * 4);
            for (int ty = 0; ty < pixelLayer->tilesY(); ty++) {
                for (int tx = 0; tx < pixelLayer->tilesX(); tx++) {
                    QByteArray tile = gl_->readSlicePixels(pixelLayer->tiles[ty][tx]); // TILE_SIZE^2*4
                    for (int y = 0; y < TILE_SIZE; y++) {
                        const char *src = tile.constData() + y * TILE_SIZE * 4;
                        char *dst = rgba.data() + ((qint64)(ty * TILE_SIZE + y) * w + tx * TILE_SIZE) * 4;
                        memcpy(dst, src, TILE_SIZE * 4);
                    }
                }
            }
            // タイル(readSlicePixels)はOpenGLの行順(原点左下)のまま。PSDは標準画像と
            // 同じ行順(原点左上)を期待するため、CanvasCompositorの他の書き出し経路
            // (mirrored(false,true))と同様にここで上下反転する。
            flipRowsVertically(rgba, w, h);
        }

        putI32(layerRecords, top);
        putI32(layerRecords, left);
        putI32(layerRecords, bottom);
        putI32(layerRecords, right);

        const bool hasMask = (maskLayer != nullptr);
        const int chanCount = hasMask ? 5 : 4;
        putU16(layerRecords, (quint16)chanCount);
        const qint16 chanIds[5] = { 0, 1, 2, -1, -2 };
        // 各チャンネルのデータ本体を先に組み立て、長さを記録する
        QByteArray chanData[5];
        for (int ci = 0; ci < 4; ci++) {
            QByteArray plane((qint64)w * h, char(0));
            if (hasPixels) {
                for (qint64 i = 0; i < (qint64)w * h; i++) {
                    quint8 r, g, b, a;
                    unpremulPixel(reinterpret_cast<const uchar*>(rgba.constData()) + i * 4, r, g, b, a);
                    quint8 v = (ci == 0) ? r : (ci == 1) ? g : (ci == 2) ? b : a;
                    plane[i] = (char)v;
                }
            }
            QByteArray chan;
            putU16(chan, 0); // compression = raw
            chan.append(plane);
            chanData[ci] = chan;
        }
        int maskLeft = 0, maskTop = 0, maskRight = 0, maskBottom = 0;
        if (hasMask) {
            const int mw = maskLayer->maskTilesX() * TILE_SIZE, mh = maskLayer->maskTilesY() * TILE_SIZE;
            maskLeft = 0; maskRight = mw;
            maskTop  = canvasH - mh; maskBottom = canvasH; // マスクの原点は常に(0,0)固定
            QByteArray plane = extractMaskGrayFlipped(*maskLayer);
            QByteArray chan;
            putU16(chan, 0);
            chan.append(plane);
            chanData[4] = chan;
        }
        for (int ci = 0; ci < chanCount; ci++) {
            putI16(layerRecords, chanIds[ci]);
            putU32(layerRecords, (quint32)chanData[ci].size());
        }

        putTag(layerRecords, "8BIM");
        putTag(layerRecords, blendModeToKey(blend).constData());
        putU8(layerRecords, opacity255);
        putU8(layerRecords, clipping ? 1 : 0); // clipping: 0=base, 1=non-base(このアプリの意味と近い)
        quint8 flags = 0;
        if (!visible) flags |= 0x02; // bit1: レイヤー非表示
        putU8(layerRecords, flags);
        putU8(layerRecords, 0); // filler

        // extra data (mask + blending ranges + name + additional info)
        QByteArray extra;
        if (hasMask) {
            putU32(extra, 20);
            putI32(extra, maskTop); putI32(extra, maskLeft);
            putI32(extra, maskBottom); putI32(extra, maskRight);
            putU8(extra, 255); // defaultColor(実データは常にチャンネル側にあるので形式上の既定値)
            putU8(extra, 0);   // flags
            putU16(extra, 0);  // padding(2バイト、20バイトぴったりに揃える)
        } else {
            putU32(extra, 0); // layer mask data: 無し
        }
        putU32(extra, 0); // layer blending ranges: 無し

        QByteArray nameBytes = name.toLocal8Bit();
        if (nameBytes.size() > 255) nameBytes.truncate(255);
        QByteArray nameBlock;
        putU8(nameBlock, (quint8)nameBytes.size());
        nameBlock.append(nameBytes);
        // 仕様上は「偶数長」だが、実際にPhotoshopが書き出すファイルは4バイト境界に
        // パディングされているため、読み込み側(loadのpascal名パース)と合わせて4バイト境界にする。
        while (nameBlock.size() % 4 != 0) nameBlock.append(char(0));
        extra.append(nameBlock);

        // Unicode名(完全な名前を保持するため。他アプリはこちらを優先して表示する)
        {
            QByteArray uniBlock;
            putU32(uniBlock, (quint32)name.size());
            for (QChar ch : name) putU16(uniBlock, ch.unicode());
            appendAdditionalInfo(extra, "luni", uniBlock);
        }

        extra.append(extraTypeInfo);

        putU32(layerRecords, (quint32)extra.size());
        layerRecords.append(extra);

        for (int ci = 0; ci < chanCount; ci++) channelImageData.append(chanData[ci]);
    };

    // doc.layers の [start,end) 範囲を、フォルダーのchildCountを使って兄弟の並びに
    // 分解しながら再帰的に書き出す(Tiepolo内部の[フォルダー, 子1..子N]という
    // 連続配置は、そのままPSDのファイル順(ボトムアップ)として使える)。
    std::function<void(int, int)> writeRange = [&](int start, int end) {
        int p = start;
        while (p < end) {
            const Layer &layer = doc.layers[p];
            const Layer *maskLayer = layer.hasMask ? &layer : nullptr;
            if (layer.layerType == LayerType::Folder) {
                const int childEnd = p + 1 + layer.childCount;

                // 区切りマーカー(グループの開始。実体を持たない不可視レコード)。
                QByteArray dividerExtra;
                { QByteArray t; putI32(t, 3); appendAdditionalInfo(dividerExtra, "lsct", t); }
                appendRecord(BlendMode::Normal, 255, false, true, QString(),
                             nullptr, nullptr, dividerExtra);

                writeRange(p + 1, childEnd);

                // グループヘッダー本体(実際の名前・表示状態・マスクはここに乗る、
                // ファイル順で中身の直後=最後)。
                QByteArray hdrExtra;
                { QByteArray t; putI32(t, 1); appendAdditionalInfo(hdrExtra, "lsct", t); }
                appendRecord(BlendMode::Normal, 255, false, layer.visible, layer.name,
                             nullptr, maskLayer, hdrExtra);

                p = childEnd;
            } else {
                const bool hasPixels = (layer.layerType == LayerType::Normal || layer.layerType == LayerType::Text);
                appendRecord(layer.blendMode, (quint8)qBound(0, qRound(layer.opacity * 255.0f), 255),
                             layer.clipping, layer.visible, layer.name,
                             hasPixels ? &layer : nullptr, maskLayer, buildLeafExtraInfo(layer));
                p++;
            }
        }
    };
    writeRange(0, doc.layerCount());

    // 「Layer info」構造 = [4-byte length][2-byte layer count(layerRecordsの先頭に既に含む)]
    // [layer records][channel image data]。長さ(4-byte length)は、この長さ自身を含まない
    // 後続バイト数(count+records+channelData)を指す。
    QByteArray layerInfo;
    putU32(layerInfo, (quint32)(layerRecords.size() + channelImageData.size()));
    layerInfo.append(layerRecords);
    layerInfo.append(channelImageData);
    padToEven(layerInfo);

    // 「Layer and Mask Information」セクションの中身 = Layer info(既に自身の長さを
    // 先頭に持つ) + Global layer mask info(4-byte length=0のみ)。ここで重複した
    // 長さフィールドを追加してはいけない(layerInfoは既に自己記述的)。
    QByteArray layerAndMaskInfo = layerInfo;
    putU32(layerAndMaskInfo, 0); // global layer mask info: 無し

    putU32(out, (quint32)layerAndMaskInfo.size());
    out.append(layerAndMaskInfo);

    // ------------------------------------------------------------------
    // 5. イメージデータ(合成プレビュー。他アプリのサムネイル/プレビュー用で、
    //    Tiepolo自身の再読み込み時はレイヤー情報から再構築するため使わない)
    // ------------------------------------------------------------------
    {
        QImage merged = gl_->exportCanvas();
        if (merged.isNull() || merged.width() != canvasW || merged.height() != canvasH)
            merged = QImage(canvasW, canvasH, QImage::Format_RGB888), merged.fill(Qt::white);
        merged = merged.convertToFormat(QImage::Format_RGB888);

        putU16(out, 0); // compression = raw
        for (int ci = 0; ci < 3; ci++) {
            for (int y = 0; y < canvasH; y++) {
                const uchar *row = merged.constScanLine(y);
                for (int x = 0; x < canvasW; x++)
                    out.append((char)row[x * 3 + ci]);
            }
        }
    }

    // ------------------------------------------------------------------
    // 6. ファイルに書き出す
    // ------------------------------------------------------------------
    // CanvasSerializer::saveと同じ理由でQSaveFileを使う(書き込み途中の失敗で
    // 既存の保存済みファイルを破壊しないよう、一時ファイル→commit()でのアトミックな
    // 差し替えにする)。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError("ファイルを開けませんでした: " + path);
        return false;
    }
    if (file.write(out) != out.size()) {
        setError("書き込みに失敗しました(ディスク容量不足の可能性があります): " + path);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError("ファイルの確定に失敗しました: " + file.errorString());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 読み込み
// ---------------------------------------------------------------------------
bool PsdCodec::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError("ファイルを開けませんでした: " + path);
        return false;
    }
    QByteArray data = file.readAll();
    file.close();

    Cursor c(data);
    QByteArray magic = c.tag4();
    if (magic != QByteArray("8BPS", 4)) { setError("不正なPSDファイルです(シグネチャ不一致)"); return false; }
    quint16 version = c.u16();
    if (version != 1) { setError("非対応のPSDバージョンです(PSB/大型ドキュメントは非対応)"); return false; }
    c.skip(6); // reserved
    quint16 numChannels = c.u16(); (void)numChannels;
    quint32 rows = c.u32();
    quint32 cols = c.u32();
    quint16 depth = c.u16();
    quint16 mode  = c.u16();
    if (!c.ok) { setError("PSDヘッダの読み込みに失敗しました"); return false; }

    if (mode != 3 /*RGB*/ && mode != 1 /*Grayscale*/) {
        setError("非対応のカラーモードです(RGB/グレースケール以外のPSDは読み込めません)");
        return false;
    }
    if (depth != 8 && depth != 16) {
        setError(QString("非対応のビット深度です: %1bit (8/16bitのみ対応)").arg(depth));
        return false;
    }
    const int canvasW = (int)cols;
    const int canvasH = (int)rows;
    if (canvasW <= 0 || canvasH <= 0) { setError("不正なキャンバスサイズです"); return false; }

    quint32 colorModeDataLen = c.u32();
    c.skip(colorModeDataLen);

    quint32 imageResourcesLen = c.u32();
    c.skip(imageResourcesLen);

    quint32 layerAndMaskInfoLen = c.u32();
    const qint64 layerAndMaskEnd = c.pos + layerAndMaskInfoLen;

    struct ChanDecl { qint16 id; quint32 len; };
    struct RawLayer {
        QString name;
        int top = 0, left = 0, bottom = 0, right = 0;
        BlendMode blendMode = BlendMode::Normal;
        quint8 opacity255 = 255;
        bool clipping = false;
        bool visible = true;
        AddlInfo info;
        // チャンネル宣言(1パス目で記録し、全レイヤー分のレコードを読み終えた後の
        // 2パス目でまとめてデコードする。PSDは「全レイヤーのレコード」→「全レイヤーの
        // チャンネル画像データ」の順で並んでおり、レイヤーごとに交互ではないため)。
        QVector<ChanDecl> decls;
        // チャンネル: id -> (デコード済みプレーンデータ)
        struct Chan { qint16 id; QByteArray raw; int w, h; };
        QVector<Chan> channels;
        // レイヤーマスク
        bool hasMask = false;
        int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
        quint8 maskDefaultColor = 0;
    };
    QVector<RawLayer> rawLayers;

    // 進捗通知の全体数。1パス目(レイヤーレコード解析)完了後、実際の作業量
    // (チャンネルデコード数+タイル書き込み枚数の見積もり)から算出する。
    int progressTotal = 0;
    int progressDone = 0;

    if (layerAndMaskInfoLen > 0) {
        quint32 layerInfoLen = c.u32();
        const qint64 layerInfoEnd = c.pos + layerInfoLen;

        qint16 layerCountField = c.i16();
        int layerCount = qAbs((int)layerCountField);

        // --- 1パス目: 全レイヤーの「レイヤーレコード」(矩形・チャンネル宣言・
        // ブレンド情報・マスク情報・名前・追加情報)だけを読む。実ピクセルデータは
        // ここでは読まず、チャンネル宣言(id+length)だけ憶えておく。
        rawLayers.resize(layerCount);
        for (int li = 0; li < layerCount && c.ok; li++) {
            RawLayer &rl = rawLayers[li];
            rl.top = c.i32(); rl.left = c.i32(); rl.bottom = c.i32(); rl.right = c.i32();
            quint16 chanCount = c.u16();
            rl.decls.resize(chanCount);
            for (int i = 0; i < chanCount; i++) { rl.decls[i].id = c.i16(); rl.decls[i].len = c.u32(); }

            QByteArray sig = c.tag4(); (void)sig;
            QByteArray blendKey = c.tag4();
            rl.blendMode = blendKeyToMode(blendKey);
            rl.opacity255 = c.u8();
            rl.clipping = c.u8() != 0;
            quint8 flags = c.u8();
            rl.visible = (flags & 0x02) == 0;
            c.u8(); // filler

            quint32 extraLen = c.u32();
            const qint64 extraEnd = c.pos + extraLen;

            quint32 maskLen = c.u32();
            if (maskLen > 0) {
                const qint64 maskEnd = c.pos + maskLen;
                rl.hasMask = true;
                rl.maskTop = c.i32(); rl.maskLeft = c.i32(); rl.maskBottom = c.i32(); rl.maskRight = c.i32();
                rl.maskDefaultColor = c.u8();
                c.u8(); // flags
                c.seekAbs(maskEnd);
            }

            quint32 blendRangesLen = c.u32();
            c.skip(blendRangesLen);

            quint8 nameLen = c.u8();
            QByteArray nameBytes = c.bytes(nameLen);
            rl.name = QString::fromLocal8Bit(nameBytes);
            // pascal文字列は(1+len)が4バイト境界になるようパディングされている
            qint64 afterLen1 = 1 + nameLen;
            qint64 pad = (4 - (afterLen1 % 4)) % 4;
            c.skip(pad);

            while (c.ok && c.pos < extraEnd) {
                QByteArray infoSig = c.tag4();
                if (infoSig != QByteArray("8BIM", 4) && infoSig != QByteArray("8B64", 4)) break;
                QByteArray key = c.tag4();
                quint32 len = c.u32();
                QByteArray blockData = c.bytes(len);
                if (len % 2 != 0) c.skip(1);
                rl.info.blocks.insert(key, blockData);
            }
            c.seekAbs(extraEnd);

            // Unicode名があれば優先
            if (rl.info.has("luni")) {
                Cursor uc(rl.info.get("luni"));
                QString uname;
                if (descReadUnicodeString(uc, uname) && !uname.isEmpty()) rl.name = uname;
            }
        }

        // 進捗の全体数を、実際にコールバックする単位(チャンネルデコール数+
        // タイル書き込み枚数の見積もり)で数え直す。レイヤー単位だと粒度が粗すぎて
        // (1レイヤーが巨大な場合に)長時間コールバックが呼ばれずOSに「応答なし」と
        // 判定されてしまうため、実際の作業量に近い単位を使う。
        {
            qint64 total = 0;
            const int canvasTilesX = (canvasW + TILE_SIZE - 1) / TILE_SIZE;
            const int canvasTilesY = (canvasH + TILE_SIZE - 1) / TILE_SIZE;
            for (const RawLayer &rl : rawLayers) {
                total += rl.decls.size(); // デコードフェーズ(チャンネル単位)
                const int w = rl.right - rl.left, h = rl.bottom - rl.top;
                if (w > 0 && h > 0)
                    total += (qint64)((w + TILE_SIZE - 1) / TILE_SIZE) * ((h + TILE_SIZE - 1) / TILE_SIZE);
                if (rl.hasMask)
                    total += (qint64)canvasTilesX * canvasTilesY; // マスクは常にキャンバス全体
                total += 1; // 構築フェーズ自体の1ステップ(レイヤー追加)ぶん
            }
            progressTotal = (int)qMax<qint64>(1, total);
        }

        // --- 2パス目: 全レイヤーレコードを読み終えた後、まとめて「チャンネル画像
        // データ」セクション(全レイヤー分が記録順に連続して並ぶ)を読む。PSDは
        // 「全レイヤーのレコード」→「全レイヤーの画像データ」の順であり、
        // レイヤーごとに交互ではないため、1パス目とは別ループにする必要がある。
        // 各チャンネルは「宣言済みの長さ(ChanDecl::len)」ぶんが記録順に隙間なく
        // 並んでいるだけなので、中身を展開しなくても位置は determinable。先に位置だけを
        // 直列で拾い、展開自体はチャンネル間で完全に独立した純CPU処理なので並列に回す
        // (TPLO側のqUncompress並列化と同じ構成: 直列で位置決め→並列で展開→直列で組み立て)。
        struct ChanJob {
            int li;            // rawLayers内のindex
            qint64 offset;     // ファイル先頭からのバイト位置(圧縮方式2バイトを含む)
            qint64 len;        // 宣言済みの長さ(圧縮方式2バイトを含む)
            int w, h;
            qint16 id;
            QByteArray plane;  // 展開結果(並列パスで埋める)
        };
        QVector<ChanJob> chanJobs;
        for (int li = 0; li < layerCount && c.ok; li++) {
            const RawLayer &rl = rawLayers[li];
            for (const ChanDecl &d : rl.decls) {
                ChanJob j;
                j.li = li;
                j.id = d.id;
                if (d.id == -2 && rl.hasMask) { j.w = rl.maskRight - rl.maskLeft; j.h = rl.maskBottom - rl.maskTop; }
                else { j.w = rl.right - rl.left; j.h = rl.bottom - rl.top; }
                j.offset = c.pos;
                j.len    = d.len;
                if (!c.require(d.len)) break; // ファイルが途中で切れている
                c.skip(d.len);
                chanJobs.append(j);
            }
        }

        {
            // 進捗表示が長時間止まらないよう、一定数ずつ区切って回す
            // (progressCallbackはダイアログを触るのでワーカーからは呼べない)。
            const uchar *fileBase = c.base;
            constexpr int kChunk = 32;
            for (int base = 0; base < chanJobs.size(); base += kChunk) {
                const int hi = qMin<int>(chanJobs.size(), base + kChunk);
                QtConcurrent::blockingMap(chanJobs.begin() + base, chanJobs.begin() + hi,
                                          [fileBase, depth](ChanJob &j) {
                    Cursor jc(fileBase + j.offset, j.len);
                    const quint16 compression = jc.u16();
                    if (depth == 8) j.plane = decodeChannelPlane(jc, compression, j.w, j.h, j.len - 2);
                    else            j.plane = decodeChannelPlane16(jc, compression, j.w, j.h, j.len - 2);
                });
                if (progressCallback) {
                    progressDone += hi - base;
                    progressCallback(progressDone, progressTotal);
                }
            }
        }

        for (ChanJob &j : chanJobs) {
            RawLayer::Chan ch;
            ch.id = j.id;
            ch.w  = j.w;
            ch.h  = j.h;
            ch.raw = std::move(j.plane);
            rawLayers[j.li].channels.append(ch);
        }
        c.seekAbs(layerInfoEnd);
    }
    c.seekAbs(layerAndMaskEnd);

    // ------------------------------------------------------------------
    // マージ済み(合成)イメージデータ ―― レイヤー情報が全く無いPSD(フラット画像)
    // からのフォールバック用に読んでおく。
    // ------------------------------------------------------------------
    QImage mergedFallback;
    if (rawLayers.isEmpty()) {
        quint16 compression = c.u16();
        const int chCount = (mode == 1) ? 1 : 3;
        QVector<QByteArray> planes(chCount);
        for (int ci = 0; ci < chCount && c.ok; ci++) {
            if (compression == 0) {
                qint64 n = (qint64)canvasW * canvasH * (depth == 16 ? 2 : 1);
                QByteArray raw = c.bytes(n);
                if (depth == 16) {
                    QByteArray narrow(canvasW * canvasH, char(0));
                    for (int i = 0; i < canvasW * canvasH; i++) narrow[i] = raw[i*2];
                    planes[ci] = narrow;
                } else planes[ci] = raw;
            } else if (compression == 1) {
                QVector<quint16> rowLens(canvasH);
                for (int y = 0; y < canvasH; y++) rowLens[y] = c.u16();
                // レイヤー側と同じく、行ごとの一時バッファを介さず直接展開する
                QByteArray plane((qint64)canvasW * canvasH, char(0));
                char *pdst = plane.data();
                for (int y = 0; y < canvasH && c.ok; y++) {
                    const qint64 rowLen = rowLens[y];
                    if (!c.require(rowLen)) break;
                    packBitsDecodeRowInto(c.base + c.pos, rowLen, pdst + (qint64)y * canvasW, canvasW);
                    c.pos += rowLen;
                }
                planes[ci] = plane;
            } else {
                planes[ci] = QByteArray(canvasW * canvasH, char(0xFF));
            }
        }
        mergedFallback = QImage(canvasW, canvasH, QImage::Format_RGBA8888_Premultiplied);
        // 各プレーンは画素ごとにoperator[]で引かず、生ポインタにしてから読む
        const uchar *p0 = reinterpret_cast<const uchar*>(planes[0].constData());
        const uchar *p1 = (chCount > 1) ? reinterpret_cast<const uchar*>(planes[1].constData()) : nullptr;
        const uchar *p2 = (chCount > 2) ? reinterpret_cast<const uchar*>(planes[2].constData()) : nullptr;
        for (int y = 0; y < canvasH; y++) {
            uchar *dst = mergedFallback.scanLine(y);
            const qint64 rowBase = (qint64)y * canvasW;
            for (int x = 0; x < canvasW; x++) {
                const qint64 idx = rowBase + x;
                const quint8 r = p0[idx];
                const quint8 g = p1 ? p1[idx] : r;
                const quint8 b = p2 ? p2[idx] : r;
                dst[x*4+0] = r; dst[x*4+1] = g; dst[x*4+2] = b; dst[x*4+3] = 255;
            }
        }
    }

    // ------------------------------------------------------------------
    // ドキュメントを再構築
    // ------------------------------------------------------------------
    gl_->recreateCanvas(canvasW, canvasH, /*createDefaultLayers=*/false);

    // これから確保するタイル(スライス)総数を概算して一括で確保しておく
    // (レイヤーごとの addLayer で少しずつ伸長+コピーが繰り返されるのを避けて高速化する)。
    // 見積もりは概算でよい(過不足があっても、足りなければ従来通り都度伸長するだけ)。
    {
        const int canvasTilesX = (canvasW + TILE_SIZE - 1) / TILE_SIZE;
        const int canvasTilesY = (canvasH + TILE_SIZE - 1) / TILE_SIZE;
        qint64 estTotal = 0;
        for (const RawLayer &rl : rawLayers) {
            const int w = rl.right - rl.left, h = rl.bottom - rl.top;
            if (w > 0 && h > 0)
                estTotal += (qint64)((w + TILE_SIZE - 1) / TILE_SIZE) * ((h + TILE_SIZE - 1) / TILE_SIZE);
            if (rl.hasMask)
                estTotal += (qint64)canvasTilesX * canvasTilesY;
        }
        if (estTotal > 0) gl_->reserveTileSlices((int)qMin<qint64>(estTotal, 0x7fffffff));
    }

    // レイヤーを1枚追加するたびにLayerDock/NavigatorDockがフルキャンバス合成の
    // サムネイル再生成を行うと、レイヤー数の多いPSDでO(レイヤー数^2)の重さになる。
    // 以降のレイヤー構築ループの間は通知を止め、最後にまとめて1回だけ通知する。
    gl_->beginBulkLayerImport();

    // left/topPsd/w/h はPSDネイティブの矩形(原点左上、Y下向き)。X軸はTiepolo内部と
    // 向きが同じだが、Y軸はTiepolo内部(原点左下、Y上向き)と逆なので、タイル配置は
    // キャンバス高さを軸に上下反転して変換する(save()側の逆変換に対応する)。
    auto addFlattenedNormalLayer = [&](const QString &name, const QByteArray &rgbaPremul,
                                        int left, int topPsd, int w, int h, bool clipping,
                                        BlendMode blend, float opacity, bool visible,
                                        const QVector<int> &ancestorFolders = {}) {
        const int tiepoloYMin = canvasH - (topPsd + h);
        int minTx = qFloor((double)left / TILE_SIZE);
        int minTy = qFloor((double)tiepoloYMin / TILE_SIZE);
        int maxTxEx = qCeil((double)(left + w) / TILE_SIZE);
        int maxTyEx = qCeil((double)(tiepoloYMin + h) / TILE_SIZE);
        if (w <= 0 || h <= 0) { minTx = 0; minTy = 0; maxTxEx = 1; maxTyEx = 1; }
        int tilesX = qMax(1, maxTxEx - minTx);
        int tilesY = qMax(1, maxTyEx - minTy);

        gl_->addLayer(name, -1, clipping, minTx, minTy, tilesX, tilesY, LayerType::Normal, ancestorFolders);
        int idx = gl_->document().layerCount() - 1;
        gl_->document().setLayerOpacity(idx, opacity);
        gl_->document().setLayerVisible(idx, visible);
        gl_->document().setLayerBlendMode(idx, blend);

        if (w <= 0 || h <= 0) return idx;

        // PSDの行順(標準画像と同じく原点左上)と、タイル(writeSlicePixels)が期待する
        // OpenGLの行順(原点左下)の違いは、下のタイル組み立てで「元バッファの行を
        // 逆から読む」ことで吸収する(save()側のflipRowsVerticallyと対になる。矩形の
        // 原点変換は上のtiepoloYMinで別途行う)。
        // 以前はここで上下反転済みのコピーを作っていたが、レイヤー1枚ぶんの
        // RGBAバッファ全体の複製+全画素の入れ替えが1レイヤーごとに走っていた。

        const Layer &layer = gl_->document().layerAt(idx);
        // タイル1枚ごとにQByteArrayを確保し直すと、256KBの確保・解放と、その都度の
        // ページフォルトがタイル枚数ぶん積み上がる(激重ファイルでは合計1.7GB相当)。
        // writeSlicePixelsはクライアントメモリを同期的に読み終えてから戻るので、
        // 1枚ぶんのバッファを確保して使い回して問題ない。
        constexpr qint64 kTileBytes = (qint64)TILE_SIZE * TILE_SIZE * 4;
        QByteArray buf(kTileBytes, char(0));
        char *bufBase = buf.data();
        for (int ty = 0; ty < layer.tilesY(); ty++) {
            for (int tx = 0; tx < layer.tilesX(); tx++) {
                const int tilePxX = (layer.originTx + tx) * TILE_SIZE;
                const int tileTiepoloY = (layer.originTy + ty) * TILE_SIZE;
                memset(bufBase, 0, (size_t)kTileBytes); // レイヤー矩形外は透明
                const int ox0 = qMax(tilePxX, left), oy0 = qMax(tileTiepoloY, tiepoloYMin);
                const int ox1 = qMin(tilePxX + TILE_SIZE, left + w), oy1 = qMin(tileTiepoloY + TILE_SIZE, tiepoloYMin + h);
                if (ox0 < ox1 && oy0 < oy1) {
                    // constData()は行ごとではなくループの外で取る
                    const char *srcBase = rgbaPremul.constData();
                    for (int y = oy0; y < oy1; y++) {
                        // 上下反転: 出力の(y - tiepoloYMin)行目は、元バッファの下から数えた同じ位置
                        const qint64 srcRow = (qint64)(h - 1 - (y - tiepoloYMin));
                        const char *src = srcBase + (srcRow * w + (ox0 - left)) * 4;
                        char *dst = bufBase + ((qint64)(y - tileTiepoloY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                        memcpy(dst, src, (size_t)(ox1 - ox0) * 4);
                    }
                }
                gl_->writeSlicePixels(layer.tiles[ty][tx], buf);
                if (progressCallback) progressCallback(++progressDone, progressTotal);
            }
        }
        return idx;
    };

    if (rawLayers.isEmpty()) {
        // レイヤー情報が無い(フラット画像として保存された)PSD: 合成イメージを
        // そのまま1枚の通常レイヤーとして取り込む。
        QByteArray rgba(reinterpret_cast<const char*>(mergedFallback.constBits()),
                         (qint64)canvasW * canvasH * 4);
        addFlattenedNormalLayer("背景", rgba, 0, 0, canvasW, canvasH, false,
                                 BlendMode::Normal, 1.0f, true);
    } else {
        // ------------------------------------------------------------------
        // レイヤーグループ(フォルダー)の木構造を復元する。
        //
        // rawLayers はPSDのファイル順(ボトムアップ)のまま並んでいる。この順で
        // 走査しながら、「区切りマーカー(lsct type=3、グループの開始)」で新しい
        // フレームを積み、「グループヘッダー(lsct type=1/2、実体・名前を持つ
        // レコード自身)」でフレームを1つ取り出してフォルダーノードにする、という
        // スタックベースの標準的な手順で木を組み立てる(save()側のwriteRangeの
        // 逆変換にあたる)。
        // ------------------------------------------------------------------
        struct PsdTreeNode {
            bool isFolder = false;
            int rawIndex = -1;       // leafなら対象のrawLayers index、folderならヘッダーのindex
            QVector<PsdTreeNode> children; // folderのときのみ意味を持つ
        };

        QVector<QVector<PsdTreeNode>> frameStack;
        frameStack.append(QVector<PsdTreeNode>()); // ルート
        for (int i = 0; i < rawLayers.size(); i++) {
            const RawLayer &rl = rawLayers[i];
            qint32 sectionType = -1;
            if (rl.info.has("lsct")) {
                Cursor lc(rl.info.get("lsct"));
                sectionType = lc.i32();
            }
            if (sectionType == 3) {
                frameStack.append(QVector<PsdTreeNode>());
            } else if (sectionType == 1 || sectionType == 2) {
                QVector<PsdTreeNode> children = frameStack.size() > 1 ? frameStack.takeLast() : QVector<PsdTreeNode>();
                PsdTreeNode node; node.isFolder = true; node.rawIndex = i; node.children = children;
                frameStack.last().append(node);
            } else {
                PsdTreeNode node; node.rawIndex = i;
                frameStack.last().append(node);
            }
        }
        // 閉じ忘れた区切りマーカーが残っていたら(不正/非対応な構造)、中身を1段
        // 上へそのまま繰り上げて取りこぼしを防ぐ。
        while (frameStack.size() > 1) {
            QVector<PsdTreeNode> leftover = frameStack.takeLast();
            for (const PsdTreeNode &n : leftover) frameStack.last().append(n);
        }
        const QVector<PsdTreeNode> rootNodes = frameStack.first();

        // レイヤーマスクを、指定indexのレイヤーへ非破壊的に適用する(通常/単色/
        // 調整/テキスト/フォルダーいずれのレイヤーにも掛けられる。合成側の
        // maskAlphaOfは全種類のレイヤーに一様に適用されるため)。
        auto applyMaskIfAny = [&](int idx, const RawLayer &rl) {
            if (!rl.hasMask || idx < 0) return;
            if (!gl_->addLayerMask(idx)) return;
            const Layer &layer = gl_->document().layerAt(idx);

            // キャンバス全体ぶんのグレースケール(既定色で初期化し、マスク矩形内だけ
            // 実データで上書きする。矩形がキャンバスより小さいのはPSDでは普通)。
            QByteArray gray((qint64)canvasW * canvasH, char(rl.maskDefaultColor));
            const QByteArray *mC = nullptr;
            for (const RawLayer::Chan &ch : rl.channels) if (ch.id == -2) { mC = &ch.raw; break; }
            if (mC) {
                const int mw = rl.maskRight - rl.maskLeft, mh = rl.maskBottom - rl.maskTop;
                // キャンバスに収まる範囲を先に求めておき、行ごとにmemcpyで流し込む
                // (画素ごとのgray[...](detach()を通る)とmC->at()の呼び出しをなくす)。
                char *gdst = gray.data();
                const char *msrc = mC->constData();
                const qint64 msize = mC->size();
                for (int y = 0; y < mh; y++) {
                    const int py = rl.maskTop + y;
                    if (py < 0 || py >= canvasH) continue;
                    const int x0 = qMax(0, -rl.maskLeft);              // マスク内での開始x
                    const int x1 = qMin(mw, canvasW - rl.maskLeft);    // マスク内での終了x
                    if (x0 >= x1) continue;
                    const qint64 srcOff = (qint64)y * mw + x0;
                    const qint64 n = qMin<qint64>(x1 - x0, msize - srcOff); // 壊れたファイル対策
                    if (n <= 0) continue;
                    memcpy(gdst + (qint64)py * canvasW + (rl.maskLeft + x0), msrc + srcOff, (size_t)n);
                }
            }
            // PSDの行順(原点左上)→タイル書き込みが期待する行順(原点左下)へ上下反転
            // 上下反転はタイル組み立てで行を逆から読むことで吸収する(反転済みの
            // コピーをキャンバス全面ぶん作らずに済む)。

            constexpr qint64 kMaskTileBytes = (qint64)TILE_SIZE * TILE_SIZE * 4;
            QByteArray buf(kMaskTileBytes, char(-1)); // 本体と同じ理由でタイル間で使い回す
            char *bufBase = buf.data();
            for (int ty = 0; ty < layer.maskTilesY(); ty++) {
                for (int tx = 0; tx < layer.maskTilesX(); tx++) {
                    memset(bufBase, 0xFF, (size_t)kMaskTileBytes); // 既定は白(=全面表示)
                    const int tilePxX = tx * TILE_SIZE, tilePxY = ty * TILE_SIZE;
                    const int ox0 = qMax(tilePxX, 0), oy0 = qMax(tilePxY, 0);
                    const int ox1 = qMin(tilePxX + TILE_SIZE, canvasW), oy1 = qMin(tilePxY + TILE_SIZE, canvasH);
                    if (ox0 < ox1 && oy0 < oy1) {
                        const char *srcBase = gray.constData();
                        for (int y = oy0; y < oy1; y++) {
                            const char *src = srcBase + (qint64)(canvasH - 1 - y) * canvasW + ox0;
                            char *dst = bufBase + ((qint64)(y - tilePxY) * TILE_SIZE + (ox0 - tilePxX)) * 4;
                            for (int x = 0; x < ox1 - ox0; x++) {
                                uchar v = (uchar)src[x];
                                dst[x*4+0] = (char)v; dst[x*4+1] = (char)v; dst[x*4+2] = (char)v; dst[x*4+3] = (char)0xFF;
                            }
                        }
                    }
                    gl_->writeSlicePixels(layer.maskTiles[ty][tx], buf);
                    if (progressCallback) progressCallback(++progressDone, progressTotal);
                }
            }
        };

        // 1枚の実体レイヤー(フォルダー以外)を取り込む。戻り値: 作成したレイヤーの
        // index(全種とも非対応で何も作られなければ-1になることは無い設計だが、
        // 念のため-1を返せるようにしておく)。
        auto importLeaf = [&](const RawLayer &rl, const QVector<int> &ancestorFolders) -> int {
            const float opacity = rl.opacity255 / 255.0f;

            // --- テキストレイヤー ---
            if (rl.info.has("tpTx") || rl.info.has("TySh")) {
                std::vector<TextParams> boxes;
                bool got = false;
                if (rl.info.has("tpTx")) {
                    QJsonObject root = QJsonDocument::fromJson(rl.info.get("tpTx")).object();
                    const QJsonArray boxesArr = root["boxes"].toArray();
                    for (const QJsonValue &v : boxesArr) {
                        QJsonObject o = v.toObject();
                        TextParams tp;
                        tp.text       = o["text"].toString();
                        tp.fontFamily = o["fontFamily"].toString(QStringLiteral("Yu Gothic UI"));
                        tp.fontSize   = o["fontSize"].toInt(48);
                        tp.color      = QColor::fromRgba((QRgb)o["color"].toVariant().toLongLong());
                        tp.bold       = o["bold"].toBool(false);
                        tp.italic     = o["italic"].toBool(false);
                        tp.cx         = (float)o["cx"].toDouble(0);
                        tp.cy         = (float)o["cy"].toDouble(0);
                        tp.width      = (float)o["width"].toDouble(300);
                        tp.height     = (float)o["height"].toDouble(100);
                        tp.rotation   = (float)o["rotation"].toDouble(0);
                        tp.scale      = (float)o["scale"].toDouble(1.0);
                        boxes.push_back(tp);
                    }
                    got = true; // tpTxブロックがあれば(空配列でも)対応済みとみなす
                } else {
                    QString text;
                    if (parseTyShText(rl.info.get("TySh"), text) && !text.isEmpty()) {
                        // フォント/サイズ情報が取れないため既定値(Yu Gothic UI, 48px)で
                        // 外接矩形を求め、それをそのままボックスサイズとして使う
                        // (見た目上は旧版の「自動フィット」相当になる)。
                        const QFont font(QStringLiteral("Yu Gothic UI"), 48);
                        const QFontMetrics fm(font);
                        const QRect bbox = fm.boundingRect(QRect(0, 0, 8192, 8192), Qt::TextWordWrap, text);
                        const int w = qMax(1, bbox.width()), h = qMax(1, bbox.height());
                        // PSD矩形(原点左上、Y下向き)→Tiepoloネイティブ(原点左下、Y上向き)。
                        const int left = rl.left;
                        const int top  = canvasH - rl.bottom;
                        TextParams tp;
                        tp.text   = text;
                        tp.cx     = left + w / 2.0f;
                        tp.cy     = top  + h / 2.0f;
                        tp.width  = float(w);
                        tp.height = float(h);
                        boxes.push_back(tp);
                        got = true;
                    }
                }
                if (got) {
                    // 空(0x0)のテキストレイヤーとして作成し、rasterizeTextLayerPublic側で
                    // ボックスの外接矩形ぶんだけタイルを確保させる(LayerDock::insertTextLayerと同じ流儀)。
                    gl_->addLayer(rl.name, -1, rl.clipping, 0, 0, 0, 0, LayerType::Text, ancestorFolders);
                    int idx = gl_->document().layerCount() - 1;
                    gl_->document().setLayerOpacity(idx, opacity);
                    gl_->document().setLayerVisible(idx, rl.visible);
                    gl_->document().setLayerBlendMode(idx, rl.blendMode);
                    gl_->document().layerRef(idx).textBoxes = boxes;
                    gl_->rasterizeTextLayerPublic(idx);
                    return idx;
                }
                // TySh解析に失敗した場合はラスタライズ済みピクセルをそのまま通常レイヤーとして取り込む
                // (下の共通処理へフォールスルー)
            }

            // --- 塗りつぶし/調整レイヤー ---
            if (rl.info.has("tpAJ")) {
                QJsonObject o = QJsonDocument::fromJson(rl.info.get("tpAJ")).object();
                AdjustmentParams ap;
                ap.kind       = static_cast<AdjustmentKind>(o["kind"].toInt(0));
                ap.brightness = o["brightness"].toInt(0);
                ap.contrast   = o["contrast"].toInt(0);
                ap.hue        = o["hue"].toInt(0);
                ap.saturation = o["saturation"].toInt(0);
                ap.lightness  = o["lightness"].toInt(0);
                gl_->addLayer(rl.name, -1, rl.clipping, 0, 0, -1, -1, LayerType::Adjustment, ancestorFolders);
                int idx = gl_->document().layerCount() - 1;
                gl_->document().setLayerOpacity(idx, opacity);
                gl_->document().setLayerVisible(idx, rl.visible);
                gl_->document().setLayerBlendMode(idx, rl.blendMode);
                gl_->document().layerRef(idx).adjustment = ap;
                return idx;
            }
            if (rl.info.has("SoCo")) {
                gl_->addSolidColorLayer(rl.name.isEmpty() ? QStringLiteral("単色レイヤー") : rl.name, -1,
                                         QColor(255, 255, 255, 255), ancestorFolders);
                int idx = gl_->document().layerCount() - 1;
                gl_->document().setLayerOpacity(idx, opacity);
                gl_->document().setLayerVisible(idx, rl.visible);
                gl_->document().setLayerBlendMode(idx, rl.blendMode);
                return idx;
            }
            if (rl.info.has("brit")) {
                AdjustmentParams ap; ap.kind = AdjustmentKind::BrightnessContrast;
                parseBrit(rl.info.get("brit"), ap.brightness, ap.contrast);
                gl_->addLayer(rl.name, -1, rl.clipping, 0, 0, -1, -1, LayerType::Adjustment, ancestorFolders);
                int idx = gl_->document().layerCount() - 1;
                gl_->document().setLayerOpacity(idx, opacity);
                gl_->document().setLayerVisible(idx, rl.visible);
                gl_->document().setLayerBlendMode(idx, rl.blendMode);
                gl_->document().layerRef(idx).adjustment = ap;
                return idx;
            }
            if (rl.info.has("hue2")) {
                AdjustmentParams ap; ap.kind = AdjustmentKind::HueSaturation;
                parseHue2(rl.info.get("hue2"), ap.hue, ap.saturation, ap.lightness);
                gl_->addLayer(rl.name, -1, rl.clipping, 0, 0, -1, -1, LayerType::Adjustment, ancestorFolders);
                int idx = gl_->document().layerCount() - 1;
                gl_->document().setLayerOpacity(idx, opacity);
                gl_->document().setLayerVisible(idx, rl.visible);
                gl_->document().setLayerBlendMode(idx, rl.blendMode);
                gl_->document().layerRef(idx).adjustment = ap;
                return idx;
            }
            for (const QByteArray &k : unsupportedAdjustmentKeys()) {
                if (rl.info.has(k.constData())) {
                    // 非対応の塗りつぶし/調整レイヤー: 透明な通常レイヤーとして配置する
                    gl_->addLayer(rl.name, -1, rl.clipping, 0, 0, -1, -1, LayerType::Normal, ancestorFolders);
                    int idx = gl_->document().layerCount() - 1;
                    gl_->document().setLayerOpacity(idx, opacity);
                    gl_->document().setLayerVisible(idx, rl.visible);
                    gl_->document().setLayerBlendMode(idx, rl.blendMode);
                    return idx;
                }
            }

            // --- 通常レイヤー(ピクセルのみ。マスクは呼び出し側でapplyMaskIfAnyが
            //     非破壊的に適用するので、ここではアルファへの合成は行わない) ---
            const int w = rl.right - rl.left, h = rl.bottom - rl.top;
            QByteArray rgba((qint64)qMax(0, w) * qMax(0, h) * 4, char(0));
            if (w > 0 && h > 0) {
                const QByteArray *rC = nullptr, *gC = nullptr, *bC = nullptr, *aC = nullptr;
                for (const RawLayer::Chan &ch : rl.channels) {
                    if (ch.id == 0) rC = &ch.raw;
                    else if (ch.id == 1) gC = &ch.raw;
                    else if (ch.id == 2) bC = &ch.raw;
                    else if (ch.id == -1) aC = &ch.raw;
                }
                // 非constなQByteArray::data()はdetach()を通るため、ピクセルごとに
                // 呼ぶと画素数ぶんの関数呼び出し+参照カウント確認になる。
                // 書き込み先も各チャンネルも、ループの外で生ポインタにしておく。
                uchar *dstBase = reinterpret_cast<uchar*>(rgba.data());
                const qint64 pxCount = (qint64)w * h;
                const uchar *rp = rC ? reinterpret_cast<const uchar*>(rC->constData()) : nullptr;
                const uchar *gp = gC ? reinterpret_cast<const uchar*>(gC->constData()) : nullptr;
                const uchar *bp = bC ? reinterpret_cast<const uchar*>(bC->constData()) : nullptr;
                const uchar *ap = aC ? reinterpret_cast<const uchar*>(aC->constData()) : nullptr;
                // チャンネルの長さが足りない壊れたファイルでも読み飛ばさないよう、
                // 有効範囲を持っているものだけを使う。
                if (rC && rC->size() < pxCount) rp = nullptr;
                if (gC && gC->size() < pxCount) gp = nullptr;
                if (bC && bC->size() < pxCount) bp = nullptr;
                if (aC && aC->size() < pxCount) ap = nullptr;

                for (qint64 idx = 0; idx < pxCount; idx++) {
                    const quint8 r = rp ? rp[idx] : 0;
                    const quint8 g = (mode == 1) ? r : (gp ? gp[idx] : 0);
                    const quint8 b = (mode == 1) ? r : (bp ? bp[idx] : 0);
                    const quint8 a = ap ? ap[idx] : 255;
                    premulPixel(r, g, b, a, dstBase + idx * 4);
                }
            }
            return addFlattenedNormalLayer(rl.name, rgba, rl.left, rl.top, w, h, rl.clipping,
                                            rl.blendMode, opacity, rl.visible, ancestorFolders);
        };

        // 木を深さ優先(先行順)で辿って実際にレイヤーを追加していく。フォルダーは
        // 常にaddLayer(insertIndex=-1、末尾追加)されるため、この順で辿ればTiepolo
        // 内部の「フォルダー直後にchildCount枚が連続する」という配置が自然に
        // 満たされる(save()側のwriteRangeとちょうど対になる)。
        std::function<void(const PsdTreeNode&, QVector<int>&)> importNode =
            [&](const PsdTreeNode &node, QVector<int> &ancestorFolders) {
            if (node.isFolder) {
                const RawLayer &rl = rawLayers[node.rawIndex];
                gl_->addLayer(rl.name, -1, false, 0, 0, 0, 0, LayerType::Folder, ancestorFolders);
                int idx = gl_->document().layerCount() - 1;
                gl_->document().setLayerVisible(idx, rl.visible);
                applyMaskIfAny(idx, rl);
                if (progressCallback) progressCallback(qMin(progressTotal, ++progressDone), progressTotal);
                ancestorFolders.append(idx);
                for (const PsdTreeNode &child : node.children) importNode(child, ancestorFolders);
                ancestorFolders.removeLast();
            } else {
                const RawLayer &rl = rawLayers[node.rawIndex];
                int idx = importLeaf(rl, ancestorFolders);
                applyMaskIfAny(idx, rl);
                if (progressCallback) progressCallback(qMin(progressTotal, ++progressDone), progressTotal);
            }
        };

        QVector<int> ancestors;
        for (const PsdTreeNode &n : rootNodes) importNode(n, ancestors);
    }

    gl_->endBulkLayerImport();

    if (gl_->document().layerCount() == 0) {
        // 全レイヤーがフォルダ/非対応要素で、実体が1枚も残らなかった場合の保険
        // (通常のaddLayer呼び出しなので、ここはゼロクリア+通知とも通常通り行われる)
        gl_->addLayer(QStringLiteral("レイヤー"));
    }

    // PSDのグループ構造からTiepoloのフォルダーへ変換する過程でchildCountが実際の
    // レイヤー並びと食い違うことがある(矛盾したままだとchildCountでindexを進める
    // 全ての箇所が配列外アクセスになる)。ここで木構造として成立する値へ丸める。
    gl_->document().sanitizeFolderChildCounts();

    gl_->document().setActiveLayer(gl_->document().layerCount() - 1);
    gl_->update();

    return true;
}
