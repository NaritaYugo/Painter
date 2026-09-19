#include "widgets/GLWidget.h"
#include "backend/CanvasSerializer.h"
#include "backend/CanvasDocument.h"

#include <QFile>
#include <QSaveFile>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QBuffer>
#include <QtConcurrent>


static constexpr char MAGIC[4] = {'T', 'P', 'L', 'O'};
static constexpr int  FORMAT_VERSION = 6; // プレビュー画像埋め込み対応で version 6 に上げる
static constexpr int  kPreviewMaxSize = 256; // 埋め込みプレビューの長辺の上限(px)
static constexpr quint32 kPreviewMaxBytes = 8 * 1024 * 1024; // 壊れたファイル対策の上限

// GLWidget::beginBulkLayerImport()/endBulkLayerImport()のRAIIラッパー。
// レイヤー1枚ごとのaddLayer()がlayersChanged()を発行すると、LayerDock/NavigatorDock側で
// フルキャンバス合成を伴う重い再描画がレイヤー枚数ぶん連鎖する(O(レイヤー数^2)相当の
// 重さになる)。読み込み中はこれを抑制し、endBulkLayerImport()で最後に1回だけ通知する。
// ループ内には検証エラーによる早期returnが複数あるため、スコープを抜けるパス全てで
// 確実にendBulkLayerImport()が呼ばれるようRAIIにしてある(生のbegin/end対応だと
// 早期returnのたびに書き忘れる恐れがあるため)。
namespace {
struct BulkLayerImportGuard {
    explicit BulkLayerImportGuard(GLWidget *gl) : gl_(gl) { gl_->beginBulkLayerImport(); }
    ~BulkLayerImportGuard() { gl_->endBulkLayerImport(); }
    GLWidget *gl_;
};
}

CanvasSerializer::CanvasSerializer(GLWidget *gl)
    : gl_(gl)
{}

// ===========================================================================
// 保存
// ===========================================================================
bool CanvasSerializer::save(const QString &path)
{
    gl_->makeCurrent();
    const CanvasDocument &doc = gl_->document();

    // ------------------------------------------------------------------
    // 1. 全レイヤーの全タイルピクセルデータを GPU から読み出す
    //    (各レイヤーは自分自身の矩形(originTx/originTy起点、tilesX x tilesY)だけを持つ。
    //    キャンバスより大きい/はみ出したレイヤーもそのまま保存する)
    // ------------------------------------------------------------------
    struct TileData {
        int        layerIndex;
        int        tx, ty; // レイヤーローカルのタイル座標(マスクはキャンバス基準)
        QByteArray raw;        // GPUから読み出した直後の生RGBA8(TILE_SIZE*TILE_SIZE*4)
        QByteArray compressed; // qCompress 済み(この後並列に埋める)
        bool       isMask;     // レイヤーマスクのタイルか
    };
    QVector<TileData> tileDataList;

    // GPU読み出しはGLコンテキストに紐づくためこのスレッドで直列に行う必要があるが、
    // readSlicePixels()をタイルごとに逐次呼ぶとglReadPixelsのたびにCPU側がGPUの
    // 完了を待ってしまう。先に全タイルのslice番号を集めてreadSlicePixelsBatch()
    // (PBOのリングバッファでパイプライン化)でまとめて読み出すことで、そのストールを
    // 削減する。その後のqCompress()はタイルごとに完全に独立しておりCPUバウンドの
    // 重い処理(キャンバスが大きい・レイヤーが多いほど直列だと保存全体を長く塞いで
    // しまう)なので、別途並列圧縮する。
    // 収集順は、下のJSON組み立てループが消費する順(レイヤーごとに 本体タイル →
    // マスクタイル)と一致させておく必要がある。
    QVector<int> allSlices;
    for (int li = 0; li < doc.layerCount(); li++) {
        const Layer &layer = doc.layers[li];
        for (int ty = 0; ty < layer.tilesY(); ty++) {
            for (int tx = 0; tx < layer.tilesX(); tx++) {
                int si = layer.tiles[ty][tx];
                tileDataList.append({ li, tx, ty, QByteArray(), QByteArray(), false });
                allSlices.append(si);
            }
        }
        // レイヤーマスクの中身も保存する(以前は hasMask のフラグだけ書いていて実体を
        // 保存していなかったため、保存→読み込みでマスクが失われていた。さらに読み込み側が
        // フラグだけ立てていたので、マスク付きレイヤーを含むファイルは合成時に落ちていた)。
        if (layer.hasMask) {
            for (int ty = 0; ty < layer.maskTilesY(); ty++) {
                for (int tx = 0; tx < layer.maskTilesX(); tx++) {
                    tileDataList.append({ li, tx, ty, QByteArray(), QByteArray(), true });
                    allSlices.append(layer.maskTiles[ty][tx]);
                }
            }
        }
    }

    const QVector<QByteArray> rawTiles = gl_->readSlicePixelsBatch(allSlices);
    for (int i = 0; i < tileDataList.size(); i++)
        tileDataList[i].raw = rawTiles[i];

    QtConcurrent::blockingMap(tileDataList, [](TileData &td) {
        td.compressed = qCompress(td.raw);
        td.raw.clear(); // 圧縮済みデータだけ残し、ピークメモリを早めに減らす
    });

    // ------------------------------------------------------------------
    // 2. JSON ヘッダを組み立てる
    // ------------------------------------------------------------------
    QJsonObject root;
    root["version"]     = FORMAT_VERSION;
    root["canvasWidth"] = gl_->canvasWidth();
    root["canvasHeight"]= gl_->canvasHeight();
    root["tileSize"]    = TILE_SIZE;
    root["wrapX"]       = doc.wrapX();
    root["wrapY"]       = doc.wrapY();

    QJsonArray layersArray;
    // 2GB超の圧縮ピクセルデータ(巨大キャンバス/大量レイヤー)でもオフセットが
    // オーバーフローしないよう qint64 で扱う(JSON上はどちらにせよ数値として
    // 保存されるため、旧バージョンのファイルとの互換性には影響しない)。
    qint64 dataOffset = 0;
    int    dataIdx    = 0;

    for (int li = 0; li < doc.layerCount(); li++) {
        const Layer &layer = doc.layers[li];

        QJsonObject layerObj;
        layerObj["name"]      = layer.name;
        layerObj["opacity"]   = (double)layer.opacity;
        layerObj["visible"]   = layer.visible;
        layerObj["blendMode"] = (int)layer.blendMode;
        layerObj["clipping"]  = layer.clipping;
        layerObj["layerType"] = (int)layer.layerType;
        layerObj["hasMask"]   = layer.hasMask;

        // 調整レイヤー(PSDのAdjustment Layerに近いデータ形式: 種類(kind) + その種類
        // 固有のパラメータをネストしたオブジェクトとして持つ)。他の種類のレイヤーには
        // 存在しないキーなので、旧バージョンのファイルとの互換性にも影響しない。
        if (layer.layerType == LayerType::Adjustment) {
            QJsonObject adjObj;
            adjObj["kind"] = (int)layer.adjustment.kind;
            if (layer.adjustment.kind == AdjustmentKind::BrightnessContrast) {
                adjObj["brightness"] = layer.adjustment.brightness;
                adjObj["contrast"]   = layer.adjustment.contrast;
            } else if (layer.adjustment.kind == AdjustmentKind::HueSaturation) {
                adjObj["hue"]        = layer.adjustment.hue;
                adjObj["saturation"] = layer.adjustment.saturation;
                adjObj["lightness"]  = layer.adjustment.lightness;
            } else if (layer.adjustment.kind == AdjustmentKind::ColorBalance) {
                adjObj["cyan"]    = layer.adjustment.cyan;
                adjObj["magenta"] = layer.adjustment.magenta;
                adjObj["yellow"]  = layer.adjustment.yellow;
            } else if (layer.adjustment.kind == AdjustmentKind::ToneCurve) {
                QJsonArray ptsArr;
                for (const QPointF &pt : layer.adjustment.curvePoints) {
                    QJsonObject ptObj;
                    ptObj["x"] = pt.x();
                    ptObj["y"] = pt.y();
                    ptsArr.append(ptObj);
                }
                adjObj["curvePoints"] = ptsArr;
            } else if (layer.adjustment.kind == AdjustmentKind::GradientMap) {
                QJsonArray stopsArr;
                for (const AdjustmentGradientStop &s : layer.adjustment.gradientStops) {
                    QJsonObject stopObj;
                    stopObj["pos"]   = s.pos;
                    stopObj["color"] = (qint64)s.color.rgba();
                    stopsArr.append(stopObj);
                }
                adjObj["gradientStops"] = stopsArr;
            }
            layerObj["adjustment"] = adjObj;
        }

        // フィルターレイヤー(PSDには相当する種別が無いので.tploにのみ保存される)。
        // 調整レイヤーと同じく「種類(kind) + その種類固有のパラメータ」の形。
        if (layer.layerType == LayerType::Filter) {
            QJsonObject fltObj;
            fltObj["kind"] = (int)layer.filter.kind;
            if (layer.filter.kind == FilterKind::ChromaticAberration) {
                fltObj["caMode"]       = layer.filter.caMode;
                fltObj["caAngleDeg"]   = layer.filter.caAngleDeg;
                fltObj["caDistancePx"] = layer.filter.caDistancePx;
                fltObj["caCenterU"]    = layer.filter.caCenterU;
                fltObj["caCenterV"]    = layer.filter.caCenterV;
            } else if (layer.filter.kind == FilterKind::GaussianBlur) {
                fltObj["blurRadiusPx"] = layer.filter.blurRadiusPx;
            } else if (layer.filter.kind == FilterKind::MotionBlur) {
                fltObj["mbMode"]         = layer.filter.mbMode;
                fltObj["mbAngleDeg"]     = layer.filter.mbAngleDeg;
                fltObj["mbDistancePx"]   = layer.filter.mbDistancePx;
                fltObj["mbCenterU"]      = layer.filter.mbCenterU;
                fltObj["mbCenterV"]      = layer.filter.mbCenterV;
                fltObj["mbAngleSpanDeg"] = layer.filter.mbAngleSpanDeg;
            } else if (layer.filter.kind == FilterKind::LensBlur) {
                fltObj["lbRadiusPx"]        = layer.filter.lbRadiusPx;
                fltObj["lbBlades"]          = layer.filter.lbBlades;
                fltObj["lbBladeRotDeg"]     = layer.filter.lbBladeRotDeg;
                fltObj["lbHighlightBoost"] = layer.filter.lbHighlightBoost;
                fltObj["lbThreshold"]       = layer.filter.lbThreshold;
            } else if (layer.filter.kind == FilterKind::Mosaic) {
                fltObj["mzBlockSize"] = layer.filter.mzBlockSize;
            } else if (layer.filter.kind == FilterKind::Noise) {
                fltObj["nsStrength"]   = layer.filter.nsStrength;
                fltObj["nsMonochrome"] = layer.filter.nsMonochrome;
                fltObj["nsGrainPx"]    = layer.filter.nsGrainPx;
                fltObj["nsSeed"]       = (double)layer.filter.nsSeed;
            }
            layerObj["filter"] = fltObj;
        }

        // テキストレイヤー(PSDのType Tool Object "TySh"に近いデータ形式: 文字列+
        // フォント+色+配置をピクセルとは独立して保持する)。実ピクセル自体は
        // 他のレイヤーと同じ通常のタイルとして下で保存されるので、再読込したファイルは
        // このtextBoxesブロックが無くても表示上は成立する(再編集時にのみ必要)。
        // 1レイヤーに複数の独立したテキストボックスを持てるため配列で保存する。
        if (layer.layerType == LayerType::Text) {
            QJsonArray boxesArr;
            for (const TextParams &box : layer.textBoxes) {
                QJsonObject boxObj;
                boxObj["text"]       = box.text;
                boxObj["fontFamily"] = box.fontFamily;
                boxObj["fontSize"]   = box.fontSize;
                boxObj["color"]      = (qint64)box.color.rgba();
                boxObj["bold"]       = box.bold;
                boxObj["italic"]     = box.italic;
                boxObj["cx"]         = box.cx;
                boxObj["cy"]         = box.cy;
                boxObj["width"]      = box.width;
                boxObj["height"]     = box.height;
                boxObj["rotation"]   = box.rotation;
                boxObj["scale"]      = box.scale;
                boxesArr.append(boxObj);
            }
            layerObj["textBoxes"] = boxesArr;
        }

        // 単色レイヤーの色。実ピクセルを持たないため、このキーだけが色の情報源。
        if (layer.layerType == LayerType::SolidColor) {
            layerObj["solidColor"] = (qint64)layer.solidColor.rgba();
        }

        layerObj["originTx"]  = layer.originTx;
        layerObj["originTy"]  = layer.originTy;
        layerObj["tilesX"]    = layer.tilesX();
        layerObj["tilesY"]    = layer.tilesY();
        // フォルダーが何枚を内包しているか。これを保存していなかったため、保存→読み込みで
        // 階層が失われ、中身がフォルダーと同じ階層に並んでしまっていた。
        if (layer.layerType == LayerType::Folder)
            layerObj["childCount"] = layer.childCount;

        // タイルごとの dataOffset/dataSize を配列で持つ(座標はレイヤーローカル)
        QJsonArray tilesArray;
        for (int ty = 0; ty < layer.tilesY(); ty++) {
            for (int tx = 0; tx < layer.tilesX(); tx++) {
                const QByteArray &comp = tileDataList[dataIdx].compressed;
                QJsonObject tileObj;
                tileObj["tx"]         = tx;
                tileObj["ty"]         = ty;
                tileObj["dataOffset"] = dataOffset;
                tileObj["dataSize"]   = (qint64)comp.size();
                tilesArray.append(tileObj);
                dataOffset += comp.size();
                dataIdx++;
            }
        }
        layerObj["tiles"] = tilesArray;

        // レイヤーマスクのタイル(キャンバス全体を覆う固定サイズ)。上の収集ループと
        // 同じ順(本体タイルの直後)で並んでいる。
        if (layer.hasMask) {
            QJsonArray maskArray;
            for (int ty = 0; ty < layer.maskTilesY(); ty++) {
                for (int tx = 0; tx < layer.maskTilesX(); tx++) {
                    const QByteArray &comp = tileDataList[dataIdx].compressed;
                    QJsonObject tileObj;
                    tileObj["tx"]         = tx;
                    tileObj["ty"]         = ty;
                    tileObj["dataOffset"] = dataOffset;
                    tileObj["dataSize"]   = (qint64)comp.size();
                    maskArray.append(tileObj);
                    dataOffset += comp.size();
                    dataIdx++;
                }
            }
            layerObj["maskTiles"] = maskArray;
        }
        layersArray.append(layerObj);
    }

    root["activeLayerIndex"] = doc.activeLayerIndex();
    root["layers"]           = layersArray;

    QByteArray headerJson = QJsonDocument(root).toJson(QJsonDocument::Compact);

    // ------------------------------------------------------------------
    // 2.5. プレビュー画像(合成済みキャンバスの縮小PNG)を作る。
    //      「最近使ったファイル」一覧等がヘッダ/ピクセルデータを読まずに
    //      プレビューだけ高速に取り出せるよう、ファイル先頭寄りに置く。
    // ------------------------------------------------------------------
    QImage previewImg = gl_->exportCanvas();
    if (!previewImg.isNull() &&
        (previewImg.width() > kPreviewMaxSize || previewImg.height() > kPreviewMaxSize)) {
        previewImg = previewImg.scaled(kPreviewMaxSize, kPreviewMaxSize,
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray previewBytes;
    if (!previewImg.isNull()) {
        QBuffer buf(&previewBytes);
        buf.open(QIODevice::WriteOnly);
        previewImg.save(&buf, "PNG");
    }

    // ------------------------------------------------------------------
    // 3. ファイルに書き出す
    // ------------------------------------------------------------------
    // QFileへ直接上書きすると、書き込み途中でディスク容量不足/クラッシュ等が
    // 起きた場合に既存の保存済みファイルごと壊れてしまう(中途半端な内容で
    // 上書きされたまま残る)。QSaveFileは実際には一時ファイルへ書き込み、
    // commit()が成功したときだけ元のパスへアトミックにリネームするため、
    // 失敗時は元のファイルがそのまま無傷で残る。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError("ファイルを開けませんでした: " + path);
        return false;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);

    auto writeChecked = [&file](const char *data, qint64 size) {
        return file.write(data, size) == size;
    };

    bool ok = writeChecked(MAGIC, 4);

    quint32 previewSize = static_cast<quint32>(previewBytes.size());
    out << previewSize;
    ok = ok && (out.status() == QDataStream::Ok);
    ok = ok && writeChecked(previewBytes.constData(), previewBytes.size());

    quint32 headerSize = static_cast<quint32>(headerJson.size());
    out << headerSize;
    ok = ok && (out.status() == QDataStream::Ok);

    ok = ok && writeChecked(headerJson.constData(), headerJson.size());

    for (const TileData &td : tileDataList)
        ok = ok && writeChecked(td.compressed.constData(), td.compressed.size());

    if (!ok) {
        setError("書き込みに失敗しました(ディスク容量不足の可能性があります): " + path);
        file.cancelWriting(); // 一時ファイルを破棄し、元のファイルには一切触れない
        return false;
    }

    if (!file.commit()) {
        setError("ファイルの確定に失敗しました: " + file.errorString());
        return false;
    }
    return true;
}

// ===========================================================================
// 読み込み
// ===========================================================================
bool CanvasSerializer::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError("ファイルを開けませんでした: " + path);
        return false;
    }

    char magic[4];
    if (file.read(magic, 4) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        setError("不正なファイル形式です");
        return false;
    }

    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 previewSize = 0;
    in >> previewSize;
    if (previewSize > kPreviewMaxBytes || file.skip(previewSize) != previewSize) {
        setError("プレビュー画像の読み込みに失敗しました");
        return false;
    }

    quint32 headerSize = 0;
    in >> headerSize;

    QByteArray headerJson = file.read(headerSize);
    if (static_cast<quint32>(headerJson.size()) != headerSize) {
        setError("ヘッダの読み込みに失敗しました");
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(headerJson, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError("JSON パースエラー: " + parseError.errorString());
        return false;
    }
    QJsonObject root = doc.object();

    int version = root["version"].toInt();
    if (version != FORMAT_VERSION) {
        setError(QString("非対応のバージョンです: %1").arg(version));
        return false;
    }

    int canvasW = root["canvasWidth"].toInt();
    int canvasH = root["canvasHeight"].toInt();

    int fileTileSize = root["tileSize"].toInt();
    if (fileTileSize != TILE_SIZE) {
        setError(QString("タイルサイズが一致しません: %1 != %2").arg(fileTileSize).arg(TILE_SIZE));
        return false;
    }

    bool wrapX = root["wrapX"].toBool(false);
    bool wrapY = root["wrapY"].toBool(false);

    QByteArray pixelBlock = file.readAll();
    file.close();

    QJsonArray layersArray = root["layers"].toArray();

    // ------------------------------------------------------------------
    // 事前検証: recreateCanvas()は既存ドキュメント(直前まで開いていたタブの内容)を
    // 丸ごと破棄する破壊的操作なので、それを呼ぶ前に全レイヤー・全タイルのデータが
    // 正しく読めることを検証しておく。こうすることで、ファイルが途中で壊れている/
    // 切り詰められている等の理由で読み込みに失敗しても、既存のタブの内容が失われず
    // 残る(読み込み失敗時に中途半端な状態のタブが残ってしまう問題への対策)。
    // 検証と同時にqUncompressで展開したタイルの生ピクセルを保持しておき、下の
    // 構築フェーズで再度展開する無駄を省く。
    // ------------------------------------------------------------------
    QVector<QVector<QByteArray>> decodedTiles; // decodedTiles[li][ti] = 展開済みRGBA8 (tilesArray[ti]の順)
    decodedTiles.resize(layersArray.size());
    QVector<QVector<QByteArray>> decodedMaskTiles; // 同じくマスクタイル(maskTilesArray[ti]の順)
    decodedMaskTiles.resize(layersArray.size());

    // 進捗通知: タイル単位(検証パス+構築パスの2周ぶん)で数える。レイヤー単位だと、
    // 1レイヤーに大量のタイルを持つ巨大ファイルで「次のコールバックまで」が
    // 長時間空いてしまい、その間イベントループが一切回らずOSに「応答なし」と
    // 判定される(呼び出し側=MainWindowがコールバック内でprocessEvents()する
    // 前提のため)。タイル単位にすることで、どんなファイルでも一定間隔で
    // コールバックが呼ばれるようにする。
    qint64 totalTiles = 0;
    for (int li = 0; li < layersArray.size(); li++) {
        const QJsonObject lo = layersArray[li].toObject();
        totalTiles += lo["tiles"].toArray().size();
        totalTiles += lo["maskTiles"].toArray().size();
    }
    const int progressTotal = (int)qMax<qint64>(1, totalTiles * 2);
    int progressDone = 0;

    // 展開は「①直列で検証しつつ仕事リストを作る → ②並列でqUncompress → ③直列で結果を検証」
    // の3段に分ける。qUncompressはタイルごとに完全に独立したCPUバウンド処理で、保存側の
    // qCompressは既に QtConcurrent で並列化されているのに、読み込み側だけ直列だったため
    // ここが読み込み全体の6割以上を占めていた(実測: 60レイヤー4813タイルで12.1秒/18.2秒)。
    //
    // 並列化にあたっての注意:
    //  ・エラー報告(setError+return false)はワーカースレッドから行えないので、
    //    座標/オフセットの検証は①、展開後サイズの検証は③に分けて直列で行う。
    //  ・progressCallbackは呼び出し側でQProgressDialogを触るためワーカーから呼べない。
    //    一定枚数ごとのチャンクに区切り、チャンクの合間に直列で呼ぶことで、
    //    並列性と進捗表示の両方を保つ。
    //  ・書き込み先はQVectorの要素そのものではなくポインタで受け取る(コンテナの
    //    operator[]はdetachを伴い、複数スレッドから同時に呼ぶと安全でないため)。
    struct DecodeJob {
        const char *src;   // pixelBlock内の圧縮データ先頭(コピーしない)
        qint64      size;
        QByteArray *dst;   // 展開先(事前にresize済みの要素を指す)
        int         li;    // エラーメッセージ用
        int         tx, ty;
        bool        isMask;
    };
    QVector<DecodeJob> jobs;
    jobs.reserve((int)qMin<qint64>(totalTiles, 0x7fffffff));

    for (int li = 0; li < layersArray.size(); li++) {
        QJsonObject layerObj = layersArray[li].toObject();
        int layerTilesX = layerObj["tilesX"].toInt(-1);
        int layerTilesY = layerObj["tilesY"].toInt(-1);
        QJsonArray tilesArray = layerObj["tiles"].toArray();

        QVector<QByteArray> &decoded = decodedTiles[li];
        decoded.resize(tilesArray.size());

        for (int ti = 0; ti < tilesArray.size(); ti++) {
            QJsonObject tileObj = tilesArray[ti].toObject();
            int    tx     = tileObj["tx"].toInt();
            int    ty     = tileObj["ty"].toInt();
            // 2GB超のファイルでもオーバーフローしないようqint64で読む
            // (JSON上はどちらにせよ数値として保存されているだけなので、
            //  intで書かれた旧バージョンのファイルもそのまま読める)。
            qint64 offset = (qint64)tileObj["dataOffset"].toDouble();
            qint64 size   = (qint64)tileObj["dataSize"].toDouble();

            if (tx < 0 || tx >= layerTilesX || ty < 0 || ty >= layerTilesY) {
                setError(QString("タイル座標不正: layer=%1 tx=%2 ty=%3").arg(li).arg(tx).arg(ty));
                return false;
            }
            if (offset < 0 || size < 0 || offset + size > pixelBlock.size()) {
                setError(QString("dataOffset/dataSize が不正: layer=%1 tile=(%2,%3)")
                         .arg(li).arg(tx).arg(ty));
                return false;
            }
            jobs.append({ pixelBlock.constData() + offset, size, &decoded[ti], li, tx, ty, false });
        }

        // マスクタイル(キャンバス全体ぶん)。旧バージョンのファイルにはこのキーが
        // 無いので、その場合は空のまま=マスク無し扱いになる。
        const QJsonArray maskArray = layerObj["maskTiles"].toArray();
        QVector<QByteArray> &decodedMask = decodedMaskTiles[li];
        decodedMask.resize(maskArray.size());
        for (int ti = 0; ti < maskArray.size(); ti++) {
            QJsonObject tileObj = maskArray[ti].toObject();
            qint64 offset = (qint64)tileObj["dataOffset"].toDouble();
            qint64 size   = (qint64)tileObj["dataSize"].toDouble();
            if (offset < 0 || size < 0 || offset + size > pixelBlock.size()) {
                setError(QString("マスクの dataOffset/dataSize が不正: layer=%1 tile=%2").arg(li).arg(ti));
                return false;
            }
            jobs.append({ pixelBlock.constData() + offset, size, &decodedMask[ti], li, ti, 0, true });
        }
    }

    // ② 並列展開(チャンクごとに進捗を通知する)
    {
        constexpr int kChunk = 512;
        for (int base = 0; base < jobs.size(); base += kChunk) {
            const int hi = qMin<int>(jobs.size(), base + kChunk);
            QtConcurrent::blockingMap(jobs.begin() + base, jobs.begin() + hi, [](DecodeJob &j) {
                // fromRawDataはコピーを作らない(mid()だとタイルごとに確保+memcpyが走る)
                *j.dst = qUncompress(QByteArray::fromRawData(j.src, (qsizetype)j.size));
            });
            progressDone += hi - base;
            if (progressCallback) progressCallback(progressDone, progressTotal);
        }
    }

    // ③ 展開結果の検証(ワーカーからはエラーを返せないのでここでまとめて確認する)
    for (const DecodeJob &j : jobs) {
        if (j.dst->size() == TILE_SIZE * TILE_SIZE * 4) continue;
        if (j.isMask)
            setError(QString("マスクの展開後サイズ不正: layer=%1 tile=%2 size=%3")
                     .arg(j.li).arg(j.tx).arg(j.dst->size()));
        else
            setError(QString("展開後サイズ不正: layer=%1 tile=(%2,%3) size=%4")
                     .arg(j.li).arg(j.tx).arg(j.ty).arg(j.dst->size()));
        return false;
    }

    pixelBlock.clear(); // 展開済みデータへ移したので圧縮ブロックはもう不要

    // ------------------------------------------------------------------
    // ここまでの検証を通過して初めて、ドキュメントを再構築する(ファイルに
    // 保存されているサイズに合わせてキャンバス自体を作り直す。呼び出し側の
    // (新規タブ等の)GLWidgetが持つ現在のサイズとは無関係に、どんなサイズの
    // ファイルでも開けるようにする)
    // ------------------------------------------------------------------
    gl_->recreateCanvas(canvasW, canvasH, /*createDefaultLayers=*/false, wrapX, wrapY);

    // これから確保するタイル(スライス)総数を見積もって一括で確保しておく
    // (レイヤーごとの addLayer で少しずつ伸長+コピーが繰り返されるのを避けて高速化する)。
    // 各レイヤーは tilesX*tilesY 枚を連続確保し、マスクを持つレイヤーはさらに
    // キャンバス全体ぶん(canvasTilesX*canvasTilesY 枚)を確保する。
    {
        const int canvasTilesX = (canvasW + TILE_SIZE - 1) / TILE_SIZE;
        const int canvasTilesY = (canvasH + TILE_SIZE - 1) / TILE_SIZE;
        qint64 estTotal = 0;
        for (int li = 0; li < layersArray.size(); li++) {
            QJsonObject lo = layersArray[li].toObject();
            int ltx = lo["tilesX"].toInt(0), lty = lo["tilesY"].toInt(0);
            if (ltx > 0 && lty > 0) estTotal += (qint64)ltx * lty;
            if (lo["hasMask"].toBool(false)) estTotal += (qint64)canvasTilesX * canvasTilesY;
        }
        gl_->reserveTileSlices((int)qMin<qint64>(estTotal, 0x7fffffff));
    }

    // フォルダーのchildCountは全レイヤーを追加し終えてから適用する(途中状態で
    // 矛盾した値が外から見えないようにするため。適用箇所のコメント参照)。
    struct PendingChildCount { int layerIndex; int count; };
    QVector<PendingChildCount> pendingChildCounts;

    // タイルのGPU転送は最後にまとめて行う。書き込み先スライスと中身のポインタを
    // 溜めておき、全レイヤーを追加し終えてから1回で流し込む(呼び出しごとの
    // makeCurrent()やテクスチャのバインドが1回で済み、テクスチャ配列の拡張が
    // すべて終わった後に転送するので順序も単純になる)。
    // decodedTiles/decodedMaskTilesはこの関数のローカルなので、転送まで
    // ポインタの参照先は生きている。
    QVector<int> uploadSlices;
    QVector<const QByteArray *> uploadData;
    auto queueUpload = [&](int slice, const QByteArray *raw) {
        uploadSlices.append(slice);
        uploadData.append(raw);
    };

    {
    // このブロックを抜ける(正常終了/検証エラーによる早期returnいずれも)瞬間に
    // bulkGuardのデストラクタが走り、endBulkLayerImport()が確実に呼ばれる。
    BulkLayerImportGuard bulkGuard(gl_);
    for (int li = 0; li < layersArray.size(); li++) {
        QJsonObject layerObj  = layersArray[li].toObject();
        QString     layerName = layerObj["name"].toString();
        float       opacity   = (float)layerObj["opacity"].toDouble(1.0);
        bool        visible   = layerObj["visible"].toBool(true);
        int         blendMode = layerObj["blendMode"].toInt(0);
        bool        clipping  = layerObj["clipping"].toBool(false);
        LayerType   layerType = static_cast<LayerType>(layerObj["layerType"].toInt(0));
        bool        hasMask   = layerObj["hasMask"].toBool(false);
        int         originTx  = layerObj["originTx"].toInt(0);
        int         originTy  = layerObj["originTy"].toInt(0);
        int         layerTilesX = layerObj["tilesX"].toInt(-1);
        int         layerTilesY = layerObj["tilesY"].toInt(-1);

        // 下から順にJSONへ積んであるので、そのまま末尾(最前面)に追加していけばよい。
        // 保存済みのレイヤー自身の矩形(キャンバスより大きい/はみ出している場合を含む)をそのまま復元する
        // (単色レイヤーはaddLayer内部で強制的に0x0になる)。
        bool ok = gl_->addLayer(layerName, /*insertIndex=*/-1, clipping,
                                 originTx, originTy, layerTilesX, layerTilesY, layerType);
        if (!ok) {
            setError(QString("レイヤー追加失敗: layer=%1").arg(li));
            return false;
        }

        int newLayIdx = gl_->document().layerCount() - 1;
        gl_->document().setLayerOpacity(newLayIdx, opacity);
        gl_->document().setLayerVisible(newLayIdx, visible);
        gl_->document().setLayerBlendMode(newLayIdx, static_cast<BlendMode>(blendMode));
        // フォルダーの内包枚数は「全レイヤーを追加し終えてから」まとめて入れる(下記)。
        // ここで入れてしまうと、中身のレイヤーがまだ追加されていない途中状態で
        // childCount が実際の枚数を超え、その状態で描画(updateLayerSSBOs →
        // computeAncestorFolders)が走ると配列外アクセスになる。
        if (layerType == LayerType::Folder)
            pendingChildCounts.append({ newLayIdx, layerObj["childCount"].toInt(0) });

        // マスク: 以前は hasMask のフラグだけ立てていて実体(maskTiles)を確保して
        // いなかったため、マスク付きレイヤーを含むファイルを開くと合成時に
        // maskTiles[0][0] が範囲外アクセスして落ちていた。
        // ここで実際にタイルを確保し、保存されている中身を書き戻す。
        // マスクの中身が保存されていない古いファイルは、フラグごと落として
        // 「マスク無し」として開く(復元できるデータが無いため)。
        // 非constなoperator[]は(共有されていれば)detachし得る。フラッシュまで要素への
        // ポインタを持ち続けるので、再確保が起きないat()で参照する。
        const QVector<QByteArray> &decodedMask = decodedMaskTiles.at(li);
        if (hasMask && !decodedMask.isEmpty() && gl_->addLayerMask(newLayIdx)) {
            const Layer &ml = gl_->document().layerAt(newLayIdx);
            const QJsonArray maskArray = layerObj["maskTiles"].toArray();
            for (int ti = 0; ti < maskArray.size() && ti < decodedMask.size(); ti++) {
                QJsonObject tileObj = maskArray[ti].toObject();
                const int mtx = tileObj["tx"].toInt();
                const int mty = tileObj["ty"].toInt();
                if (mtx < 0 || mtx >= ml.maskTilesX() || mty < 0 || mty >= ml.maskTilesY()) {
                    setError(QString("マスクのタイル座標不正: layer=%1 tx=%2 ty=%3").arg(li).arg(mtx).arg(mty));
                    return false;
                }
                queueUpload(ml.maskTiles[mty][mtx], &decodedMask[ti]);
            }
        }

        if (layerType == LayerType::Adjustment) {
            QJsonObject adjObj = layerObj["adjustment"].toObject();
            AdjustmentParams adj;
            adj.kind = static_cast<AdjustmentKind>(adjObj["kind"].toInt(0));
            adj.brightness = adjObj["brightness"].toInt(0);
            adj.contrast   = adjObj["contrast"].toInt(0);
            adj.hue        = adjObj["hue"].toInt(0);
            adj.saturation = adjObj["saturation"].toInt(0);
            adj.lightness  = adjObj["lightness"].toInt(0);
            adj.cyan       = adjObj["cyan"].toInt(0);
            adj.magenta    = adjObj["magenta"].toInt(0);
            adj.yellow     = adjObj["yellow"].toInt(0);

            const QJsonArray ptsArr = adjObj["curvePoints"].toArray();
            if (!ptsArr.isEmpty()) {
                QVector<QPointF> pts;
                pts.reserve(ptsArr.size());
                for (const QJsonValue &v : ptsArr) {
                    QJsonObject o = v.toObject();
                    pts.append(QPointF(o["x"].toDouble(0), o["y"].toDouble(0)));
                }
                if (pts.size() >= 2)
                    adj.curvePoints = pts; // 2点未満(壊れたデータ)なら既定の恒等カーブのまま
            }

            const QJsonArray stopsArr = adjObj["gradientStops"].toArray();
            if (!stopsArr.isEmpty()) {
                QVector<AdjustmentGradientStop> stops;
                stops.reserve(stopsArr.size());
                for (const QJsonValue &v : stopsArr) {
                    QJsonObject o = v.toObject();
                    AdjustmentGradientStop s;
                    s.pos   = (float)o["pos"].toDouble(0.0);
                    s.color = QColor::fromRgba((QRgb)o["color"].toVariant().toLongLong());
                    stops.append(s);
                }
                if (stops.size() >= 2)
                    adj.gradientStops = stops; // 2個未満(壊れたデータ)なら既定の黒→白のまま
            }

            gl_->document().layerRef(newLayIdx).adjustment = adj;
        }

        if (layerType == LayerType::Filter) {
            QJsonObject fltObj = layerObj["filter"].toObject();
            FilterParams flt;
            flt.kind         = static_cast<FilterKind>(fltObj["kind"].toInt(0));
            flt.caMode       = fltObj["caMode"].toInt(0);
            flt.caAngleDeg   = (float)fltObj["caAngleDeg"].toDouble(0.0);
            flt.caDistancePx = (float)fltObj["caDistancePx"].toDouble(8.0);
            flt.caCenterU    = (float)fltObj["caCenterU"].toDouble(0.5);
            flt.caCenterV    = (float)fltObj["caCenterV"].toDouble(0.5);
            flt.blurRadiusPx = (float)fltObj["blurRadiusPx"].toDouble(8.0);
            flt.mbMode         = fltObj["mbMode"].toInt(0);
            flt.mbAngleDeg     = (float)fltObj["mbAngleDeg"].toDouble(0.0);
            flt.mbDistancePx   = (float)fltObj["mbDistancePx"].toDouble(24.0);
            flt.mbCenterU      = (float)fltObj["mbCenterU"].toDouble(0.5);
            flt.mbCenterV      = (float)fltObj["mbCenterV"].toDouble(0.5);
            flt.mbAngleSpanDeg = (float)fltObj["mbAngleSpanDeg"].toDouble(10.0);
            flt.lbRadiusPx        = (float)fltObj["lbRadiusPx"].toDouble(16.0);
            flt.lbBlades           = fltObj["lbBlades"].toInt(0);
            flt.lbBladeRotDeg     = (float)fltObj["lbBladeRotDeg"].toDouble(0.0);
            flt.lbHighlightBoost = (float)fltObj["lbHighlightBoost"].toDouble(0.6);
            flt.lbThreshold        = (float)fltObj["lbThreshold"].toDouble(0.7);
            flt.mzBlockSize = fltObj["mzBlockSize"].toInt(16);
            flt.nsStrength   = (float)fltObj["nsStrength"].toDouble(0.25);
            flt.nsMonochrome = fltObj["nsMonochrome"].toBool(true);
            flt.nsGrainPx    = (float)fltObj["nsGrainPx"].toDouble(1.0);
            flt.nsSeed       = (unsigned int)fltObj["nsSeed"].toDouble(0.0);
            gl_->document().layerRef(newLayIdx).filter = flt;
        }

        if (layerType == LayerType::Text) {
            std::vector<TextParams> boxes;
            const QJsonArray boxesArr = layerObj["textBoxes"].toArray();
            for (const QJsonValue &v : boxesArr) {
                QJsonObject boxObj = v.toObject();
                TextParams tp;
                tp.text       = boxObj["text"].toString();
                tp.fontFamily = boxObj["fontFamily"].toString(QStringLiteral("Yu Gothic UI"));
                tp.fontSize   = boxObj["fontSize"].toInt(48);
                tp.color      = QColor::fromRgba((QRgb)boxObj["color"].toVariant().toLongLong());
                tp.bold       = boxObj["bold"].toBool(false);
                tp.italic     = boxObj["italic"].toBool(false);
                tp.cx         = (float)boxObj["cx"].toDouble(0);
                tp.cy         = (float)boxObj["cy"].toDouble(0);
                tp.width      = (float)boxObj["width"].toDouble(300);
                tp.height     = (float)boxObj["height"].toDouble(100);
                tp.rotation   = (float)boxObj["rotation"].toDouble(0);
                tp.scale      = (float)boxObj["scale"].toDouble(1.0);
                boxes.push_back(tp);
            }
            gl_->document().layerRef(newLayIdx).textBoxes = boxes;
        }

        if (layerType == LayerType::SolidColor) {
            QColor color = QColor::fromRgba((QRgb)layerObj["solidColor"].toVariant().toLongLong());
            if (!layerObj.contains("solidColor")) color = QColor(255, 255, 255, 255); // 旧バージョンのファイルは白扱い
            gl_->document().layerRef(newLayIdx).solidColor = color;
        }

        // タイルごとにピクセルデータを書き込む(座標はレイヤーローカル)。
        // dataOffset/dataSizeの妥当性・展開後サイズは上の事前検証パスで
        // 既に確認済みなので、ここではdecodedTilesの中身をそのまま書き込むだけでよい。
        const Layer &layer = gl_->document().layerAt(newLayIdx);
        const int tilesX = layer.tilesX(), tilesY = layer.tilesY();
        QJsonArray tilesArray = layerObj["tiles"].toArray();
        const QVector<QByteArray> &decoded = decodedTiles.at(li); // 上と同じ理由でat()

        for (int ti = 0; ti < tilesArray.size(); ti++) {
            QJsonObject tileObj = tilesArray[ti].toObject();
            int tx = tileObj["tx"].toInt();
            int ty = tileObj["ty"].toInt();

            if (tx < 0 || tx >= tilesX || ty < 0 || ty >= tilesY) {
                setError(QString("タイル座標不正: tx=%1 ty=%2").arg(tx).arg(ty));
                return false;
            }

            queueUpload(layer.tiles[ty][tx], &decoded[ti]);
        }
    }
    // 溜めたタイルをまとめてGPUへ送る。
    const qint64 doneBefore = progressDone;
    gl_->writeSlicePixelsBatch(uploadSlices, uploadData, [&](int done) {
        progressDone = doneBefore + done;
        if (progressCallback) progressCallback(progressDone, progressTotal);
    });
    } // BulkLayerImportGuardのスコープ終わり(ここでendBulkLayerImport()が呼ばれ、
      // 1回だけまとめてlayersChanged()が発行される)


    // 全レイヤーが揃ったこの時点でフォルダーの内包枚数を入れる
    for (const PendingChildCount &pc : pendingChildCounts)
        if (pc.layerIndex >= 0 && pc.layerIndex < gl_->document().layerCount())
            gl_->document().layerRef(pc.layerIndex).childCount = pc.count;

    // 復元したchildCountがレイヤー配列と矛盾していないことを保証する
    // (矛盾したまま使うと合成/レイヤードックの index 計算が配列外アクセスになる)
    gl_->document().sanitizeFolderChildCounts();

    int savedLayer = root["activeLayerIndex"].toInt(0);
    gl_->document().setActiveLayer(qBound(0, savedLayer, gl_->document().layerCount() - 1));

    gl_->update();

    return true;
}

// ===========================================================================
// プレビュー画像のみの高速読み込み
// ===========================================================================
QImage CanvasSerializer::readPreview(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QImage();

    char magic[4];
    if (file.read(magic, 4) != 4 || memcmp(magic, MAGIC, 4) != 0) return QImage();

    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 previewSize = 0;
    in >> previewSize;
    if (previewSize == 0 || previewSize > kPreviewMaxBytes) return QImage();

    QByteArray previewBytes = file.read(previewSize);
    if (static_cast<quint32>(previewBytes.size()) != previewSize) return QImage();

    return QImage::fromData(previewBytes, "PNG");
}
