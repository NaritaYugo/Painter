#pragma once
#include "dialogs/DraggablePanel.h"

#include <QPlainTextEdit>
#include <QWidget>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QTextCharFormat>

class QPlainTextEdit;
class QLabel;
class QTimer;
class LineNumberArea;

// ---------------------------------------------------------------------------
// GLSLHighlighter
// ---------------------------------------------------------------------------
class GLSLHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit GLSLHighlighter(QTextDocument *parent = nullptr);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QList<HighlightingRule> highlightingRules;

    QRegularExpression commentStartExpression;
    QRegularExpression commentEndExpression;
    QTextCharFormat multiLineCommentFormat;
};

class LineNumberArea;

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);
    ~CodeEditor() override = default;

    void lineNumberAreaPaintEvent(QPaintEvent *event);
    int lineNumberAreaWidth();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();

private:
    QWidget *lineNumberArea;

    // 選択範囲の各行をまとめてインデント/インデント解除する(Tab/Shift+Tab用)。
    // 選択が無いとき(1行のみ)にも同じ経路で使う。
    void indentSelection(bool indent);
    // 選択範囲の各行の "// " コメントをまとめてトグルする(Ctrl+/用)。
    void toggleLineComment();
};

// 行番号描画用の補助ウィジェットクラス
class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *editor) : QWidget(editor), m_editor(editor) {}

    QSize sizeHint() const override { return QSize(0, 0); }

protected:
    void paintEvent(QPaintEvent *event) override {
        m_editor->lineNumberAreaPaintEvent(event);
    }

private:
    CodeEditor *m_editor;
};

// ---------------------------------------------------------------------------
// CustomShaderPanel
// ---------------------------------------------------------------------------
// フィルターメニュー「カスタムシェーダー」アクション用のポップアップパネル。
// GLSL関数本体を編集するテキストエリア+エラー表示ラベル。ColorBalancePanel等と
// 同様、GLWidgetの子として「キャンバス上に」浮かせて表示する非モーダルのQWidget。
// 入力停止から一定時間後にデバウンスして sourceChanged() を発行する
// (キー入力のたびにシェーダーを再コンパイルするのは重いため)。
// ---------------------------------------------------------------------------
class CustomShaderPanel : public DraggablePanel
{
    Q_OBJECT
public:
    explicit CustomShaderPanel(QWidget *parent = nullptr);

    // 外部からテキストをセットする(シグナルは出さない)。パネルを開く際に使う。
    void setSource(const QString &src);
    QString source() const;

    // コンパイル結果のエラーメッセージを表示する。空文字列ならエラー表示を消す。
    void setError(const QString &errorText);

signals:
    void sourceChanged(const QString &src); // デバウンス後に発行
    void confirmed();
    void cancelled();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QPlainTextEdit *editor_     = nullptr;
    QLabel          *errorLabel_ = nullptr;
    QTimer          *debounceTimer_ = nullptr;
    bool             settingProgrammatically_ = false;
    QWidget         *resizeGrip_ = nullptr; // 右下隅のリサイズハンドル
};

