#pragma once

#include <QImage>
#include <QMouseEvent>
#include <QWidget>

// MaskOpacityPreview ― 不透明度スライダーを置き換える統合コントロールレイヤーマスクは要するに「場所ごとに違う不透明度」なので、一律の不透明度とレイヤーマスクを1つのプレビュー枠にまとめる。
class MaskOpacityPreview : public QWidget
{
    Q_OBJECT
public:
    explicit MaskOpacityPreview(QWidget *parent = nullptr);

    void setState(bool enabled, float opacity, bool hasMask, const QImage &maskThumb);

signals:
    void opacityChanged(float value); // 左右ドラッグ中(0..1)
    void clicked();                   // ドラッグを伴わないクリック

protected:
    void paintEvent(QPaintEvent *) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    bool   enabled_ = false;
    float  opacity_ = 1.0f;
    bool   hasMask_ = false;
    QImage maskThumb_;
    bool   pressed_ = false;
    bool   dragging_ = false;
    QPoint pressPos_;
};
