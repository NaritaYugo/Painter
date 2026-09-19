#pragma once

#include "tools/core/ToolConfig.h"

#include <QWidget>
#include <QColor>

class ColorWheelWidget;

// ===========================================================================
// ColorCircleDock
//
// OKLCHベースのカラーサークル(ColorWheelWidget)をドックとして埋め込むだけの
// 薄いラッパー。カラーサークル本体の実装はColorWheelWidgetに一元化されており
// (単色レイヤーの色ポップアップ等、ドック以外からも同じ実装を再利用できる)、
// ここでは公開APIをそのまま委譲する。
// ===========================================================================
class ColorCircleDock : public QWidget
{
    Q_OBJECT

public:
    explicit ColorCircleDock(QWidget *parent = nullptr);

    void   setColor(const QColor &color);
    QColor color() const;
    void   pickColor(const QColor &color);

    // 「透明色」(塗るのではなく消す色)の選択状態。setTransparent()はシグナルを出さない。
    bool isTransparent() const;
    void setTransparent(bool on);

    void setColorMode(ColorMode m);
    void setCalibration(int brightness, int contrast, int cyan, int magenta, int yellow);

signals:
    void colorChanged(const QColor &color);
    void transparentChanged(bool on);

private:
    ColorWheelWidget *wheel_ = nullptr;
};
