#include <ui_core.h>
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <exception>

using namespace Gdiplus;

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dbghelp.lib")

// ---------- crash diagnostics ----------

static void CrashLog(const char* ctx, PVOID* frames, int n)
{
    FILE* f = NULL;
    fopen_s(&f, "crash.log", "a");
    if (!f)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", (int)st.wHour, (int)st.wMinute,
            (int)st.wSecond, (int)st.wMilliseconds, ctx);
    if (frames && n > 0)
    {
        for (int i = 0; i < n; i++)
            fprintf(f, "  %p\n", frames[i]);
    }
    fputs("\n", f);
    fclose(f);
    OutputDebugStringA(ctx);
}

static void CrashLogNow(const char* ctx)
{
    PVOID fr[32];
    USHORT n = CaptureStackBackTrace(0, 32, fr, NULL);
    CrashLog(ctx, fr, n);
}

static LONG NTAPI CrashVe(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000409 || code == 0xC0000005 || code == 0x80000003)
    {
        char ctx[160];
        sprintf_s(ctx, "EXCEPTION 0x%08X addr=%p (params0=%llu, n=%lu)",
                  code, (void*)ep->ExceptionRecord->ExceptionAddress,
                  (unsigned long long)(code == 0xC0000409
                      ? ep->ExceptionRecord->ExceptionInformation[0] : 0),
                  (unsigned long)ep->ExceptionRecord->NumberParameters);
        PVOID fr[32];
        USHORT n = CaptureStackBackTrace(0, 32, fr, NULL);
        CrashLog(ctx, fr, n);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void TermHandler()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILE* f = NULL;
    fopen_s(&f, "crash.log", "a");
    if (f)
    {
        fprintf(f, "\n[%02d:%02d:%02d.%03d] std::terminate: uncaught C++ exception (tid=%lu)\n",
                (int)st.wHour, (int)st.wMinute, (int)st.wSecond, (int)st.wMilliseconds,
                (unsigned long)GetCurrentThreadId());
        MEMORYSTATUSEX ms;
        ZeroMemory(&ms, sizeof(ms));
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
            fprintf(f, "  sys mem: load=%u%% avail=%lluMB\n", ms.dwMemoryLoad,
                    (unsigned long long)(ms.ullAvailPhys >> 20));
        fclose(f);
    }
    CrashLogNow("std::terminate: uncaught C++ exception");
    abort();
}

static void InvalidParamHandler(const wchar_t*, const wchar_t*, const wchar_t*,
                                unsigned int, uintptr_t)
{
    CrashLogNow("_invalid_parameter (returning, app continues)");
}

// ---------- state ----------

enum
{
    HOTKEY_REGION = 1,
    HOTKEY_CAPTURE = 2,
};

struct Item
{
    std::vector<unsigned char> jpg;  // 截图时即 JPEG 压缩，不保留原始位图
    int w = 0, h = 0;
    bool bg;                         // 第一张 = 背景图
    int seq;
    int id;
    SYSTEMTIME time;
    std::string thumbName;           // 当前注册的缩略图资源名（含版本号）
    int thumbGen;                    // 缩略图生成版本（-1 = 尚未生成）

    Item() : bg(false), seq(0), id(0), thumbGen(-1)
    {
        ZeroMemory(&time, sizeof(time));
    }
};

static HINSTANCE g_hInst = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static CLSID g_pngClsid = {0};
static CLSID g_jpegClsid = {0};

static UiPage g_page = 0;
static UiWindow g_win = 0;
static UiWidget g_gh = 0;

static std::vector<Item*> g_items;
static std::map<std::string, std::vector<unsigned char>> g_blobs;
static std::mutex g_blobsMutex;

static int ThumbResolver(const char* name, const void** out_bytes,
                         size_t* out_size, void* userdata)
{
    std::lock_guard<std::mutex> lk(g_blobsMutex);
    auto it = g_blobs.find(name);
    if (it == g_blobs.end())
        return 0;
    *out_bytes = it->second.data();
    *out_size = it->second.size();
    return 1;
}
static int g_seq = 0;
static int g_nextId = 1;
static int g_thumbGen = 0;
static RECT g_region = {0, 0, 0, 0};
static bool g_hasRegion = false;
static int g_topMargin = 80;
static int g_bottomMargin = 40;
static int g_gap = 0;
static bool g_pinned = false;
static UiWidget g_pinBtn = 0;

// 图钉图标（Fluent UI System Icons Pin 24 regular）
static const char* kPinSvg =
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M16.2425 2.93189L21.0682 7.75765C22.3955 9.08491 22.0324 11.3224"
    " 20.3535 12.1619L15.4826 14.5973C15.3073 14.685 15.1732 14.8379 15.1092"
    " 15.0232L13.6699 19.1895C13.3684 20.0622 12.2574 20.3181 11.6045 19.6653"
    "L8.50002 16.5607L4.06074 21.0001H3L3.00008 19.9394L7.43936 15.5001L4.33487"
    " 12.3956C3.682 11.7427 3.93791 10.6317 4.81061 10.3302L8.97688 8.89096C9.16223"
    " 8.82694 9.31512 8.69287 9.40281 8.51748L11.8382 3.6466C12.6777 1.96772 14.9152"
    " 1.60462 16.2425 2.93189ZM20.0076 8.81831L15.1818 3.99255C14.5785 3.38924"
    " 13.5614 3.55429 13.1799 4.31742L10.7445 9.18829C10.4814 9.71446 10.0227"
    " 10.1167 9.46666 10.3087L5.67812 11.6175L12.3826 18.322L13.6914 14.5335C13.8835"
    " 13.9774 14.2857 13.5188 14.8118 13.2557L19.6827 10.8202C20.4458 10.4387"
    " 20.6109 9.42161 20.0076 8.81831Z\"/>"
    "</svg>";

static std::string g_itemsJson;
static int g_pushedCount = -1;
static int g_pushedTop = -1;
static int g_pushedBottom = -1;
static int g_pushedGap = -1;

// ---- region overlay ----
static HBITMAP g_snap = NULL;
static HBITMAP g_mask = NULL;
static HBITMAP g_dim = NULL;  // 预合成"压暗快照"（快照+遮罩一次算好，重绘只 BitBlt）
static HDC g_dimDC = NULL;    // 常驻 DC，重绘期间零分配，避免内存吃紧时创建失败导致闪烁
static HDC g_snapDC = NULL;
static HPEN g_selPen = NULL;
static HWND g_regionAppHwnd = NULL;  // 选区期间隐藏的 app 窗口
static int g_snapW = 0, g_snapH = 0;
static int g_regOx = 0, g_regOy = 0;
static bool g_dragging = false;
static POINT g_selStart = {0, 0}, g_selCur = {0, 0};
static RECT g_newRegion = {0, 0, 0, 0};
static bool g_newRegionValid = false;

// ---------- helpers ----------

static std::string W2U(const wchar_t* s)
{
    if (!s || !*s)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 1)
        return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, NULL, NULL);
    return out;
}

static std::wstring ToWide(const char* s)
{
    if (!s || !*s)
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

static std::string JsonStr(const wchar_t* s)
{
    std::string r = std::string("\"");
    for (; s && *s; s++)
    {
        if (*s == L'\\')
            r += "\\\\";
        else if (*s == L'"')
            r += "\\\"";
        else if (*s == L'\n')
            r += "\\n";
        else if (*s == L'\t')
            r += "\\t";
        else if (*s < 0x20 || *s > 0x10FFFF)
        {
            wchar_t esc[8];
            swprintf_s(esc, L"\\u%04X", (unsigned)*s);
            r += W2U(esc);
        }
        else
            r += W2U(std::wstring(1, *s).c_str());
    }
    r += "\"";
    return r;
}

static void SetState(const char* key, const std::string& json)
{
    if (g_page)
        ui_page_set_json(g_page, key, json.c_str());
}

static void SetRegionInfo(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    SetState("regionInfo", JsonStr(buf));
}

// ---------- capture / stitch ----------

static Bitmap* CaptureScreenRect(int x, int y, int w, int h)
{
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP hb = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, hb);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    Bitmap* gb = NULL;
    try
    {
        gb = new Bitmap(hb, (HPALETTE)NULL);
    }
    catch (...)
    {
        gb = NULL;
    }
    DeleteObject(hb);
    return gb;
}

// 第一张背景/其余裁剪上下边距；每条带尾随 gap，全量与增量共用公式
static int StripPxH(int w, int h, bool isBg, double scale)
{
    int sh = isBg ? h : std::max(0, h - g_topMargin - g_bottomMargin);
    if (sh <= 0)
        return 0;
    return std::max(1, (int)((sh + g_gap) * scale));
}

// 单帧解码复用
static Bitmap* g_frame = NULL;   // 复用缓冲
static int g_frameW = 0, g_frameH = 0;

// 把 Item 的 JPEG 解码到复用帧，返回帧指针（有效至下一次 ItemFrame/尺寸变化）
static Bitmap* ItemFrame(Item* it)
{
    if (!it || it->jpg.empty())
        return NULL;
    if (it->w != g_frameW || it->h != g_frameH)
    {
        delete g_frame;
        g_frame = NULL;
    }
    if (!g_frame && it->w > 0 && it->h > 0)
        g_frame = new Bitmap(it->w, it->h, PixelFormat32bppARGB);
    if (!g_frame)
        return NULL;
    IStream* s = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &s) != S_OK)
        return NULL;
    ULONG written = 0;
    if (s->Write(it->jpg.data(), (ULONG)it->jpg.size(), &written) != S_OK)
    {
        s->Release();
        return NULL;
    }
    LARGE_INTEGER pos = {};
    pos.QuadPart = 0;
    if (s->Seek(pos, STREAM_SEEK_SET, NULL) != S_OK)
    {
        s->Release();
        return NULL;
    }
    Bitmap dec(s);
    if (dec.GetLastStatus() != Ok)
    {
        s->Release();
        return NULL;
    }
    Graphics gg(g_frame);
    gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gg.Clear(Color(255, 255, 255, 255));
    gg.DrawImage(&dec, 0, 0, it->w, it->h);
    s->Release();
    g_frameW = it->w;
    g_frameH = it->h;
    return g_frame;
}

static std::vector<unsigned char> EncodeJpeg(Bitmap* bmp, int quality)
{
    std::vector<unsigned char> out;
    if (!bmp)
        return out;
    IStream* s = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &s) != S_OK)
        return out;
    EncoderParameters ep;
    ep.Count = 1;
    ep.Parameter[0].Guid = EncoderQuality;
    ep.Parameter[0].Type = EncoderParameterValueTypeLong;
    ep.Parameter[0].NumberOfValues = 1;
    ep.Parameter[0].Value = &quality;
    if (bmp->Save(s, &g_jpegClsid, &ep) == Ok)
    {
        HGLOBAL hg = NULL;
        if (GetHGlobalFromStream(s, &hg) == S_OK && hg)
        {
            SIZE_T sz = GlobalSize(hg);
            LPVOID p = GlobalLock(hg);
            if (p && sz > 0)
                out.assign((unsigned char*)p, (unsigned char*)p + sz);
            GlobalUnlock(hg);
        }
    }
    s->Release();
    return out;
}

static void CopyBitmapRows(Bitmap* dst, Bitmap* src, int srcY, int dstY, int rows)
{
    if (rows <= 0)
        return;
    int w = (int)src->GetWidth();
    BitmapData ds, dd;
    if (src->LockBits(&Rect(0, srcY, w, rows), ImageLockModeRead,
                      PixelFormat32bppARGB, &ds) != Ok)
        return;
    if (dst->LockBits(&Rect(0, dstY, w, rows), ImageLockModeWrite,
                      PixelFormat32bppARGB, &dd) != Ok)
    {
        src->UnlockBits(&ds);
        return;
    }
    BYTE* sp = (BYTE*)ds.Scan0;
    BYTE* dp = (BYTE*)dd.Scan0;
    int ss = ds.Stride, dsv = dd.Stride;
    if (ss < 0)
    {
        sp += (rows - 1) * ss;
        ss = -ss;
    }
    if (dsv < 0)
    {
        dp += (rows - 1) * dsv;
        dsv = -dsv;
    }
    int cpy = w * 4;
    for (int r = 0; r < rows; r++)
        memcpy(dp + (size_t)r * dsv, sp + (size_t)r * ss, cpy);
    dst->UnlockBits(&dd);
    src->UnlockBits(&ds);
}

// 位图内行搬移
static void ShiftRowsInPlace(Bitmap* bm, int srcY, int dstY, int rows)
{
    if (rows <= 0 || srcY == dstY)
        return;
    int w = (int)bm->GetWidth();
    int lo = std::min(srcY, dstY);
    int hi = std::max(srcY, dstY) + rows;
    BitmapData d;
    if (bm->LockBits(&Rect(0, lo, w, hi - lo), ImageLockModeWrite,
                     PixelFormat32bppARGB, &d) != Ok)
        return;
    BYTE* base = (BYTE*)d.Scan0;
    int stride = d.Stride;
    if (stride < 0)
    {
        base += (hi - lo - 1) * stride;
        stride = -stride;
    }
    int cpy = w * 4;
    BYTE* sp = base + (size_t)(srcY - lo) * stride;
    BYTE* dp = base + (size_t)(dstY - lo) * stride;
    if (dp > sp)
    {
        for (int r = rows - 1; r >= 0; r--)
            memcpy(dp + (size_t)r * stride, sp + (size_t)r * stride, cpy);
    }
    else
    {
        for (int r = 0; r < rows; r++)
            memcpy(dp + (size_t)r * stride, sp + (size_t)r * stride, cpy);
    }
    bm->UnlockBits(&d);
}

static Bitmap* BuildStitched(int capW, InterpolationMode interp = InterpolationModeHighQualityBicubic)
{
    if (g_items.empty())
        return NULL;
    int maxW = 0;
    for (size_t i = 0; i < g_items.size(); i++)
        maxW = std::max(maxW, g_items[i]->w);
    double scale = (capW > 0 && maxW > capW) ? (double)capW / maxW : 1.0;
    int tw = std::max(1, (int)(maxW * scale));
    int th = 0;
    for (size_t i = 0; i < g_items.size(); i++)
        th += StripPxH(g_items[i]->w, g_items[i]->h, i == 0, scale);

    Bitmap* res = new Bitmap(tw, std::max(1, th), PixelFormat32bppARGB);
    Graphics g(res);
    g.SetInterpolationMode(interp);
    g.Clear(Color(255, 255, 255, 255));

    int y = 0;
    for (size_t i = 0; i < g_items.size(); i++)
    {
        Bitmap* b = ItemFrame(g_items[i]);
        if (!b)
            continue;
        bool bg = (i == 0);
        int sp = StripPxH(g_items[i]->w, g_items[i]->h, bg, scale);
        if (sp <= 0)
            continue;
        if (bg)
        {
            double bw = b->GetWidth() * scale;
            double bh = b->GetHeight() * scale;
            g.DrawImage(b, (REAL)((tw - bw) / 2), (REAL)y, (REAL)bw, (REAL)bh);
        }
        else
        {
            int sh = g_items[i]->h - g_topMargin - g_bottomMargin;
            if (sh <= 0)
                continue;
            double sw = b->GetWidth() * scale;
            double shh = sh * scale;
            g.DrawImage(b, RectF((REAL)((tw - sw) / 2), (REAL)y, (REAL)sw, (REAL)shh),
                        0, (REAL)g_topMargin, (REAL)b->GetWidth(), (REAL)sh, UnitPixel);
        }
        y += sp;
    }
    return res;
}

// ---------- UI push ----------

// 最终成品图尺寸
static void PushFinalSize()
{
    if (g_items.empty())
    {
        SetState("finalSize", JsonStr(L"--"));
        return;
    }
    int maxW = 0, th = 0;
    for (size_t i = 0; i < g_items.size(); i++)
    {
        maxW = std::max(maxW, g_items[i]->w);
        th += StripPxH(g_items[i]->w, g_items[i]->h, i == 0, 1.0);
    }
    wchar_t buf[32];
    swprintf_s(buf, L"%dx%d", maxW, th);
    SetState("finalSize", JsonStr(buf));
}

// 滑块由 C 回调接管, 覆盖 v-model，推回 UI 刷新 px 标签
static void PushMarginState()
{
    SetState("topMargin", std::to_string(g_topMargin));
    SetState("bottomMargin", std::to_string(g_bottomMargin));
    SetState("gap", std::to_string(g_gap));
    PushFinalSize();
}

static void OnDelMount(UiPage, UiWidget w, void* ud);
static void MakeThumbPng(Item* it, std::string& out_name,
                         std::vector<unsigned char>& out_bytes);

static void PushItems(bool forceAll = false)
{
    std::vector<std::string> live;
    std::string j = "[";
    for (size_t i = 0; i < g_items.size(); i++)
    {
        Item* it = g_items[i];
        if (forceAll || it->thumbGen < 0)
        {
            std::string name;
            std::vector<unsigned char> bytes;
            MakeThumbPng(it, name, bytes);   // 注册缩略图 blob，src 带版本号
        }
        live.push_back(it->thumbName);
        if (i)
            j += ",";
        wchar_t tm[16];
        swprintf_s(tm, L"%02d:%02d:%02d", (int)it->time.wHour,
                   (int)it->time.wMinute, (int)it->time.wSecond);
        char buf[160];
        sprintf_s(buf, "{\"id\":%d,\"seq\":%d,\"time\":", it->id, it->seq);
        j += buf;
        j += JsonStr(tm);
        j += ",\"thumb\":\"" + it->thumbName + "\"}";
    }
    j += "]";
    // 清理不再被任何条目引用的旧缩略图 blob
    {
        std::lock_guard<std::mutex> lk(g_blobsMutex);
        for (auto it = g_blobs.begin(); it != g_blobs.end(); )
        {
            bool ref = false;
            for (size_t i = 0; i < live.size() && !ref; i++)
                ref = (live[i] == it->first);
            if (ref)
                ++it;
            else
                it = g_blobs.erase(it);
        }
    }
    g_itemsJson = j;
    SetState("items", g_itemsJson);
    char cnt[32];
    sprintf_s(cnt, "%d", (int)g_items.size());
    SetState("count", cnt);
    PushFinalSize();
    for (size_t i = 0; i < g_items.size(); i++)
    {
        char idBuf[32];
        sprintf_s(idBuf, "del_%d", g_items[i]->id);
        ui_page_on_widget_mount(g_page, idBuf, OnDelMount,
                                (void*)(intptr_t)g_items[i]->id);
    }
    g_pushedCount = (int)g_items.size();
}

static void MakeThumbPng(Item* it, std::string& out_name,
                         std::vector<unsigned char>& out_bytes)
{
    it->thumbGen = ++g_thumbGen;
    Bitmap* src = ItemFrame(it);
    if (!src)
        return;
    int w = it->w;
    int h = it->h;
    int sw = w, sh = h, sy = 0;
    if (!it->bg)
    {
        sh = std::max(1, h - g_topMargin - g_bottomMargin);
        sy = std::min(g_topMargin, h - sh);
    }
    double s = std::min(200.0 / sw, 60.0 / sh);
    int nw = std::max(1, (int)(sw * s));
    int nh = std::max(1, (int)(sh * s));
    Bitmap b(nw, nh, PixelFormat32bppARGB);
    Graphics g(&b);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(Color(255, 255, 255, 255));
    g.DrawImage(src, RectF(0, 0, (REAL)nw, (REAL)nh),
                0, (REAL)sy, (REAL)w, (REAL)sh, UnitPixel);

    IStream* stream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK)
        return;
    if (b.Save(stream, &g_pngClsid, NULL) == Ok)
    {
        HGLOBAL hg = NULL;
        if (GetHGlobalFromStream(stream, &hg) == S_OK && hg)
        {
            SIZE_T sz = GlobalSize(hg);
            LPVOID p = GlobalLock(hg);
            if (p && sz > 0)
            {
                out_bytes.assign((unsigned char*)p,
                                 (unsigned char*)p + sz);
            }
            GlobalUnlock(hg);
        }
    }
    stream->Release();

    std::string name = "t" + std::to_string(it->id) + "_v" +
                       std::to_string(it->thumbGen) + ".png";
    it->thumbName = name;
    {
        std::lock_guard<std::mutex> lk(g_blobsMutex);
        g_blobs[name] = out_bytes;
    }
    out_name = name;
}

static Bitmap* g_previewCache = NULL;
static int g_previewCacheCount = 0;  // 已拼进缓存的条目数
static int g_previewCacheMaxW = 0;   // 缓存采用的 maxW（决定缩放比例）
static int g_previewCacheTop = 0, g_previewCacheBottom = 0, g_previewCacheGap = 0;
static int g_previewCacheCapH = 0;   // 缓存位图分配的容量高（≥ 实际内容高）
static std::vector<int> g_cacheStrips;  // 每条目条带像素高（含尾随 gap）
static bool g_dbgIncrDump = false;   // debug：增量重建后保存 dump_incr.png

static void FreePreviewCache()
{
    if (g_previewCache)
    {
        delete g_previewCache;
        g_previewCache = NULL;
    }
    g_previewCacheCount = 0;
    g_previewCacheMaxW = 0;
    g_previewCacheCapH = 0;
    g_cacheStrips.clear();
}

// 把整张拼接缓存喂给 gh_img_view：begin 重建 + 整体 preview（原版整图推送）
static void PushCacheToView(Bitmap* full, uint32_t logicalH = 0)
{
    if (!full || !g_gh || !g_win)
        return;
    if (logicalH == 0)
        logicalH = (uint32_t)full->GetHeight();
    if (logicalH > (uint32_t)full->GetHeight())
        logicalH = (uint32_t)full->GetHeight();
    BitmapData data;
    if (full->LockBits(&Rect(0, 0, (int)full->GetWidth(), (int)full->GetHeight()),
                       ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok)
    {
        UiGhImgViewInfo info{};
        info.full_width = (uint32_t)full->GetWidth();
        info.full_height = logicalH;
        info.tile_size = 256;
        info.levels = 1;
        info.keep_preview = 0;
        ui_gh_img_view_begin(g_gh, g_win, &info);
        ui_gh_img_view_set_preview(g_gh, g_win, data.Scan0,
                                   (uint32_t)full->GetWidth(),
                                   logicalH,
                                   (uint32_t)data.Stride);
        ui_gh_img_view_fit(g_gh);
        full->UnlockBits(&data);
    }
}

// 拖动边距/间隔滑块时的增量重建
static void PushPreview(int deletedAt = -1);
static void PushPreviewIncremental()
{
    if (!g_gh || !g_win || !g_previewCache)
    {
        PushPreview();
        return;
    }
    if (g_items.empty() || (int)g_items.size() != g_previewCacheCount ||
        (int)g_cacheStrips.size() != g_previewCacheCount)
    {
        PushPreview();
        return;
    }
    if (g_previewCacheTop == g_topMargin && g_previewCacheBottom == g_bottomMargin &&
        g_previewCacheGap == g_gap)
        return;
    const size_t n = g_items.size();
    const int oldTop = g_previewCacheTop, oldBottom = g_previewCacheBottom,
              oldGap = g_previewCacheGap;
    double scale = g_previewCacheMaxW > 960 ? 960.0 / g_previewCacheMaxW : 1.0;
    int newW = std::max(1, (int)(g_previewCacheMaxW * scale));
    if (newW != (int)g_previewCache->GetWidth())
    {
        PushPreview();
        return;
    }
    std::vector<int> newStrips(n), oldOff(n + 1, 0), newOff(n + 1, 0);
    int newH = 0;
    for (size_t i = 0; i < n; i++)
    {
        newStrips[i] = StripPxH(g_items[i]->w, g_items[i]->h, i == 0, scale);
        newH += newStrips[i];
    }
    for (size_t i = 0; i < n; i++)
    {
        oldOff[i + 1] = oldOff[i] + g_cacheStrips[i];
        newOff[i + 1] = newOff[i] + newStrips[i];
    }
    if (newH > g_previewCacheCapH || newH <= 0)
    {
        PushPreview();
        return;
    }
    // 缩放像素单位的边距/间隔增量
    double topD = (n > 1) ? (g_topMargin - oldTop) * scale : 0.0;
    double bottomD = (n > 1) ? (g_bottomMargin - oldBottom) * scale : 0.0;
    int drawTop = std::max(0, (int)(-topD));
    int drawBottom = std::max(0, (int)(-bottomD));
    int dropTop = std::max(0, (int)topD);
    int dropBottom = std::max(0, (int)bottomD);
    // 每条目内容块的位移方向
    // 混合方向时相邻条目可能互相覆盖源区，回退全量重建
    bool anyUp = false, anyDown = false;
    for (int i = 1; i < (int)n; i++)
    {
        int dTop = (i == 0) ? 0 : dropTop;
        int delta = (newOff[i] + drawTop) - (oldOff[i] + dTop);
        if (delta < 0)
            anyUp = true;
        else if (delta > 0)
            anyDown = true;
    }
    if (anyUp && anyDown)
    {
        PushPreview();
        return;
    }
    // 内容行搬移
    if (anyUp)
    {
        for (int i = 1; i < (int)n; i++)
        {
            double contentOld = g_cacheStrips[i] - oldGap * scale;
            double contentNew = newStrips[i] - g_gap * scale;
            int dTop = (i == 0) ? 0 : dropTop;
            int common = std::max(0, (int)(contentNew - drawTop - drawBottom));
            common = std::min(common, std::max(0, (int)(contentOld - dTop)));
            if (common > 0)
                ShiftRowsInPlace(g_previewCache, oldOff[i] + dTop,
                                 newOff[i] + drawTop, common);
        }
    }
    else
    {
        for (int i = (int)n - 1; i >= 1; i--)
        {
            double contentOld = g_cacheStrips[i] - oldGap * scale;
            double contentNew = newStrips[i] - g_gap * scale;
            int dTop = (i == 0) ? 0 : dropTop;
            int common = std::max(0, (int)(contentNew - drawTop - drawBottom));
            common = std::min(common, std::max(0, (int)(contentOld - dTop)));
            if (common > 0)
                ShiftRowsInPlace(g_previewCache, oldOff[i] + dTop,
                                 newOff[i] + drawTop, common);
        }
    }
    {
        Graphics g(g_previewCache);
        g.SetInterpolationMode(InterpolationModeBilinear);
        for (size_t i = 0; i < n; i++)
        {
            int no = newOff[i];
            int dTopNew = (i == 0) ? 0 : drawTop;
            int common = std::max(0, (int)(newStrips[i] - g_gap * scale -
                                           dTopNew - drawBottom));
            int gapRows = newStrips[i] - (dTopNew + common + drawBottom);
            if (i > 0)
            {
                Bitmap* b = ItemFrame(g_items[i]);
                if (!b)
                    continue;
                double sw = b->GetWidth() * scale;
                double sx = (newW - sw) / 2;
                if (drawTop > 0)
                {
                    g.DrawImage(b, RectF((REAL)sx, (REAL)no, (REAL)sw, (REAL)drawTop),
                                0, (REAL)g_topMargin, (REAL)b->GetWidth(),
                                (REAL)(oldTop - g_topMargin), UnitPixel);
                }
                if (drawBottom > 0)
                {
                    g.DrawImage(b, RectF((REAL)sx, (REAL)(no + dTopNew + common),
                                         (REAL)sw, (REAL)drawBottom),
                                0, (REAL)((int)b->GetHeight() - oldBottom),
                                (REAL)b->GetWidth(),
                                (REAL)(oldBottom - g_bottomMargin), UnitPixel);
                }
            }
if (gapRows > 0)
                g.FillRectangle(&SolidBrush(Color(255, 255, 255, 255)),
                                RectF(0, (REAL)(no + dTopNew + common + drawBottom),
                                      (REAL)newW, (REAL)gapRows));
        }
    }
    PushCacheToView(g_previewCache, (uint32_t)newH);
    if (g_dbgIncrDump)
    {
        char fn[128];
        snprintf(fn, sizeof(fn), "dump_incr_%d_%d_%d.png", g_topMargin,
                 g_bottomMargin, g_gap);
        try { g_previewCache->Save(ToWide(fn).c_str(), &g_pngClsid, NULL); }
        catch (...) {}
    }
    g_cacheStrips = newStrips;
    g_previewCacheTop = g_topMargin;
    g_previewCacheBottom = g_bottomMargin;
    g_previewCacheGap = g_gap;
}

static void PushPreview(int deletedAt)
{
    if (!g_gh || !g_win)
        return;
    if (g_items.empty())
    {
        ui_gh_img_view_clear(g_gh);
        FreePreviewCache();
        return;
    }
    Bitmap* full = NULL;
    const bool marginsSame = (g_previewCacheTop == g_topMargin) &&
                             (g_previewCacheBottom == g_bottomMargin) &&
                             (g_previewCacheGap == g_gap);
    Item* last = g_items.back();
    double scale = g_previewCacheMaxW > 960 ? 960.0 / g_previewCacheMaxW : 1.0;
    int newW = std::max(1, (int)(g_previewCacheMaxW * scale));
    const bool appendable = g_previewCache &&
        marginsSame &&
        (g_previewCacheCount + 1 == (int)g_items.size()) &&
        (last->w <= g_previewCacheMaxW);
    const bool deletable = g_previewCache &&
        marginsSame && deletedAt >= 0 &&
        (g_previewCacheCount == (int)g_items.size() + 1) &&
        (g_cacheStrips.size() == (size_t)g_previewCacheCount);
    if (appendable)
    {
        // 增量
        Bitmap* lastF = ItemFrame(last);
        int oldH = g_previewCache->GetHeight();
        int addH = StripPxH(last->w, last->h, false, scale);
        int newH = oldH + addH;
        Bitmap* nb = new Bitmap(newW, newH, PixelFormat32bppARGB);
        CopyBitmapRows(nb, g_previewCache, 0, 0, oldH);
        if (addH > 0 && lastF)
        {
            Graphics g(nb);
            g.SetInterpolationMode(InterpolationModeBilinear);
            int sh = last->h - g_topMargin - g_bottomMargin;
            double sw = last->w * scale;
            double sH = sh * scale;
            g.DrawImage(lastF, RectF((REAL)((newW - sw) / 2), (REAL)oldH,
                                     (REAL)sw, (REAL)sH),
                        0, (REAL)g_topMargin, (REAL)lastF->GetWidth(), (REAL)sh,
                        UnitPixel);
        }
        delete g_previewCache;
        g_previewCache = NULL;
        g_cacheStrips.push_back(addH);
        full = nb;
    }
    else if (deletable)
    {
        int k = deletedAt;
        int oldH = g_previewCache->GetHeight();
        int stripK = g_cacheStrips[k];
        int offK = 0;
        for (int t = 0; t < k; t++)
            offK += g_cacheStrips[t];
        int newFirstStrip = (k == 0) ? StripPxH(g_items[0]->w, g_items[0]->h, true, scale) : 0;
        int newH = oldH - stripK + (k == 0 ? newFirstStrip - stripK : 0);
        Bitmap* nb = new Bitmap(newW, std::max(1, newH), PixelFormat32bppARGB);
        if (k > 0)
        {
            CopyBitmapRows(nb, g_previewCache, 0, 0, offK);
            CopyBitmapRows(nb, g_previewCache, offK + stripK, offK,
                           oldH - offK - stripK);
        }
        else
        {
            // 删除第 0 张（背景）后：旧条带向上平移 newFirstStrip - stripK，
            // 目标起点必须是 newFirstStrip - stripK（等于 newH - (oldH-stripK)），
            // 否则会越界写入 nb（终点 = newH + stripK）。
            int dstY = newFirstStrip - stripK;
            if (dstY < 0) dstY = 0;
            CopyBitmapRows(nb, g_previewCache, stripK, dstY,
                           oldH - stripK - (dstY));
            if (newFirstStrip > 0)
            {
                Bitmap* f0 = ItemFrame(g_items[0]);
                if (f0)
                {
                    Graphics g(nb);
                    g.SetInterpolationMode(InterpolationModeBilinear);
                    double bw = f0->GetWidth() * scale;
                    double bh = f0->GetHeight() * scale;
                    g.DrawImage(f0, (REAL)((newW - bw) / 2), 0.0f, (REAL)bw, (REAL)bh);
                }
            }
        }
        delete g_previewCache;
        g_previewCache = NULL;
        g_cacheStrips.erase(g_cacheStrips.begin() + k);
        if (k == 0 && !g_cacheStrips.empty())
            g_cacheStrips[0] = newFirstStrip;
        full = nb;
    }
    else
    {
        delete g_previewCache;
        g_previewCache = NULL;
        full = BuildStitched(960, InterpolationModeBilinear);
        g_cacheStrips.clear();
    }
    if (!full)
        return;
    g_previewCache = full;   // 先接管新指针：后面任何异常都不会让 g_previewCache 悬垂
    PushCacheToView(full);
    g_previewCacheCapH = (int)full->GetHeight();
    g_previewCacheCount = (int)g_items.size();
    {
        int mw = 0;
        for (size_t i = 0; i < g_items.size(); i++)
            mw = std::max(mw, g_items[i]->w);
        g_previewCacheMaxW = mw;
    }
    if (g_cacheStrips.size() != g_items.size())
    {
        double s2 = g_previewCacheMaxW > 960 ? 960.0 / g_previewCacheMaxW : 1.0;
        g_cacheStrips.clear();
        for (size_t i = 0; i < g_items.size(); i++)
            g_cacheStrips.push_back(StripPxH(g_items[i]->w, g_items[i]->h, i == 0, s2));
    }
    g_previewCacheTop = g_topMargin;
    g_previewCacheBottom = g_bottomMargin;
    g_previewCacheGap = g_gap;
}

// ---------- actions ----------

template <typename Fn>
static void Guarded(const char* what, Fn&& fn)
{
    try
    {
        fn();
    }
    catch (...)
    {
        CrashLogNow(what);
    }
}

static void DeleteItemById(int id)
{
    for (size_t i = 0; i < g_items.size(); i++)
    {
        if (g_items[i]->id == id)
        {
            delete g_items[i];
            g_items.erase(g_items.begin() + i);
            PushItems();
            PushPreview((int)i);
            return;
        }
    }
}

static void DoClear()
{
    if (g_items.empty())
        return;
    size_t n = g_items.size();
    for (size_t i = 0; i < n; i++)
    {
        delete g_items[i];
    }
    g_items.clear();
    g_seq = 0;
    PushItems();
    PushPreview();
}

static void DoCapture()
{
    if (!g_hasRegion || g_region.left >= g_region.right || g_region.top >= g_region.bottom)
        return;
    int w = g_region.right - g_region.left;
    int h = g_region.bottom - g_region.top;
    Bitmap* b = CaptureScreenRect(g_region.left, g_region.top, w, h);
    if (!b)
        return;
    std::vector<unsigned char> jpg = EncodeJpeg(b, 100);
    delete b;
    if (jpg.empty())
        return;
    Item* it = new Item();
    it->jpg = std::move(jpg);
    it->w = w;
    it->h = h;
    it->bg = g_items.empty();
    it->seq = ++g_seq;
    it->id = g_nextId++;
    GetLocalTime(&it->time);
    g_items.push_back(it);

    PushItems();
    PushPreview();
    // 截图峰值后立刻把工作集页面交还系统，避免依赖 Windows 2 秒左右的
    // 工作集修剪延迟（任务管理器内存列才能即时回落）。
    GdiFlush();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void DoSave()
{
    if (g_items.empty())
        return;
    wchar_t path[MAX_PATH] = L"";
    wchar_t defName[64];
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(defName, L"subtitle_%04d%02d%02d_%02d%02d%02d.png",
               (int)st.wYear, (int)st.wMonth, (int)st.wDay,
               (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    wcscpy_s(path, defName);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    if (g_win)
        ofn.hwndOwner = (HWND)ui_window_hwnd(g_win);
    ofn.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn))
        return;

    Bitmap* full = BuildStitched(0);
    if (!full)
        return;
    full->Save(ofn.lpstrFile, &g_pngClsid, NULL);
    delete full;
}

// ---------- region overlay window ----------

static RECT SelRect()
{
    RECT r;
    r.left = std::min(g_selStart.x, g_selCur.x);
    r.top = std::min(g_selStart.y, g_selCur.y);
    r.right = std::max(g_selStart.x, g_selCur.x);
    r.bottom = std::max(g_selStart.y, g_selCur.y);
    return r;
}

static LRESULT CALLBACK RegionProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    try
    {
        switch (msg)
        {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
HDC hdc = BeginPaint(hwnd, &ps);
        if (g_dimDC)
            BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   g_dimDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        if (g_dragging)
        {
            RECT r = SelRect();
            int rw = r.right - r.left, rh = r.bottom - r.top;
            if (rw > 0 && rh > 0 && g_snapDC)
            {
                BitBlt(hdc, r.left, r.top, rw, rh, g_snapDC, r.left, r.top, SRCCOPY);
            }
            if (g_selPen)
            {
                HGDIOBJ op = SelectObject(hdc, g_selPen);
                HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                SelectObject(hdc, ob);
                SelectObject(hdc, op);
            }
            wchar_t buf[128];
            swprintf_s(buf, L"X=%d  Y=%d  %dx%d    松开鼠标确认，Esc 取消",
                       g_regOx + r.left, g_regOy + r.top,
                       r.right - r.left, r.bottom - r.top);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            RECT tr = {8, 8, 640, 40};
            DrawTextW(hdc, buf, -1, &tr, 0);
        }
        else
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            RECT tr = {8, 8, 640, 40};
            DrawTextW(hdc, L"拖拽选择区域，松开确认，Esc 取消", -1, &tr, 0);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_CROSS));
        return TRUE;
    case WM_LBUTTONDOWN:
        g_dragging = true;
        g_selStart.x = g_selCur.x = GET_X_LPARAM(lp);
        g_selStart.y = g_selCur.y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging)
        {
            RECT oldR = SelRect();
            g_selCur.x = GET_X_LPARAM(lp);
            g_selCur.y = GET_Y_LPARAM(lp);
            RECT neuR = SelRect();
            RECT u, i;
            u.left = std::min(oldR.left, neuR.left);
            u.top = std::min(oldR.top, neuR.top);
            u.right = std::max(oldR.right, neuR.right);
            u.bottom = std::max(oldR.bottom, neuR.bottom);
            i.left = std::max(oldR.left, neuR.left);
            i.top = std::max(oldR.top, neuR.top);
            i.right = std::min(oldR.right, neuR.right);
            i.bottom = std::min(oldR.bottom, neuR.bottom);
            RECT strips[4];
            int n = 0;
            if (i.right > i.left && i.bottom > i.top)
            {
                if (i.left > u.left)
                    strips[n++] = RECT{u.left, u.top, i.left, u.bottom};
                if (u.right > i.right)
                    strips[n++] = RECT{i.right, u.top, u.right, u.bottom};
                if (i.top > u.top)
                    strips[n++] = RECT{i.left, u.top, i.right, i.top};
                if (u.bottom > i.bottom)
                    strips[n++] = RECT{i.left, i.bottom, i.right, u.bottom};
            }
            else
            {
                strips[n++] = u;
            }
            for (int s = 0; s < n; s++)
            {
                RECT inv = strips[s];
                inv.left = std::max((LONG)0, inv.left - 3);
                inv.top = std::max((LONG)0, inv.top - 3);
                inv.right = std::min((LONG)g_snapW, inv.right + 3);
                inv.bottom = std::min((LONG)g_snapH, inv.bottom + 3);
                InvalidateRect(hwnd, &inv, FALSE);
            }
            RECT tr = {0, 0, 650, 48};
            InvalidateRect(hwnd, &tr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    {
        if (!g_dragging)
            return 0;
        g_dragging = false;
        ReleaseCapture();
        RECT r = SelRect();
        if (r.right - r.left >= 2 && r.bottom - r.top >= 2)
        {
            g_newRegion.left = g_regOx + r.left;
            g_newRegion.top = g_regOy + r.top;
            g_newRegion.right = g_regOx + r.right;
            g_newRegion.bottom = g_regOy + r.bottom;
            g_newRegionValid = true;
        }
        else
        {
            g_newRegionValid = false;
        }
        if (g_newRegionValid)
        {
            g_region = g_newRegion;
            g_hasRegion = true;
            SetRegionInfo(L"视频区域 %dx%d（左 %d, 上 %d）",
                          g_region.right - g_region.left,
                          g_region.bottom - g_region.top,
                          g_region.left, g_region.top);
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        if (g_snap)
        {
            DeleteObject(g_snap);
            g_snap = NULL;
        }
        if (g_mask)
        {
            DeleteObject(g_mask);
            g_mask = NULL;
        }
        if (g_dim)
        {
            DeleteObject(g_dim);
            g_dim = NULL;
        }
        if (g_dimDC)
        {
            DeleteDC(g_dimDC);
            g_dimDC = NULL;
        }
        if (g_snapDC)
        {
            DeleteDC(g_snapDC);
            g_snapDC = NULL;
        }
        if (g_selPen)
        {
            DeleteObject(g_selPen);
            g_selPen = NULL;
        }
        // 恢复被隐藏的 app 窗口
        if (g_regionAppHwnd && IsWindow(g_regionAppHwnd))
        {
            if (!IsWindowVisible(g_regionAppHwnd))
            {
                ShowWindow(g_regionAppHwnd, SW_SHOW);
                SetForegroundWindow(g_regionAppHwnd);
            }
            g_regionAppHwnd = NULL;
        }
        return 0;
    }
    }
    catch (...)
    {
        CrashLogNow("EXCEPTION in RegionProc");
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenRegionSelect()
{
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hm, &mi))
        return;
    RECT rc = mi.rcMonitor;
    g_regOx = rc.left;
    g_regOy = rc.top;
    g_snapW = rc.right - rc.left;
    g_snapH = rc.bottom - rc.top;
    if (g_snapW <= 0 || g_snapH <= 0)
        return;

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    g_snap = CreateCompatibleBitmap(screen, g_snapW, g_snapH);
    HGDIOBJ old = SelectObject(mem, g_snap);
    BitBlt(mem, 0, 0, g_snapW, g_snapH, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_snapW;
    bi.bmiHeader.biHeight = -g_snapH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    g_mask = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (g_mask && bits)
    {
        DWORD* p = (DWORD*)bits;
        DWORD c = 0x5A000000;  // 半透明黑 (alpha=90)，选中区外变暗
        for (int i = 0; i < g_snapW * g_snapH; i++)
            p[i] = c;
    }

    {
        HDC ddc = CreateCompatibleDC(screen);
        g_dim = CreateCompatibleBitmap(screen, g_snapW, g_snapH);
        HGDIOBJ od = SelectObject(ddc, g_dim);
        HDC sdc = CreateCompatibleDC(ddc);
        HGDIOBJ os = SelectObject(sdc, g_snap);
        BitBlt(ddc, 0, 0, g_snapW, g_snapH, sdc, 0, 0, SRCCOPY);
        SelectObject(sdc, os);
        DeleteDC(sdc);
        if (g_mask)
        {
            HDC mskdc = CreateCompatibleDC(ddc);
            HGDIOBJ om = SelectObject(mskdc, g_mask);
            BLENDFUNCTION bf;
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(ddc, 0, 0, g_snapW, g_snapH, mskdc, 0, 0, g_snapW, g_snapH, bf);
            SelectObject(mskdc, om);
            DeleteDC(mskdc);
        }
        SelectObject(ddc, od);
        DeleteDC(ddc);
    }
    ReleaseDC(NULL, screen);

    // 常驻 GDI 对象
    g_dimDC = CreateCompatibleDC(NULL);
    if (g_dimDC)
        SelectObject(g_dimDC, g_dim ? g_dim : g_snap);
    g_snapDC = CreateCompatibleDC(NULL);
    if (g_snapDC)
        SelectObject(g_snapDC, g_snap);
    g_selPen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));

    g_regionAppHwnd = g_win ? (HWND)ui_window_hwnd(g_win) : NULL;
    if (g_regionAppHwnd && IsWindowVisible(g_regionAppHwnd))
        ShowWindow(g_regionAppHwnd, SW_HIDE);

    g_dragging = false;
    HWND w = CreateWindowExW(WS_EX_TOPMOST, L"SubRegionClass", L"", WS_POPUP,
                             rc.left, rc.top, g_snapW, g_snapH, NULL, NULL, g_hInst, NULL);
    if (w)
        ShowWindow(w, SW_SHOW);
}

// ---------- system theme ----------

static bool ReadSystemLightTheme()
{
    bool light = true;
    HKEY hk = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hk) == ERROR_SUCCESS)
    {
        DWORD v = 1, cb = sizeof(v);
        if (RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL,
                             (LPBYTE)&v, &cb) == ERROR_SUCCESS)
            light = (v != 0);
        RegCloseKey(hk);
    }
    return light;
}

static void ApplySystemTheme()
{
    UiThemeMode m = ReadSystemLightTheme() ? UI_THEME_LIGHT : UI_THEME_DARK;
    if (ui_theme_get_mode() != m)
        ui_theme_set_mode(m);
}

static WNDPROC g_origWndProc = NULL;

#define TIMER_MARGIN 1

static uint64_t g_lastMarginTs = 0;
static bool g_marginTimerOn = false;

static LRESULT CALLBACK ThemeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == TIMER_MARGIN)
    {
        KillTimer(hwnd, TIMER_MARGIN);
        g_marginTimerOn = false;
        try
        {
            if (GetTickCount64() - g_lastMarginTs >= 250)
            {
                // 已停止拖动：先保存增量重建现场，再全量精确重建并重编码缩略图
                if (g_dbgIncrDump)
                {
                    g_dbgIncrDump = false;
                    if (g_previewCache)
                    {
                        try { g_previewCache->Save(ToWide("dump_incr.png").c_str(), &g_pngClsid, NULL); }
                        catch (...) {}
                    }
                }
                PushItems(true);
                PushPreview();
            }
            else
            {
                PushPreviewIncremental();
                SetTimer(hwnd, TIMER_MARGIN, 50, NULL);
                g_marginTimerOn = true;
            }
        }
        catch (...)
        {
            CrashLogNow("EXCEPTION in margin throttle timer");
        }
        return 0;
    }
    LRESULT r;
    try
    {
        r = g_origWndProc
                ? g_origWndProc(hwnd, msg, wp, lp)
                : DefWindowProcW(hwnd, msg, wp, lp);
    }
    catch (const std::exception& e)
    {
        char buf[256];
        sprintf_s(buf, "EXCEPTION from core-ui WndProc (msg=0x%X, w=%llu, l=%llu): %s - swallowed",
                  msg, (unsigned long long)wp, (unsigned long long)lp, e.what());
        CrashLogNow(buf);
        r = DefWindowProcW(hwnd, msg, wp, lp);
    }
    catch (...)
    {
        char buf[160];
        sprintf_s(buf, "EXCEPTION from core-ui WndProc (msg=0x%X, w=%llu, l=%llu) [unknown type] - swallowed",
                  msg, (unsigned long long)wp, (unsigned long long)lp);
        CrashLogNow(buf);
        r = DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_SETTINGCHANGE)
        ApplySystemTheme();
    return r;
}

// ---------- hotkeys ----------

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_HOTKEY)
    {
        try
        {
            if (wp == HOTKEY_REGION)
                OpenRegionSelect();
            else if (wp == HOTKEY_CAPTURE)
                DoCapture();
        }
        catch (...)
        {
            CrashLogNow("EXCEPTION in MsgWndProc WM_HOTKEY");
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- direct widget callbacks (no polling) ----------

static void ScheduleMarginRebuild()
{
    HWND hw = (HWND)ui_window_hwnd(g_win);
    if (!hw)
        return;
    g_lastMarginTs = GetTickCount64();
    if (!g_marginTimerOn)
    {
        SetTimer(hw, TIMER_MARGIN, 50, NULL);
        g_marginTimerOn = true;
    }
}

static void OnSliderTop(UiWidget, float v, void*)   { g_topMargin = (int)v;  PushMarginState(); ScheduleMarginRebuild(); }
static void OnSliderBottom(UiWidget, float v, void*) { g_bottomMargin = (int)v; PushMarginState(); ScheduleMarginRebuild(); }
static void OnSliderGap(UiWidget, float v, void*)    { g_gap = (int)v;       PushMarginState(); ScheduleMarginRebuild(); }

static void OnBtnRegion(UiWidget, void*)  { Guarded("EXCEPTION in OnBtnRegion", OpenRegionSelect); }
static void OnBtnCapture(UiWidget, void*) { Guarded("EXCEPTION in OnBtnCapture", DoCapture); }
static void OnBtnSave(UiWidget, void*)    { Guarded("EXCEPTION in OnBtnSave", DoSave); }
static void OnBtnClear(UiWidget, void*)   { Guarded("EXCEPTION in OnBtnClear", DoClear); }

static void OnStaticMount(UiPage, UiWidget w, void* ud)
{
    switch ((intptr_t)ud)
    {
    case 0: ui_widget_on_click(w, OnBtnRegion, NULL); break;
    case 1: ui_widget_on_click(w, OnBtnCapture, NULL); break;
    case 2: ui_widget_on_click(w, OnBtnSave, NULL); break;
    case 3: ui_widget_on_click(w, OnBtnClear, NULL); break;
    case 4: ui_slider_on_changed(w, OnSliderTop, NULL); break;
    case 5: ui_slider_on_changed(w, OnSliderBottom, NULL); break;
    case 6: ui_slider_on_changed(w, OnSliderGap, NULL); break;
    }
}

static void OnDelClicked(UiWidget, void* ud)
{
    int id = (int)(intptr_t)ud;
    Guarded("EXCEPTION in OnDelClicked", [id]() { DeleteItemById(id); });
}

static void OnDelMount(UiPage, UiWidget w, void* ud)
{
    ui_widget_on_click(w, OnDelClicked, ud);
}

static void RegisterWidgetCallbacks()
{
    ui_page_on_widget_mount(g_page, "btnRegion", OnStaticMount, (void*)0);
    ui_page_on_widget_mount(g_page, "btnCapture", OnStaticMount, (void*)1);
    ui_page_on_widget_mount(g_page, "btnSave", OnStaticMount, (void*)2);
    ui_page_on_widget_mount(g_page, "btnClear", OnStaticMount, (void*)3);
    ui_page_on_widget_mount(g_page, "slTop", OnStaticMount, (void*)4);
    ui_page_on_widget_mount(g_page, "slBottom", OnStaticMount, (void*)5);
    ui_page_on_widget_mount(g_page, "slGap", OnStaticMount, (void*)6);
    for (size_t i = 0; i < g_items.size(); i++)
    {
        char idBuf[32];
        sprintf_s(idBuf, "del_%d", g_items[i]->id);
        ui_page_on_widget_mount(g_page, idBuf, OnDelMount,
                                (void*)(intptr_t)g_items[i]->id);
    }
}

static void ApplyPinVisual()
{
    if (!g_pinBtn)
        return;
    if (g_pinned)
        ui_icon_button_set_icon_color_role(g_pinBtn, UI_ICON_COLOR_ACCENT);
    else
        ui_icon_button_set_icon_color_role(g_pinBtn, UI_ICON_COLOR_BUTTON_TEXT);
}

static void TogglePin();

static void OnPinClicked(UiWidget, void*)
{
    TogglePin();
}

static void TogglePin()
{
    g_pinned = !g_pinned;
    HWND hw = (HWND)ui_window_hwnd(g_win);
    SetWindowPos(hw, g_pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ApplyPinVisual();
}

// ---------- entry ----------

static int DbgThumbs(char* out, int outSize)
{
    std::string j = "{";
    j += "\"items\":[";
    for (size_t i = 0; i < g_items.size(); i++)
    {
        if (i) j += ",";
        j += "{\"id\":" + std::to_string(g_items[i]->id) +
             ",\"thumb\":\"" + g_items[i]->thumbName + "\"}";
    }
    j += "],\"blobs\":[";
    {
        std::lock_guard<std::mutex> lk(g_blobsMutex);
        size_t i = 0;
        for (auto& kv : g_blobs)
        {
            if (i++) j += ",";
            j += "{\"name\":\"" + kv.first + "\",\"size\":" +
                 std::to_string(kv.second.size()) + "}";
        }
    }
    j += "]}";
    int n = snprintf(out, outSize, "%s", j.c_str());
    return (n > 0 && n < outSize) ? n : (outSize > 1 ? outSize - 1 : 1);
}

static int DbgCmd(const char* cmd, const char* args,
                  char* out, int outSize, void* userdata)
{
    try
    {
        if (strcmp(cmd, "thumbs") == 0)
            return DbgThumbs(out, outSize);
        if (strcmp(cmd, "dump") == 0)
        {
            Bitmap* src = g_previewCache;
            if (src)
            {
                char fn[128];
                snprintf(fn, sizeof(fn), "dump_%s.png", args && *args ? args : "x");
                src->Save(ToWide(fn).c_str(), &g_pngClsid, NULL);
            }
            int len = snprintf(out, outSize, "{\"ok\":true,\"saved\":%s}",
                               src ? "true" : "false");
            return (len > 0 && len < outSize) ? len : (outSize > 1 ? outSize - 1 : 1);
        }
        if (strcmp(cmd, "incrdump") == 0)
        {
            g_dbgIncrDump = true;
            int len = snprintf(out, outSize, "{\"ok\":true}");
            return (len > 0 && len < outSize) ? len : (outSize > 1 ? outSize - 1 : 1);
        }
        if (strcmp(cmd, "margin") == 0)
        {
            int t = 0, b = 0, g = 0;
            if (sscanf_s(args ? args : "", "%d %d %d", &t, &b, &g) >= 1)
            {
                g_topMargin = t;
                g_bottomMargin = b;
                g_gap = g;
                ScheduleMarginRebuild();
                PushMarginState();
            }
            int len = snprintf(out, outSize, "{\"ok\":true,\"t\":%d,\"b\":%d,\"g\":%d}",
                               g_topMargin, g_bottomMargin, g_gap);
            return (len > 0 && len < outSize) ? len : (outSize > 1 ? outSize - 1 : 1);
        }
        if (strcmp(cmd, "pstate") == 0)
        {
            std::string j = "{\"items\":" + std::to_string(g_items.size()) +
                           ",\"w\":" + std::to_string(g_previewCache ? (int)g_previewCache->GetWidth() : 0) +
                           ",\"h\":" + std::to_string(g_previewCache ? (int)g_previewCache->GetHeight() : 0) +
                           ",\"cap\":" + std::to_string(g_previewCacheCapH) +
                           ",\"margins\":[" + std::to_string(g_topMargin) + "," +
                           std::to_string(g_bottomMargin) + "," + std::to_string(g_gap) + "]" +
                           ",\"cacheMargins\":[" + std::to_string(g_previewCacheTop) + "," +
                           std::to_string(g_previewCacheBottom) + "," + std::to_string(g_previewCacheGap) + "]" +
                           ",\"strips\":[";
            for (size_t i = 0; i < g_cacheStrips.size(); i++)
            {
                if (i) j += ",";
                j += std::to_string(g_cacheStrips[i]);
            }
            j += "],\"newH\":";
            if (g_items.empty())
                j += "0";
            else
            {
                double s2 = g_previewCacheMaxW > 960 ? 960.0 / g_previewCacheMaxW : 1.0;
                int nh = 0;
                for (size_t i = 0; i < g_items.size(); i++)
                    nh += StripPxH(g_items[i]->w, g_items[i]->h, i == 0, s2);
                j += std::to_string(nh);
            }
            j += "}";
            int len = snprintf(out, outSize, "%s", j.c_str());
            return (len > 0 && len < outSize) ? len : (outSize > 1 ? outSize - 1 : 1);
        }
        if (strcmp(cmd, "theme") == 0)
        {
            if (args && (args[0] == 'l' || args[0] == 'L'))
                ui_theme_set_mode(UI_THEME_LIGHT);
            else if (args && (args[0] == 'd' || args[0] == 'D'))
                ui_theme_set_mode(UI_THEME_DARK);
            else
            {
                ApplySystemTheme();
            }
            int len = snprintf(out, outSize, "{\"ok\":true,\"mode\":%d}",
                               (int)ui_theme_get_mode());
            return (len > 0 && len < outSize) ? len : (outSize > 1 ? outSize - 1 : 1);
        }
        return 0;
    }
    catch (...)
    {
        CrashLogNow("EXCEPTION in DbgCmd");
        int len = snprintf(out, outSize, "{\"ok\":false}");
        return (len > 0 && len < outSize) ? len : (outSize > 1 ? outSize - 1 : 1);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    g_hInst = hInstance;
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    AddVectoredExceptionHandler(1, CrashVe);
    std::set_terminate(TermHandler);
    _set_invalid_parameter_handler(InvalidParamHandler);

    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiplusToken, &gsi, NULL) != Ok)
        ExitProcess(1);
    {
        UINT num = 0, size = 0;
        GetImageEncodersSize(&num, &size);
        if (size > 0)
        {
            ImageCodecInfo* info = (ImageCodecInfo*)malloc(size);
            if (info)
            {
                if (GetImageEncoders(num, size, info) == Ok)
                {
                    for (UINT i = 0; i < num; i++)
                    {
                        if (size < (i + 1) * sizeof(ImageCodecInfo))
                            break;
                        if (g_pngClsid.Data1 == 0 &&
                            wcscmp(info[i].MimeType, L"image/png") == 0)
                        {
                            g_pngClsid = info[i].Clsid;
                        }
                        else if (g_jpegClsid.Data1 == 0 &&
                                 wcscmp(info[i].MimeType, L"image/jpeg") == 0)
                        {
                            g_jpegClsid = info[i].Clsid;
                        }
                    }
                }
                free(info);
            }
        }
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RegionProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"SubRegionClass";
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"SubMsgClass";
    RegisterClassExW(&wc);

    HWND msgwnd = CreateWindowExW(0, L"SubMsgClass", L"", 0,
                                  0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    RegisterHotKey(msgwnd, HOTKEY_REGION, 0, VK_F8);
    RegisterHotKey(msgwnd, HOTKEY_CAPTURE, 0, VK_F9);

    ui_init_with_theme(ReadSystemLightTheme() ? UI_THEME_LIGHT : UI_THEME_DARK);
    ui_asset_register_resolver(ThumbResolver, NULL);

    UiPage page = 0;
    HRSRC hr = FindResourceW(NULL, L"APP_UIX", RT_RCDATA);
    std::string uixSrc;
    if (hr) {
        HGLOBAL hg = LoadResource(NULL, hr);
        if (hg) {
            const char* data = (const char*)LockResource(hg);
            uixSrc.assign(data, SizeofResource(NULL, hr));
        }
    }
    page = uixSrc.empty() ? 0 : ui_page_load_string(uixSrc.c_str());
    if (!page)
    {
        ui_shutdown();
        GdiplusShutdown(g_gdiplusToken);
        ExitProcess(2);
    }
    g_page = page;

    UiWindow win = ui_page_open_window(page, NULL);
    if (!win)
    {
        ui_page_destroy(page);
        ui_shutdown();
        GdiplusShutdown(g_gdiplusToken);
        ExitProcess(3);
    }
    g_win = win;

    UiWidget root = ui_page_root(page);
    g_gh = root ? ui_widget_find_by_id(root, "preview") : 0;

    {
        UiWidget tb = root ? ui_widget_find_by_id(root, "titlebar") : 0;
        if (tb)
        {
            g_pinBtn = ui_icon_button(kPinSvg, 1);
            ui_widget_set_width(g_pinBtn, 36);
            ui_widget_set_height(g_pinBtn, 28);
            ui_icon_button_set_icon_padding(g_pinBtn, 4.0f);
            ui_titlebar_add_widget(tb, g_pinBtn);
            ui_widget_on_click(g_pinBtn, OnPinClicked, NULL);
        }
    }

    HWND hw = (HWND)ui_window_hwnd(win);
    g_origWndProc =
        (WNDPROC)SetWindowLongPtrW(hw, GWLP_WNDPROC, (LONG_PTR)&ThemeWndProc);

    RegisterWidgetCallbacks();
    PushItems();
    PushMarginState();
    ui_debug_server_start(win, "ui_core_debug");
    ui_debug_server_set_handler(DbgCmd, NULL);

    int code = ui_run();

    KillTimer(hw, TIMER_MARGIN);
    UnregisterHotKey(msgwnd, HOTKEY_REGION);
    UnregisterHotKey(msgwnd, HOTKEY_CAPTURE);
    Guarded("EXCEPTION in teardown ui_debug_server_stop", ui_debug_server_stop);
    Guarded("EXCEPTION in teardown ui_page_destroy",
            [&]() { ui_page_destroy(page); });
    Guarded("EXCEPTION in teardown ui_shutdown", ui_shutdown);
    Guarded("EXCEPTION in teardown item cleanup",
            [&]() {
                for (size_t i = 0; i < g_items.size(); i++)
                {
                    delete g_items[i];
                }
                g_items.clear();
                FreePreviewCache();
            });
    Guarded("EXCEPTION in teardown GdiplusShutdown",
            [&]() { GdiplusShutdown(g_gdiplusToken); });
    ExitProcess(code);
    return code;
}
