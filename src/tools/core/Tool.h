#pragma once

#include "tools/core/ToolContext.h"
#include <QMouseEvent>
#include <QCursor>
#include <optional>

class QPainter;

class Tool
{
public:
    virtual ~Tool() = default;

    virtual void onMousePress(QMouseEvent *event, ToolContext &ctx)   {}
    virtual void onMouseMove(QMouseEvent *event, ToolContext &ctx)    {}
    virtual void onMouseRelease(QMouseEvent *event, ToolContext &ctx) {}
    virtual void onMouseDoubleClick(QMouseEvent *event, ToolContext &ctx) { Q_UNUSED(event); Q_UNUSED(ctx); }

    // 入力中かどうか。
    virtual bool isActive() const { return false; }

    virtual bool needsCanvasRepaintWhileActive() const { return true; }

    virtual void setPressure(float pressure) { pressure_ = pressure; }

    virtual void setTilt(float amount, float angle, float rotation)
    {
        tiltAmount_  = amount;
        tiltAngle_   = angle;
        penRotation_ = rotation;
    }

    virtual std::optional<QCursor> cursor(const ToolContext &ctx) const { return std::nullopt; }

    // GL描画後のオーバーレイ。
    virtual void paintOverlay(QPainter &painter, const ToolContext &ctx) const { Q_UNUSED(painter); Q_UNUSED(ctx); }

    // 入力バッチをGPUへ送る。
    virtual void flushPendingInput(ToolContext &ctx) { Q_UNUSED(ctx); }

    virtual bool hasPendingInput() const { return false; }

    // キャンバス内容ではなくビューを操作するツールか。
    virtual bool transformsViewWhileActive() const { return false; }

    // 入力をバッチする最大時間(ms)。
    virtual int flushIntervalMs() const { return 0; }

protected:
    float pressure_ = 1.0f;
    float tiltAmount_  = 0.0f;
    float tiltAngle_   = 0.0f;
    float penRotation_ = 0.0f;
};
