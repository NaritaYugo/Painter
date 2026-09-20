#include "app/MainWindow.h"
#include "app/MainWindowHelpers.h"
#include "docks/LayerDock.h"
#include "docks/ColorCircleDock.h"
#include "docks/ToolDock.h"
#include "docks/ToolPropertyDock.h"
#include "docks/ToolPresetDock.h"
#include "docks/BrushSizeDock.h"
#include "docks/NavigatorDock.h"
#include "components/DockTitleBar.h"
#include "components/ColorWheelWidget.h" // 色相ツイスト(環境設定)の反映
#include "app/StartPage.h"
#include "canvas/CanvasPane.h"
#include "canvas/CanvasTabBar.h"
#include "canvas/CanvasTabPage.h"
#include "app/NativeWindowLog.h"
#include "document/RecentFiles.h"
#include "io/AbrCodec.h"
#include "io/AbrPenMapping.h"
#include "dialogs/CalibrationDialog.h"

#include <QScreen>
#include <QShowEvent>
#include <QMenuBar>
#include <QToolButton>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <QToolBar>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QAction>
#include <QActionGroup>
#include <QShortcut>
#include <QFileDialog>
#include <QInputDialog>
#include <QColorDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QPushButton>
#include <QDialog>
#include <QTextBrowser>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTabWidget>
#include <QTabBar>
#include <QStackedWidget>
#include <QSplitter>
#include <QTimer>
#include <QStyle>
#include <QMouseEvent>
#include <QCursor>
#include <QProgressDialog>
#include <QElapsedTimer>

using MainWindowHelpers::brushSizeFor;
using MainWindowHelpers::setBrushSizeFor;
// MainWindowFiles.cpp: responsibilities separated from the MainWindow integration hub.

void MainWindow::importPenSettingsFromAbr()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "ブラシ設定を読み込み", QString(),
        "Photoshopブラシ (*.abr);;すべてのファイル (*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "読み込み失敗", "ファイルを開けませんでした。");
        return;
    }
    const AbrReadResult res = AbrCodec::read(f.readAll());

    // 未知のキーを後から調べられるよう、記述子の中身をログへ落としておく
    // (対応付けはファイル側の公開仕様が無く推定を含むため)。
    if (WinLog::enabled()) {
        WINLOG(QStringLiteral("ABR read: version=%1.%2 brushes=%3 %4")
                   .arg(res.version).arg(res.subversion).arg(res.brushes.size())
                   .arg(res.ok ? QString() : "error: " + res.error));
        for (const QString &line : res.dumpLines) WINLOG("ABR   " + line);
    }

    if (!res.ok || res.brushes.isEmpty()) {
        QMessageBox::warning(this, "読み込み失敗",
            QStringLiteral("このファイルからブラシ設定を読み取れませんでした。\n%1").arg(res.error));
        return;
    }

    // 複数本入っていることが多いので、どれを取り込むか選ばせる。
    int index = 0;
    if (res.brushes.size() > 1) {
        QStringList names;
        for (int i = 0; i < res.brushes.size(); i++) {
            const AbrBrush &b = res.brushes[i];
            names << QStringLiteral("%1: %2%3").arg(i + 1)
                         .arg(b.name.isEmpty() ? QStringLiteral("(名前なし)") : b.name)
                         .arg(b.tip.isNull() ? QString()
                                             : QStringLiteral("  [先端画像 %1×%2]")
                                                   .arg(b.tip.width()).arg(b.tip.height()));
        }
        bool okPressed = false;
        const QString chosen = QInputDialog::getItem(this, "ブラシを選択",
            QStringLiteral("%1本のブラシが入っています。取り込むものを選んでください:")
                .arg(res.brushes.size()),
            names, 0, false, &okPressed);
        if (!okPressed) return;
        index = qMax(0, names.indexOf(chosen));
    }

    QStringList notes = res.notes;
    notes += AbrPenMapping::applyToPen(res.brushes[index], toolCfg->pen());

    toolPropertyDock->refreshFromSettings();
    brushSizeDock->syncSize(toolCfg->pen().size());
    if (glWidget) glWidget->updateCursor();

    QString msg = QStringLiteral("「%1」をペンの設定へ取り込みました。")
                      .arg(res.brushes[index].name.isEmpty() ? "(名前なし)" : res.brushes[index].name);
    if (!notes.isEmpty()) msg += "\n\n・" + notes.join("\n・");
    QMessageBox::information(this, "読み込み完了", msg);
}

void MainWindow::exportPenSettingsToAbr()
{
    QString path = QFileDialog::getSaveFileName(
        this, "ブラシ設定を書き出し", QStringLiteral("brush.abr"),
        "Photoshopブラシ (*.abr)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".abr", Qt::CaseInsensitive)) path += ".abr";

    const QString name = QFileInfo(path).completeBaseName();
    const AbrBrush brush = AbrPenMapping::fromPen(toolCfg->pen(), name);
    const QByteArray data = AbrCodec::write(brush);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        QMessageBox::warning(this, "書き出し失敗", "ファイルを書き込めませんでした。");
        return;
    }
    f.close();

    QString msg = QStringLiteral("「%1」を書き出しました。").arg(name);
    msg += brush.tip.isNull()
             ? QStringLiteral("\n(手続き的な丸ブラシとして出力)")
             : QStringLiteral("\n(先端画像 %1×%2 を含む)").arg(brush.tip.width()).arg(brush.tip.height());
    const QStringList lost = AbrPenMapping::unmappedFromPen(toolCfg->pen());
    if (!lost.isEmpty())
        msg += QStringLiteral("\n\n次の設定は.abrに対応する項目が無いため含まれません:\n・%1")
                   .arg(lost.join("\n・"));
    QMessageBox::information(this, "書き出し完了", msg);
}

void MainWindow::exportImage()
{
    QDir exportDir(SettingsDialog::loadValues().exportSaveDir);
    if (!exportDir.exists()) {
        exportDir.mkpath(".");
    }

    QString docName = "Canvas";
    if (!glWidget->filePath().isEmpty()) {
        QFileInfo projectFileInfo(glWidget->filePath());
        docName = projectFileInfo.baseName();
    }

    QString candidatePath = exportDir.filePath(docName + "_out" + ".png");
    QString defaultPath = getUniqueFilePath(candidatePath, 2);

    QString selectedFilter;
    QString selectedPath = QFileDialog::getSaveFileName(
        this,
        "画像として書き出し",
        defaultPath,
        "PNG 画像 (*.png);;JPEG 画像 (*.jpg *.jpeg);;Photoshop 形式 (*.psd);;すべてのファイル (*)",
        &selectedFilter
    );

    if (selectedPath.isEmpty()) return;

    QString finalPath = selectedPath;

    // 拡張子が省略されている場合は、選んだフィルタに応じて補う
    if (QFileInfo(finalPath).suffix().isEmpty()) {
        if (selectedFilter.contains("*.psd")) finalPath += ".psd";
        else if (selectedFilter.contains("*.jpg")) finalPath += ".jpg";
        else finalPath += ".png";
    }

    if (QFileInfo(finalPath).suffix().compare("psd", Qt::CaseInsensitive) == 0) {
        PsdCodec codec(glWidget);
        if (!codec.save(finalPath)) {
            QMessageBox::warning(this, "書き出し失敗", codec.lastError());
            return;
        }
        return;
    }

    if (!glWidget->exportToImage(finalPath)) {
        QMessageBox::warning(this, "書き出し失敗", "画像の書き出しに失敗しました。");
        return;
    }
}

void MainWindow::saveFile()
{
    if (glWidget->filePath().isEmpty()) {
        saveFileAs();
        return;
    }

    CanvasSerializer serializer(glWidget);
    if (!serializer.save(glWidget->filePath())) {
        QMessageBox::warning(this, "保存失敗", serializer.lastError());
        return;
    }
    glWidget->markSaved();
    RecentFiles::touch(glWidget->filePath());
    updateWindowTitle();
}
 
void MainWindow::saveFileAs()
{
    QDir projectDir(SettingsDialog::loadValues().projectSaveDir);
    if (!projectDir.exists()) {
        projectDir.mkpath(".");
    }

    QString candidatePath = projectDir.filePath("Canvas.tplo");
    QString defaultPath = getUniqueFilePath(candidatePath, 3); // ここで untitled_01.tplo などになる

    QString path = QFileDialog::getSaveFileName(
        this, 
        "名前を付けて保存",
        defaultPath,
        "Tiepolo ファイル (*.tplo)"
    );
    if (path.isEmpty()) return;

    QString finalPath = path;

    CanvasSerializer serializer(glWidget);
    if (!serializer.save(finalPath)) {
        QMessageBox::warning(this, "保存失敗", serializer.lastError());
        return;
    }

    glWidget->setFilePath(finalPath);
    glWidget->markSaved();
    RecentFiles::touch(finalPath);
    updateWindowTitle();
}

// 「画像を追加」: 画像ファイルを選び、現在開いているドキュメントの
// 現在選択中のレイヤーの直上に新規レイヤーとして挿入する
// (loadFile()と違い、新しいタブ/ドキュメントは作らない)。
void MainWindow::addImageLayer()
{
    if (!glWidget || glWidget->document().layerCount() == 0) return;

    const QString path = QFileDialog::getOpenFileName(
        this, "画像を追加",
        QString(),
        "画像ファイル (*.png *.jpg *.jpeg *.bmp)"
    );
    if (path.isEmpty()) return;

    const QImage image(path);
    if (image.isNull() || !glWidget->insertImageLayerAboveActive(image, QFileInfo(path).completeBaseName())) {
        QMessageBox::warning(this, "読み込み失敗", "画像ファイルを読み込めませんでした。");
        return;
    }
}

void MainWindow::loadFile()
{
    loadFileIntoTab(nullptr); // 開くパスが確定してから追加先タブを決める
}

// pageがnullptrなら、パスが確定した時点でtargetTabPageForNewDocument()に決めさせる
// (キャンセルされたときに空のタブを作ってしまわないようにするため)。
void MainWindow::loadFileIntoTab(CanvasTabPage *page)
{
    QString path = QFileDialog::getOpenFileName(
        this, "ファイルを開く",
        QString(),
        "対応ファイル (*.tplo *.psd *.png *.jpg *.jpeg *.bmp);;"
        "Tiepolo ファイル (*.tplo);;Photoshop 形式 (*.psd);;"
        "画像ファイル (*.png *.jpg *.jpeg *.bmp)"
    );
    if (path.isEmpty()) return;

    if (!page) page = targetTabPageForNewDocument();
    openFileIntoTab(page, path);
}

void MainWindow::openFilesFromArgs(const QStringList &paths)
{
    for (const QString &p : paths) {
        if (p.isEmpty() || !QFileInfo::exists(p)) continue;
        openFileIntoNewTab(p);
    }
}

void MainWindow::openFileIntoNewTab(const QString &path)
{
    openFileIntoTab(targetTabPageForNewDocument(), path);
}

// pageをキャンバスに変えてpathを読み込む。スタートページのカードのクリック
// (=そのタブ自身をキャンバスにする)と、メニューの「開く」の両方から使う。
void MainWindow::openFileIntoTab(CanvasTabPage *page, const QString &path)
{
    if (!page) return;
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool isPsd = suffix == "psd";
    const bool isImage = suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "bmp";

    // CanvasWidgetは作られた直後はinitializeGL()完了(=シェーダーコンパイル・テクスチャ確保
    // 完了)が非同期なため、CanvasWidget::initialized を待ってから実際の読み込みを行う。
    CanvasWidget *gl = ensureCanvas(page);
    connect(gl, &CanvasWidget::initialized, gl, [this, gl, path, isPsd, isImage]() {
        // CanvasWidget::initialized はinitializeGL()の最後(=Qtが内部でこのウィジェットの
        // 初回paint/exposeイベントを処理している最中)に同期的にemitされる。この
        // コールスタックの中でQCoreApplication::processEvents()を呼んでイベント
        // ループを再入させると、initializeGL/paintGLへの再入(GLコンテキストの
        // 競合)で応答なし・クラッシュを招く。QTimer::singleShot(0, ...)で次の
        // イベントループの繰り返しまで遅延させ、Qtの内部処理を抜けてから実行する。
        QTimer::singleShot(0, gl, [this, gl, path, isPsd, isImage]() {
        // 進捗表示中にQCoreApplication::processEvents()でイベントループを回すと、
        // 他のタブ(や自分自身)のCanvasWidgetへ塗り直しイベントが配送されうる。
        // codec.load()/serializer.load()は「1つのGLコンテキストを掴んだまま
        // 一連のGL呼び出しを行う」ことを前提にしており、その最中にpaintGL()が
        // 割り込むとコンテキスト状態が壊れて応答なし/クラッシュにつながる。
        // 読み込みが終わるまで全タブのCanvasWidgetの再描画を止めて、これを防ぐ。
        struct DisableGLRepaintGuard {
            QVector<CanvasWidget*> widgets;
            explicit DisableGLRepaintGuard(const QVector<CanvasWidget *> &all) {
                for (CanvasWidget *w : all) {
                    widgets.append(w);
                    w->setUpdatesEnabled(false);
                }
            }
            ~DisableGLRepaintGuard() {
                for (CanvasWidget *w : widgets) w->setUpdatesEnabled(true);
            }
        } noRepaint(allCanvasWidgets());

        // 画像1枚(png/jpg等)はレイヤー展開が無く基本軽いため対象外。tplo/psdは
        // レイヤー数・タイル数に比例して重くなり得るため進捗バーを出す。
        // setMinimumDurationにより、実際に指定時間以上かかった場合だけ表示される
        // (短時間で終わればちらつかせず一切出ない)。
        QProgressDialog progress("ファイルを読み込んでいます…", QString(), 0, 100, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(500);
        progress.setValue(0);
        // 毎レイヤー呼ばれうるので、イベントループを回す(processEvents)のは
        // 一定間隔おきに間引く(応答性は保ちつつ、無駄なオーバーヘッドを避ける)。
        QElapsedTimer pumpTimer;
        pumpTimer.start();
        auto reportProgress = [&progress, &pumpTimer](int current, int total) {
            if (total <= 0) return;
            // 間引きの判定はsetValue()より前に行う必要がある。QProgressDialogが
            // モーダル(上でWindowModalにしている)のとき、setValue()は内部で
            // processEvents()を呼ぶため、setValue()を毎回通してしまうと
            // 「processEvents()だけ間引く」つもりが実際には全く間引かれず、
            // タイル1枚ごとにイベントループが回って読み込み時間の大半を食う。
            // 完了時(current==total)だけは必ず反映して表示を100%にする。
            if (current < total && pumpTimer.elapsed() < 60) return;
            pumpTimer.restart();
            progress.setMaximum(total);
            progress.setValue(current);
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        };

        if (isPsd) {
            PsdCodec codec(gl);
            codec.progressCallback = reportProgress;
            if (!codec.load(path)) {
                QMessageBox::warning(this, "読み込み失敗", codec.lastError());
                return;
            }
            // PSDはTiepolo独自形式ではないため、そのまま上書き保存はできない。
            // 「保存」時に改めて名前を付けて保存(.tplo)させるため、現在のパスは持たない
            // (未保存状態のまま=タブ見出し/タイトルバーに変更ありの印が出る、という扱いでよい)。
            gl->setFilePath(QString());
        } else if (isImage) {
            const QImage image(path);
            // 単一の画像ファイル(ドキュメント形式ではない)を開いた場合は、
            // レイヤー構成等の情報を持たないため、レイヤーを1枚だけ作りそこへセットする。
            if (image.isNull() || !gl->loadImageAsSingleLayer(image, QFileInfo(path).completeBaseName())) {
                QMessageBox::warning(this, "読み込み失敗", "画像ファイルを読み込めませんでした。");
                return;
            }
            // PSDと同様、Tiepolo独自形式ではないためそのまま上書き保存はできない。
            gl->setFilePath(QString());
        } else {
            CanvasSerializer serializer(gl);
            serializer.progressCallback = reportProgress;
            if (!serializer.load(path)) {
                QMessageBox::warning(this, "読み込み失敗", serializer.lastError());
                return;
            }
            gl->setFilePath(path);
            RecentFiles::touch(path);
        }
        updateTabLabel(gl);
        if (gl == glWidget) updateWindowTitle();
        }); // QTimer::singleShot(0, ...) 終わり
    }, Qt::SingleShotConnection);

    // 接続を張り終えたのでキャンバス表示へ切り替える。ここで初めてCanvasWidgetが表示され、
    // initializeGL() → 上のinitializedハンドラ → 読み込み、の順で動く。
    page->showCanvas();
    activateTabPage(page);
    updateCentralPage();
}

// ファイルパスがあればそのファイル名、無ければ仮の名前("Canvas001"等、無ければ「無題」)
