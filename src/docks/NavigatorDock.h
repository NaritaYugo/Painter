#pragma once

#include <QWidget>
#include <QImage>
#include <QMetaObject>

class GLWidget;
class QPushButton;
class QSlider;
class QTimer;

// ---------------------------------------------------------------------------
// NavigatorDock
// キャンバス全体 or アクティブレイヤー単体のプレビューを表示する
// ---------------------------------------------------------------------------
class NavigatorDock : public QWidget
{
    Q_OBJECT
public:
    explicit NavigatorDock(GLWidget *gl, QWidget *parent = nullptr);

    void refresh(); // 外部から呼んでプレビューを更新する

    // GLWidget::layersChanged()から呼ぶ想定。フルキャンバス合成+glReadPixelsを伴う
    // refresh()を、連続発火時にまとめて最後の1回だけ実行する(デバウンス)。
    void scheduleRefresh();

    // タブ切替時に、表示対象のGLWidget(=キャンバス)を差し替える
    void setGLWidget(GLWidget *gl);

    static constexpr int BASE_WIDTH  = 200;
    static constexpr int BASE_HEIGHT = 200;

    QSize sizeHint() const override { return QSize(BASE_WIDTH, BASE_HEIGHT); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    GLWidget    *glWidget    = nullptr;
    QMetaObject::Connection viewChangedConn_;
    QTimer      *refreshDebounceTimer_ = nullptr; // scheduleRefresh()用(singleShot、都度restart)
    QPushButton *btnCanvas   = nullptr; // キャンバス全体
    QPushButton *btnLayer    = nullptr; // レイヤー単体
    QSlider     *zoomSlider  = nullptr; // キャンバス表示倍率
    QPushButton *resetButton = nullptr; // サイズ・回転のリセット(キャンバスをフィット)
    QPushButton *flipButton  = nullptr; // 表示上の左右反転トグル
    bool         m_fullCanvas = true;   // true=全体, false=単体

    QImage m_preview;

    // 表示範囲枠のドラッグ操作用
    bool  m_draggingFrame = false;
    QPoint m_lastDragPos;

    // 上のボタン行/下のズームスライダー・反転ボタン行を除いた、プレビューを
    // 描画する領域の縦方向オフセット/高さ(updatePreview()とpaintEvent()の
    // 両方で同じ計算をするので、ここに集約する)。
    int previewTopOffset() const;
    int previewBottomHeight() const;
    // 実際のウィジェット配置(btnCanvas/zoomSliderの実ジオメトリ)を基準に
    // プレビュー領域を求める。previewTopOffset()/previewBottomHeight()の
    // 手計算値がレイアウトの実際の配置とズレて操作部分に重なって見える
    // 問題を避けるため、こちらを正とする。
    QRect previewAreaRect() const;
    QRect previewDstRect() const; // previewAreaRect()内でアスペクト比を保った実際の描画矩形

    void updatePreview();
    void applyButtonStyles();
};
