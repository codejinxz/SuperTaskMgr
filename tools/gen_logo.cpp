// tools/gen_logo.cpp —— 「Windows 超级任务管理器」Logo 一次性生成器（可保留作再生成工具）。
//
// 用途：无图像工具环境下程序化绘制应用 Logo，输出：
//   assets/app.ico      多尺寸 ICO（16/24/32/48/64/128 走 32bpp BMP 块 + 256 走 PNG 块，
//                       手写 ICONDIR+ICONDIRENTRY 容器，PNG 块直接内嵌）
//   assets/logo_256.png 关于页展示用 256x256 RGBA PNG
//
// 设计（体现"监控"）：
//   · 深藏青垂直渐变圆角方块底（16px 下主体面积 ≥70%、高对比）；
//   · 青色→蓝色水平渐变的脉冲/速率折线（心电式尖峰），先宽幅低透明辉光再实心核心，
//     圆头圆角（逐段距离场天然形成）；
//   · 圆角方块细描边（青色低透明，上亮下暗）。
//
// 实现：全部软件光栅化。每个目标尺寸先按 4x 超采样绘制再盒式降采样（预乘 alpha 平均，
// 避免透明边缘暗边），得到全要素抗锯齿。PNG 编码器手写：zlib(stored 块) + IHDR/IDAT/IEND。
//
// 编译运行（VS 2022 BuildTools，任一 shell）：
//   cl /nologo /O2 /W4 /EHsc /utf-8 /Fe:gen_logo.exe gen_logo.cpp
//   gen_logo.exe <仓库根目录>        // 默认当前目录下的 assets/
//
// 本文件为独立工具，不参与 CMake 构建；中文注释按 /utf-8 处理。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 基础图像与颜色
// ---------------------------------------------------------------------------

struct Rgba {
    uint8_t r, g, b, a;
};

struct Image {
    int w = 0;
    int h = 0;
    std::vector<Rgba> px;  // 行优先，y 向下
    Rgba& At(int x, int y) { return px[static_cast<size_t>(y) * w + x]; }
    const Rgba& At(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// 结果覆盖在 dst 上（src 在上层，standard over 合成）。
void Over(Rgba* dst, float a, float r, float g, float b) {
    if (a <= 0.0f) return;
    const float da = dst->a / 255.0f;
    const float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) {
        dst->r = dst->g = dst->b = dst->a = 0;
        return;
    }
    const float outR = (r * a + dst->r * da * (1.0f - a)) / oa;
    const float outG = (g * a + dst->g * da * (1.0f - a)) / oa;
    const float outB = (b * a + dst->b * da * (1.0f - a)) / oa;
    dst->r = static_cast<uint8_t>(outR * 255.0f + 0.5f);
    dst->g = static_cast<uint8_t>(outG * 255.0f + 0.5f);
    dst->b = static_cast<uint8_t>(outB * 255.0f + 0.5f);
    dst->a = static_cast<uint8_t>(oa * 255.0f + 0.5f);
}

// ---------------------------------------------------------------------------
// 几何：圆角方块 SDF 与点到线段距离
// ---------------------------------------------------------------------------

// 圆角方块 SDF（p 相对中心；half 为半宽半高；r 为圆角半径），负值在内部。
// 标准形式：min(max(qx,qy),0) + length(max(q,0)) - r，其中 q = |p| - half + r。
float SdRoundedBox(float px, float py, float halfX, float halfY, float r) {
    const float qx = std::fabs(px) - halfX + r;
    const float qy = std::fabs(py) - halfY + r;
    const float axx = qx > 0.0f ? qx : 0.0f;
    const float ayy = qy > 0.0f ? qy : 0.0f;
    return fminf(fmaxf(qx, qy), 0.0f) + std::sqrt(axx * axx + ayy * ayy) - r;
}

// 点到线段的最小距离（圆头端帽由此天然形成）。
float SdSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float abx = bx - ax, aby = by - ay;
    const float apx = px - ax, apy = py - ay;
    const float dd = abx * abx + aby * aby;
    float t = dd > 0.0f ? (apx * abx + apy * aby) / dd : 0.0f;
    t = Clamp01(t);
    const float cx = ax + abx * t - px;
    const float cy = ay + aby * t - py;
    return std::sqrt(cx * cx + cy * cy);
}

// ---------------------------------------------------------------------------
// Logo 绘制（全部坐标以画布为单位 [0,1]，y 向下）
// ---------------------------------------------------------------------------

// 脉冲折线控制点（心电式：平—升—深降—回—小峰—回平）。
constexpr float kPulse[][2] = {
    {0.135f, 0.600f}, {0.320f, 0.600f}, {0.400f, 0.355f}, {0.480f, 0.710f},
    {0.560f, 0.470f}, {0.620f, 0.600f}, {0.865f, 0.600f},
};
constexpr int kPulseN = sizeof(kPulse) / sizeof(kPulse[0]);

// 渐变端色：青 → 蓝（青 #22D3EE → 蓝 #3B82F6）。
void PulseColor(float t, float* r, float* g, float* b) {
    *r = 34.0f + (59.0f - 34.0f) * t;
    *g = 211.0f + (130.0f - 211.0f) * t;
    *b = 238.0f + (246.0f - 238.0f) * t;
}

// 在超采样画布上绘制一帧（S 为超采样后的边长，sizePx 仅用于按物理像素决定线宽）。
Image DrawSupersampled(int sizePx, int ss) {
    const int S = sizePx * ss;
    Image im;
    im.w = im.h = S;
    im.px.assign(static_cast<size_t>(S) * S, Rgba{0, 0, 0, 0});

    // 主体边距：小尺寸固定 1 物理像素（16px 主体面积 (14/16)^2 ≈ 77% ≥ 70%），
    // 大尺寸按比例 4.5%。
    const float insetPx = sizePx <= 24 ? 1.0f : 0.045f * sizePx;
    const float inset = insetPx * ss;
    const float body = static_cast<float>(S) - 2.0f * inset;  // 方块边长（超采样像素）
    const float half = body * 0.5f;
    const float cx = static_cast<float>(S) * 0.5f;
    const float cy = cx;
    const float radius = body * 0.24f;              // 圆角半径
    const float outlineHalf = 0.6f * ss;            // 细描边半宽（≈1.2 物理像素）

    // 折线端点换算到超采样坐标。
    float ax[kPulseN], ay[kPulseN];
    for (int i = 0; i < kPulseN; ++i) {
        ax[i] = kPulse[i][0] * static_cast<float>(S);
        ay[i] = kPulse[i][1] * static_cast<float>(S);
    }
    // 核心线半宽：小尺寸至少 1 物理像素（即 2px 线宽），大尺寸按 8.5% 比例。
    const float coreHalf =
        (sizePx <= 24 ? 1.0f : 0.0425f * sizePx) * ss;
    const float glowHalf = coreHalf * 2.6f;

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            const float fx = static_cast<float>(x) + 0.5f;
            const float fy = static_cast<float>(y) + 0.5f;
            Rgba out{0, 0, 0, 0};

            // 1) 深藏青底：垂直渐变（上亮下暗），带一点对角提亮增加体积感。
            const float dBox = SdRoundedBox(fx - cx, fy - cy, half, half, radius);
            const float aBg = Clamp01(0.5f - dBox);
            if (aBg > 0.0f) {
                const float tGrad = Clamp01((fy - inset) / body);
                const float lift = 0.5f * (1.0f - tGrad);
                float r = 12.0f + 15.0f * (1.0f - tGrad) + lift * 6.0f;
                float g = 22.0f + 18.0f * (1.0f - tGrad) + lift * 8.0f;
                float b = 42.0f + 32.0f * (1.0f - tGrad) + lift * 10.0f;
                Over(&out, aBg, r, g, b);
            }

            // 2) 脉冲折线：先辉光层（宽幅低透明），再核心实心层。
            //    颜色沿 x 从青到蓝，端点取折线两端。
            float dLine = 1e9f;
            for (int i = 0; i + 1 < kPulseN; ++i) {
                dLine = fminf(dLine, SdSegment(fx, fy, ax[i], ay[i], ax[i + 1], ay[i + 1]));
            }
            const float tColor = Clamp01((fx - ax[0]) / (ax[kPulseN - 1] - ax[0]));
            float cr, cg, cb;
            PulseColor(tColor, &cr, &cg, &cb);
            const float aGlow = Clamp01(glowHalf + 0.5f - dLine) * 0.16f;
            Over(&out, aGlow, cr, cg, cb);
            const float aCore = Clamp01(coreHalf + 0.5f - dLine);
            Over(&out, aCore, cr, cg, cb);

            // 3) 细描边：圆角方块边缘 1.2px 青色描边，上亮下暗。
            const float aOut = Clamp01(outlineHalf + 0.5f - std::fabs(dBox));
            if (aOut > 0.0f) {
                const float tY = Clamp01((fy - inset) / body);
                const float k = 0.55f + 0.45f * (1.0f - tY);
                Over(&out, aOut * 0.42f, 125.0f * k + 20.0f, 232.0f * k, 255.0f * k);
            }

            im.At(x, y) = out;
        }
    }
    return im;
}

// 盒式降采样：预乘 alpha 累加再还原，避免透明边缘暗边。
Image Downsample(const Image& src, int ss) {
    const int w = src.w / ss;
    const int h = src.h / ss;
    Image out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h);
    const float norm = 1.0f / static_cast<float>(ss * ss);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < ss; ++j) {
                for (int i = 0; i < ss; ++i) {
                    const Rgba& p = src.At(x * ss + i, y * ss + j);
                    const float fa = p.a / 255.0f;
                    r += p.r * fa;
                    g += p.g * fa;
                    b += p.b * fa;
                    a += p.a;
                }
            }
            Rgba& o = out.At(x, y);
            const float af = a * norm / 255.0f;
            if (af > 1e-6f) {
                o.r = static_cast<uint8_t>(Clamp01(r * norm / 255.0f) * 255.0f + 0.5f);
                o.g = static_cast<uint8_t>(Clamp01(g * norm / 255.0f) * 255.0f + 0.5f);
                o.b = static_cast<uint8_t>(Clamp01(b * norm / 255.0f) * 255.0f + 0.5f);
            } else {
                o.r = o.g = o.b = 0;
            }
            o.a = static_cast<uint8_t>(Clamp01(af) * 255.0f + 0.5f);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// PNG 编码（RGBA8：deflate 压缩流 + IHDR/IDAT/IEND）
// ---------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t Adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void PutU32be(std::vector<uint8_t>* v, uint32_t x) {
    v->push_back(static_cast<uint8_t>(x >> 24));
    v->push_back(static_cast<uint8_t>(x >> 16));
    v->push_back(static_cast<uint8_t>(x >> 8));
    v->push_back(static_cast<uint8_t>(x));
}

void PutU16le(std::vector<uint8_t>* v, uint32_t x) {
    v->push_back(static_cast<uint8_t>(x));
    v->push_back(static_cast<uint8_t>(x >> 8));
}

void PngChunk(std::vector<uint8_t>* png, const char* type, const std::vector<uint8_t>& data) {
    PutU32be(png, static_cast<uint32_t>(data.size()));
    const size_t start = png->size();
    png->insert(png->end(), type, type + 4);
    png->insert(png->end(), data.begin(), data.end());
    const uint32_t crc = Crc32(png->data() + start, png->size() - start);
    PutU32be(png, crc);
}

// --- deflate 压缩（固定 Huffman + 简易 LZ77 哈希链匹配） ---------------------

struct BitWriter {
    std::vector<uint8_t>* out = nullptr;
    uint32_t buf = 0;  // 位缓冲（低位先出）
    int count = 0;
    void Put(uint32_t value, int n) {          // EXTRA 位等：value 低位在前
        buf |= value << count;
        count += n;
        while (count >= 8) {
            out->push_back(static_cast<uint8_t>(buf & 0xFF));
            buf >>= 8;
            count -= 8;
        }
    }
    void PutCode(uint32_t code, int n) {       // Huffman 码：高位在前
        for (int i = n - 1; i >= 0; --i) Put((code >> i) & 1u, 1);
    }
    void Flush() {
        if (count > 0) {
            out->push_back(static_cast<uint8_t>(buf & 0xFF));
            buf = 0;
            count = 0;
        }
    }
};

// 固定 Huffman 字面量/长度符号（0-143:8 位 / 144-255:9 位 / 256-279:7 位 / 280-287:8 位）。
void PutFixedSym(BitWriter& bw, int sym) {
    if (sym < 144) {
        bw.PutCode(0x30u + static_cast<uint32_t>(sym), 8);
    } else if (sym < 256) {
        bw.PutCode(0x190u + static_cast<uint32_t>(sym - 144), 9);
    } else if (sym < 280) {
        bw.PutCode(static_cast<uint32_t>(sym - 256), 7);
    } else {
        bw.PutCode(0xC0u + static_cast<uint32_t>(sym - 280), 8);
    }
}

// 长度 3..258 → (符号, EXTRA 位, EXTRA 值)。
void LenToSym(int len, int* sym, int* extraBits, int* extraVal) {
    if (len < 11) { *sym = 257 + len - 3; *extraBits = 0; *extraVal = 0; }
    else if (len < 19) { *sym = 265 + (len - 11) / 2; *extraBits = 1; *extraVal = (len - 11) % 2; }
    else if (len < 35) { *sym = 269 + (len - 19) / 4; *extraBits = 2; *extraVal = (len - 19) % 4; }
    else if (len < 67) { *sym = 273 + (len - 35) / 8; *extraBits = 3; *extraVal = (len - 35) % 8; }
    else if (len < 131) { *sym = 277 + (len - 67) / 16; *extraBits = 4; *extraVal = (len - 67) % 16; }
    else if (len < 258) { *sym = 281 + (len - 131) / 32; *extraBits = 5; *extraVal = (len - 131) % 32; }
    else { *sym = 285; *extraBits = 0; *extraVal = 0; }
}

// 距离 1..32768 → (符号, EXTRA 位, EXTRA 值)。
void DistToSym(int dist, int* sym, int* extraBits, int* extraVal) {
    if (dist <= 4) {
        *sym = dist - 1;
        *extraBits = 0;
        *extraVal = 0;
        return;
    }
    const int v = dist - 1;
    int hb = 0;
    while ((v >> (hb + 1)) != 0) ++hb;              // 最高有效位
    *sym = 2 * hb + ((v >> (hb - 1)) & 1);          // RFC1951 §3.2.5 距离表（V19-P0 修正：原式 -2 使 32764 个距离错码）
    *extraBits = hb - 1;
    *extraVal = v & ((1 << (hb - 1)) - 1);
}

// 单个固定 Huffman 动态块的 LZ77 压缩。
std::vector<uint8_t> DeflateRaw(const std::vector<uint8_t>& raw) {
    const size_t n = raw.size();
    std::vector<uint8_t> out;
    BitWriter bw;
    bw.out = &out;
    bw.Put(1, 1);  // BFINAL
    bw.Put(1, 2);  // BTYPE=01 固定 Huffman

    std::vector<int32_t> head(1 << 15, -1);
    std::vector<int32_t> prev(n, -1);
    auto hashAt = [&raw](size_t i) -> int {
        return static_cast<int>(((static_cast<uint32_t>(raw[i]) << 10) ^
                                 (static_cast<uint32_t>(raw[i + 1]) << 5) ^
                                 static_cast<uint32_t>(raw[i + 2])) &
                                0x7FFFu);
    };
    auto insertAt = [&](size_t i) {
        const int h = hashAt(i);
        prev[i] = head[h];
        head[h] = static_cast<int32_t>(i);
    };

    size_t i = 0;
    while (i < n) {
        int bestLen = 0;
        int bestDist = 0;
        if (i + 3 <= n) {
            int32_t cand = head[hashAt(i)];
            int tries = 0;
            while (cand >= 0 && tries < 32) {
                const int dist = static_cast<int>(i) - cand;
                if (dist > 32768) break;
                int l = 0;
                while (l < 258 && i + static_cast<size_t>(l) < n &&
                       raw[static_cast<size_t>(cand) + l] == raw[i + l]) {
                    ++l;
                }
                if (l >= 3 && l > bestLen) {
                    bestLen = l;
                    bestDist = dist;
                    if (l >= 258) break;
                }
                cand = prev[cand];
                ++tries;
            }
        }
        if (bestLen >= 3) {
            int sym, eb, ev, dsym, deb, dev;
            LenToSym(bestLen, &sym, &eb, &ev);
            PutFixedSym(bw, sym);
            if (eb > 0) bw.Put(static_cast<uint32_t>(ev), eb);
            DistToSym(bestDist, &dsym, &deb, &dev);
            bw.PutCode(static_cast<uint32_t>(dsym), 5);  // 距离码是独立的 5 位 Huffman 码表（V19-P0 修正：原按 8 位字面量写入）
            if (deb > 0) bw.Put(static_cast<uint32_t>(dev), deb);
            for (size_t k = i; k < i + static_cast<size_t>(bestLen) && k + 3 <= n; ++k) {
                insertAt(k);
            }
            i += static_cast<size_t>(bestLen);
        } else {
            PutFixedSym(bw, raw[i]);
            if (i + 3 <= n) insertAt(i);
            ++i;
        }
    }
    PutFixedSym(bw, 256);  // 块结束符号
    bw.Flush();
    return out;
}
std::vector<uint8_t> EncodePngRgba(const uint8_t* rgba, int w, int h) {
    // 原始流：每行前置 filter 字节 0。
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((rowBytes + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + static_cast<size_t>(y) * rowBytes,
                   rgba + static_cast<size_t>(y) * rowBytes + rowBytes);
    }

    // zlib 包装：0x78 0x01（32K 窗口、最快模式、校验和合法）+ deflate 压缩流 + Adler32。
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    const std::vector<uint8_t> body = DeflateRaw(raw);
    z.insert(z.end(), body.begin(), body.end());
    PutU32be(&z, Adler32(raw.data(), raw.size()));

    std::vector<uint8_t> png;
    const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);
    std::vector<uint8_t> ihdr;
    PutU32be(&ihdr, static_cast<uint32_t>(w));
    PutU32be(&ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // color type RGBA
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter
    ihdr.push_back(0);   // interlace
    PngChunk(&png, "IHDR", ihdr);
    PngChunk(&png, "IDAT", z);
    PngChunk(&png, "IEND", std::vector<uint8_t>{});
    return png;
}

// ---------------------------------------------------------------------------
// ICO 容器（ICONDIR + ICONDIRENTRY[] + 各图数据）
// ---------------------------------------------------------------------------

// 32bpp BMP 块：BITMAPINFOHEADER(40) + 自下而上 XOR(BGRA) + 自下而上 AND 掩码(1bpp,4字节行对齐)。
std::vector<uint8_t> BmpEntryData(const Image& im) {
    const int w = im.w, h = im.h;
    const size_t maskRow = ((static_cast<size_t>(w) + 31) / 32) * 4;
    const size_t maskBytes = maskRow * h;
    std::vector<uint8_t> d;
    d.reserve(40 + static_cast<size_t>(w) * h * 4 + maskBytes);

    const uint32_t biSizeImage =
        static_cast<uint32_t>(static_cast<size_t>(w) * h * 4 + maskBytes);
    auto put32 = [&d](uint32_t x) {
        d.push_back(static_cast<uint8_t>(x));
        d.push_back(static_cast<uint8_t>(x >> 8));
        d.push_back(static_cast<uint8_t>(x >> 16));
        d.push_back(static_cast<uint8_t>(x >> 24));
    };
    put32(40);        // biSize
    put32(static_cast<uint32_t>(w));   // biWidth
    put32(static_cast<uint32_t>(h) * 2);  // biHeight：XOR+AND 两倍高
    d.push_back(1);   // biPlanes
    d.push_back(0);
    d.push_back(32);  // biBitCount
    d.push_back(0);
    put32(0);         // biCompression = BI_RGB
    put32(biSizeImage);
    put32(0); put32(0); put32(0); put32(0);  // biXPels/biYPels/biClrUsed/biClrImportant

    for (int y = h - 1; y >= 0; --y) {  // XOR：自下而上 BGRA
        for (int x = 0; x < w; ++x) {
            const Rgba& p = im.At(x, y);
            d.push_back(p.b);
            d.push_back(p.g);
            d.push_back(p.r);
            d.push_back(p.a);
        }
    }
    for (int y = h - 1; y >= 0; --y) {  // AND 掩码：1=透明（alpha 通道为准的兜底）
        std::vector<uint8_t> row(maskRow, 0);
        for (int x = 0; x < w; ++x) {
            if (im.At(x, y).a < 128) row[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
        }
        d.insert(d.end(), row.begin(), row.end());
    }
    return d;
}

std::vector<uint8_t> BuildIco(const std::vector<int>& bmpSizes) {
    struct Entry {
        int size;
        bool isPng;
        std::vector<uint8_t> data;
    };
    std::vector<Entry> entries;
    for (int s : bmpSizes) {
        Image big = DrawSupersampled(s, 4);
        Entry e;
        e.size = s;
        e.isPng = false;
        e.data = BmpEntryData(Downsample(big, 4));
        entries.push_back(std::move(e));
    }
    Image big = DrawSupersampled(256, 4);
    Entry p;
    p.size = 256;
    p.isPng = true;
    p.data = EncodePngRgba(reinterpret_cast<const uint8_t*>(Downsample(big, 4).px.data()), 256, 256);
    entries.push_back(std::move(p));

    std::vector<uint8_t> ico;
    ico.push_back(0); ico.push_back(0);  // reserved
    ico.push_back(1); ico.push_back(0);  // type = icon
    ico.push_back(static_cast<uint8_t>(entries.size()));
    ico.push_back(0);
    uint32_t offset = 6 + static_cast<uint32_t>(entries.size()) * 16;
    for (const Entry& e : entries) {
        ico.push_back(e.size >= 256 ? 0 : static_cast<uint8_t>(e.size));  // 0 表示 256
        ico.push_back(e.size >= 256 ? 0 : static_cast<uint8_t>(e.size));
        ico.push_back(0);  // 调色板色数
        ico.push_back(0);  // reserved
        PutU16le(&ico, 1);                             // planes
        PutU16le(&ico, 32);                            // bit count
        auto put32 = [&ico](uint32_t x) {
            ico.push_back(static_cast<uint8_t>(x));
            ico.push_back(static_cast<uint8_t>(x >> 8));
            ico.push_back(static_cast<uint8_t>(x >> 16));
            ico.push_back(static_cast<uint8_t>(x >> 24));
        };
        put32(static_cast<uint32_t>(e.data.size()));
        put32(offset);
        offset += static_cast<uint32_t>(e.data.size());
    }
    for (const Entry& e : entries) ico.insert(ico.end(), e.data.begin(), e.data.end());
    return ico;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 输出目录：argv[1] 或默认 ./assets。
    std::wstring dir = argc > 1 ? argv[1] : L"assets";
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();

    const std::vector<int> kBmpSizes = {16, 24, 32, 48, 64, 128};
    const std::vector<uint8_t> ico = BuildIco(kBmpSizes);
    const std::vector<uint8_t> png = [&] {
        Image big = DrawSupersampled(256, 4);
        Image o = Downsample(big, 4);
        return EncodePngRgba(reinterpret_cast<const uint8_t*>(o.px.data()), o.w, o.h);
    }();

    const std::wstring icoPath = dir + L"\\app.ico";
    const std::wstring pngPath = dir + L"\\logo_256.png";
    if (!WriteFileBytes(icoPath, ico)) {
        wprintf(L"FAIL write %s\n", icoPath.c_str());
        return 1;
    }
    if (!WriteFileBytes(pngPath, png)) {
        wprintf(L"FAIL write %s\n", pngPath.c_str());
        return 1;
    }
    wprintf(L"OK %s (%zu bytes, %zu sizes) | %s (%zu bytes)\n", icoPath.c_str(), ico.size(),
            kBmpSizes.size() + 1, pngPath.c_str(), png.size());

    // argv[2] 传 --previews：额外把每个 ICO 尺寸导出为 preview_<n>.png（人工检查用）。
    if (argc > 2 && wcscmp(argv[2], L"--previews") == 0) {
        for (int s : kBmpSizes) {
            Image o = Downsample(DrawSupersampled(s, 4), 4);
            const std::vector<uint8_t> pv = EncodePngRgba(
                reinterpret_cast<const uint8_t*>(o.px.data()), o.w, o.h);
            const std::wstring path = dir + L"\\preview_" + std::to_wstring(s) + L".png";
            if (!WriteFileBytes(path, pv)) return 1;
        }
        wprintf(L"previews written\n");
    }

    // 自检：重新解析 ICO 目录，校验偏移连续、条目尺寸与数据一致。
    uint32_t off = 6 + static_cast<uint32_t>(kBmpSizes.size() + 1) * 16;
    for (size_t i = 0; i < kBmpSizes.size() + 1; ++i) {
        const uint32_t bytes =
            ico[6 + i * 16 + 8] | (ico[6 + i * 16 + 9] << 8) | (ico[6 + i * 16 + 10] << 16) |
            (static_cast<uint32_t>(ico[6 + i * 16 + 11]) << 24);
        const uint32_t dataOff =
            ico[6 + i * 16 + 12] | (ico[6 + i * 16 + 13] << 8) | (ico[6 + i * 16 + 14] << 16) |
            (static_cast<uint32_t>(ico[6 + i * 16 + 15]) << 24);
        if (dataOff != off) {
            wprintf(L"FAIL ico entry %zu offset %u != %u\n", i, dataOff, off);
            return 1;
        }
        off += bytes;
    }
    if (off != ico.size()) {
        wprintf(L"FAIL ico total %u != %zu\n", off, ico.size());
        return 1;
    }
    wprintf(L"ICO container self-check OK\n");
    return 0;
}
