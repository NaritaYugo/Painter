#include "io/PsdDescriptor.h"

#include <QStringList>
#include <cstring>

// ===========================================================================
// バイト列の読み書き(すべてビッグエンディアン。Photoshopの流儀)
// ===========================================================================
namespace {

struct Reader {
    const QByteArray &d;
    int pos = 0;
    bool ok = true;

    explicit Reader(const QByteArray &data, int start) : d(data), pos(start) {}

    bool need(int n) {
        if (!ok) return false;
        if (pos < 0 || pos + n > d.size()) { ok = false; return false; }
        return true;
    }
    quint8  u8()  { if (!need(1)) return 0; return (quint8)d[pos++]; }
    quint16 u16() { if (!need(2)) return 0; quint16 v = ((quint8)d[pos] << 8) | (quint8)d[pos+1]; pos += 2; return v; }
    quint32 u32() {
        if (!need(4)) return 0;
        quint32 v = ((quint32)(quint8)d[pos] << 24) | ((quint32)(quint8)d[pos+1] << 16)
                  | ((quint32)(quint8)d[pos+2] << 8) | (quint32)(quint8)d[pos+3];
        pos += 4; return v;
    }
    qint32  i32() { return (qint32)u32(); }
    double  f64() {
        if (!need(8)) return 0.0;
        quint64 v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | (quint8)d[pos + i];
        pos += 8;
        double out; std::memcpy(&out, &v, 8); return out;
    }
    QByteArray tag4() { if (!need(4)) return {}; QByteArray t = d.mid(pos, 4); pos += 4; return t; }
    QByteArray bytes(int n) { if (n < 0 || !need(n)) { ok = false; return {}; } QByteArray b = d.mid(pos, n); pos += n; return b; }
};

void putU8 (QByteArray &o, quint8 v)  { o.append((char)v); }
void putU16(QByteArray &o, quint16 v) { o.append((char)(v >> 8)); o.append((char)(v & 0xff)); }
void putU32(QByteArray &o, quint32 v) {
    o.append((char)(v >> 24)); o.append((char)((v >> 16) & 0xff));
    o.append((char)((v >> 8) & 0xff)); o.append((char)(v & 0xff));
}
void putF64(QByteArray &o, double v) {
    quint64 bits; std::memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; i--) o.append((char)((bits >> (i * 8)) & 0xff));
}
void putTag(QByteArray &o, const char *t) { o.append(t, 4); }

// Unicode文字列: 長さ(文字数, u32) + UTF-16BE。Photoshopは終端のNULも長さに含める。
bool readUnicode(Reader &r, QString &out) {
    quint32 len = r.u32();
    if (!r.ok) return false;
    QByteArray raw = r.bytes((int)len * 2);
    if (!r.ok) return false;
    QVector<ushort> u16((int)len);
    for (quint32 i = 0; i < len; i++)
        u16[(int)i] = (ushort)(((quint8)raw[(int)i * 2] << 8) | (quint8)raw[(int)i * 2 + 1]);
    out = QString::fromUtf16(u16.constData(), (int)len);
    while (!out.isEmpty() && out.back() == QChar(0)) out.chop(1);
    return true;
}
void writeUnicode(QByteArray &o, const QString &s) {
    putU32(o, (quint32)(s.size() + 1)); // 終端NUL込み
    for (QChar ch : s) putU16(o, ch.unicode());
    putU16(o, 0);
}

// キー/クラスID: 長さ(u32)が0なら4文字コード、そうでなければその長さのASCII。
bool readId(Reader &r, QByteArray &out) {
    quint32 len = r.u32();
    if (!r.ok) return false;
    out = (len == 0) ? r.tag4() : r.bytes((int)len);
    return r.ok;
}
void writeId(QByteArray &o, const QByteArray &id) {
    if (id.size() == 4) { putU32(o, 0); o.append(id); }
    else { putU32(o, (quint32)id.size()); o.append(id); }
}

bool readValue(Reader &r, const QByteArray &type, DescValue &out);
bool readBody(Reader &r, DescValue &out);

// 参照(obj )は読み飛ばすだけ。ABRのブラシ設定には出てこないが、
// 未知のファイルで詰まらないように構造だけ辿れるようにしておく。
bool skipReference(Reader &r) {
    quint32 count = r.u32();
    if (!r.ok) return false;
    for (quint32 i = 0; i < count && r.ok; i++) {
        QByteArray t = r.tag4();
        if (!r.ok) return false;
        QByteArray id; QString nm;
        if (t == "Clss" || t == "type" || t == "GlbC") { if (!readId(r, id)) return false; }
        else if (t == "Enmr") { if (!readId(r, id) || !readId(r, id)) return false; }
        else if (t == "Idnt" || t == "indx" || t == "rele") { r.u32(); }
        else if (t == "name") { if (!readUnicode(r, nm) || !readId(r, id)) return false; }
        else if (t == "prop") { if (!readId(r, id) || !readUnicode(r, nm) || !readId(r, id)) return false; }
        else return false;
    }
    return r.ok;
}

bool readValue(Reader &r, const QByteArray &type, DescValue &out)
{
    out = DescValue();
    out.type = type;
    if (type == "Objc" || type == "GlbO") return readBody(r, out);
    if (type == "VlLs") {
        quint32 count = r.u32();
        if (!r.ok) return false;
        for (quint32 i = 0; i < count && r.ok; i++) {
            QByteArray it = r.tag4();
            if (!r.ok) return false;
            DescValue v;
            if (!readValue(r, it, v)) return false;
            out.items.append(v);
        }
        return r.ok;
    }
    if (type == "doub") { out.d = r.f64(); return r.ok; }
    if (type == "UntF") { out.unit = r.tag4(); out.d = r.f64(); return r.ok; }
    if (type == "TEXT") { return readUnicode(r, out.text); }
    if (type == "enum") { return readId(r, out.enumType) && readId(r, out.enumValue); }
    if (type == "long") { out.i = r.i32(); return r.ok; }
    if (type == "bool") { out.b = (r.u8() != 0); return r.ok; }
    if (type == "comp") { out.raw = r.bytes(8); return r.ok; }
    if (type == "type" || type == "GlbC") { return readId(r, out.classId); }
    if (type == "alis" || type == "tdta") {
        quint32 len = r.u32();
        if (!r.ok) return false;
        out.raw = r.bytes((int)len);
        return r.ok;
    }
    if (type == "obj ") return skipReference(r);
    return false; // 未知の型。ここで諦める(以降の位置が読めなくなるため)
}

bool readBody(Reader &r, DescValue &out)
{
    out.type = "Objc";
    if (!readUnicode(r, out.objName)) return false;
    if (!readId(r, out.classId))      return false;
    quint32 count = r.u32();
    if (!r.ok) return false;
    for (quint32 i = 0; i < count && r.ok; i++) {
        QByteArray key;
        if (!readId(r, key)) return false;
        QByteArray type = r.tag4();
        if (!r.ok) return false;
        DescValue v;
        if (!readValue(r, type, v)) return false;
        out.fields.append({ key, v });
    }
    return r.ok;
}

void writeValue(QByteArray &o, const DescValue &v);

void writeBody(QByteArray &o, const DescValue &objc)
{
    writeUnicode(o, objc.objName);
    writeId(o, objc.classId.isEmpty() ? QByteArray("null") : objc.classId);
    putU32(o, (quint32)objc.fields.size());
    for (const DescField &f : objc.fields) {
        writeId(o, f.first);
        o.append(f.second.type.isEmpty() ? QByteArray("long") : f.second.type);
        writeValue(o, f.second);
    }
}

void writeValue(QByteArray &o, const DescValue &v)
{
    const QByteArray &t = v.type;
    if (t == "Objc" || t == "GlbO") { writeBody(o, v); return; }
    if (t == "VlLs") {
        putU32(o, (quint32)v.items.size());
        for (const DescValue &it : v.items) { o.append(it.type); writeValue(o, it); }
        return;
    }
    if (t == "doub") { putF64(o, v.d); return; }
    if (t == "UntF") { o.append(v.unit.isEmpty() ? QByteArray("#Prc") : v.unit); putF64(o, v.d); return; }
    if (t == "TEXT") { writeUnicode(o, v.text); return; }
    if (t == "enum") { writeId(o, v.enumType); writeId(o, v.enumValue); return; }
    if (t == "long") { putU32(o, (quint32)v.i); return; }
    if (t == "bool") { putU8(o, v.b ? 1 : 0); return; }
    if (t == "type" || t == "GlbC") { writeId(o, v.classId); return; }
    if (t == "alis" || t == "tdta") { putU32(o, (quint32)v.raw.size()); o.append(v.raw); return; }
    if (t == "comp") { o.append(v.raw.leftJustified(8, '\0')); return; }
    // ここに来るのは書き手側の組み立てミス。longの0として出しておく
    putU32(o, 0);
}

} // namespace

// ===========================================================================
const DescValue *DescValue::field(const QByteArray &key) const
{
    for (const DescField &f : fields)
        if (f.first == key) return &f.second;
    return nullptr;
}

bool DescValue::asNumber(double &out) const
{
    if (type == "doub" || type == "UntF") { out = d; return true; }
    if (type == "long") { out = (double)i; return true; }
    return false;
}

namespace PsdDescriptor {

bool read(const QByteArray &data, int &pos, DescValue &out)
{
    Reader r(data, pos);
    if (!readBody(r, out)) return false;
    pos = r.pos;
    return true;
}

void write(QByteArray &out, const DescValue &objc)
{
    writeBody(out, objc);
}

QStringList dump(const DescValue &v, int indent)
{
    const QString pad(indent * 2, QChar(' '));
    QStringList lines;
    if (v.type == "Objc" || v.type == "GlbO") {
        lines << pad + QStringLiteral("Objc <%1>").arg(QString::fromLatin1(v.classId));
        for (const DescField &f : v.fields) {
            const QString key = pad + QStringLiteral("  '%1' ").arg(QString::fromLatin1(f.first));
            const DescValue &c = f.second;
            if (c.type == "Objc" || c.type == "GlbO" || c.type == "VlLs") {
                lines << key;
                lines << dump(c, indent + 2);
            } else {
                QString s;
                if      (c.type == "long") s = QString::number(c.i);
                else if (c.type == "doub") s = QString::number(c.d);
                else if (c.type == "UntF") s = QStringLiteral("%1 [%2]").arg(c.d).arg(QString::fromLatin1(c.unit));
                else if (c.type == "bool") s = c.b ? "true" : "false";
                else if (c.type == "TEXT") s = QStringLiteral("\"%1\"").arg(c.text);
                else if (c.type == "enum") s = QStringLiteral("%1.%2").arg(QString::fromLatin1(c.enumType),
                                                                          QString::fromLatin1(c.enumValue));
                else                        s = QStringLiteral("<%1 %2bytes>").arg(QString::fromLatin1(c.type)).arg(c.raw.size());
                lines << key + QStringLiteral(": %1 (%2)").arg(s, QString::fromLatin1(c.type));
            }
        }
    } else if (v.type == "VlLs") {
        lines << pad + QStringLiteral("VlLs [%1]").arg(v.items.size());
        for (const DescValue &it : v.items) lines << dump(it, indent + 1);
    } else {
        lines << pad + QStringLiteral("<%1>").arg(QString::fromLatin1(v.type));
    }
    return lines;
}

} // namespace PsdDescriptor
