#pragma once

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QPalette>
#include <QString>
#include <QTextStream>

// ===========================================================================
// Theme
// ---------------------------------------------------------------------------
// アプリ全体の配色を1箇所に集約する。resources/style.qssに現れる色は
// 全てここの変数から取っており(applyThemeTokens()がstyle.qss内の
// "@トークン名@"をここの値へ置換する)、QPainterで直接描画する箇所
// (パネルの半透明背景など、QSSでは塗れない部分)も同じ変数を直接参照する。
//
// ライト/ダークの2テーマを切り替え可能にするため、色は(以前と違い)
// const ではなく、setTheme()で書き換え可能な変数にしてある。
// setTheme()は起動時に1回、および「設定」メニューでのテーマ切り替え時に呼ぶ。
// ===========================================================================
namespace Theme {

enum class Name { Dark, Light };

// ---- ベース ----------------------------------------------------------------
inline QColor bgBase;   // ウィジェット既定背景・メニューバー/ツールバー・スクロールバー溝
inline QColor bgPanel;  // QFrame/QGroupBox・QMenu背景
inline QColor bgInput;  // チェック/ラジオの枠・スピンボックス/ラインエディット背景
inline QColor bgButton; // ボタン既定背景・スライダー溝・各種ボーダー色

// ---- テキスト ----------------------------------------------------------------
// text:       既定文字色(QWidget/QLabel/チェックボックス等、bgBase上)
// textBright: アクセント色(常に彩度の高い青)の上に乗る文字色。ライト/ダーク
//             どちらのテーマでもアクセントは十分暗いので白のまま固定でよい。
// panelText:  フローティングパネル(overlayPanelBg)の上に乗る文字色。
//             パネル背景の明暗がテーマで反転するため、text/textBrightとは
//             別に用意する。
// textDisabled: 無効時の文字・区切り線ホバー等
inline QColor text;
inline QColor textBright;
inline QColor panelText;
inline QColor textDisabled;

// ---- ホバー/アクセント -------------------------------------------------------
inline QColor hoverBg;           // ボタン/メニュー項目ホバー
inline QColor accent;            // 選択・押下・フォーカス色
inline QColor accentHoverLight;  // スライダーハンドルホバー・ラジオホバー枠
inline QColor accentHoverLight2; // チェックボックスのチェック済みホバー
inline QColor scrollHandleHover; // スクロールバーハンドルホバー・コンボボックス無効時文字

// ---- 無効化 ------------------------------------------------------------------
inline QColor bgDisabled; // 無効時背景

// ---- フローティングパネル(DraggablePanel派生) -------------------------------
inline QColor overlayPanelBg; // 半透明の丸角パネル背景

// ---- CustomShaderPanel固有(コードエディタは両テーマ共通で常に暗色) -------------
inline const QColor codeEditorBg     = QColor(0x1e, 0x1e, 0x1e);
inline const QColor codeEditorText   = QColor(0xe0, 0xe0, 0xe0);
inline const QColor codeEditorBorder = QColor(0x55, 0x55, 0x55);
inline QColor hintText;
inline QColor errorText;

// ---- 市松模様(透過部分を示すチェッカーパターン) -------------------------------
// 先端画像プレビュー(ToolPropDock)・レイヤープレビュー(LayerDock)・キャンバス
// 背景(render.frag)の3箇所で共通して使う。両テーマ共通の固定色にしてあるので、
// ここの2値を変えるだけで全箇所に反映される。
inline const QColor checkerDark  = QColor(0xcc, 0xcc, 0xcc);
inline const QColor checkerLight = QColor(0xdd, 0xdd, 0xdd);

inline Name currentName = Name::Dark;

inline void setTheme(Name n)
{
    currentName = n;
    if (n == Name::Dark) {
        bgBase   = QColor(0x44, 0x44, 0x44);
        bgPanel  = QColor(0x4D, 0x4D, 0x4D);
        bgInput  = QColor(0x33, 0x33, 0x33);
        bgButton = QColor(0x55, 0x55, 0x55);

        text         = QColor(0xE0, 0xE0, 0xE0);
        textBright   = QColor(0xFF, 0xFF, 0xFF);
        panelText    = QColor(0xFF, 0xFF, 0xFF);
        textDisabled = QColor(0x77, 0x77, 0x77);

        hoverBg           = QColor(0x66, 0x66, 0x66);
        accent            = QColor(0x56, 0x95, 0xdd);
        accentHoverLight  = QColor(0x64, 0xa4, 0xd2);
        accentHoverLight2 = QColor(0x64, 0xad, 0xd2);
        scrollHandleHover = QColor(0x88, 0x88, 0x88);

        bgDisabled = QColor(0x3A, 0x3A, 0x3A);

        overlayPanelBg = QColor(40, 40, 40, 235);

        hintText  = QColor(0xcc, 0xcc, 0xcc);
        errorText = QColor(0xff, 0x6b, 0x6b);
    } else {
        bgBase   = QColor(0xF0, 0xF0, 0xF0);
        bgPanel  = QColor(0xFF, 0xFF, 0xFF);
        bgInput  = QColor(0xFF, 0xFF, 0xFF);
        bgButton = QColor(0xDA, 0xDA, 0xDA);

        text         = QColor(0x20, 0x20, 0x20);
        textBright   = QColor(0xFF, 0xFF, 0xFF); // アクセント(青)上の文字は両テーマとも白のまま
        panelText    = QColor(0x20, 0x20, 0x20);
        textDisabled = QColor(0xA0, 0xA0, 0xA0);

        hoverBg           = QColor(0xC8, 0xC8, 0xC8);
        accent            = QColor(0x3c, 0x7d, 0xd9);
        accentHoverLight  = QColor(0x5b, 0x9b, 0xe0);
        accentHoverLight2 = QColor(0x5b, 0x9b, 0xe0);
        scrollHandleHover = QColor(0xA8, 0xA8, 0xA8);

        bgDisabled = QColor(0xE8, 0xE8, 0xE8);

        overlayPanelBg = QColor(245, 245, 245, 235);

        hintText  = QColor(0x70, 0x70, 0x70);
        errorText = QColor(0xc0, 0x39, 0x2b);
    }
}

// resources/style.qss内の"@トークン名@"をTheme::の現在値へ置換する。
inline QString applyThemeTokens(QString qss)
{
    qss.replace("@bgBase@",    bgBase.name());
    qss.replace("@bgPanel@",   bgPanel.name());
    qss.replace("@bgInput@",   bgInput.name());
    qss.replace("@bgButton@",  bgButton.name());
    qss.replace("@text@",         text.name());
    qss.replace("@textBright@",   textBright.name());
    qss.replace("@panelText@",    panelText.name());
    qss.replace("@textDisabled@", textDisabled.name());
    qss.replace("@hoverBg@",           hoverBg.name());
    qss.replace("@accent@",            accent.name());
    qss.replace("@accentHoverLight@",  accentHoverLight.name());
    qss.replace("@accentHoverLight2@", accentHoverLight2.name());
    qss.replace("@scrollHandleHover@", scrollHandleHover.name());
    qss.replace("@bgDisabled@", bgDisabled.name());
    qss.replace("@codeEditorBg@",     codeEditorBg.name());
    qss.replace("@codeEditorText@",   codeEditorText.name());
    qss.replace("@codeEditorBorder@", codeEditorBorder.name());
    qss.replace("@hintText@",  hintText.name());
    qss.replace("@errorText@", errorText.name());
    return qss;
}

// resources/style.qss(トークン付きテンプレート)を読み込み、現在のTheme値で
// 置換した上でアプリ全体のスタイルシート・パレットに適用する。起動時、および
// 「設定」メニューでのテーマ切り替え時の両方から呼ぶ。
inline void applyToApplication(QApplication &app)
{
    QFile file(":/style.qss");
    (void)file.open(QFile::ReadOnly | QFile::Text);
    QTextStream stream(&file);
    app.setStyleSheet(applyThemeTokens(stream.readAll()));

    QPalette palette;
    palette.setColor(QPalette::Window, bgBase);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Button, hoverBg);
    palette.setColor(QPalette::ButtonText, text);
    app.setPalette(palette);
}

} // namespace Theme
