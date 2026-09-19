#include "backend/AbrCodec.h"
#include "backend/PsdDescriptor.h"

#include <QHash>
#include <QUuid>
#include <QtMath>

#include <cstring>

// ===========================================================================
// バイト列の読み書き(すべてビッグエンディアン)
// ===========================================================================
namespace {

struct Reader {
    const QByteArray &d;
    int pos = 0;
    bool ok = true;
    explicit Reader(const QByteArray &data) : d(data) {}

    bool need(int n) {
        if (!ok) return false;
        if (pos < 0 || pos + n > d.size()) { ok = false; return false; }
        return true;
    }
    quint8  u8()  { if (!need(1)) return 0; return (quint8)d[pos++]; }
    quint16 u16() { if (!need(2)) return 0; quint16 v = ((quint8)d[pos] << 8) | (quint8)d[pos+1]; pos += 2; return v; }
    qint16  i16() { return (qint16)u16(); }
    quint32 u32() {
        if (!need(4)) return 0;
        quint32 v = ((quint32)(quint8)d[pos] << 24) | ((quint32)(quint8)d[pos+1] << 16)
                  | ((quint32)(quint8)d[pos+2] << 8) | (quint32)(quint8)d[pos+3];
        pos += 4; return v;
    }
    qint32     i32()        { return (qint32)u32(); }
    QByteArray tag4()       { if (!need(4)) return {}; QByteArray t = d.mid(pos, 4); pos += 4; return t; }
    QByteArray bytes(int n) { if (n < 0 || !need(n)) { ok = false; return {}; } QByteArray b = d.mid(pos, n); pos += n; return b; }
};

void putU8 (QByteArray &o, quint8 v)  { o.append((char)v); }
void putU16(QByteArray &o, quint16 v) { o.append((char)(v >> 8)); o.append((char)(v & 0xff)); }
void putU32(QByteArray &o, quint32 v) {
    o.append((char)(v >> 24)); o.append((char)((v >> 16) & 0xff));
    o.append((char)((v >> 8) & 0xff)); o.append((char)(v & 0xff));
}

// ---------------------------------------------------------------------------
// PackBits (RLE)
// ---------------------------------------------------------------------------
// PSDと同じ方式。制御バイト n を符号付きで見て、
//   n >= 0 : 続く n+1 バイトをそのまま
//   n <  0 : 続く1バイトを 1-n 回
//   n == -128 : 何もしない
// PsdCodec.cpp にもデコーダがあるが、あちらは行長が既知の前提で戻り値を返さない
// (ABRでは「消費したバイト数」で行の終わりを追う必要がある)ため別に持つ。
namespace packbits {

// dst へ width バイト展開する。戻り値は消費したソースバイト数。
int decodeRow(const uchar *src, int srcLen, uchar *dst, int width)
{
    int si = 0, di = 0;
    while (di < width && si < srcLen) {
        const int n = (signed char)src[si++];
        if (n >= 0) {
            const int cnt = qMin(n + 1, qMin(width - di, srcLen - si));
            std::memcpy(dst + di, src + si, (size_t)cnt);
            di += cnt; si += cnt;
        } else if (n != -128) {
            if (si >= srcLen) break;
            const int cnt = qMin(1 - n, width - di);
            std::memset(dst + di, src[si++], (size_t)cnt);
            di += cnt;
        }
    }
    if (di < width) std::memset(dst + di, 0, (size_t)(width - di));
    return si;
}

QByteArray encodeRow(const uchar *src, int width)
{
    QByteArray out;
    int i = 0;
    while (i < width) {
        // 同じ値の並びを探す(最大128)
        int run = 1;
        while (i + run < width && src[i + run] == src[i] && run < 128) run++;
        if (run >= 2) {
            out.append((char)(qint8)(1 - run));   // -(run-1)
            out.append((char)src[i]);
            i += run;
            continue;
        }
        // そうでなければ、次に「3個以上の並び」が現れるまでを生データとして出す
        const int start = i;
        int lit = 0;
        while (i < width && lit < 128) {
            if (i + 2 < width && src[i] == src[i + 1] && src[i] == src[i + 2]) break;
            i++; lit++;
        }
        out.append((char)(qint8)(lit - 1));
        out.append((const char *)src + start, lit);
    }
    return out;
}

} // namespace packbits

// ---------------------------------------------------------------------------
// 先端画像(8bit/16bitの濃さ)→ このアプリの先端画像(アルファに濃さを入れたRGBA)
// ---------------------------------------------------------------------------
QImage tipImageFrom(const QByteArray &gray, int w, int h)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    if (img.isNull()) return {};
    for (int y = 0; y < h; y++) {
        uchar *dst = img.scanLine(y);
        const uchar *src = (const uchar *)gray.constData() + (qsizetype)y * w;
        for (int x = 0; x < w; x++) {
            dst[x*4+0] = 0; dst[x*4+1] = 0; dst[x*4+2] = 0; dst[x*4+3] = src[x];
        }
    }
    return img;
}

// 境界(long) + 深度 + 圧縮 + 画素、という並びを pos から読む。
// version 1/2 と 6.x で前置きは違うが、ここから先は共通。
QImage readTipPixels(const QByteArray &d, int pos, int end)
{
    if (pos + 19 > end) return {};
    Reader r(d); r.pos = pos;
    const qint32 top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
    const qint16 depth = r.i16();
    const quint8 compression = r.u8();
    if (!r.ok) return {};

    const int w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return {};
    if (depth != 8 && depth != 16) return {};
    const int bytesPerSample = depth / 8;

    QByteArray gray((qsizetype)w * h, '\0');
    if (compression == 0) {
        const QByteArray px = r.bytes(w * h * bytesPerSample);
        if (!r.ok) return {};
        for (int i = 0; i < w * h; i++)             // 16bitは上位バイトだけ使う
            gray[i] = px[i * bytesPerSample];
    } else if (compression == 1) {
        // 行ごとのバイト数の表(u16 × 高さ)→ 行データ
        QVector<int> rowLen(h);
        for (int y = 0; y < h; y++) rowLen[y] = (int)r.u16();
        if (!r.ok) return {};
        QByteArray row((qsizetype)w * bytesPerSample, '\0');
        for (int y = 0; y < h; y++) {
            if (r.pos + rowLen[y] > end) return {};
            packbits::decodeRow((const uchar *)d.constData() + r.pos, rowLen[y],
                                (uchar *)row.data(), w * bytesPerSample);
            r.pos += rowLen[y];
            for (int x = 0; x < w; x++)
                gray[(qsizetype)y * w + x] = row[x * bytesPerSample];
        }
    } else {
        return {};
    }
    return tipImageFrom(gray, w, h);
}

// ---------------------------------------------------------------------------
// 記述子からの値の取り出し
// ---------------------------------------------------------------------------
bool numberOf(const DescValue *v, double &out)
{
    return v && v->asNumber(out);
}

// 百分率として取り出す(0〜1へ正規化)。Photoshopは UntF '#Prc' で 0〜100
// (散布などは1000まで)として持つ。
bool percentOf(const DescValue *v, double &out)
{
    if (!v) return false;
    double n = 0.0;
    if (!v->asNumber(n)) return false;
    if (v->type == "UntF" && v->unit == "#Prc") n /= 100.0;
    else if (n > 1.0) n /= 100.0;   // 単位が付いていなくても100超なら百分率とみなす
    out = n;
    return true;
}

// 入れ子の動的設定(szVr / opVr / scatterDynamics …)から 'jitter' を百分率で取り出す。
bool jitterOf(const DescValue &brush, const QByteArray &groupKey, double &out)
{
    const DescValue *g = brush.field(groupKey);
    if (!g) return false;
    if (g->type == "Objc") return percentOf(g->field("jitter"), out);
    return percentOf(g, out);       // 直接値が入っている場合もある
}

// 動的設定の「制御」。Photoshopが書くのは brVr/bVTy(long)で、UIのドロップダウンの
// 並び順そのままの番号。実ファイルで確認できたのは 0(オフ)と 2(筆圧)、
// および角度の 6。角度のドロップダウンは
//   0 オフ / 1 フェード / 2 筆圧 / 3 傾き / 4 ホイール / 5 初期進行方向 /
//   6 進行方向 / 7 回転
// なので 6 = 進行方向 とみなす(初期進行方向 5 も、このアプリには区別が無いので
// 同じ扱いにして注記を出す)。古い書き手が 'control' 列挙で書いている場合にも
// 備えて、そちらも見る。
enum class DynControl { Off, PenPressure, Direction, InitialDirection, Other };

DynControl controlOf(const DescValue &brush, const QByteArray &groupKey)
{
    const DescValue *g = brush.field(groupKey);
    if (!g || g->type != "Objc") return DynControl::Off;
    if (const DescValue *c = g->field("control")) {
        if (c->type == "enum") {
            if (c->enumValue == "Drct") return DynControl::Direction;
            if (c->enumValue == "Off ") return DynControl::Off;
        }
    }
    const DescValue *t = g->field("bVTy");
    if (!t || t->type != "long") return DynControl::Off;
    switch (t->i) {
    case 0:  return DynControl::Off;
    case 2:  return DynControl::PenPressure;
    case 5:  return DynControl::InitialDirection;
    case 6:  return DynControl::Direction;
    default: return DynControl::Other;
    }
}

// ---------------------------------------------------------------------------
// 記述子1本ぶん(brushPreset) → AbrBrush
// ---------------------------------------------------------------------------
// 実ファイルでの並びは
//   brushPreset { Nm, Brsh{computedBrush|sampledBrush}, useTipDynamics,
//                 minimumDiameter, minimumRoundness, tiltScale,
//                 szVr, angleDynamics, roundnessDynamics,
//                 useScatter, [Spcn, Cnt, bothAxes, countDynamics, scatterDynamics],
//                 dualBrush, useTexture, usePaintDynamics, prVr, opVr,
//                 useColorDynamics, ... }
// 先端の形(Dmtr/Hrdn/Spcn/Angl/Rndn)は 'Brsh' の入れ子の中にある。
AbrBrush brushFromDescriptor(const DescValue &b, const QHash<QString, QImage> &tips,
                             QStringList &notes)
{
    AbrBrush out;

    if (const DescValue *nm = b.field("Nm  ")) out.name = nm->text;
    if (out.name.isEmpty()) out.name = b.objName;

    // computedBrush = 手続き的な丸ブラシ(硬さを持つ)
    // sampledBrush  = 画像から作った先端。'sampledData' が samp セクションのIDを指す
    const DescValue *tipObj = b.field("Brsh");
    const DescValue &t = (tipObj && tipObj->type == "Objc") ? *tipObj : b;

    double v = 0.0;
    if (numberOf(t.field("Dmtr"), v))  out.diameterPx.set(v);
    if (percentOf(t.field("Hrdn"), v)) out.hardness.set(qBound(0.0, v, 1.0));
    if (percentOf(t.field("Spcn"), v)) out.spacing.set(qMax(0.0, v));
    if (percentOf(t.field("Rndn"), v)) out.roundness.set(qBound(0.0, v, 1.0));
    if (numberOf(t.field("Angl"), v))  out.angleDeg.set(v);

    if (const DescValue *sd = t.field("sampledData")) {
        out.tip = tips.value(sd->text);
        if (out.tip.isNull() && !tips.isEmpty())
            notes << QStringLiteral("先端画像がファイル内に見つかりませんでした");
    }

    // 動的設定。use*** が偽のときはその一群まるごと省かれるので、
    // 「書かれていない」ではなく「オフ」として0を入れる(そうしないと直前の
    // ブラシの値が残ってしまい、同じファイルを読んでも結果が変わる)。
    const auto groupOn = [&b](const QByteArray &key) {
        const DescValue *f = b.field(key);
        return !f || f->b;
    };

    if (groupOn("useTipDynamics")) {
        if (jitterOf(b, "szVr", v))          out.sizeJitter.set(qBound(0.0, v, 1.0));
        if (jitterOf(b, "angleDynamics", v) || jitterOf(b, "angVr", v))
            out.angleJitter.set(qBound(0.0, v, 1.0));
        // 最小サイズはプリセット直下。古い書き手が szVr の中に入れている場合にも備える。
        if (percentOf(b.field("minimumDiameter"), v)) out.minDiameter.set(qBound(0.0, v, 1.0));
        else if (const DescValue *g = b.field("szVr")) {
            if (g->type == "Objc" && percentOf(g->field("minimumDiameter"), v))
                out.minDiameter.set(qBound(0.0, v, 1.0));
        }
        switch (controlOf(b, "angleDynamics")) {
        case DynControl::Direction:
            out.followDirection.set(true);
            break;
        case DynControl::InitialDirection:
            out.followDirection.set(true);
            notes << QStringLiteral("角度の制御「初期進行方向」は「進行方向に追従」として取り込みました");
            break;
        default:
            out.followDirection.set(false);
            break;
        }
    } else {
        out.sizeJitter.set(0.0);
        out.angleJitter.set(0.0);
        out.minDiameter.set(0.0);
        out.followDirection.set(false);
    }

    if (groupOn("usePaintDynamics")) {
        if (jitterOf(b, "opVr", v)) out.opacityJitter.set(qBound(0.0, v, 1.0));
    } else {
        out.opacityJitter.set(0.0);
    }

    if (groupOn("useScatter")) {
        if (jitterOf(b, "scatterDynamics", v) || jitterOf(b, "Scat", v))
            out.scatter.set(qMax(0.0, v));
        if (const DescValue *c = b.field("Cnt ")) {
            double n = 0.0;
            if (c->asNumber(n)) out.count.set(qBound(1, (int)qRound(n), 16));
        }
    } else {
        out.scatter.set(0.0);
        out.count.set(1);
    }

    return out;
}

// ---------------------------------------------------------------------------
// version 1/2: サンプルされた先端画像だけを持つ古い形式
// ---------------------------------------------------------------------------
bool readV1V2(Reader &r, int version, AbrReadResult &res)
{
    const quint16 count = r.u16();
    if (!r.ok) return false;

    for (quint16 i = 0; i < count && r.ok; i++) {
        const qint16 type = r.i16();
        const qint32 size = r.i32();
        if (!r.ok || size < 0) break;
        const int next = r.pos + size;

        if (type == 2) { // sampled brush
            AbrBrush b;
            r.i32();                    // misc
            const qint16 spacing = r.i16();
            if (spacing > 0) b.spacing.set(spacing / 100.0);
            if (version == 2) {         // Unicode名
                const quint32 len = r.u32();
                QByteArray raw = r.bytes((int)len * 2);
                QVector<ushort> u16v((int)len);
                for (quint32 k = 0; k < len; k++)
                    u16v[(int)k] = (ushort)(((quint8)raw[(int)k*2] << 8) | (quint8)raw[(int)k*2+1]);
                b.name = QString::fromUtf16(u16v.constData(), (int)len);
                while (!b.name.isEmpty() && b.name.back() == QChar(0)) b.name.chop(1);
            }
            const quint8 nameLen = r.u8();       // Pascal文字列(v1のみ意味を持つ)
            const QByteArray asciiName = r.bytes(nameLen);
            if (b.name.isEmpty()) b.name = QString::fromLatin1(asciiName);

            r.u8();                              // anti-aliasing
            for (int k = 0; k < 4; k++) r.i16(); // 境界(short)
            b.tip = readTipPixels(r.d, r.pos, next);
            if (b.tip.isNull())
                res.notes << QStringLiteral("先端画像を読み取れませんでした");
            res.brushes.append(b);
        }
        r.pos = next;   // 型が何であれ、宣言された長さぶんだけ進む
    }
    return !res.brushes.isEmpty();
}

// ---------------------------------------------------------------------------
// version 6.x の 'samp' セクション
// ---------------------------------------------------------------------------
// レコードは [長さ(u32)][本体] の並びで、本体は
//   [1バイト長 + 36文字のID][前置き][境界(long)×4][深度][圧縮][画素]
// 前置きまで含めた「境界(long)の開始位置」が 6.1 では47、6.2では301バイト。
//   6.1(47) = ID 37 + 境界(short) 8 + 深度 2
//   6.2(301) = 上記に、大半が0で埋まった264バイトの追加情報が挟まる
// 版が増えて長さが変わっても壊れないよう、想定位置が妥当でなければ前方を走査する。
bool looksLikeTipHeader(const QByteArray &d, int at, int end)
{
    if (at + 19 > end) return false;
    Reader r(d); r.pos = at;
    const qint32 top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
    const qint16 depth = r.i16();
    const quint8 comp = r.u8();
    if (!r.ok) return false;
    const int w = right - left, h = bottom - top;
    return (depth == 8 || depth == 16) && (comp == 0 || comp == 1)
        && w > 0 && h > 0 && w <= 8192 && h <= 8192
        && qAbs(top) <= 8192 && qAbs(left) <= 8192;
}

QHash<QString, QImage> readSamp(const QByteArray &d, int start, int end, int subversion,
                                QStringList &notes)
{
    QHash<QString, QImage> tips;
    int failed = 0;
    int p = start;
    while (p + 4 <= end) {
        Reader r(d); r.pos = p;
        const qint32 len = r.i32();
        if (!r.ok || len <= 0 || r.pos + len > end) break;
        const int bodyStart = r.pos, bodyEnd = r.pos + len;

        const quint8 idLen = r.u8();
        const QString id = QString::fromLatin1(r.bytes(idLen));

        int at = bodyStart + ((subversion >= 2) ? 301 : 47);
        if (!looksLikeTipHeader(d, at, bodyEnd)) {
            at = -1;
            for (int o = 1 + idLen; o + 19 <= qMin(bodyStart + 512, bodyEnd); o++) {
                if (looksLikeTipHeader(d, bodyStart + o, bodyEnd)) { at = bodyStart + o; break; }
            }
        }
        const QImage img = (at >= 0) ? readTipPixels(d, at, bodyEnd) : QImage();
        if (img.isNull()) failed++;
        else if (!id.isEmpty()) tips.insert(id, img);

        p = bodyEnd;
        while (p % 4) p++;      // レコードは4バイト境界へ揃えられている
    }
    if (failed > 0)
        notes << QStringLiteral("先端画像を%1件読み取れませんでした").arg(failed);
    return tips;
}

// ---------------------------------------------------------------------------
// version 6.x: 8BIMセクション列
// ---------------------------------------------------------------------------
bool readV6(Reader &r, AbrReadResult &res)
{
    // セクションの位置をまず全部拾う。'samp' は 'desc' より前に置かれるのが普通だが、
    // 順序に依存しないよう2周に分ける(先端画像を先に読み、記述子から参照する)。
    struct Section { QByteArray key; int start; int len; };
    QVector<Section> sections;
    while (r.ok && r.pos + 12 <= r.d.size()) {
        const QByteArray sig = r.tag4();
        if (sig != "8BIM") break;
        const QByteArray key = r.tag4();
        const qint32 len = r.i32();
        if (!r.ok || len < 0 || r.pos + len > r.d.size()) break;
        sections.append({ key, r.pos, len });

        // セクションはファイル先頭からの4バイト境界へ揃えられている。
        // 揃えない書き手も居るので、次が "8BIM" になる方を採る。
        r.pos += len;
        const auto isSectionStart = [&r](int at) {
            return at + 4 <= r.d.size() && r.d.mid(at, 4) == QByteArray("8BIM", 4);
        };
        int aligned = r.pos;
        while (aligned % 4) aligned++;
        if (!isSectionStart(r.pos) && isSectionStart(aligned)) r.pos = aligned;
    }

    QHash<QString, QImage> tips;
    for (const Section &s : sections)
        if (s.key == "samp")
            tips = readSamp(r.d, s.start, s.start + s.len, res.subversion, res.notes);

    bool sawDesc = false;
    for (const Section &s : sections) {
        if (s.key != "desc") continue;
        int p = s.start + 4;            // 先頭4バイトは descriptor version(16)
        DescValue root;
        if (!PsdDescriptor::read(r.d, p, root)) {
            res.notes << QStringLiteral("設定(desc)セクションを解析できませんでした");
            continue;
        }
        sawDesc = true;
        res.dumpLines = PsdDescriptor::dump(root);
        // ブラシ本体は 'Brsh' の配列。見つからなければルート自身を1本とみなす。
        const DescValue *list = root.field("Brsh");
        if (list && list->type == "VlLs") {
            for (const DescValue &b : list->items)
                if (b.type == "Objc") res.brushes.append(brushFromDescriptor(b, tips, res.notes));
        } else {
            res.brushes.append(brushFromDescriptor(root, tips, res.notes));
        }
    }
    res.notes.removeDuplicates();
    return sawDesc;
}

// ===========================================================================
// 書き出し
// ===========================================================================
// 動的設定の入れ物。実ファイルでは brVr クラスで
//   bVTy(制御) / fStp(フェードの歩数) / jitter(ばらつき)
// が入っている。
DescValue makeBrVr(double jitterPercent, qint32 control = 0)
{
    return DescValue::makeObject("brVr", DescFields{
        { "bVTy",   DescValue::makeLong(control) },
        { "fStp",   DescValue::makeLong(25) },
        { "jitter", DescValue::makeUnit("#Prc", jitterPercent) },
    });
}

void appendSection(QByteArray &out, const char *key, const QByteArray &body)
{
    out.append("8BIM", 4);
    out.append(key, 4);
    putU32(out, (quint32)body.size());
    out.append(body);
    while (out.size() % 4) out.append('\0');   // 次のセクションは4バイト境界から
}

// 先端画像 → 'samp' セクション1本ぶん(version 6.1 の並び)
QByteArray buildSampSection(const QImage &tipRgba, const QString &id)
{
    const QImage img = tipRgba.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width(), h = img.height();

    QByteArray rec;
    const QByteArray idBytes = id.toLatin1();
    putU8(rec, (quint8)idBytes.size());
    rec.append(idBytes);
    // ここまで37バイト。続く short境界 + 深度 の10バイトを足して47バイトが前置き。
    putU16(rec, 0); putU16(rec, 0); putU16(rec, (quint16)h); putU16(rec, (quint16)w);
    putU16(rec, 8);
    putU32(rec, 0); putU32(rec, 0); putU32(rec, (quint32)h); putU32(rec, (quint32)w);
    putU16(rec, 8);
    putU8(rec, 1);                       // 圧縮: PackBits(実ファイルもこちら)

    // 行長の表 → 行データ。表は先に場所を空けておき、後から埋める。
    const int lenTableAt = rec.size();
    rec.append(QByteArray(h * 2, '\0'));
    QByteArray row((qsizetype)w, '\0');
    for (int y = 0; y < h; y++) {
        const uchar *src = img.constScanLine(y);
        for (int x = 0; x < w; x++) row[x] = (char)src[x*4+3];   // アルファ=濃さ
        const QByteArray packed = packbits::encodeRow((const uchar *)row.constData(), w);
        rec[lenTableAt + y*2]     = (char)((packed.size() >> 8) & 0xff);
        rec[lenTableAt + y*2 + 1] = (char)(packed.size() & 0xff);
        rec.append(packed);
    }

    QByteArray body;
    putU32(body, (quint32)rec.size());
    body.append(rec);
    while (body.size() % 4) body.append('\0');
    return body;
}

} // namespace

// ===========================================================================
namespace AbrCodec {

AbrReadResult read(const QByteArray &data)
{
    AbrReadResult res;
    if (data.size() < 4) { res.error = QStringLiteral("ファイルが小さすぎます"); return res; }

    Reader r(data);
    res.version = r.u16();

    if (res.version == 1 || res.version == 2) {
        res.ok = readV1V2(r, res.version, res);
        if (!res.ok) res.error = QStringLiteral("ブラシが1本も見つかりませんでした");
        else res.notes << QStringLiteral("version %1 は先端画像だけの古い形式で、"
                                         "サイズ・硬さなどの設定は入っていません").arg(res.version);
        return res;
    }
    if (res.version == 6 || res.version == 7 || res.version == 10) {
        res.subversion = r.u16();
        res.ok = readV6(r, res);
        if (!res.ok) res.error = QStringLiteral("設定(desc)セクションが見つかりませんでした");
        return res;
    }

    res.error = QStringLiteral("未対応のバージョンです (version=%1)").arg(res.version);
    return res;
}

QByteArray write(const AbrBrush &b)
{
    // 先端画像があれば sampledBrush(画像を samp セクションに入れる)、
    // 無ければ computedBrush(手続き的な丸ブラシ。硬さを持てる)として出す。
    const bool sampled = !b.tip.isNull();
    const QString tipId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const double dmtr    = b.diameterPx.has ? b.diameterPx.v : 25.0;
    const double spcnPct = (b.spacing.has ? b.spacing.v : 0.1) * 100.0;
    const double rndnPct = (b.roundness.has ? b.roundness.v : 1.0) * 100.0;

    DescFields tip;
    tip << DescField("Dmtr", DescValue::makeUnit("#Pxl", dmtr));
    if (!sampled)
        tip << DescField("Hrdn", DescValue::makeUnit("#Prc", (b.hardness.has ? b.hardness.v : 0.5) * 100.0));
    tip << DescField("Angl", DescValue::makeUnit("#Ang", b.angleDeg.has ? b.angleDeg.v : 0.0))
        << DescField("Rndn", DescValue::makeUnit("#Prc", rndnPct));
    if (sampled)
        tip << DescField("Nm  ", DescValue::makeText(b.name));
    tip << DescField("Spcn", DescValue::makeUnit("#Prc", spcnPct))
        << DescField("Intr", DescValue::makeBool(true));
    if (sampled)
        tip << DescField("sampledData", DescValue::makeText(tipId));

    DescFields preset;
    preset << DescField("Nm  ", DescValue::makeText(b.name))
           << DescField("Brsh", DescValue::makeObject(sampled ? "sampledBrush" : "computedBrush", tip));

    // --- 先端のばらつき ---
    const bool followDir = b.followDirection.has && b.followDirection.v;
    const bool useTipDyn = (b.sizeJitter.has  && b.sizeJitter.v  > 0.0)
                        || (b.angleJitter.has && b.angleJitter.v > 0.0)
                        || (b.minDiameter.has && b.minDiameter.v > 0.0)
                        || followDir;
    preset << DescField("useTipDynamics",   DescValue::makeBool(useTipDyn))
           << DescField("minimumDiameter",  DescValue::makeUnit("#Prc", (b.minDiameter.has ? b.minDiameter.v : 0.0) * 100.0))
           << DescField("minimumRoundness", DescValue::makeUnit("#Prc", 100.0))
           << DescField("tiltScale",        DescValue::makeUnit("#Prc", 200.0))
           << DescField("szVr",             makeBrVr((b.sizeJitter.has  ? b.sizeJitter.v  : 0.0) * 100.0))
           // 角度の制御 6 = 進行方向(AbrCodec.h の「キー名について」を参照)
           << DescField("angleDynamics",    makeBrVr((b.angleJitter.has ? b.angleJitter.v : 0.0) * 100.0,
                                                     followDir ? 6 : 0))
           << DescField("roundnessDynamics", makeBrVr(0.0));

    // --- 散布 ---
    // 実ファイルでは useScatter が偽のときこの一群ごと省かれるので、それに倣う。
    const bool useScatter = (b.scatter.has && b.scatter.v > 0.0) || (b.count.has && b.count.v > 1);
    preset << DescField("useScatter", DescValue::makeBool(useScatter));
    if (useScatter) {
        preset << DescField("Spcn",     DescValue::makeUnit("#Prc", 100.0))
               << DescField("Cnt ",     DescValue::makeDouble(b.count.has ? b.count.v : 1))
               << DescField("bothAxes", DescValue::makeBool(true))
               << DescField("countDynamics",   makeBrVr(0.0))
               << DescField("scatterDynamics", makeBrVr((b.scatter.has ? b.scatter.v : 0.0) * 100.0));
    }

    preset << DescField("dualBrush", DescValue::makeObject("dualBrush", DescFields{
                            { "useDualBrush", DescValue::makeBool(false) } }))
           << DescField("useTexture", DescValue::makeBool(false));

    // --- 描画のばらつき ---
    const bool usePaintDyn = b.opacityJitter.has && b.opacityJitter.v > 0.0;
    preset << DescField("usePaintDynamics", DescValue::makeBool(usePaintDyn))
           << DescField("prVr", makeBrVr(0.0))
           << DescField("opVr", makeBrVr((b.opacityJitter.has ? b.opacityJitter.v : 0.0) * 100.0))
           << DescField("useColorDynamics", DescValue::makeBool(false))
           << DescField("Wtdg", DescValue::makeBool(false))
           << DescField("Nose", DescValue::makeBool(false))
           << DescField("Rpt ", DescValue::makeBool(false));

    DescValue root = DescValue::makeObject("null", DescFields{
        { "Brsh", DescValue::makeList({ DescValue::makeObject("brushPreset", preset) }) },
    });

    QByteArray desc;
    putU32(desc, 16);                       // descriptor version
    PsdDescriptor::write(desc, root);

    // --- コンテナ (version 6.1。セクションの並びは実ファイルに合わせる) ---
    QByteArray out;
    putU16(out, 6);   // version
    putU16(out, 1);   // subversion
    if (sampled) appendSection(out, "samp", buildSampSection(b.tip, tipId));
    appendSection(out, "patt", QByteArray());
    appendSection(out, "desc", desc);
    return out;
}

} // namespace AbrCodec
