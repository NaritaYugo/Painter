#include "app/MainWindow.h"
#include "canvas/CanvasWidget.h"
#include "docks/BrushSizeDock.h"
#include "docks/ToolDock.h"
#include "dialogs/ShortcutsDialog.h"

#include <QAction>
#include <QSettings>
#include <QFile>
#include <QMessageBox>
#include <QDeskTopServices>
#include <QUrl>

// ===========================================================================
// MainWindow::setupActions()
// ---------------------------------------------------------------------------
// アプリの全QActionをここで生成する。1エントリ = 「キー」+「振る舞い(connect)」を
// 並べて書くだけになっていて、MainWindow本体(MainWindow.cpp)には一切はみ出さない。
//
// 新しいショートカット/アクションを増やしたいときは、このファイルに
// registerCommand()/registerTool() を1ブロック足すだけでよい
// (QActionの生成・shortcutの反映・設定の保存/読込・ダイアログへの表示・
//  キー競合チェックまで ShortcutRegistry が自動的に面倒を見る)。
//
// キーを割り当てたくないアクションは defaultKey に 0 を渡す
// (ショートカットキー設定ダイアログには表示され、ユーザーが後から
//  割り当てることもできる)。
// ===========================================================================
void MainWindow::setupActions()
{
    auto &sc = shortcuts_;

    // ---- ツール切り替え(短押し=切り替えて維持、長押し=離すと元のツールに戻る) ----
    sc.registerTool(ToolType::Pen,     "tool_pen",     "ペン",           "ツール", Qt::Key_Q);
    sc.registerTool(ToolType::Eraser,  "tool_eraser",  "消しゴム",       "ツール", Qt::Key_E);
    sc.registerTool(ToolType::Fill,    "tool_fill",    "塗りつぶし",     "ツール", Qt::Key_F);
    sc.registerTool(ToolType::Dropper, "tool_dropper", "スポイト",       "ツール", Qt::Key_D);
    sc.registerTool(ToolType::Move,    "tool_move",    "キャンバス移動", "ツール", Qt::Key_Space);
    sc.registerTool(ToolType::Rotate,  "tool_rotate",  "キャンバス回転", "ツール", Qt::Key_R);
    sc.registerTool(ToolType::Blur,    "tool_blur",    "ぼかし",         "ツール", Qt::Key_B);
    sc.registerTool(ToolType::Warp,    "tool_warp",    "ゆがみ",         "ツール", Qt::Key_W);
    sc.registerTool(ToolType::Selection, "tool_selection", "選択",       "ツール", Qt::Key_S);
    sc.registerTool(ToolType::Text,    "tool_text",    "テキスト",       "ツール", Qt::Key_T);
    sc.registerTool(ToolType::Airbrush, "tool_airbrush", "エアブラシ",   "ツール", Qt::Key_A);

    // ---- ファイル -----------------------------------------------------------
    newTabAction = sc.registerCommand(this, "cmd_new_tab", "タブを追加", "ファイル",
        Qt::ControlModifier | Qt::Key_T,
        [this] { addNewTab(); });

    newCanvasAction = sc.registerCommand(this, "cmd_new", "新規...", "ファイル",
        Qt::ControlModifier | Qt::Key_N,
        [this] { openNewCanvasDialog(); });

    loadAction = sc.registerCommand(this, "cmd_load", "開く...", "ファイル",
        Qt::ControlModifier | Qt::Key_O,
        [this] { loadFile(); });

    saveAction = sc.registerCommand(this, "cmd_save", "保存", "ファイル",
        Qt::ControlModifier | Qt::Key_S,
        [this] { saveFile(); });

    saveAsAction = sc.registerCommand(this, "cmd_save_as", "別名で保存...", "ファイル",
        Qt::ControlModifier | Qt::AltModifier | Qt::Key_S,
        [this] { saveFileAs(); });

    exportAction = sc.registerCommand(this, "cmd_export", "画像を書き出し...", "ファイル",
        Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_S,
        [this] { exportImage(); });

    addImageLayerAction = sc.registerCommand(this, "cmd_add_image_layer", "画像を追加...", "ファイル",
        Qt::ControlModifier | Qt::Key_I,
        [this] { addImageLayer(); });

    settingsAction = sc.registerCommand(this, "cmd_settings", "環境設定...", "ファイル",
        0,
        [this] { openSettingsDialog(); });

    shortcutsAction = sc.registerCommand(this, "cmd_shortcuts", "ショートカットキー設定...", "ファイル",
        0,
        [this] {
            ShortcutsDialog dialog(shortcuts_, toolCfg, this);
            dialog.exec(); // 反映・保存はダイアログのOKハンドラ内で即座に行われる
            toolDock->refreshTooltips(); // ツールドックの「ペン(Q)」等の表示も更新する
        });

    // ---- 編集 -----------------------------------------------------------
    cutAction = sc.registerCommand(this, "cmd_cut", "切り取り", "編集",
        Qt::ControlModifier | Qt::Key_X,
        [this] { glWidget->cutSelection(); });

    fitAction = sc.registerCommand(this, "cmd_fit", "キャンバスをフィット", "編集",
        Qt::ControlModifier | Qt::Key_F,
        [this] { glWidget->fitCanvasToView(); });

    // 現在の表示倍率に対して10%拡大/縮小する(見た目だけの表示倍率で、
    // キャンバスの実データは変更しない)。
    zoomInAction = sc.registerCommand(this, "cmd_zoom_in", "キャンバスを拡大", "編集",
        Qt::ShiftModifier | Qt::Key_E,
        [this] { glWidget->zoomStep(1.1f); });

    zoomOutAction = sc.registerCommand(this, "cmd_zoom_out", "キャンバスを縮小", "編集",
        Qt::ShiftModifier | Qt::Key_Q,
        [this] { glWidget->zoomStep(1.0f / 1.1f); });

    // 表示上の左右反転(見た目だけで、キャンバスの実データは変更しない。
    // ナビゲータードックの反転トグルボタンと同じ)。
    viewFlipXAction = sc.registerCommand(this, "cmd_view_flip_x", "表示を左右反転", "編集",
        Qt::Key_Tab,
        [this] { glWidget->setFlippedX(!glWidget->isFlippedX()); });

    // アクティブレイヤーの実ピクセル内容を1pxずつ平行移動する(拡大・縮小・回転/
    // 自由変形アクション実行中はそのプレビューの平行移動として扱う)。
    nudgeUpAction = sc.registerCommand(this, "cmd_nudge_up", "上へ移動", "編集",
        Qt::ShiftModifier | Qt::Key_W,
        [this] { glWidget->nudgeActiveContent(0, 1); });

    nudgeLeftAction = sc.registerCommand(this, "cmd_nudge_left", "左へ移動", "編集",
        Qt::ShiftModifier | Qt::Key_A,
        [this] { glWidget->nudgeActiveContent(-1, 0); });

    nudgeDownAction = sc.registerCommand(this, "cmd_nudge_down", "下へ移動", "編集",
        Qt::ShiftModifier | Qt::Key_S,
        [this] { glWidget->nudgeActiveContent(0, -1); });

    nudgeRightAction = sc.registerCommand(this, "cmd_nudge_right", "右へ移動", "編集",
        Qt::ShiftModifier | Qt::Key_D,
        [this] { glWidget->nudgeActiveContent(1, 0); });

    undoAction = sc.registerCommand(this, "cmd_undo", "元に戻す", "編集",
        Qt::ControlModifier | Qt::Key_Z,
        [this] { glWidget->undo(); });

    redoAction = sc.registerCommand(this, "cmd_redo", "やり直す", "編集",
        Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Z,
        [this] { glWidget->redo(); });

    copyAction = sc.registerCommand(this, "cmd_copy", "コピー", "編集",
        Qt::ControlModifier | Qt::Key_C,
        [this] { glWidget->copySelection(); });

    pasteAction = sc.registerCommand(this, "cmd_paste", "ペースト", "編集",
        Qt::ControlModifier | Qt::Key_V,
        [this] { glWidget->pasteClipboard(); });

    selectAllAction = sc.registerCommand(this, "cmd_select_all", "全選択", "編集",
        Qt::ControlModifier | Qt::Key_A,
        [this] { glWidget->selectAll(); });

    clearSelectionAction = sc.registerCommand(this, "cmd_clear_selection", "選択を解除", "編集",
        Qt::Key_Escape,
        [this] {
            // 変形アクション実行中はEscapeをキャンセルとして優先する
            // (同じキーで複数のアクションを兼ねるため、選択解除より変形キャンセルを優先する。
            // 拡大・縮小・回転と自由変形は排他なので同時に両方アクティブになることはない)
            if (glWidget->isTransformActionActive()) glWidget->cancelTransformAction();
            else if (glWidget->isFreeTransformActionActive()) glWidget->cancelFreeTransformAction();
            else if (glWidget->isCanvasSizeActionActive()) glWidget->cancelCanvasSizeAction();
            else if (glWidget->isImageResolutionActionActive()) glWidget->cancelImageResolutionAction();
            else if (glWidget->isAnyCanvasActionActive()) glWidget->cancelActiveAction(); // 色調整/フィルター系
            else glWidget->clearSelection();
        });

    transformAction = sc.registerCommand(this, "cmd_transform", "拡大・縮小・回転", "編集",
        Qt::ControlModifier | Qt::Key_T,
        [this] { glWidget->startTransformAction(); });

    freeTransformAction = sc.registerCommand(this, "cmd_free_transform", "自由変形", "編集",
        Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_T,
        [this] { glWidget->startFreeTransformAction(); });

    hueSatLightAction = sc.registerCommand(this, "cmd_hue_sat_light", "色相・彩度・明度...", "編集",
        Qt::AltModifier | Qt::Key_2,
        [this] { glWidget->startHueSatLightAction(); });

    brightnessContrastAction = sc.registerCommand(this, "cmd_brightness_contrast", "明るさ・コントラスト...", "編集",
        Qt::AltModifier | Qt::Key_1,
        [this] { glWidget->startBrightnessContrastAction(); });

    colorBalanceAction = sc.registerCommand(this, "cmd_color_balance", "カラーバランス...", "編集",
        Qt::AltModifier | Qt::Key_3,
        [this] { glWidget->startColorBalanceAction(); });

    toneCurveAction = sc.registerCommand(this, "cmd_tone_curve", "トーンカーブ(Pro)...", "編集",
        Qt::AltModifier | Qt::Key_T,
        [this] { glWidget->startToneCurveAction(); });

    canvasSizeAction = sc.registerCommand(this, "cmd_canvas_size", "キャンバスサイズ変更...", "編集",
        Qt::ControlModifier | Qt::AltModifier | Qt::Key_C,
        [this] { glWidget->startCanvasSizeAction(); });

    imageResolutionAction = sc.registerCommand(this, "cmd_image_resolution", "画像解像度変更...", "編集",
        Qt::ControlModifier | Qt::AltModifier | Qt::Key_I,
        [this] { glWidget->startImageResolutionAction(); });

    // 見た目(ViewTransform)ではなく、キャンバスの実ピクセルデータを回転・反転する
    // (反時計回り)。CanvasWidget::rotateCanvas/flipCanvasHorizontal/flipCanvasVerticalを参照。
    rotateCanvas90Action = sc.registerCommand(this, "cmd_rotate_canvas_90", "キャンバスを90度回転", "編集",
        0,
        [this] { glWidget->rotateCanvas(90); });

    rotateCanvas180Action = sc.registerCommand(this, "cmd_rotate_canvas_180", "キャンバスを180度回転", "編集",
        0,
        [this] { glWidget->rotateCanvas(180); });

    rotateCanvas270Action = sc.registerCommand(this, "cmd_rotate_canvas_270", "キャンバスを270度回転", "編集",
        0,
        [this] { glWidget->rotateCanvas(270); });

    flipCanvasHorizontalAction = sc.registerCommand(this, "cmd_flip_canvas_h", "キャンバスを左右反転", "編集",
        0,
        [this] { glWidget->flipCanvasHorizontal(); });

    flipCanvasVerticalAction = sc.registerCommand(this, "cmd_flip_canvas_v", "キャンバスを上下反転", "編集",
        0,
        [this] { glWidget->flipCanvasVertical(); });

    confirmTransformAction = sc.registerCommand(this, "cmd_transform_confirm", "変形を確定", "編集",
        Qt::Key_Return,
        [this] {
            if (glWidget->isTransformActionActive()) glWidget->confirmTransformAction();
            else if (glWidget->isFreeTransformActionActive()) glWidget->confirmFreeTransformAction();
            else if (glWidget->isCanvasSizeActionActive()) glWidget->confirmCanvasSizeAction();
            else if (glWidget->isImageResolutionActionActive()) glWidget->confirmImageResolutionAction();
            else glWidget->confirmActiveAction(); // 色調整/フィルター系(何もアクティブでなければ no-op)
        });

    // ---- フィルター -----------------------------------------------------------
    gaussianBlurFilterAction = sc.registerCommand(this, "cmd_gaussian_blur_filter", "ガウスぼかし...", "フィルター",
        Qt::AltModifier | Qt::Key_B,
        [this] { glWidget->startGaussianBlurAction(); });

    customShaderFilterAction = sc.registerCommand(this, "cmd_custom_shader_filter", "カスタムシェーダー...", "フィルター",
        Qt::AltModifier | Qt::Key_C,
        [this] { glWidget->startCustomShaderAction(); });

    mosaicFilterAction = sc.registerCommand(this, "cmd_mosaic_filter", "モザイク...", "フィルター",
        Qt::AltModifier | Qt::Key_M,
        [this] { glWidget->startMosaicAction(); });

    motionBlurFilterAction = sc.registerCommand(this, "cmd_motion_blur_filter", "移動ぼかし...", "フィルター",
        0,
        [this] { glWidget->startMotionBlurAction(); });

    noiseFilterAction = sc.registerCommand(this, "cmd_noise_filter", "ノイズ...", "フィルター",
        0,
        [this] { glWidget->startNoiseAction(); });

    chromaticAberrationFilterAction = sc.registerCommand(this, "cmd_chromatic_aberration_filter", "色収差(Pro)...", "フィルター",
        0,
        [this] { glWidget->startChromaticAberrationAction(); });

    lensBlurFilterAction = sc.registerCommand(this, "cmd_lens_blur_filter", "レンズぼかし(Pro)...", "フィルター",
        0,
        [this] { glWidget->startLensBlurAction(); });

    gradientMapAction = sc.registerCommand(this, "cmd_gradient_map", "グラデーションマップ(Pro)...", "フィルター",
        0,
        [this] { glWidget->startGradientMapAction(); });

    // ---- ブラシ -----------------------------------------------------------
    brushBiggerAction = sc.registerCommand(brushSizeDock, "cmd_size_up", "ブラシサイズを大きくする", "ブラシ",
        Qt::Key_2,
        [this] { brushSizeDock->stepUp(); });

    brushSmallerAction = sc.registerCommand(brushSizeDock, "cmd_size_down", "ブラシサイズを小さくする", "ブラシ",
        Qt::Key_1,
        [this] { brushSizeDock->stepDown(); });

    // ---- ウィンドウ -----------------------------------------------------------
    resetLayoutAction = sc.registerCommand(this, "cmd_reset_layout", "ドック配置をリセット", "ウィンドウ",
        0,
        [this] { resetDockLayout(); });

    dumpStateAction = sc.registerCommand(this, "cmd_dump_state", "レイアウトをダンプ(デバッグ用)", "ウィンドウ",
        0,
        [this] {
            QByteArray state = saveState();
            QFile f("layout_dump.txt");
            bool _ = f.open(QIODevice::WriteOnly);
            f.write(state.toBase64());
            f.close();
            QMessageBox::information(this, "完了", "layout_dump.txt に書き出しました");
        });

    resetAllSettingsAction = sc.registerCommand(this, "cmd_reset_all_settings",
        "全設定をリセットして終了(デバッグ用)", "設定",
        0,
        [this] { resetAllSettingsAndQuit(); });

    // ---- その他 -----------------------------------------------------------
    helpAction = sc.registerCommand(this, "cmd_help", "ヘルプ", "その他",
        Qt::Key_F1,
        [this] { QDesktopServices::openUrl(QUrl("https://github.com/pwxwx/Tiepolo/blob/main/docs/index.md")); });

    versionAction = sc.registerCommand(this, "cmd_version", "バージョン情報", "その他",
        0,
        [this] { showVersionDialog(); });

    // 保存済みのキー割り当てを読み込む(未保存ならデフォルトキーのまま)
    QSettings settings;
    shortcuts_.load(settings);
}
