#pragma once

#include <QWidget>
#include <QVector>
#include <QMetaObject>
#include <QVariantMap>
#include <QColor>
#include <QImage>
#include "tools/core/ToolType.h"

class CanvasWidget;
class ToolConfig;
class QVBoxLayout;
class QTimer;
class QScrollArea;
class QPushButton;
class QLabel;
class ToolPresetRowWidget;

// ===========================================================================
// ToolPresetDock  ―  選択中ツールの「ツールプリセット」一覧パネル
//
// ペンなら複数のペン設定、消しゴムなら複数の消しゴム設定というように、
// ToolType単位で保持されるツールプリセット一覧(ToolConfig::toolPresetList())を
// 表示・選択・追加・複製・削除・改名する。移動/回転/スポイトのように
// パラメータを持たないツールでも同じ仕組みでツールプリセットを1つ持ち、
// 「移動ツール > 移動ツールプリセット」のように常にツールプリセット経由で操作する。
// ===========================================================================
class ToolPresetDock : public QWidget
{
    Q_OBJECT
public:
    explicit ToolPresetDock(CanvasWidget *gl, ToolConfig *toolCfg, QWidget *parent = nullptr);

    // 外部から一覧を作り直したいとき用(設定の読み込み直後など)
    void refresh();

    // タブ切替時に、表示対象のCanvasWidget(=キャンバス)を差し替える
    void setCanvasWidget(CanvasWidget *gl);

public slots:
    // 現在表示中のツール種別を切り替える(CanvasWidget::activeToolChangedに接続)
    void setCurrentTool(ToolType tool);

private:
    CanvasWidget   *glWidget = nullptr;
    ToolConfig *toolCfg_ = nullptr;
    ToolType    currentType_ = ToolType::Pen;
    QMetaObject::Connection activeToolChangedConn_;

    QLabel      *titleLabel    = nullptr;
    QScrollArea *scrollArea    = nullptr;
    QWidget     *listContainer = nullptr;
    QVBoxLayout *listLayout    = nullptr;
    QPushButton *addBtn        = nullptr;

    QVector<ToolPresetRowWidget*> rows_;

    // ---- プレビューを設定変更へ追従させる --------------------------------
    // ToolPropertyDockには「どれか設定が変わった」を知らせるシグナルが無く(sizeChanged
    // だけ)、項目ごとに繋ぐと項目を増やすたびに繋ぎ忘れる。アクティブなツール
    // プリセットの設定値と描画色を軽く見張り、変わったときだけその行の絵を
    // 描き直す(行そのものは作り直さない ― 名前の編集中に消えてしまうため)。
    QTimer     *previewWatchTimer_ = nullptr;
    QVariantMap watchedValues_;
    QColor      watchedInk_;
    bool        watchedInkTransparent_ = false;
    void updateActivePreview();
    QImage renderPreviewFor(int index) const;

    void rebuildRows();
    void selectIndex(int index);
    void renameIndex(int index, const QString &name);
    void duplicateIndex(int index);
    void deleteIndex(int index);
    void addNew();
};
