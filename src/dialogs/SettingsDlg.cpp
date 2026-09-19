#include "dialogs/SettingsDlg.h"
#include "components/ColorWheelWidget.h" // 色相ツイストの範囲(HUE_TWIST_MIN/MAX)
#include "dialogs/ToneCurveEditor.h"
#include "tools/core/PressureCurve.h"   // カーブ制御点の文字列化(toString/fromString)

#include <QSettings>
#include <QListWidget>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>

// ===========================================================================
// QSettings との対応
// ===========================================================================
SettingsDlg::Values SettingsDlg::loadValues()
{
    QSettings s;
    Values v;

    // ファイル保存先の既定値(初回起動時など未設定の場合のフォールバック)
    v.projectSaveDir = QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/TiepoloProjects");
    v.exportSaveDir = QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/TiepoloExports");

    s.beginGroup("preferences");

    v.defaultCanvasW    = s.value("general/canvasW",    v.defaultCanvasW).toInt();
    v.defaultCanvasH    = s.value("general/canvasH",    v.defaultCanvasH).toInt();
    v.autoSaveEnabled   = s.value("general/autoSave",   v.autoSaveEnabled).toBool();
    v.autoSaveInterval  = s.value("general/autoSaveInterval", v.autoSaveInterval).toInt();

    v.undoHistoryLimit  = s.value("drawing/undoLimit",  v.undoHistoryLimit).toInt();
    v.hueTwist          = s.value("color/hueTwist",     v.hueTwist).toInt();

    // 筆圧カーブは PressureCurve と同じ "x,y;x,y;..." 形式で持つ
    // (未設定なら PressureCurve::fromString("") が恒等を返す)。
    v.pressureCurve = PressureCurve::fromString(s.value("input/pressureCurve").toString()).points();

    v.projectSaveDir    = s.value("file/projectSaveDir", v.projectSaveDir).toString();
    v.exportSaveDir     = s.value("file/exportSaveDir",  v.exportSaveDir).toString();

    s.endGroup();
    return v;
}

void SettingsDlg::saveValues(const Values &v)
{
    QSettings s;
    s.beginGroup("preferences");

    s.setValue("general/canvasW",    v.defaultCanvasW);
    s.setValue("general/canvasH",    v.defaultCanvasH);
    s.setValue("general/autoSave",   v.autoSaveEnabled);
    s.setValue("general/autoSaveInterval", v.autoSaveInterval);

    s.setValue("drawing/undoLimit",  v.undoHistoryLimit);
    s.setValue("color/hueTwist",     v.hueTwist);

    {
        PressureCurve c;
        c.setPoints(v.pressureCurve);
        s.setValue("input/pressureCurve", c.toString());
    }

    s.setValue("file/projectSaveDir", v.projectSaveDir);
    s.setValue("file/exportSaveDir",  v.exportSaveDir);

    s.endGroup();
}

// ===========================================================================
// コンストラクタ
// ===========================================================================
SettingsDlg::SettingsDlg(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("環境設定");
    resize(700, 500);

    current = loadValues();

    QVBoxLayout *rootLayout = new QVBoxLayout(this);

    QHBoxLayout *bodyLayout = new QHBoxLayout();

    // ---- 左: カテゴリ一覧 ----
    categoryList = new QListWidget(); {
        categoryList->setFixedWidth(140);
        categoryList->addItem("一般");
        categoryList->addItem("入力");
        categoryList->addItem("ファイル保存");
    }
    bodyLayout->addWidget(categoryList);

    // ---- 右: 詳細設定 (カテゴリに応じて切り替え) ----
    pageStack = new QStackedWidget(); {
        pageStack->addWidget(createGeneralPage());
        pageStack->addWidget(createInputPage());
        pageStack->addWidget(createFilePage());
    }
    bodyLayout->addWidget(pageStack, 1);

    rootLayout->addLayout(bodyLayout, 1);

    connect(categoryList, &QListWidget::currentRowChanged,
            pageStack, &QStackedWidget::setCurrentIndex);
    categoryList->setCurrentRow(0);

    // ---- 下部: OK / キャンセル ----
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDlg::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rootLayout->addWidget(buttons);

    adjustSize();
}

// ===========================================================================
// 一般ページ
// ===========================================================================
QWidget *SettingsDlg::createGeneralPage()
{
    QWidget     *page = new QWidget();
    QFormLayout *form = new QFormLayout(page);

    canvasWSpin = new QSpinBox(); {
        canvasWSpin->setRange(64, 8192);
        canvasWSpin->setValue(current.defaultCanvasW);
    }
    form->addRow("新規キャンバス幅", canvasWSpin);

    canvasHSpin = new QSpinBox(); {
        canvasHSpin->setRange(64, 8192);
        canvasHSpin->setValue(current.defaultCanvasH);
    }
    form->addRow("新規キャンバス高さ", canvasHSpin);

    autoSaveCheck = new QCheckBox("自動保存を有効にする"); {
        autoSaveCheck->setChecked(current.autoSaveEnabled);
    }
    form->addRow(autoSaveCheck);

    autoSaveIntervalSpin = new QSpinBox(); {
        autoSaveIntervalSpin->setRange(1, 60);
        autoSaveIntervalSpin->setSuffix(" 分");
        autoSaveIntervalSpin->setValue(current.autoSaveInterval);
        autoSaveIntervalSpin->setEnabled(current.autoSaveEnabled);
    }
    form->addRow("自動保存の間隔", autoSaveIntervalSpin);

    connect(autoSaveCheck, &QCheckBox::toggled,
            autoSaveIntervalSpin, &QSpinBox::setEnabled);

    // 自動保存は、保存先が決まっている(=一度でも保存/別名で保存/開くを行った)
    // タブに対してのみ、そのファイルへ上書き保存する形で行う(未保存の新規タブは
    // 対象外。保存ダイアログを勝手に出すと作業の邪魔になるため)。
    QLabel *autoSaveNote = new QLabel(
        "※自動保存は、保存先が決まっているキャンバス(保存済み/開いたファイル)のみが対象。");
    autoSaveNote->setWordWrap(true);
    form->addRow(autoSaveNote);

    undoLimitSpin = new QSpinBox(); {
        // タイル差分ベースの通常のUndo(ペンのストローク等)は軽いが、キャンバス
        // サイズ変更のUndoはレイヤー全内容のスナップショットを前後2枚保持するため
        // 非常に重い。両者が同じ履歴を共有する設計のため、上限は控えめにしてある
        // (CLIP STUDIO PAINTは初期値30・上限200、Photoshopは初期値50・上限1000)。
        undoLimitSpin->setRange(1, 300);
        undoLimitSpin->setValue(current.undoHistoryLimit);
    }
    form->addRow("Undo履歴の保持数", undoLimitSpin);

    hueTwistSpin = new QSpinBox(); {
        hueTwistSpin->setRange(ColorWheelWidget::HUE_TWIST_MIN, ColorWheelWidget::HUE_TWIST_MAX);
        hueTwistSpin->setSuffix(" °/L");
        hueTwistSpin->setValue(current.hueTwist);
    }
    form->addRow("色相ツイスト", hueTwistSpin);

    // カラーサークルのLCスクエアは、そのHueで表現できるL範囲の外側(上下の余白)へ
    // はみ出した位置でも色を返す。そこで明度差に比例して色相を Yellow / Blue 側へ
    // ずらす量がこの値(0で色相を固定)。以前はカラーサークル下部の「D」スライダー
    // だったが、色そのものではなくサークルの振る舞いの設定なのでここへ移した。
    QLabel *hueTwistNote = new QLabel(
        "※カラーサークルで、明るさを振ったときに色相を暖色/寒色側へずらす量");
    hueTwistNote->setWordWrap(true);
    form->addRow(hueTwistNote);

    return page;
}

// ===========================================================================
// 入力ページ(筆圧カーブ)
// ---------------------------------------------------------------------------
// 全ツール共通の筆圧カーブ。横軸=タブレットから来た生の筆圧、縦軸=実際に使う
// 筆圧。曲線上をクリックで節点追加、ドラッグで移動、選択して削除。
// ここで使っているのはトーンカーブと同じ編集ウィジェット(ToneCurveEditor)。
// ===========================================================================
QWidget *SettingsDlg::createInputPage()
{
    QWidget     *page = new QWidget();
    QVBoxLayout *v    = new QVBoxLayout(page);

    auto *title = new QLabel("筆圧カーブ(全体)");
    title->setStyleSheet("font-weight: bold;");
    v->addWidget(title);

    auto *desc = new QLabel(
        "横軸がタブレットから届く筆圧、縦軸が実際に使う筆圧です。\n"
        "左下を持ち上げると軽い力で濃く/太く、右下へ引くと強く押さないと乗らなくなります。");
    desc->setWordWrap(true);
    v->addWidget(desc);

    auto *curveRow = new QHBoxLayout();
    pressureCurveEditor_ = new ToneCurveEditor(page);
    pressureCurveEditor_->setPoints(current.pressureCurve);
    curveRow->addWidget(pressureCurveEditor_, 0, Qt::AlignTop);

    auto *btnCol = new QVBoxLayout();
    pressureDeleteBtn_ = new QPushButton("選択した点を削除");
    pressureDeleteBtn_->setEnabled(false);
    auto *resetBtn = new QPushButton("リセット");
    btnCol->addWidget(pressureDeleteBtn_);
    btnCol->addWidget(resetBtn);
    btnCol->addStretch();
    curveRow->addLayout(btnCol);
    curveRow->addStretch();
    v->addLayout(curveRow);

    connect(pressureCurveEditor_, &ToneCurveEditor::selectionChanged,
            pressureDeleteBtn_, &QPushButton::setEnabled);
    connect(pressureDeleteBtn_, &QPushButton::clicked,
            pressureCurveEditor_, &ToneCurveEditor::removeSelected);
    connect(resetBtn, &QPushButton::clicked, pressureCurveEditor_, &ToneCurveEditor::reset);
    // ToneCurveEditor は現在の制御点を取り出すAPIを持たないので、変更のたびに
    // ここで受けておき、OK時(onAccept)にそのまま保存する。
    connect(pressureCurveEditor_, &ToneCurveEditor::pointsChanged,
            this, [this](const QVector<QPointF> &pts) { current.pressureCurve = pts; });

    auto *note = new QLabel(
        "※このカーブを通した後に、さらに各ツールの設定にある筆圧カーブが掛かります。\n"
        "　ここはタブレット自体の硬さの癖をならすため、ツール側はブラシごとの効き方を"
        "作るため、という使い分けを想定しています。");
    note->setWordWrap(true);
    v->addWidget(note);

    v->addStretch();
    return page;
}

// ===========================================================================
// ファイルページ
// ===========================================================================
QWidget *SettingsDlg::createFilePage()
{
    QWidget     *page = new QWidget();
    QFormLayout *form = new QFormLayout(page);

    auto *projectRow = new QHBoxLayout();
    projectDirEdit = new QLineEdit(); {
        projectDirEdit->setText(current.projectSaveDir);
    }
    auto *projectBrowseBtn = new QPushButton("参照...");
    projectRow->addWidget(projectDirEdit, 1);
    projectRow->addWidget(projectBrowseBtn);
    form->addRow("プロジェクトファイル保存先", projectRow);
    connect(projectBrowseBtn, &QPushButton::clicked, this, &SettingsDlg::browseProjectDir);

    auto *exportRow = new QHBoxLayout();
    exportDirEdit = new QLineEdit(); {
        exportDirEdit->setText(current.exportSaveDir);
    }
    auto *exportBrowseBtn = new QPushButton("参照...");
    exportRow->addWidget(exportDirEdit, 1);
    exportRow->addWidget(exportBrowseBtn);
    form->addRow("書き出しファイル保存先", exportRow);
    connect(exportBrowseBtn, &QPushButton::clicked, this, &SettingsDlg::browseExportDir);

    return page;
}

void SettingsDlg::browseProjectDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "プロジェクトファイル保存先", projectDirEdit->text());
    if (!dir.isEmpty()) projectDirEdit->setText(QDir::toNativeSeparators(dir));
}

void SettingsDlg::browseExportDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "書き出しファイル保存先", exportDirEdit->text());
    if (!dir.isEmpty()) exportDirEdit->setText(QDir::toNativeSeparators(dir));
}

// ===========================================================================
// OK 押下時: 全ページの値を current にまとめてから保存
// ===========================================================================
void SettingsDlg::onAccept()
{
    current.defaultCanvasW   = canvasWSpin->value();
    current.defaultCanvasH   = canvasHSpin->value();
    current.autoSaveEnabled  = autoSaveCheck->isChecked();
    current.autoSaveInterval = autoSaveIntervalSpin->value();

    current.undoHistoryLimit = undoLimitSpin->value();
    current.hueTwist         = hueTwistSpin->value();

    current.projectSaveDir = projectDirEdit->text();
    current.exportSaveDir  = exportDirEdit->text();

    saveValues(current);
    accept();
}
