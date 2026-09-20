#pragma once

#include "components/ThemeColors.h"
#include "document/BlendModeList.h"
#include "document/CanvasDocument.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QRegularExpression>
#include <QSize>
#include <QString>
#include <QVector>
#include <cmath>

// レイヤーを「クリッピングの行」にグルーピングする
QVector<QVector<int>> computeRows(const CanvasDocument &doc, int scope = -1);

// idxが指すレイヤーの直後、かつ(フォルダーなら)その中身をすべて含んだ直後のflatインデックスを返す(「同じ階層で兄弟として続ける」ときの挿入/移動先境界)。
int afterLayerBlock(const CanvasDocument &doc, int idx);

// scope(フォルダーのレイヤーindex、-1ならルート)の中身の終端flatインデックス(該当行が見つからない/空スコープのときのデフォルト挿入位置に使う)。
int scopeEndIndex(const CanvasDocument &doc, int scope);

// prefix + 数字 の形式を持つ既存レイヤー名の中から最大の数字を探し、+1(未使用なら1)を付けて返す。
template <typename Pred>
static QString nextNumberedName(const CanvasDocument &doc, const QString &prefix, Pred pred)
{
    const QRegularExpression re(QRegularExpression::anchoredPattern(
        QRegularExpression::escape(prefix) + "(\\d+)"));
    int maxN = 0;
    for (const Layer &l : doc.layers) {
        if (!pred(l)) continue;
        QRegularExpressionMatch m = re.match(l.name);
        if (m.hasMatch()) maxN = qMax(maxN, m.captured(1).toInt());
    }
    return prefix + QString::number(maxN + 1);
}

QColor blendModeColor(BlendMode mode);

// 5稜の星形ポリゴンをrに内接するよう描く(フォルダー用)。
void drawStarShape(QPainter &p, const QRectF &r);

// インジケーターのドットを、レイヤーの種類に応じた図形で描く
void drawLayerIndicatorShape(QPainter &p, const QRectF &r, LayerType type);

QString blendModeNameJa(BlendMode mode);

QString adjustmentKindNameJa(AdjustmentKind kind);

QString filterKindNameJa(FilterKind kind);

// 単色/テキスト/調整レイヤーは、実際の合成結果(単色レイヤーは常に単色、調整レイヤーは自分自身にはピクセルが無い)よりも、種類が一目で分かる固定のプレビューを出したほうが分かりやすいため、種類ごとに決め打ちの画像を返す。
QPixmap layerTypeThumbnail(const Layer &layer, const QSize &size);

// 透過部分が分かるよう市松模様の背景に画像を重ねたサムネイルを作る。
QPixmap checkeredThumbnail(const QImage &img, const QSize &size);
