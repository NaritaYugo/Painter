#include "backend/StrokeUndoRecorder.h"
#include <cstring>

void StrokeUndoRecorder::beginStroke(ToolContext &ctx)
{
    // 前回のストロークが何らかの理由でtakeStrokeEntry()まで到達せず終わった場合
    // (通常は起こらないが、ウィジェット外でのマウスリリース等の異常系に備えた
    // 保険)、取り出されずに残ったPBOをここで後始末する。
    if (!pendingPbos_.isEmpty())
        ctx.gl->glDeleteBuffers(pendingPbos_.size(), pendingPbos_.data());
    pending_.clear();
    pendingPbos_.clear();
    capturedTiles_.clear();
}

void StrokeUndoRecorder::expandRegion(ToolContext &ctx, int canvasTxMin, int canvasTxMax,
                                       int canvasTyMin, int canvasTyMax)
{
    // 新しく触れるタイルをキャプチャ(既存のものはスキップ)
    const Layer &layer = ctx.doc->activeLayer();
    const int tileSize = ctx.tileSize;
    const GLsizeiptr tileBytes = (GLsizeiptr)tileSize * tileSize * 4;

    // マスク編集中は、レイヤー本体ではなくマスクのタイルをキャプチャ対象にする。
    // マスクは原点が常に(0,0)固定なので、キャンバスタイル座標がそのまま
    // マスクローカル座標になる(原点補正不要)。
    const bool useMask = layer.hasMask && ctx.editingMaskLayerIndex == ctx.doc->activeLayerIndex();

    // 呼び出し側は常にキャンバスタイル座標系で範囲を渡す。ここでレイヤーローカルの
    // タイル座標系(layer.tiles[ty][tx]が直接使える座標系)に変換する
    // (レイヤーの原点がキャンバスと一致しない場合があるため。マスクは原点補正不要)。
    const int txMin = useMask ? canvasTxMin : canvasTxMin - layer.originTx;
    const int txMax = useMask ? canvasTxMax : canvasTxMax - layer.originTx;
    const int tyMin = useMask ? canvasTyMin : canvasTyMin - layer.originTy;
    const int tyMax = useMask ? canvasTyMax : canvasTyMax - layer.originTy;

    if (!expandRegionFbo_) ctx.gl->glGenFramebuffers(1, &expandRegionFbo_);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, expandRegionFbo_);

    for (int ty = tyMin; ty <= tyMax; ty++) {
        for (int tx = txMin; tx <= txMax; tx++) {
            // すでにキャプチャ済みなら skip(タイル単位で厳密に判定する。バウンディング
            // ボックスだけで判定すると、ジグザグストロークのように触れたタイルが
            // 矩形にならない軌跡で、範囲内だが実際は未キャプチャのタイルを
            // 誤ってスキップしてしまう)
            qint64 key = tileKey(tx, ty);
            if (capturedTiles_.contains(key)) continue;
            capturedTiles_.insert(key);

            int si = useMask ? layer.maskTileSlice(tx, ty) : layer.tileSlice(tx, ty);
            if (si < 0) continue; // レイヤー(またはマスク)の矩形外(通常は起こらない)

            TileUndo tu;
            tu.tx = tx; tu.ty = ty;
            tu.sliceIndex = si;
            // pixelsはまだ確保しない(取り出しはtakeStrokeEntry()まで遅延するため、
            // ここで確保してもPBOからのコピー時にresizeし直すだけで無駄)。

            // GL_PIXEL_PACK_BUFFERバインド中はglReadPixelsの最終引数がクライアント
            // メモリへのポインタではなく、そのバッファ内オフセットとして解釈される
            // (=CPUを待たせずキューイングされるだけで即座に戻る)。読み出しの完了は
            // 待たず、PBOハンドルだけpendingPbos_に控えて先へ進む。
            GLuint pbo = 0;
            ctx.gl->glGenBuffers(1, &pbo);
            ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            ctx.gl->glBufferData(GL_PIXEL_PACK_BUFFER, tileBytes, nullptr, GL_STREAM_READ);

            ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            ctx.gl->glReadPixels(0, 0, tileSize, tileSize, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

            pending_.append(tu);
            pendingPbos_.append(pbo);
        }
    }

    ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    // FBOのバインドを外す。ctx.defaultFbo()(Qtのdefault FBO取得)はイベント処理中に
    // 呼ぶと一部Intelドライバでクラッシュするため0で外す。FBO自体は
    // expandRegionFbo_として使い回すので削除しない。
    //
    // 【注意】この関数はマウスイベント処理中だけでなく、GLWidget::paintGL()の
    // 冒頭が呼ぶ Tool::flushPendingInput() 経由でも呼ばれる。そこでここが0に
    // 外したままだと、そのフレームのclear/drawがウィジェットのFBOではなく
    // フレームバッファ0へ行き、描画がまるごと捨てられる(症状: ストローク開始
    // 直後の部分再描画が出ず、あとで全面再描画が来たときにポンと現れる)。
    // GLWidget::paintGL()が入り口でバインドを控えておき、flushPendingInput()と
    // prepareCompositeBase()のあとに戻すことで面倒を見ている(あちらの
    // prevDrawFbo のコメント参照)。ここを変更するときはあの復帰処理も併せて見ること。
    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

UndoEntry StrokeUndoRecorder::takeStrokeEntry(ToolContext &ctx, int layerIndex)
{
    UndoEntry entry;
    entry.layerIndex = layerIndex;

    // ここで初めて、ストローク中に発行しておいた各PBOの転送結果を取り出す
    // (この時点までにGPU側の転送はとうに完了しているはずなので、expandRegion()の
    // 呼び出しごとに同期していた従来方式に比べ、ここで待つのは実質1回分の
    // 残り(あれば)だけで済む)。
    const int tileSize = ctx.tileSize;
    const GLsizeiptr tileBytes = (GLsizeiptr)tileSize * tileSize * 4;
    for (int i = 0; i < pending_.size(); i++) {
        TileUndo &tu = pending_[i];
        GLuint pbo = pendingPbos_[i];
        tu.pixels.resize(tileSize * tileSize * 4);
        ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        const void *mapped = ctx.gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, tileBytes, GL_MAP_READ_BIT);
        if (mapped) memcpy(tu.pixels.data(), mapped, tileBytes);
        ctx.gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!pendingPbos_.isEmpty())
        ctx.gl->glDeleteBuffers(pendingPbos_.size(), pendingPbos_.data());

    entry.tiles = pending_;
    pending_.clear();
    pendingPbos_.clear();
    return entry;
}

UndoEntry StrokeUndoRecorder::captureAllTiles(ToolContext &ctx, int layerIndex)
{
    UndoEntry entry;
    entry.layerIndex = layerIndex;

    const Layer &layer = ctx.doc->layers[layerIndex];
    const int tileSize = ctx.tileSize;

    GLuint fbo = 0;
    ctx.gl->glGenFramebuffers(1, &fbo);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);

    // レイヤー自身の矩形(キャンバスより大きい/はみ出している場合を含む)を丸ごと捉える
    for (int ty = 0; ty < layer.tilesY(); ty++) {
        for (int tx = 0; tx < layer.tilesX(); tx++) {
            int si = layer.tiles[ty][tx];
            TileUndo tu;
            tu.tx = tx; tu.ty = ty;
            tu.sliceIndex = si;
            tu.pixels.resize(tileSize * tileSize * 4);
            ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
            ctx.gl->glReadPixels(0, 0, tileSize, tileSize,
                                 GL_RGBA, GL_UNSIGNED_BYTE, tu.pixels.data());
            entry.tiles.append(tu);
        }
    }

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, 0); // 一時FBOの後片付け。ctx.defaultFbo()(Qtのdefault FBO取得)はイベント処理中に呼ぶと一部Intelドライバでクラッシュするため0で外す(この後paintGL()が自前のFBOを束縛し直すので実害なし)
    ctx.gl->glDeleteFramebuffers(1, &fbo);
    return entry;
}

UndoEntry StrokeUndoRecorder::captureTilesLike(ToolContext &ctx, int layerIndex, const QVector<TileUndo> &shape)
{
    UndoEntry entry;
    entry.layerIndex = layerIndex;

    const Layer &layer = ctx.doc->layers[layerIndex];
    const int tileSize = ctx.tileSize;

    GLuint fbo = 0;
    ctx.gl->glGenFramebuffers(1, &fbo);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);

    for (const TileUndo &ref : shape) {
        const int si = layer.tileSlice(ref.tx, ref.ty);
        if (si < 0) continue; // レイヤーの矩形外(通常起こらない)

        TileUndo tu;
        tu.tx = ref.tx; tu.ty = ref.ty;
        tu.sliceIndex = si;
        tu.pixels.resize(tileSize * tileSize * 4);
        ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          ctx.bankTexOf(si), 0, ctx.localSliceOf(si));
        ctx.gl->glReadPixels(0, 0, tileSize, tileSize,
                             GL_RGBA, GL_UNSIGNED_BYTE, tu.pixels.data());
        entry.tiles.append(tu);
    }

    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, 0); // 一時FBOの後片付け。ctx.defaultFbo()(Qtのdefault FBO取得)はイベント処理中に呼ぶと一部Intelドライバでクラッシュするため0で外す(この後paintGL()が自前のFBOを束縛し直すので実害なし)
    ctx.gl->glDeleteFramebuffers(1, &fbo);
    return entry;
}

void StrokeUndoRecorder::restoreLayer(ToolContext &ctx, const UndoEntry &entry)
{
    const int tileSize = ctx.tileSize;
    for (const TileUndo &tu : entry.tiles) {
        // スライスはバンクをまたぎ得るのでタイルごとに対象バンクをバインドし直す。
        ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.bankTexOf(tu.sliceIndex));
        ctx.gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                0, 0, ctx.localSliceOf(tu.sliceIndex),
                                tileSize, tileSize, 1,
                                GL_RGBA, GL_UNSIGNED_BYTE, tu.pixels.constData());
    }
    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

QByteArray StrokeUndoRecorder::readSlicePixels(ToolContext &ctx, int texArraySlice)
{
    const int tileSize = ctx.tileSize;
    QVector<uint8_t> pixels(tileSize * tileSize * 4);

    GLuint fbo = 0;
    ctx.gl->glGenFramebuffers(1, &fbo);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      ctx.bankTexOf(texArraySlice), 0, ctx.localSliceOf(texArraySlice));
    ctx.gl->glReadPixels(0, 0, tileSize, tileSize,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, 0); // 一時FBOの後片付け。ctx.defaultFbo()(Qtのdefault FBO取得)はイベント処理中に呼ぶと一部Intelドライバでクラッシュするため0で外す(この後paintGL()が自前のFBOを束縛し直すので実害なし)
    ctx.gl->glDeleteFramebuffers(1, &fbo);

    return QByteArray(reinterpret_cast<const char*>(pixels.constData()), pixels.size());
}

QVector<QByteArray> StrokeUndoRecorder::readSlicesBatch(ToolContext &ctx, const QVector<int> &slices)
{
    QVector<QByteArray> result(slices.size());
    if (slices.isEmpty()) return result;

    const int tileSize = ctx.tileSize;
    const GLsizeiptr tileBytes = (GLsizeiptr)tileSize * tileSize * 4;

    // PBOを複数(kRingSize枚)用意して使い回すリングバッファ方式。詳細はヘッダのコメント参照。
    constexpr int kRingSize = 3;
    GLuint pbos[kRingSize] = {0};
    ctx.gl->glGenBuffers(kRingSize, pbos);
    for (int i = 0; i < kRingSize; i++) {
        ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
        ctx.gl->glBufferData(GL_PIXEL_PACK_BUFFER, tileBytes, nullptr, GL_STREAM_READ);
    }

    GLuint fbo = 0;
    ctx.gl->glGenFramebuffers(1, &fbo);
    ctx.gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);

    // ringOwner[i] = 現在pbos[i]に発行済み(未取得)のglReadPixelsが、slices[]の
    // どのインデックス向けのものか(-1なら未使用/取得済み)
    int ringOwner[kRingSize];
    for (int i = 0; i < kRingSize; i++) ringOwner[i] = -1;

    auto drain = [&](int ringIdx) {
        const int owner = ringOwner[ringIdx];
        if (owner < 0) return;
        ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[ringIdx]);
        const void *mapped = ctx.gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, tileBytes, GL_MAP_READ_BIT);
        if (mapped)
            result[owner] = QByteArray(reinterpret_cast<const char*>(mapped), tileBytes);
        ctx.gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        ringOwner[ringIdx] = -1;
    };

    for (int i = 0; i < slices.size(); i++) {
        const int ringIdx = i % kRingSize;
        drain(ringIdx); // このスロットを前に使っていた分が残っていれば先に取り出す(CPUが待つのはここだけ)

        ctx.gl->glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          ctx.bankTexOf(slices[i]), 0, ctx.localSliceOf(slices[i]));
        ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[ringIdx]);
        // GL_PIXEL_PACK_BUFFERバインド中はglReadPixelsの最終引数がクライアントメモリへの
        // ポインタではなく、そのバッファ内オフセットとして解釈される(=非同期にキューイングされる)。
        ctx.gl->glReadPixels(0, 0, tileSize, tileSize, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        ringOwner[ringIdx] = i;
    }
    // 発行済みだがまだ取り出していない分(直近kRingSize枚)をすべて取り出す
    for (int i = 0; i < kRingSize; i++) drain(i);

    ctx.gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    ctx.gl->glBindFramebuffer(GL_FRAMEBUFFER, 0); // 一時FBOの後片付け。ctx.defaultFbo()はイベント処理中に呼ぶと一部Intelドライバでクラッシュするため0で外す
    ctx.gl->glDeleteFramebuffers(1, &fbo);
    ctx.gl->glDeleteBuffers(kRingSize, pbos);

    return result;
}

void StrokeUndoRecorder::writeSlicesBatch(ToolContext &ctx, const QVector<int> &slices,
                                          const QVector<const QByteArray *> &data,
                                          const std::function<void(int)> &onProgress)
{
    if (slices.isEmpty()) return;

    const int tileSize = ctx.tileSize;
    const GLsizeiptr tileBytes = (GLsizeiptr)tileSize * tileSize * 4;

    // 読み出し側(readSlicesBatch)と違い、書き込みはPBO経由にしない。
    // 一度PBOのリングバッファ方式を試したが、GL_MAP_INVALIDATE_BUFFER_BITで
    // 再マップしたバッファを、そのバッファからの転送がまだ実行中のうちに
    // ドライバが使い回してしまい、タイルの一部が欠けて読み込まれた
    // (かつ速度も1枚あたり0.311ms→0.483msと逆に遅くなった)。
    // 正しさのために、クライアントメモリから直接送る同期転送のままにしている。
    // バッチにしているのは、呼び出しごとのmakeCurrent()と、同じバンクが続く間の
    // glBindTextureを省くためで、転送そのものは1枚ずつ同期的に行う。
    GLuint boundTex = 0;
    for (int i = 0; i < slices.size(); i++) {
        const QByteArray *raw = data[i];
        if (!raw || raw->size() != (int)tileBytes) continue; // 想定外サイズは書かない(呼び出し側で検証済み)

        const GLuint tex = ctx.bankTexOf(slices[i]);
        if (tex != boundTex) {
            ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
            boundTex = tex;
        }
        ctx.gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                0, 0, ctx.localSliceOf(slices[i]),
                                tileSize, tileSize, 1,
                                GL_RGBA, GL_UNSIGNED_BYTE,
                                reinterpret_cast<const uint8_t*>(raw->constData()));

        // 進捗は一定間隔で(毎回呼ぶとコールバック側のダイアログ更新が支配的になる)
        if (onProgress && ((i + 1) % 512 == 0 || i + 1 == slices.size()))
            onProgress(i + 1);
    }

    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void StrokeUndoRecorder::writeSlicePixels(ToolContext &ctx, int texArraySlice, const QByteArray &raw)
{
    const int tileSize = ctx.tileSize;
    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.bankTexOf(texArraySlice));
    ctx.gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                            0, 0, ctx.localSliceOf(texArraySlice),
                            tileSize, tileSize, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE,
                            reinterpret_cast<const uint8_t*>(raw.constData()));
    ctx.gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}