#pragma once

// ---------------------------------------------------------------------------
// ツール種別
// ---------------------------------------------------------------------------
// 各値は「どのToolインスタンス(CanvasWidget::currentTool())に処理を委譲するか」を表す。
// 実際の設定値(ブラシサイズ等)はToolTypeそのものではなく、ToolConfig側の
// ツールプリセット一覧(ToolPresetList<T>、ToolConfig::toolPresetList())がToolTypeごとに保持する。
// 新しいツール種別を増やす場合はここに値を足したうえで、
// ToolRegistry.cpp のテーブルと ToolConfig のツールプリセット一覧を1つ追加する。
// ---------------------------------------------------------------------------
enum class ToolType {
    Pen,
    Eraser,
    Fill,
    Dropper,
    Move,
    Rotate,
    Blur,
    Warp,
    Selection,
    Text,
    Airbrush,
};
