// ---------------------------------------------------------------------------
// ブレンドモード
// すべて pre-multiplied alpha を前提とする
// ---------------------------------------------------------------------------

// 通常合成 (Porter-Duff src-over)
vec4 normalBlend(vec4 bg, vec4 fg) {
    return fg + bg * (1.0 - fg.a);
}

// アルファ0のときに 0/0 (NaN) にならないようにする unpremultiply
vec3 safeUnpremul(vec3 c, float a) {
    return a > 0.0001 ? c / a : vec3(0.0);
}

// 非通常ブレンドモード共通の合成式。
// B は「straight(非pre-multiplied)色同士に対する生のブレンド関数の結果」。
// bg.a==0 や fg.a==0 の領域(=キャンバスが空/ブラシが乗っていない領域)では
// B の寄与が自動的に消え、もう片方の色がそのまま透過するようになっている
// (Photoshopで乗算レイヤーの背景が透明なら、そのレイヤー自身の色がそのまま
// 見えるのと同じ挙動)。
vec4 compositeBlend(vec4 bg, vec4 fg, vec3 B) {
    float ao = fg.a + bg.a * (1.0 - fg.a);
    vec3  co = fg.rgb * (1.0 - bg.a) + bg.rgb * (1.0 - fg.a) + B * (bg.a * fg.a);
    return vec4(co, ao);
}

// 乗算 (Multiply)
vec4 multiplyBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = bgS * fgS;
    return compositeBlend(bg, fg, B);
}

// スクリーン (Screen)
vec4 screenBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = 1.0 - (1.0 - bgS) * (1.0 - fgS);
    return compositeBlend(bg, fg, B);
}

// オーバーレイ (Overlay)
float overlayChannel(float bg, float fg) {
    return bg < 0.5 ? 2.0 * bg * fg : 1.0 - 2.0 * (1.0 - bg) * (1.0 - fg);
}
vec4 overlayBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(
        overlayChannel(bgS.r, fgS.r),
        overlayChannel(bgS.g, fgS.g),
        overlayChannel(bgS.b, fgS.b)
    );
    return compositeBlend(bg, fg, B);
}

// ---------------------------------------------------------------------------
// Photoshop全ブレンドモード追加分
// bgS/fgSはunpremultiply済み(straight)のRGB。各チャンネル関数は[0,1]の
// straight値同士に対して定義し、compositeBlend()でPorter-Duty合成に組み込む。
// ---------------------------------------------------------------------------

// ---- 暗くする系 ----
vec4 darkenBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    return compositeBlend(bg, fg, min(bgS, fgS));
}

float colorBurnChannel(float bg, float fg) {
    if (fg <= 0.0) return 0.0;
    if (bg >= 1.0) return 1.0;
    return 1.0 - min(1.0, (1.0 - bg) / fg);
}
vec4 colorBurnBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(colorBurnChannel(bgS.r, fgS.r), colorBurnChannel(bgS.g, fgS.g), colorBurnChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

float linearBurnChannel(float bg, float fg) { return clamp(bg + fg - 1.0, 0.0, 1.0); }
vec4 linearBurnBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(linearBurnChannel(bgS.r, fgS.r), linearBurnChannel(bgS.g, fgS.g), linearBurnChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

// 単純加重輝度(全チャンネル一括比較。Photoshopの Darker/Lighter Color と同じ考え方)
float blendLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec4 darkerColorBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = blendLuma(bgS) <= blendLuma(fgS) ? bgS : fgS;
    return compositeBlend(bg, fg, B);
}

// ---- 明るくする系 ----
vec4 lightenBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    return compositeBlend(bg, fg, max(bgS, fgS));
}

float colorDodgeChannel(float bg, float fg) {
    if (fg >= 1.0) return 1.0;
    if (bg <= 0.0) return 0.0;
    return min(1.0, bg / (1.0 - fg));
}
vec4 colorDodgeBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(colorDodgeChannel(bgS.r, fgS.r), colorDodgeChannel(bgS.g, fgS.g), colorDodgeChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

float linearDodgeChannel(float bg, float fg) { return clamp(bg + fg, 0.0, 1.0); }
vec4 linearDodgeBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(linearDodgeChannel(bgS.r, fgS.r), linearDodgeChannel(bgS.g, fgS.g), linearDodgeChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

vec4 lighterColorBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = blendLuma(bgS) >= blendLuma(fgS) ? bgS : fgS;
    return compositeBlend(bg, fg, B);
}

// ---- コントラスト系 ----
float softLightChannel(float bg, float fg) {
    if (fg <= 0.5) return bg - (1.0 - 2.0 * fg) * bg * (1.0 - bg);
    float d = (bg <= 0.25) ? ((16.0 * bg - 12.0) * bg + 4.0) * bg : sqrt(bg);
    return bg + (2.0 * fg - 1.0) * (d - bg);
}
vec4 softLightBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(softLightChannel(bgS.r, fgS.r), softLightChannel(bgS.g, fgS.g), softLightChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

float hardLightChannel(float bg, float fg) {
    return fg < 0.5 ? 2.0 * bg * fg : 1.0 - 2.0 * (1.0 - bg) * (1.0 - fg);
}
vec4 hardLightBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(hardLightChannel(bgS.r, fgS.r), hardLightChannel(bgS.g, fgS.g), hardLightChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

float vividLightChannel(float bg, float fg) {
    return fg < 0.5 ? colorBurnChannel(bg, 2.0 * fg) : colorDodgeChannel(bg, 2.0 * (fg - 0.5));
}
vec4 vividLightBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(vividLightChannel(bgS.r, fgS.r), vividLightChannel(bgS.g, fgS.g), vividLightChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

float linearLightChannel(float bg, float fg) { return clamp(bg + 2.0 * fg - 1.0, 0.0, 1.0); }
vec4 linearLightBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(linearLightChannel(bgS.r, fgS.r), linearLightChannel(bgS.g, fgS.g), linearLightChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

float pinLightChannel(float bg, float fg) {
    return fg < 0.5 ? min(bg, 2.0 * fg) : max(bg, 2.0 * (fg - 0.5));
}
vec4 pinLightBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(pinLightChannel(bgS.r, fgS.r), pinLightChannel(bgS.g, fgS.g), pinLightChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

float hardMixChannel(float bg, float fg) { return vividLightChannel(bg, fg) < 0.5 ? 0.0 : 1.0; }
vec4 hardMixBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(hardMixChannel(bgS.r, fgS.r), hardMixChannel(bgS.g, fgS.g), hardMixChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

// ---- 比較系 ----
vec4 differenceBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    return compositeBlend(bg, fg, abs(bgS - fgS));
}

vec4 exclusionBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = bgS + fgS - 2.0 * bgS * fgS;
    return compositeBlend(bg, fg, B);
}

vec4 subtractBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = clamp(bgS - fgS, 0.0, 1.0);
    return compositeBlend(bg, fg, B);
}

float divideChannel(float bg, float fg) { return fg <= 0.0001 ? 1.0 : clamp(bg / fg, 0.0, 1.0); }
vec4 divideBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = vec3(divideChannel(bgS.r, fgS.r), divideChannel(bgS.g, fgS.g), divideChannel(bgS.b, fgS.b));
    return compositeBlend(bg, fg, B);
}

// ---- 色相/彩度/カラー/輝度 (HSL成分ごとの合成。Photoshop仕様の非線形Lum/Satを使う) ----
float hslLum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
float hslSat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }

vec3 hslClipColor(vec3 c) {
    float l = hslLum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * (l / max(l - n, 1e-6));
    if (x > 1.0) c = l + (c - l) * ((1.0 - l) / max(x - l, 1e-6));
    return c;
}

vec3 hslSetLum(vec3 c, float l) {
    return hslClipColor(c + (l - hslLum(c)));
}

vec3 hslSetSat(vec3 c, float s) {
    int maxI = (c.r >= c.g) ? ((c.r >= c.b) ? 0 : 2) : ((c.g >= c.b) ? 1 : 2);
    int minI = (c.r <= c.g) ? ((c.r <= c.b) ? 0 : 2) : ((c.g <= c.b) ? 1 : 2);
    int midI = 3 - maxI - minI;

    float cmax = c[maxI];
    float cmin = c[minI];
    float cmid = c[midI];

    if (cmax > cmin) {
        cmid = (cmid - cmin) * s / (cmax - cmin);
        cmax = s;
    } else {
        cmid = 0.0;
        cmax = 0.0;
    }
    cmin = 0.0;

    vec3 r;
    r[maxI] = cmax;
    r[midI] = cmid;
    r[minI] = cmin;
    return r;
}

vec4 hueBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = hslSetLum(hslSetSat(fgS, hslSat(bgS)), hslLum(bgS));
    return compositeBlend(bg, fg, B);
}

vec4 saturationBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = hslSetLum(hslSetSat(bgS, hslSat(fgS)), hslLum(bgS));
    return compositeBlend(bg, fg, B);
}

vec4 colorBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = hslSetLum(fgS, hslLum(bgS));
    return compositeBlend(bg, fg, B);
}

vec4 luminosityBlend(vec4 bg, vec4 fg) {
    vec3 bgS = safeUnpremul(bg.rgb, bg.a);
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    vec3 B = hslSetLum(bgS, hslLum(fgS));
    return compositeBlend(bg, fg, B);
}

// ---- 描画(Dissolve, ディザ合成) ----
// ピクセル位置ベースの疑似乱数でfg.aを閾値として、そのピクセルをfg色そのもの(不透明)に
// するかbgのまま透過させるかを二値で決める(Photoshopの「ドット合成」と同じ考え方)。
float blendPseudoRand(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}
vec4 dissolveBlend(vec4 bg, vec4 fg, vec2 pixelPos) {
    if (blendPseudoRand(pixelPos) >= fg.a) return bg;
    vec3 fgS = safeUnpremul(fg.rgb, fg.a);
    return normalBlend(bg, vec4(fgS, 1.0));
}

// スイッチ (pixelPos: キャンバス絶対ピクセル座標。Dissolveの乱数シードにのみ使う)
vec4 applyBlend(vec4 bg, vec4 fg, int mode, vec2 pixelPos) {
    if (mode == 1)  return multiplyBlend(bg, fg);
    if (mode == 2)  return screenBlend(bg, fg);
    if (mode == 3)  return overlayBlend(bg, fg);
    if (mode == 4)  return dissolveBlend(bg, fg, pixelPos);
    if (mode == 5)  return darkenBlend(bg, fg);
    if (mode == 6)  return colorBurnBlend(bg, fg);
    if (mode == 7)  return linearBurnBlend(bg, fg);
    if (mode == 8)  return darkerColorBlend(bg, fg);
    if (mode == 9)  return lightenBlend(bg, fg);
    if (mode == 10) return colorDodgeBlend(bg, fg);
    if (mode == 11) return linearDodgeBlend(bg, fg);
    if (mode == 12) return lighterColorBlend(bg, fg);
    if (mode == 13) return softLightBlend(bg, fg);
    if (mode == 14) return hardLightBlend(bg, fg);
    if (mode == 15) return vividLightBlend(bg, fg);
    if (mode == 16) return linearLightBlend(bg, fg);
    if (mode == 17) return pinLightBlend(bg, fg);
    if (mode == 18) return hardMixBlend(bg, fg);
    if (mode == 19) return differenceBlend(bg, fg);
    if (mode == 20) return exclusionBlend(bg, fg);
    if (mode == 21) return subtractBlend(bg, fg);
    if (mode == 22) return divideBlend(bg, fg);
    if (mode == 23) return hueBlend(bg, fg);
    if (mode == 24) return saturationBlend(bg, fg);
    if (mode == 25) return colorBlend(bg, fg);
    if (mode == 26) return luminosityBlend(bg, fg);
    return normalBlend(bg, fg);
}

// ---------------------------------------------------------------------------
// ストロークの適用
// ---------------------------------------------------------------------------
// 「描きかけのストロークを1画素へ乗せる」処理を1箇所にまとめたもの。
// 焼き込み(bake.comp)・ライブプレビュー(render.frag / belowComposite.comp)の
// 3経路が同じ式を使うためにある。以前はこの式が3箇所へ写し取られていて、
// 片方だけ直すとプレビューと確定結果が食い違う状態になりやすかった
// (実際に不透明度の二重掛けはその形で長く残っていた)。
// ---------------------------------------------------------------------------

// ストロークがこの画素へ乗せる色(事前乗算済み)。
//
// useStrokeColor == 0:
//   ストローク全体で1色(brushColor)なので、マスク値を掛けるだけでよい。
//   マスクは「このブラシの全力のうち、この画素にどれだけ乗るか」(0〜1)なので、
//   事前乗算済みのまま正しいsrcになる(実効アルファ = 不透明度 × maskAt)。
//
// useStrokeColor != 0:
//   スタンプごとに色が変わる場合(色のランダム、将来の混色・水彩)。
//   strokeRGB はストローク色バッファの値で、「この画素へ置かれた色 × マスク値」
//   (=マスクで事前乗算済み、ただし不透明度は含まない)。不透明度はここで掛ける。
//   色が一定なら strokeRGB == 色 × maskAt なので、上の式と同じ値になる。
//
// 【呼び出し側の約束】strokeRGB は maskAt と同じ係数(選択範囲マスク等)を
// 掛けた状態で渡すこと。片方だけ掛けると色とアルファの整合が崩れる。
vec4 strokeSourceColor(vec4 brushColor, float maskAt, vec3 strokeRGB, int useStrokeColor) {
    if (useStrokeColor == 0) return brushColor * maskAt;
    return vec4(strokeRGB * brushColor.a, brushColor.a * maskAt);
}

// ストローク1本ぶんをこの画素へ適用した結果を返す。
//   dst            : 適用先の画素(事前乗算済み)
//   brushColor     : 事前乗算済みのブラシ色。消すモードでは a が「消す強さ」
//   maskAt         : この画素のマスク値(0〜1)。選択範囲マスクは掛けた後の値を渡すこと
//   strokeRGB      : ストローク色バッファの値。maskAtと同じ係数を掛けて渡すこと
//   useStrokeColor : 0ならstrokeRGBを無視してbrushColor1色で塗る
//   eraseMode      : 0以外なら、色を乗せる代わりにアルファを削る(消しゴム/透明色)
//   blendMode      : ブラシの合成モード(BlendModeの整数値。0=普通)
//   layerOpacity   : プレビュー用。dstに既にレイヤー不透明度が掛かっている場合、
//                    srcにも同じだけ掛けて見た目を焼き込み後へ寄せる(焼き込み側は1.0)。
//                    「消す」側は「そこにあるものの何割を消すか」なので掛けない。
//   pixelPos       : ディザ合成の乱数シードに使うキャンバス絶対座標
//
// maskAt<=0 の早期リターンは最適化のみ(applyBlendはfg=0でbgをそのまま返す:
// compositeBlendの ao=bg.a, co=bg.rgb)。
vec4 applyStrokeToPixel(vec4 dst, vec4 brushColor, float maskAt,
                        vec3 strokeRGB, int useStrokeColor,
                        int eraseMode, int blendMode, float layerOpacity, vec2 pixelPos)
{
    if (maskAt <= 0.0) return dst;
    if (eraseMode != 0) return dst * (1.0 - brushColor.a * maskAt);
    // レイヤー不透明度はsrc全体へ掛ける(1色経路では maskAt に畳んだ場合と同値)。
    vec4 src = strokeSourceColor(brushColor, maskAt, strokeRGB, useStrokeColor) * layerOpacity;
    return applyBlend(dst, src, blendMode, pixelPos);
}

// ---------------------------------------------------------------------------
// 逆双一次補間(自由変形/シアー変形用)
// ---------------------------------------------------------------------------
// 頂点A,B,C,D(それぞれ(u,v)=(0,0),(1,0),(1,1),(0,1)に対応する四角形)について、
// 点Pに対応する(u,v)を逆算する。P(u,v) = A + u*(B-A) + v*(D-A) + u*v*(A-B+C-D)
// という双一次式をu,vについて解く(2次方程式になるため2つの解が出るが、
// 通常は[0,1]範囲に近い方の解を採用する)。
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

vec2 invBilinear(vec2 P, vec2 A, vec2 B, vec2 C, vec2 D) {
    vec2 E = B - A;
    vec2 F = D - A;
    vec2 G = A - B + C - D;
    vec2 H = P - A;

    float a = cross2(G, F);
    float b = cross2(E, F) + cross2(H, G);
    float c = cross2(H, E);

    float v;
    if (abs(a) < 1e-6) {
        v = (abs(b) < 1e-8) ? 0.0 : (-c / b);
    } else {
        float disc = max(b * b - 4.0 * a * c, 0.0);
        float sq = sqrt(disc);
        float v1 = (-b + sq) / (2.0 * a);
        float v2 = (-b - sq) / (2.0 * a);
        float d1 = abs(v1 - clamp(v1, 0.0, 1.0));
        float d2 = abs(v2 - clamp(v2, 0.0, 1.0));
        v = (d1 <= d2) ? v1 : v2;
    }

    float denomX = E.x + v * G.x;
    float denomY = E.y + v * G.y;
    float u = (abs(denomX) > abs(denomY)) ? (H.x - v * F.x) / denomX
                                           : (H.y - v * F.y) / denomY;
    return vec2(u, v);
}

// ---------------------------------------------------------------------------
// 近傍サンプリング時の境界処理(ブラー/ワープ等のカーネルサンプリング用)
// ---------------------------------------------------------------------------
// 1点の近傍サンプリング座標を解決する。wrapX/wrapYがfalseなら従来通りの
// エッジクランプ、trueなら周回(モジュロ)。呼び出し側が定数falseを渡す限り
// コンパイラが分岐を畳み込むため、現状の挙動・コストは変わらない
// (将来のキャンバスループ機能で、この関数を呼ぶ側がuniformで
//  wrapX/wrapYを渡すように変えるだけで対応できる)。
ivec2 resolveSampleCoord(ivec2 coord, ivec2 size, bool wrapX, bool wrapY) {
    ivec2 c = coord;
    c.x = wrapX ? ((c.x % size.x) + size.x) % size.x : clamp(c.x, 0, size.x - 1);
    c.y = wrapY ? ((c.y % size.y) + size.y) % size.y : clamp(c.y, 0, size.y - 1);
    return c;
}

// ---------------------------------------------------------------------------
// フィルターの近傍サンプリングを「キャンバス矩形」に限定するヘルパー
// ---------------------------------------------------------------------------
// フィルター系(gaussianBlurFilter / motionBlurFilter / lensBlurFilter /
// chromaticAberrationFilter)が読む uSrc(= fullLayerTex)は、レイヤーのタイル境界に
// 合わせたサイズ(タイル256pxの倍数)で確保されている(GLWidget::ensureTransformScratchSize)。
// そのためキャンバスの幅・高さが256の倍数でないとき、テクスチャの右端・下端には
// キャンバス外の「一度も描かれていない完全透明なパディング」が最大255px残っている。
//
// これを近傍サンプリングに巻き込むと、キャンバスの縁付近だけおかしくなる:
//   ・ぼかし系 … 透明と混ざってアルファが落ち、下地が透けて白っぽくなる
//   ・色収差   … 透明を拾うとR/G/Bが原色で出るため、3つ揃うとスクリーン合成で白になる
// テクスチャ範囲でクランプ(clamp to edge)しても、クランプ先がそのパディングなので直らない。
//
// そこで、サンプリングして良い範囲は「テクスチャ全体」ではなく「キャンバス矩形」と定義する。
// canvasOffset には各フィルターが既に持っている uSelMaskOffset(レイヤー原点のキャンバス座標)、
// canvasSize には imageSize(selMask)(選択マスクは常にキャンバスサイズ)をそのまま渡せばよい。
//
// 補足: レイヤーが変形などでキャンバス外へはみ出して実データを持っている場合、その分は
// サンプルされなくなるが、見えている範囲の結果は変わらないため実害はない。

// p(レイヤーローカル座標)をキャンバス矩形で判定/ラップし、実際に読むレイヤーローカル
// 座標を outCoord へ返す。キャンバス外(かつラップ無効)なら false を返す。
// false のサンプルは「データが無い」ので、加算にも重みの分母にも数えないこと
// (有効なサンプルだけで正規化すれば、縁でも平均枚数が減るだけでアルファは落ちない)。
bool resolveCanvasSampleCoord(ivec2 p, ivec2 canvasOffset, ivec2 canvasSize, ivec2 texSize,
                              bool wrapX, bool wrapY, out ivec2 outCoord) {
    ivec2 cc = p + canvasOffset; // レイヤーローカル -> キャンバス座標
    if (wrapX) cc.x = ((cc.x % canvasSize.x) + canvasSize.x) % canvasSize.x;
    else if (cc.x < 0 || cc.x >= canvasSize.x) { outCoord = p; return false; }
    if (wrapY) cc.y = ((cc.y % canvasSize.y) + canvasSize.y) % canvasSize.y;
    else if (cc.y < 0 || cc.y >= canvasSize.y) { outCoord = p; return false; }
    // キャンバス座標 -> レイヤーローカルへ戻す(テクスチャ範囲は保険でクランプ)
    outCoord = clamp(cc - canvasOffset, ivec2(0), texSize - ivec2(1));
    return true;
}

// 同じくキャンバス矩形に限定するが、範囲外を捨てるのではなく縁へ寄せる版。
// サンプル数が固定で正規化し直せないフィルター(色収差はR/G/Bの3点で1画素を作るため
// 「このサンプルは無し」にできない)向け。
ivec2 clampCanvasSampleCoord(ivec2 p, ivec2 canvasOffset, ivec2 canvasSize, ivec2 texSize) {
    ivec2 cc = clamp(p + canvasOffset, ivec2(0), canvasSize - ivec2(1));
    return clamp(cc - canvasOffset, ivec2(0), texSize - ivec2(1));
}

// 点pから線分[a,b]までの最短距離。
float distToSegment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    if (dot(ab, ab) < 0.0001) return distance(p, a);
    float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
    return distance(p, a + t * ab);
}

// ブラシ(ぼかし/ゆがみ)の距離判定のラップ対応版。キャンバスがタイル1枚分以下
// しかない場合など、ラップ先が「今まさに処理している側」と同じ座標範囲に
// 重なってしまい、ディスパッチ領域を反対側へ拡張できない(拡張すると同じ
// ピクセルへ二重に書き込まれ結果が不定になる)ケースがある。そのため
// ディスパッチ範囲の拡張とは別に、距離判定そのものを「キャンバス1つぶん
// ずらした位置の線分」候補も含めて最小距離を取るようにし、拡張なしでも
// 正しく周回した距離が得られるようにする(通常サイズのキャンバスでは
// ずらした候補の距離は必ず本来の候補よりずっと大きくなるため無害)。
float distToSegmentWrapped(vec2 p, vec2 a, vec2 b, vec2 size, bool wrapX, bool wrapY) {
    float best = distToSegment(p, a, b);
    if (wrapX) {
        best = min(best, distToSegment(p, a + vec2(size.x, 0.0), b + vec2(size.x, 0.0)));
        best = min(best, distToSegment(p, a - vec2(size.x, 0.0), b - vec2(size.x, 0.0)));
    }
    if (wrapY) {
        best = min(best, distToSegment(p, a + vec2(0.0, size.y), b + vec2(0.0, size.y)));
        best = min(best, distToSegment(p, a - vec2(0.0, size.y), b - vec2(0.0, size.y)));
    }
    if (wrapX && wrapY) {
        best = min(best, distToSegment(p, a + vec2( size.x,  size.y), b + vec2( size.x,  size.y)));
        best = min(best, distToSegment(p, a + vec2( size.x, -size.y), b + vec2( size.x, -size.y)));
        best = min(best, distToSegment(p, a + vec2(-size.x,  size.y), b + vec2(-size.x,  size.y)));
        best = min(best, distToSegment(p, a - vec2( size.x,  size.y), b - vec2( size.x,  size.y)));
    }
    return best;
}

// モザイクフィルター等の矩形ブロック範囲を画像境界へクランプする。
// blockStart0: クランプ前のブロック開始座標(uBlockSizeで割り切れる格子上の点)。
// 戻り値: 実際にサンプリングすべき[blockStart, blockEnd)。
// (周回モード時はブロックがシーム部分で2つに割れる可能性があり単純なクランプの
//  置き換えでは済まないため、現時点ではクランプ版のみを共通化する)
void resolveBlockRange(ivec2 blockStart0, int blockSize, ivec2 size,
                        out ivec2 blockStart, out ivec2 blockEnd) {
    blockEnd   = min(blockStart0 + ivec2(blockSize), size);
    blockStart = max(blockStart0, ivec2(0));
}

// ---------------------------------------------------------------------------
// RGB <-> HSL 変換(色相・彩度・明度調整用)
// ---------------------------------------------------------------------------
vec3 rgb2hsl(vec3 c) {
    float maxc = max(max(c.r, c.g), c.b);
    float minc = min(min(c.r, c.g), c.b);
    float l = (maxc + minc) * 0.5;
    float h = 0.0, s = 0.0;
    float d = maxc - minc;
    if (d > 0.00001) {
        s = l < 0.5 ? d / (maxc + minc) : d / (2.0 - maxc - minc);
        if (maxc == c.r)      h = mod((c.g - c.b) / d, 6.0);
        else if (maxc == c.g) h = (c.b - c.r) / d + 2.0;
        else                  h = (c.r - c.g) / d + 4.0;
        h /= 6.0;
        if (h < 0.0) h += 1.0;
    }
    return vec3(h, s, l);
}

float hue2rgb(float p, float q, float t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

vec3 hsl2rgb(vec3 hsl) {
    float h = hsl.x, s = hsl.y, l = hsl.z;
    if (s <= 0.00001) return vec3(l);
    float q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    float p = 2.0 * l - q;
    return vec3(hue2rgb(p, q, h + 1.0 / 3.0), hue2rgb(p, q, h), hue2rgb(p, q, h - 1.0 / 3.0));
}

// hueShiftDeg: 色相シフト(度、-180〜180) / satShift,lightShift: 加算オフセット(-1〜1)
vec3 adjustHSL(vec3 rgb, float hueShiftDeg, float satShift, float lightShift) {
    vec3 hsl = rgb2hsl(rgb);
    hsl.x = fract(hsl.x + hueShiftDeg / 360.0);
    hsl.y = clamp(hsl.y + satShift,   0.0, 1.0);
    hsl.z = clamp(hsl.z + lightShift, 0.0, 1.0);
    return hsl2rgb(hsl);
}

// ---------------------------------------------------------------------------
// 明るさ・コントラスト調整
// ---------------------------------------------------------------------------
// brightnessShift: 加算オフセット(-0.5〜0.5) / contrastFactor: 0.5を中心とした倍率(0〜2)
vec3 adjustBrightnessContrast(vec3 rgb, float brightnessShift, float contrastFactor) {
    vec3 c = (rgb - 0.5) * contrastFactor + 0.5 + brightnessShift;
    return clamp(c, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// カラーバランス(C/M/Yスライダー)
// ---------------------------------------------------------------------------
// cyanShift/magentaShift/yellowShift: それぞれR/G/Bチャンネルからの減算オフセット(-1〜1)。
// 正の値ほどそのインク(シアン/マゼンタ/イエロー)が強くなり、負の値で反対色(赤/緑/青)寄りになる。
vec3 adjustColorBalance(vec3 rgb, float cyanShift, float magentaShift, float yellowShift) {
    vec3 c = rgb - vec3(cyanShift, magentaShift, yellowShift);
    return clamp(c, 0.0, 1.0);
}
