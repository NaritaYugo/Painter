#pragma once

#include <QMatrix4x4>
#include <QVector2D>

// ---------------------------------------------------------------------------
// ViewTransform
//
// キャンバスのビュー変換 (移動・拡大縮小・回転) を管理する。
// GLWidget とは独立して、純粋にデータと行列計算のみを担う。
// ---------------------------------------------------------------------------
class ViewTransform
{
public:
    ViewTransform() = default;

    // ------------------------------------------------------------------
    // パラメータ操作
    // ------------------------------------------------------------------
    void  setOffset(const QVector2D &offset)  { offset_ = offset; }
    void  setScale(float scale)               { scale_  = qMax(0.02f, scale); }
    void  setRotation(float radians)          { rotation_ = radians; }
    void  setFlipX(bool flip)                 { flipX_ = flip; }

    QVector2D offset()   const { return offset_; }
    float     scale()    const { return scale_;  }
    float     rotation() const { return rotation_; }
    bool      flipX()    const { return flipX_; }

    // 移動 (差分を加算)
    void pan(const QVector2D &delta)          { offset_ += delta; }

    // 画面上で見て center 周りに screenDelta だけ回す(呼び出し側はマウスの動きから
    // 「画面上でどれだけ回したいか」を渡す)。
    //
    // matrix()は T(offset) * S(±scale,scale) * R(rotation) の順で合成されている。
    // 左右反転中は S に -1 が入るため、S*R(d)*S⁻¹ = R(-d) となり、rotation_ を
    // +d 動かすと画面上では逆向きに d 回る。よって:
    //   ・rotation_  は反転中だけ符号を反転させて足す(画像がマウスに追従するように)
    //   ・offset_    は画面座標なので、常に「画面上の回転量」で回す
    // この2つを同じ符号にしてしまうと、反転中に画像の回転と原点の移動が食い違い、
    // キャンバスが center を軸に回らず画面外へ飛んでいく。
    void rotate(const QVector2D &center, float screenDelta) {
        // center周りにoffsetを回転させる
        float c = qCos(screenDelta);
        float s = qSin(screenDelta);

        QVector2D rel = offset_ - center;
        offset_ = QVector2D(
            rel.x() * c - rel.y() * s,
            rel.x() * s + rel.y() * c
        ) + center;

        rotation_ += flipX_ ? -screenDelta : screenDelta;
    }

    // マウス位置を中心としたズーム
    void zoomAround(const QVector2D &center, float factor) {
        offset_ = center + (offset_ - center) * factor;
        scale_ *= factor;
        scale_  = qMax(0.02f, scale_);
    }

    // ------------------------------------------------------------------
    // 行列
    // ------------------------------------------------------------------
    // スクリーン座標系 → キャンバス座標系 への変換行列
    QMatrix4x4 matrix() const {
        QMatrix4x4 m;
        m.translate(offset_.x(), offset_.y());
        // 表示上の左右反転(データそのものは変更しない)。x方向のスケールだけ符号を
        // 反転させる。widgetToCanvas()はこの行列のinverted()を使うため、マウス
        // 座標→キャンバス座標の変換も自動的にこの反転を考慮した形になる。
        m.scale(flipX_ ? -scale_ : scale_, scale_, 1.0f);
        m.rotate(qRadiansToDegrees(rotation_), 0, 0, 1);
        return m;
    }

    QMatrix4x4 inverseMatrix() const {
        return matrix().inverted();
    }

    // ------------------------------------------------------------------
    // 座標変換
    // ------------------------------------------------------------------
    // ウィジェット座標 (Y下向き) → キャンバスピクセル座標 (Y上向き)
    QVector2D widgetToCanvas(const QPointF &widgetPos, int widgetHeight) const {
        QMatrix4x4 inv = inverseMatrix();
        QVector4D p = inv * QVector4D(
            static_cast<float>(widgetPos.x()),
            static_cast<float>(widgetHeight) - static_cast<float>(widgetPos.y()),
            0.0f, 1.0f
        );
        return QVector2D(p.x(), p.y());
    }

private:
    QVector2D offset_   = { 0.0f, 0.0f };
    float     scale_    = 1.0f;
    float     rotation_ = 0.0f;
    bool      flipX_    = false;
};