#pragma once

#include "tools/core/ToneCurveMath.h"

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

// ===========================================================================
// PressureCurve
// ---------------------------------------------------------------------------
// タブレットから来た生の筆圧を、実際に描画へ使う筆圧へ写像するカーブ。
// 横軸=入力(生の筆圧)、縦軸=出力(使う筆圧)で、どちらも 0..255 の曲線座標
// (トーンカーブと同じ形式なので ToneCurveEditor をそのまま編集UIに使える)。
//
// カーブは2段構えで、この順に適用する:
//   1. 環境設定の「全体の筆圧カーブ」(ToolConfig::globalPressureCurve)
//      … 使っているタブレットの硬さの癖をここで一度ならす
//   2. ツール設定の「ツールごとの筆圧カーブ」(PenToolConfig::pressureCurve 等)
//      … そのうえでツール/プリセットごとの効き方を作る
// 実際の合成は GLWidget::mapPressure() が行う。
//
// 評価は256段のLUT+線形補間。制御点のスプライン評価をイベントごとに行うと
// (ペンタブは200Hz前後で届く)毎回 computeTangents() の確保が走るため、
// 制御点が変わったときだけ作り直す。
// ===========================================================================
class PressureCurve
{
public:
    PressureCurve() { setPoints(identityPoints()); }

    // 何もしない(入力=出力)カーブの制御点
    static QVector<QPointF> identityPoints() { return { QPointF(0, 0), QPointF(255, 255) }; }

    const QVector<QPointF> &points() const { return points_; }

    void setPoints(const QVector<QPointF> &pts)
    {
        points_ = (pts.size() >= 2) ? pts : identityPoints();
        ToneCurveMath::buildLut256(points_, lut_);

        // 恒等なら apply() を素通しにできる(タブレット使用時の毎イベント処理なので、
        // 既定状態でわざわざLUTを引かない)。丸め誤差で1ずれることがあるため
        // 完全一致ではなく許容差で見る。
        identity_ = true;
        for (int i = 0; i < 256; i++) {
            if (qAbs(int(lut_[i]) - i) > 1) { identity_ = false; break; }
        }
    }

    void reset() { setPoints(identityPoints()); }

    bool isIdentity() const { return identity_; }

    // 0..1 の筆圧を写像して 0..1 で返す。
    float apply(float pressure01) const
    {
        if (identity_) return pressure01;
        const float x = qBound(0.0f, pressure01, 1.0f) * 255.0f;
        const int   i = int(x);
        const int   j = qMin(i + 1, 255);
        const float f = x - float(i);
        return (float(lut_[i]) * (1.0f - f) + float(lut_[j]) * f) / 255.0f;
    }

    // ---- 永続化 -----------------------------------------------------------
    // "x,y;x,y;..." の文字列にする。QVariantList<QPointF> をそのまま QSettings へ
    // 渡すとバックエンド(レジストリ/ini)によってバイナリ blob になり、中身が
    // 確認できず移植性も落ちるため、素直な文字列にしておく。
    QString toString() const
    {
        QStringList parts;
        parts.reserve(points_.size());
        for (const QPointF &p : points_)
            parts << QStringLiteral("%1,%2").arg(p.x()).arg(p.y());
        return parts.join(QLatin1Char(';'));
    }

    static PressureCurve fromString(const QString &s)
    {
        PressureCurve c;
        if (s.isEmpty()) return c;

        QVector<QPointF> pts;
        const QStringList parts = s.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QStringList xy = part.split(QLatin1Char(','));
            if (xy.size() != 2) continue;
            bool okX = false, okY = false;
            const double x = xy[0].toDouble(&okX);
            const double y = xy[1].toDouble(&okY);
            if (!okX || !okY) continue;
            pts.append(QPointF(qBound(0.0, x, 255.0), qBound(0.0, y, 255.0)));
        }
        // 壊れた文字列を読んでも描けなくならないよう、2点未満なら恒等へ倒す
        if (pts.size() >= 2) c.setPoints(pts);
        return c;
    }

private:
    QVector<QPointF> points_;
    quint8 lut_[256] = {};
    bool   identity_ = true;
};
