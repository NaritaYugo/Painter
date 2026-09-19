#pragma once
#include <QVector>
#include <QPointF>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// ToneCurveMath
// ---------------------------------------------------------------------------
// トーンカーブの制御点列を3次エルミートスプラインで補間する共通ロジック。
// dialogs/ToneCurveEditor.cpp(プレビュー曲線の描画)・tools/actions/ToneCurveTool.cpp
// (破壊的フィルターのLUT構築)・調整レイヤーのLUT構築(GLWidget)の3箇所で全く
// 同じ補間が必要なため、ここへ集約する(以前は前2箇所にそれぞれ個別実装があり、
// 区間幅を無視した「一様」Catmull-Romのバグ(節点で折れ曲がる)が生じていた。
// 詳細はcomputeTangents/evalCurveYのコメント参照)。
// ---------------------------------------------------------------------------
namespace ToneCurveMath {

// 各節点における接線の傾き(dy/dx)を求める。端点は片側差分、内側は両側の傾きの
// 平均(x間隔が均一なら標準的なCatmull-Romの(p[i+1]-p[i-1])/2に一致する一般化)。
inline QVector<double> computeTangents(const QVector<QPointF> &points)
{
    const int n = points.size();
    QVector<double> m(n, 0.0);
    for (int i = 0; i < n; i++) {
        double left = 0.0, right = 0.0;
        bool hasLeft = false, hasRight = false;
        if (i > 0) {
            const double dx = points[i].x() - points[i - 1].x();
            if (dx > 0.0001) { left = (points[i].y() - points[i - 1].y()) / dx; hasLeft = true; }
        }
        if (i < n - 1) {
            const double dx = points[i + 1].x() - points[i].x();
            if (dx > 0.0001) { right = (points[i + 1].y() - points[i].y()) / dx; hasRight = true; }
        }
        if (hasLeft && hasRight) m[i] = 0.5 * (left + right);
        else if (hasLeft)        m[i] = left;
        else if (hasRight)       m[i] = right;
    }
    return m;
}

// 制御点間を3次エルミートスプラインで補間し、曲線座標(x,yともに0..255)を返す。
// 接線はcomputeTangentsで求めたもの(区間の実際の幅dxで正規化して使う)。
// 旧実装(区間の幅を無視した一様Catmull-Rom)は、新しい節点をクリックで追加すると
// 片側の区間だけ極端に狭く/広くなることが普通に起こるため、その節点での接線の
// 大きさが実際の区間幅に対して合わず、節点のところで曲線が折れ曲がって見える
// 不具合があった(3次エルミート化により、各区間の接線をdx倍してから使うので
// 区間幅が不揃いでも正しく滑らかになる)。
inline double evalCurveY(const QVector<QPointF> &points, const QVector<double> &tangents, double fx)
{
    const int n = points.size();
    int seg = 0;
    if (fx <= points.front().x()) seg = 0;
    else if (fx >= points.back().x()) seg = n - 2;
    else {
        for (int i = 0; i < n - 1; i++) {
            if (fx >= points[i].x() && fx <= points[i + 1].x()) { seg = i; break; }
        }
    }
    seg = std::clamp(seg, 0, std::max(0, n - 2));

    const QPointF &p0 = points[seg], &p1 = points[seg + 1];
    const double dx = p1.x() - p0.x();
    double t = dx > 0.0001 ? (fx - p0.x()) / dx : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double t2 = t * t, t3 = t2 * t;

    const double h00 =  2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 =        t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 =        t3 -       t2;

    double y = h00 * p0.y() + h10 * dx * tangents[seg]
             + h01 * p1.y() + h11 * dx * tangents[seg + 1];
    return std::clamp(y, 0.0, 255.0);
}

// 256段階(0-255入力)のLUTを一括構築する(quint8出力、0..255)。
inline void buildLut256(const QVector<QPointF> &points, quint8 *outLut /* [256] */)
{
    const QVector<double> tangents = computeTangents(points);
    for (int x = 0; x < 256; x++) {
        const double y = evalCurveY(points, tangents, (double)x);
        outLut[x] = (quint8)std::lround(y);
    }
}

} // namespace ToneCurveMath
