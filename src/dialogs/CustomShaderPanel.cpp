#include "dialogs/CustomShaderPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QFont>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QPainter>

GLSLHighlighter::GLSLHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    HighlightingRule rule;

    // 1. 関数 (暗い黄色) - `(` の前にある単語を関数としてマッチ
    QTextCharFormat functionFormat;
    functionFormat.setForeground(QColor("#946f00"));
    rule.pattern = QRegularExpression(QStringLiteral("\\b[A-Za-z0-9_]+(?=\\s*\\()"));
    rule.format = functionFormat;
    highlightingRules.append(rule);

    // 2. 制御構文 (ピンク)
    QTextCharFormat controlFormat;
    controlFormat.setForeground(QColor("#a52871"));
    const QString controlPatterns[] = {
        QStringLiteral("\\bif\\b"), QStringLiteral("\\belse\\b"),
        QStringLiteral("\\bfor\\b"), QStringLiteral("\\bwhile\\b"),
        QStringLiteral("\\bdo\\b"), QStringLiteral("\\breturn\\b"),
        QStringLiteral("\\bbreak\\b"), QStringLiteral("\\bcontinue\\b"),
        QStringLiteral("\\bdiscard\\b")
    };
    for (const QString &pattern : controlPatterns) {
        rule.pattern = QRegularExpression(pattern);
        rule.format = controlFormat;
        highlightingRules.append(rule);
    }

    // 3. 修飾子 (青)
    QTextCharFormat modifierFormat;
    modifierFormat.setForeground(QColor("#004696"));
    const QString modifierPatterns[] = {
        QStringLiteral("\\bconst\\b"), QStringLiteral("\\buniform\\b"),
        QStringLiteral("\\bin\\b"), QStringLiteral("\\bout\\b"),
        QStringLiteral("\\binout\\b"), QStringLiteral("\\bvarying\\b"),
        QStringLiteral("\\battribute\\b"), QStringLiteral("\\blayout\\b"),
        QStringLiteral("\\bflat\\b"), QStringLiteral("\\bsmooth\\b")
    };
    for (const QString &pattern : modifierPatterns) {
        rule.pattern = QRegularExpression(pattern);
        rule.format = modifierFormat;
        highlightingRules.append(rule);
    }

    // 4. 型 (水色)
    QTextCharFormat typeFormat;
    typeFormat.setForeground(QColor("#008195"));
    const QString typePatterns[] = {
        QStringLiteral("\\bvoid\\b"), QStringLiteral("\\bbool\\b"),
        QStringLiteral("\\bint\\b"), QStringLiteral("\\buint\\b"),
        QStringLiteral("\\bfloat\\b"), QStringLiteral("\\bvec2\\b"),
        QStringLiteral("\\bvec3\\b"), QStringLiteral("\\bvec4\\b"),
        QStringLiteral("\\bmat2\\b"), QStringLiteral("\\bmat3\\b"),
        QStringLiteral("\\bmat4\\b"), QStringLiteral("\\bsampler2D\\b")
    };
    for (const QString &pattern : typePatterns) {
        rule.pattern = QRegularExpression(pattern);
        rule.format = typeFormat;
        highlightingRules.append(rule);
    }

    // 5. 括弧類
    // 丸括弧 () : 青
    QTextCharFormat parenFormat;
    parenFormat.setForeground(QColor("#004696"));
    rule.pattern = QRegularExpression(QStringLiteral("[\\(\\)]"));
    rule.format = parenFormat;
    highlightingRules.append(rule);

    // 波括弧 {} : 緑
    QTextCharFormat braceFormat;
    braceFormat.setForeground(QColor("#268f3e"));
    rule.pattern = QRegularExpression(QStringLiteral("[\\{\\}]"));
    rule.format = braceFormat;
    highlightingRules.append(rule);

    // 角括弧 [] : 茶色
    QTextCharFormat bracketFormat;
    bracketFormat.setForeground(QColor("#8b4513"));
    rule.pattern = QRegularExpression(QStringLiteral("[\\[\\]]"));
    rule.format = bracketFormat;
    highlightingRules.append(rule);

    // 6. 単一行コメント (緑)
    QTextCharFormat singleLineCommentFormat;
    singleLineCommentFormat.setForeground(QColor("#268f3e"));
    rule.pattern = QRegularExpression(QStringLiteral("//[^\n]*"));
    rule.format = singleLineCommentFormat;
    highlightingRules.append(rule);

    // 7. 複数行コメント (緑)
    multiLineCommentFormat.setForeground(QColor("#268f3e"));
    commentStartExpression = QRegularExpression(QStringLiteral("/\\*"));
    commentEndExpression = QRegularExpression(QStringLiteral("\\*/"));
}

void GLSLHighlighter::highlightBlock(const QString &text)
{
    // 通常のルールを適用
    for (const HighlightingRule &rule : qAsConst(highlightingRules)) {
        QRegularExpressionMatchIterator matchIterator = rule.pattern.globalMatch(text);
        while (matchIterator.hasNext()) {
            QRegularExpressionMatch match = matchIterator.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    // 複数行コメントのブロック状態を管理
    setCurrentBlockState(0);
    int startIndex = 0;
    if (previousBlockState() != 1) {
        startIndex = text.indexOf(commentStartExpression);
    }

    while (startIndex >= 0) {
        QRegularExpressionMatch match = commentEndExpression.match(text, startIndex);
        int endIndex = match.capturedStart();
        int commentLength = 0;
        if (endIndex == -1) {
            setCurrentBlockState(1);
            commentLength = text.length() - startIndex;
        } else {
            commentLength = endIndex - startIndex + match.capturedLength();
        }
        setFormat(startIndex, commentLength, multiLineCommentFormat);
        startIndex = text.indexOf(commentStartExpression, startIndex + commentLength);
    }
}

// ---------------------------------------------------------------------------
// CodeEditor  ―  CustomShaderPanelのGLSL入力欄
//
// Tabキーで4個分のスペースを挿入し(フォーカス移動には使わせない)、改行時は
// 直前の行の先頭の空白(インデント)をそのまま次の行へ引き継ぐ、簡易的な
// コードエディタらしい挙動を追加する。
// ---------------------------------------------------------------------------
CodeEditor::CodeEditor(QWidget *parent) : QPlainTextEdit(parent)
{
    lineNumberArea = new LineNumberArea(this);

    setStyleSheet("background-color: #f2f2f2; color: #333; font-size: 15px; font-family: Consolas;");
    new GLSLHighlighter(this->document());

    // シグナル・スロットの接続
    connect(this, &CodeEditor::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &CodeEditor::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &CodeEditor::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
}

int CodeEditor::lineNumberAreaWidth()
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    int space = 15 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

void CodeEditor::updateLineNumberAreaWidth(int /* newBlockCount */)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy) {
        lineNumberArea->scroll(0, dy);
    } else {
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);

    QRect cr = contentsRect();
    lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> extraSelections;

    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        QColor lineColor = QColor("#dee9f6");
        selection.format.setBackground(lineColor);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        extraSelections.append(selection);
    }

    setExtraSelections(extraSelections);
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(), QColor("#f2f2f2"));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            QString number = QString::number(blockNumber + 1);
            painter.setPen(QColor("#333"));
            painter.drawText(0, top, lineNumberArea->width() - 8, fontMetrics().height(),
                             Qt::AlignRight | Qt::AlignVCenter, number);
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    // 1. Tab/Shift+Tabキー：複数行選択中はまとめてインデント/インデント解除、
    // 選択が無ければ(1行内なら)従来通りスペース4個を挿入するだけ。
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
        const bool indent = (event->key() == Qt::Key_Tab);
        if (textCursor().hasSelection()) {
            indentSelection(indent);
        } else if (indent) {
            insertPlainText(QStringLiteral("    "));
        }
        return;
    }

    // 2. Return/Enterキー：オートインデント
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const QString prevLine = textCursor().block().text();
        QString indent;
        for (const QChar &c : prevLine) {
            if (c == ' ' || c == '\t') indent += c;
            else break;
        }
        QPlainTextEdit::keyPressEvent(event);
        if (!indent.isEmpty()) insertPlainText(indent);
        return;
    }

    // 3. Backspaceキー：スペース4個をまとめて削除
    if (event->key() == Qt::Key_Backspace) {
        QTextCursor cursor = textCursor();
        if (!cursor.hasSelection() && cursor.positionInBlock() > 0) {
            const QString lineText = cursor.block().text();
            const int posInBlock = cursor.positionInBlock();
            if (posInBlock >= 4) {
                QString leftFourChars = lineText.mid(posInBlock - 4, 4);
                if (leftFourChars == QStringLiteral("    ")) {
                    cursor.beginEditBlock();
                    for (int i = 0; i < 4; ++i) {
                        cursor.deletePreviousChar();
                    }
                    cursor.endEditBlock();
                    return;
                }
            }
        }
    }

    // 4. Ctrl+/ : 選択行(選択が無ければカーソル行)の "// " コメントをまとめてトグル
    if (event->key() == Qt::Key_Slash && (event->modifiers() & Qt::ControlModifier)) {
        toggleLineComment();
        return;
    }

    // 4.5. 括弧の自動補完 ( {}, [], () )
    if (event->text() == "{" || event->text() == "[" || event->text() == "(") {
        QChar openChar = event->text().at(0);
        QChar closeChar;
        if (openChar == '{') closeChar = '}';
        else if (openChar == '[') closeChar = ']';
        else closeChar = ')';

        QTextCursor cursor = textCursor();
        cursor.beginEditBlock();
        
        if (cursor.hasSelection()) {
            QString selectedText = cursor.selectedText();
            cursor.insertText(openChar + selectedText + closeChar);
        } else {
            cursor.insertText(QString(openChar) + closeChar);
            cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 1);
            setTextCursor(cursor);
        }
        
        cursor.endEditBlock();
        return;
    }

    // 5. 閉じ括弧がすでにある場合のオーバードライブ
    if (event->text() == "}" || event->text() == "]" || event->text() == ")") {
        QTextCursor cursor = textCursor();
        if (!cursor.hasSelection()) {
            cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
            if (cursor.selectedText() == event->text()) {
                cursor.clearSelection();
                setTextCursor(cursor);
                return;
            }
        }
    }

    QPlainTextEdit::keyPressEvent(event);
}

// 選択範囲が含むブロック番号の範囲[startBlock, endBlock]を求める。
// 選択終端がちょうど行頭に乗っている(=その行自体は選択に含んでいないつもりで
// 複数行選択した)場合は、多くのエディタと同じ挙動に合わせてその行を除外する。
static void selectionBlockRange(QTextDocument *doc, const QTextCursor &cursor, int &startBlock, int &endBlock)
{
    QTextCursor startCursor(doc);
    startCursor.setPosition(cursor.selectionStart());
    QTextCursor endCursor(doc);
    endCursor.setPosition(cursor.selectionEnd());

    startBlock = startCursor.blockNumber();
    endBlock   = endCursor.blockNumber();
    if (endBlock > startBlock && endCursor.atBlockStart())
        endBlock--;
}

void CodeEditor::indentSelection(bool indent)
{
    QTextCursor cursor = textCursor();
    int startBlock, endBlock;
    selectionBlockRange(document(), cursor, startBlock, endBlock);

    cursor.beginEditBlock();
    QTextBlock block = document()->findBlockByNumber(startBlock);
    for (int i = startBlock; i <= endBlock && block.isValid(); ++i, block = block.next()) {
        QTextCursor lineCursor(block);
        if (indent) {
            lineCursor.movePosition(QTextCursor::StartOfBlock);
            lineCursor.insertText(QStringLiteral("    "));
        } else {
            const QString text = block.text();
            int removeCount = 0;
            while (removeCount < 4 && removeCount < text.length() && text.at(removeCount) == ' ')
                removeCount++;
            if (removeCount == 0 && !text.isEmpty() && text.at(0) == '\t')
                removeCount = 1;
            if (removeCount > 0) {
                lineCursor.movePosition(QTextCursor::StartOfBlock);
                lineCursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, removeCount);
                lineCursor.removeSelectedText();
            }
        }
    }
    cursor.endEditBlock();
}

void CodeEditor::toggleLineComment()
{
    QTextCursor cursor = textCursor();
    int startBlock, endBlock;
    selectionBlockRange(document(), cursor, startBlock, endBlock);

    // 選択範囲内に未コメントの行(空行を除く)が1つでもあれば「全行コメント化」、
    // 全行すでにコメント済みなら「全行解除」にする(多くのエディタと同じ挙動)。
    bool allCommented = true;
    QTextBlock block = document()->findBlockByNumber(startBlock);
    for (int i = startBlock; i <= endBlock && block.isValid(); ++i, block = block.next()) {
        const QString trimmed = block.text().trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QStringLiteral("//"))) {
            allCommented = false;
            break;
        }
    }

    cursor.beginEditBlock();
    block = document()->findBlockByNumber(startBlock);
    for (int i = startBlock; i <= endBlock && block.isValid(); ++i, block = block.next()) {
        const QString text = block.text();
        QTextCursor lineCursor(block);
        if (allCommented) {
            const int idx = text.indexOf(QStringLiteral("//"));
            if (idx >= 0) {
                const int removeLen = (idx + 2 < text.length() && text.at(idx + 2) == ' ') ? 3 : 2;
                lineCursor.setPosition(block.position() + idx);
                lineCursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, removeLen);
                lineCursor.removeSelectedText();
            }
        } else if (!text.trimmed().isEmpty()) {
            // 空行はコメント化しない(無駄な"//"だけの行が増えるのを避ける)
            lineCursor.movePosition(QTextCursor::StartOfBlock);
            lineCursor.insertText(QStringLiteral("// "));
        }
    }
    cursor.endEditBlock();
}

// ---------------------------------------------------------------------------
// PanelResizeGrip
// ---------------------------------------------------------------------------
// パネル右下隅に置く、ドラッグでパネル自体をリサイズするための小さなハンドル。
// DraggablePanelの「背景ドラッグ=移動」は子ウィジェット上のクリックには反応しない
// ため、独立した子ウィジェットとして実装するだけでDraggablePanel側には一切
// 手を加えずに済む。
// ---------------------------------------------------------------------------
namespace {
class PanelResizeGrip : public QWidget
{
public:
    PanelResizeGrip(QWidget *panel, QSize minSize)
        : QWidget(panel), panel_(panel), minSize_(minSize)
    {
        setCursor(Qt::SizeFDiagCursor);
        setFixedSize(14, 14);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor(255, 255, 255, 140));
        pen.setWidthF(1.5);
        p.setPen(pen);
        for (int i = 1; i <= 3; ++i) {
            const int off = i * 4;
            p.drawLine(width() - off, height() - 2, width() - 2, height() - off);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            dragStartMousePos_ = event->globalPosition().toPoint();
            dragStartSize_ = panel_->size();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            const QPoint delta = event->globalPosition().toPoint() - dragStartMousePos_;
            QSize newSize = (dragStartSize_ + QSize(delta.x(), delta.y())).expandedTo(minSize_);
            panel_->resize(newSize);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && dragging_) {
            dragging_ = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QWidget *panel_;
    QSize    minSize_;
    bool     dragging_ = false;
    QPoint   dragStartMousePos_;
    QSize    dragStartSize_;
};
} // namespace

CustomShaderPanel::CustomShaderPanel(QWidget *parent)
    : DraggablePanel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("カスタムシェーダー"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    auto *hint = new QLabel(
        QStringLiteral(
            "vec4 main(vec2 uv, vec2 pos, vec2 res, vec4 src) で新たな色を作成して返してください\n"
            "uv: 0〜1の正規化座標 / pos: 中央が原点で縦横比を保った座標 / res: ピクセルサイズ(px) / src: 現在の色"),
        this);
    hint->setObjectName("panelHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    editor_ = new CodeEditor(this);
    editor_->setTabChangesFocus(false);
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setObjectName("codeEditor");
    editor_->setMinimumSize(200, 80);
    layout->addWidget(editor_, /*stretch=*/1);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName("panelError");
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();
    layout->addWidget(errorLabel_);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *cancelBtn  = new QPushButton(QStringLiteral("キャンセル"), this);
    auto *confirmBtn = new QPushButton(QStringLiteral("確定"), this);
    confirmBtn->setDefault(true);
    cancelBtn->setObjectName("panelCancelButton");
    confirmBtn->setObjectName("panelConfirmButton");
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(confirmBtn);
    layout->addLayout(btnRow);

    // パネル自体は横幅固定(setFixedWidth)だったが、GLSLは横に長い行を書くことが
    // 多く420px幅では窮屈なため、右下のハンドルドラッグで自由に伸縮できるようにする。
    static constexpr QSize kMinPanelSize(340, 280);
    setMinimumSize(kMinPanelSize);
    resize(560, 360);

    resizeGrip_ = new PanelResizeGrip(this, kMinPanelSize);
    resizeGrip_->raise();

    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(400);
    connect(debounceTimer_, &QTimer::timeout, this, [this] {
        emit sourceChanged(editor_->toPlainText());
    });

    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        if (settingProgrammatically_) return;
        debounceTimer_->start();
    });

    connect(confirmBtn, &QPushButton::clicked, this, &CustomShaderPanel::confirmed);
    connect(cancelBtn,  &QPushButton::clicked, this, &CustomShaderPanel::cancelled);
}

void CustomShaderPanel::resizeEvent(QResizeEvent *event)
{
    DraggablePanel::resizeEvent(event);
    if (resizeGrip_)
        resizeGrip_->move(width() - resizeGrip_->width() - 4, height() - resizeGrip_->height() - 4);
}

void CustomShaderPanel::setSource(const QString &src)
{
    settingProgrammatically_ = true;
    editor_->setPlainText(src);
    settingProgrammatically_ = false;
}

QString CustomShaderPanel::source() const
{
    return editor_->toPlainText();
}

void CustomShaderPanel::setError(const QString &errorText)
{
    if (errorText.isEmpty()) {
        errorLabel_->hide();
        errorLabel_->clear();
    } else {
        errorLabel_->setText(errorText);
        errorLabel_->show();
    }
}
