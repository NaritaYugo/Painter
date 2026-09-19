#include "docks/ColorCircleDock.h"
#include "components/ColorWheelWidget.h"

#include <QVBoxLayout>

ColorCircleDock::ColorCircleDock(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    wheel_ = new ColorWheelWidget(this);
    // ドックのカラーサークルは描画色を選ぶものなので「透明色」も選べる
    // (単色レイヤーの色ポップアップ等、消す概念が無い用途では出さない)。
    wheel_->setTransparentSelectable(true);
    layout->addWidget(wheel_);

    connect(wheel_, &ColorWheelWidget::colorChanged, this, &ColorCircleDock::colorChanged);
    connect(wheel_, &ColorWheelWidget::transparentChanged, this, &ColorCircleDock::transparentChanged);
}

void ColorCircleDock::setColor(const QColor &color) { wheel_->setColor(color); }
QColor ColorCircleDock::color() const { return wheel_->color(); }
void ColorCircleDock::pickColor(const QColor &color) { wheel_->pickColor(color); }
bool ColorCircleDock::isTransparent() const { return wheel_->isTransparent(); }
void ColorCircleDock::setTransparent(bool on) { wheel_->setTransparent(on); }
void ColorCircleDock::setColorMode(ColorMode m) { wheel_->setColorMode(m); }

void ColorCircleDock::setCalibration(int brightness, int contrast, int cyan, int magenta, int yellow)
{
    wheel_->setCalibration(brightness, contrast, cyan, magenta, yellow);
}
