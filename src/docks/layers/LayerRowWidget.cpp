#include "docks/layers/LayerRowWidget.h"

#include "canvas/CanvasWidget.h"
#include "components/ThemeColors.h"
#include "docks/layers/LayerDockLayout.h"
#include "docks/layers/LayerPickerPopup.h"

#include <QCheckBox>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

LayerRowWidget::LayerRowWidget(CanvasWidget *gl, QVector<int> rowLayers, QWidget *parent) : QFrame(parent), glWidget(gl), rowLayers_(std::move(rowLayers))
{
        setFrameShape(QFrame::NoFrame);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(50);

        longPressTimer = new QTimer(this);
        longPressTimer->setSingleShot(true);
        connect(longPressTimer, &QTimer::timeout, this, [this]() {
            if (rowLayers_.size() > 1) openPicker();
        });

        auto *outer = new QHBoxLayout(this);
        outer->setContentsMargins(8, 4, 8, 4);
        outer->setSpacing(6);

        // クリッピングレイヤー(root以外の列)を選択中であることを分かりやすくするための矢印。
        clipArrowLabel = new QLabel("←", this);
        clipArrowLabel->setFixedWidth(10);
        clipArrowLabel->setAlignment(Qt::AlignCenter);
        {
            QFont f = clipArrowLabel->font();
            f.setBold(true);
            clipArrowLabel->setFont(f);
        }
        clipArrowLabel->setVisible(false);
        outer->addWidget(clipArrowLabel);

        // ブレンドモードを一目で分かるようにする左端の色付き縦線。
        blendColorBar = new QWidget(this);
        blendColorBar->setFixedWidth(4);
        outer->addWidget(blendColorBar);

        visibleCheck = new QCheckBox(this);
        visibleCheck->setStyleSheet(R"(
            QCheckBox {
                background: transparent;
                border: none;
            }

            QCheckBox::indicator {
                width: 18px;
                height: 18px;
                background: transparent;
                border: none;
            }

            QCheckBox::indicator:unchecked {
                image: url(:/icons/common/invisible.png);
            }
            QCheckBox::indicator:checked {
                image: url(:/icons/common/visible.png);
            }
        )");
        visibleCheck->setToolTip("表示/非表示");
        connect(visibleCheck, &QCheckBox::toggled, this, [this](bool v) {
            glWidget->document().setLayerVisible(currentLayerIndex(), v);
        });
        outer->addWidget(visibleCheck);

        thumbLabel = new QLabel(this);
        thumbLabel->setFixedSize(40, 30);
        thumbLabel->setAlignment(Qt::AlignCenter);
        outer->addWidget(thumbLabel);

        auto *info = new QVBoxLayout();
        info->setSpacing(1);

        nameEdit = new QLineEdit(this);
        nameEdit->setStyleSheet(QString("background: transparent; border: none; font-size: 10px; font-weight: bold; color: %1;")
            .arg(Theme::panelText.name()));
        nameEdit->setReadOnly(true);
        nameEdit->setFocusPolicy(Qt::NoFocus);
        nameEdit->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        connect(nameEdit, &QLineEdit::editingFinished, this, &LayerRowWidget::finishRename);

        metaLabel = new QLabel(this);
        metaLabel->setStyleSheet(QString("color: %1; font-size: 10px; background: transparent;")
            .arg(Theme::hintText.name()));

        info->addWidget(nameEdit);
        info->addWidget(metaLabel);
        outer->addLayout(info, 1);

        refreshFromDocument();
    }

LayerRowWidget::~LayerRowWidget()
{ delete picker_; }

void LayerRowWidget::setSelected(bool selected)
{
        selected_ = selected;
        applyStyle();
    }

int LayerRowWidget::displayColumn() const
{
        int active = glWidget->document().activeLayerIndex();
        int idx = rowLayers_.indexOf(active);
        return idx >= 0 ? idx : 0;
    }

int LayerRowWidget::currentLayerIndex() const
{ return rowLayers_[displayColumn()]; }

void LayerRowWidget::refreshFromDocument()
{
        const CanvasDocument &doc = glWidget->document();
        int li = currentLayerIndex();
        if (li < 0 || li >= doc.layerCount()) return;
        const Layer &layer = doc.layers[li];

        QSignalBlocker b1(visibleCheck);
        visibleCheck->setChecked(layer.visible);
        if (!nameEdit->hasFocus())
            nameEdit->setText(layer.name);
        const QString modeLabel = (layer.layerType == LayerType::Adjustment)
            ? adjustmentKindNameJa(layer.adjustment.kind)
            : (layer.layerType == LayerType::Filter)
                ? filterKindNameJa(layer.filter.kind)
                : blendModeNameJa(layer.blendMode);
        metaLabel->setText(QString("%1: %2%")
            .arg(modeLabel)
            .arg((int)(layer.opacity * 100)));
        const QString blendColorName = blendModeColor(layer.blendMode).name();
        setStyleSheetIfChanged(blendColorBar, lastBlendBarQss_,
            QString("background-color: %1; border-radius: 2px;").arg(blendColorName));

        // rootではなくクリッピング列を表示中のときだけ、縦線の左に矢印を出して。
        bool showClipArrow = rowLayers_.size() > 1 && displayColumn() > 0;
        clipArrowLabel->setVisible(showClipArrow);
        if (showClipArrow)
            setStyleSheetIfChanged(clipArrowLabel, lastClipArrowQss_,
                QString("color: %1; background: transparent;").arg(blendColorName));

        // アクティブレイヤーの行は(ペン等のツールがそのレイヤーのピクセルをopacity等を変えずに書き換えうるため)常に再生成、
        // それ以外の行は表示中レイヤーの番号・opacity・visible・blendMode・layerType・nameのいずれも前回描画時から変わっていなければキャッシュ済みサムネイルをそのまま使い回す。
        bool isActiveRow = rowLayers_.contains(doc.activeLayerIndex());
        bool sigChanged = !thumbCacheValid_
            || cachedLi_ != li
            || cachedOpacity_ != layer.opacity
            || cachedVisible_ != layer.visible
            || cachedBlend_ != layer.blendMode
            || cachedType_ != layer.layerType
            || cachedName_ != layer.name;

        if (isActiveRow || sigChanged) {
            if (layer.layerType == LayerType::SolidColor || layer.layerType == LayerType::Text
                || layer.layerType == LayerType::Adjustment || layer.layerType == LayerType::Filter
                || layer.layerType == LayerType::Folder) {
                thumbLabel->setPixmap(layerTypeThumbnail(layer, thumbLabel->size()));
            } else {
                QImage preview = glWidget->getLayerPreview(li, li, 40);
                thumbLabel->setPixmap(checkeredThumbnail(preview, thumbLabel->size()));
            }
            thumbCacheValid_ = true;
            cachedLi_ = li;
            cachedOpacity_ = layer.opacity;
            cachedVisible_ = layer.visible;
            cachedBlend_ = layer.blendMode;
            cachedType_ = layer.layerType;
            cachedName_ = layer.name;
        }

        applyStyle();
    }

const QVector<int> &LayerRowWidget::rowLayers() const
{ return rowLayers_; }

void LayerRowWidget::mousePressEvent(QMouseEvent *event)
{
        if (event->button() == Qt::LeftButton) {
            pressPos_ = event->pos();
            pickerOpen_ = false;
            if (rowLayers_.size() > 1)
                longPressTimer->start(280);
        }
        QFrame::mousePressEvent(event);
    }

void LayerRowWidget::mouseMoveEvent(QMouseEvent *event)
{
        if (rowLayers_.size() <= 1) { QFrame::mouseMoveEvent(event); return; }

        if (!pickerOpen_) {
            int dx = event->pos().x() - pressPos_.x();
            if (std::abs(dx) > 8) {
                longPressTimer->stop();
                openPicker();
            } else {
                QFrame::mouseMoveEvent(event);
                return;
            }
        } else {
            updatePickerVisual();
        }
        QFrame::mouseMoveEvent(event);
    }

void LayerRowWidget::mouseReleaseEvent(QMouseEvent *event)
{
        longPressTimer->stop();
        if (pickerOpen_) {
            int col = pickerActiveCol_;
            closePicker();
            emit clicked(rowLayers_[col]);
        } else if (event->button() == Qt::LeftButton) {
            emit clicked(currentLayerIndex());
        }
        QFrame::mouseReleaseEvent(event);
    }

void LayerRowWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
        longPressTimer->stop();
        // 名前編集はnameEdit欄をダブルクリックしたときだけ開始する(名前欄以外のダブルクリックは、調整レイヤーならスライダーパネルを開く、フォルダーなら中へ入る操作に使うため)。
        if (nameEdit->geometry().contains(event->pos())) {
            startRename();
        } else {
            const CanvasDocument &doc = glWidget->document();
            int li = currentLayerIndex();
            if (li >= 0 && li < doc.layerCount()) {
                LayerType t = doc.layers[li].layerType;
                if (t == LayerType::Adjustment)
                    emit adjustmentLayerDoubleClicked(li);
                else if (t == LayerType::SolidColor)
                    emit solidColorLayerDoubleClicked(li);
                else if (t == LayerType::Filter)
                    emit filterLayerDoubleClicked(li);
                else if (t == LayerType::Folder)
                    emit folderDoubleClicked(li); // 右のインジケーターのダブルクリックと同じ
            }
        }
        QFrame::mouseDoubleClickEvent(event);
    }

void LayerRowWidget::openPicker()
{
        pickerOpen_    = true;
        pickerBaseCol_ = displayColumn();
        pickerActiveCol_ = pickerBaseCol_;
        lastScrollOffset_ = 0;

        if (!picker_) picker_ = new LayerPickerPopup();

        const CanvasDocument &doc = glWidget->document();
        QVector<LayerPickerPopup::ItemInfo> items;
        items.reserve(rowLayers_.size());
        const QSize itemThumbSize(LayerPickerPopup::ItemW, LayerPickerPopup::ThumbH);
        for (int li : rowLayers_) {
            const Layer &layer = doc.layers[li];
            QPixmap thumb;
            if (layer.layerType == LayerType::SolidColor || layer.layerType == LayerType::Text
                || layer.layerType == LayerType::Adjustment || layer.layerType == LayerType::Filter
                || layer.layerType == LayerType::Folder) {
                thumb = layerTypeThumbnail(layer, itemThumbSize);
            } else {
                QImage img = glWidget->getLayerPreview(li, li, LayerPickerPopup::ItemW);
                thumb = checkeredThumbnail(img, itemThumbSize);
            }
            const QString modeLabel = (layer.layerType == LayerType::Adjustment)
                ? adjustmentKindNameJa(layer.adjustment.kind)
                : (layer.layerType == LayerType::Filter)
                    ? filterKindNameJa(layer.filter.kind)
                    : blendModeNameJa(layer.blendMode);
            LayerPickerPopup::ItemInfo item;
            item.thumb = thumb;
            item.name  = layer.name;
            item.meta  = QString("%1 %2%").arg(modeLabel).arg((int)(layer.opacity * 100));
            items.append(item);
        }
        picker_->setItems(items);
        picker_->setOriginIndex(pickerBaseCol_);

        pickerContentW_ = LayerPickerPopup::Padding * 2 + rowLayers_.size() * LayerPickerPopup::Step;
        const int h = LayerPickerPopup::Padding * 2 + LayerPickerPopup::ItemH;

        QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QRect screenGeom = screen->geometry();

        // コンテンツが画面幅より広いときはウィンドウ幅を画面幅に収め、はみ出た分はupdatePickerVisual()側のスクロールオフセットで表示する。
        int w = qMin(pickerContentW_, screenGeom.width());
        picker_->resize(w, h);

        // ドラッグを始めた場所に依存しない固定の位置(=行の中心)に出す。
        QPoint rowCenter = mapToGlobal(rect().center());
        int x = qRound(rowCenter.x() - w / 2.0);
        x = qBound(screenGeom.left(), x, screenGeom.right() - w);
        int y = rowCenter.y() - h / 2;
        y = qBound(screenGeom.top(), y, screenGeom.bottom() - h);

        picker_->move(x, y);
        picker_->show();
        updatePickerVisual();
    }

void LayerRowWidget::updatePickerVisual()
{
        if (!picker_) return;
        qreal x0    = LayerPickerPopup::itemCenterX(0);
        qreal xLast = LayerPickerPopup::itemCenterX(rowLayers_.size() - 1);

        // 選択枠は「ドラッグ開始位置からの移動量」ではなく、マウスの現在の実際の画面座標をそのままポップアップ内の位置に変換して表示する(区切りにスナップしない、項目の間の中途半端な位置になってもよい)。
        qreal mouseLocalX = QCursor::pos().x() - picker_->x();
        qreal frameCenterX = qBound(x0, mouseLocalX - lastScrollOffset_, xLast);

        int col = qRound((frameCenterX - x0) / qreal(LayerPickerPopup::Step));
        col = qBound(0, col, rowLayers_.size() - 1);
        pickerActiveCol_ = col;

        // 選択枠が画面上のマウス位置と一致するよう、コンテンツ全体をスクロールさせる。
        qreal desiredScroll = mouseLocalX - frameCenterX;
        qreal minScroll = qMin(0.0, qreal(picker_->width()) - pickerContentW_);
        desiredScroll = qBound(minScroll, desiredScroll, 0.0);
        lastScrollOffset_ = desiredScroll;

        picker_->setFrame(frameCenterX, col);
        picker_->setScrollOffset(desiredScroll);
    }

void LayerRowWidget::closePicker()
{
        pickerOpen_ = false;
        if (picker_) picker_->hide();
    }

void LayerRowWidget::startRename()
{
        nameEdit->setReadOnly(false);
        nameEdit->setFocusPolicy(Qt::StrongFocus);
        nameEdit->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        nameEdit->setFocus();
        nameEdit->selectAll();
    }

void LayerRowWidget::finishRename()
{
        if (nameEdit->isReadOnly()) return; // 普通時のeditingFinishedは無視
        glWidget->document().setLayerName(currentLayerIndex(), nameEdit->text());
        nameEdit->setReadOnly(true);
        nameEdit->setFocusPolicy(Qt::NoFocus);
        nameEdit->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        nameEdit->clearFocus();
    }

void LayerRowWidget::setStyleSheetIfChanged(QWidget *w, QString &cache, const QString &qss)
{
        if (cache == qss) return;
        cache = qss;
        w->setStyleSheet(qss);
    }

void LayerRowWidget::applyStyle()
{
        bool multi = rowLayers_.size() > 1;
        const CanvasDocument &doc = glWidget->document();
        int li = currentLayerIndex();
        bool isFolder = (li >= 0 && li < doc.layerCount()
                          && doc.layers[li].layerType == LayerType::Folder);

        // 左端に二重枠を出すと縦線(blendColorBar/clipArrowLabel)の位置がずれてしまうため、重なりを示す二重枠は右端だけに出す。
        const QString multiBorder = multi
            ? QString("border-right: 5px double %1;").arg(Theme::textDisabled.name())
            : QString();
        // フォルダー行は、他のレイヤー行と一目で見分けられるよう背景をアンバー系で薄く塗る(選択中はいつも通りの選択ハイライトを優先する)。
        QString bg = selected_ ? Theme::scrollHandleHover.name()
                   : isFolder  ? QStringLiteral("rgba(245, 166, 35, 60)")
                               : QStringLiteral("transparent");
        setStyleSheetIfChanged(this, lastRowQss_, QString(
            "LayerRowWidget { background: %1; border-radius: 6px; border: 2px solid transparent; %2 }")
            .arg(bg)
            .arg(multiBorder));
    }
