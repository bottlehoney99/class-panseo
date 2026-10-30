// 화면 판서 - 수업용 화면 위 그리기 도구 (Windows, 단일 exe)
// 빌드: x86_64-w64-mingw32-g++ (build.sh 참고)

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define NOMINMAX
#define _USE_MATH_DEFINES
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
using std::min;
using std::max;
#include <gdiplus.h>
using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 설정값
// ---------------------------------------------------------------------------
enum Tool { T_PEN, T_HL, T_ERASER, T_LINE, T_ARROW, T_RECT, T_ELLIPSE, T_TEXT };

static const uint32_t PALETTE[6] = {
    0xFFE53935, // 빨강
    0xFF1E5FD8, // 파랑
    0xFF2E9E48, // 초록
    0xFFFFC51F, // 노랑
    0xFF1B1B1B, // 검정
    0xFFFFFFFF  // 흰색
};
static const wchar_t* PALETTE_TIP[6] = {
    L"빨강 (1)", L"파랑 (2)", L"초록 (3)", L"노랑 (4)", L"검정 (5)", L"흰색 (6)"
};
static const float PEN_W[3] = {3.f, 6.f, 11.f};
static const float HL_W[3]  = {14.f, 24.f, 38.f};
static const float ER_R[3]  = {10.f, 20.f, 34.f};
static const float TXT_PX[3] = {26.f, 38.f, 56.f}; // 글자 크기(96dpi 기준)
static const BYTE  HL_ALPHA = 105;

// ---------------------------------------------------------------------------
// 도형 데이터와 실행 취소 기록
// ---------------------------------------------------------------------------
struct Shape {
    Tool type;
    uint32_t argb;
    float width;
    std::vector<PointF> pts; // 화면(가상 스크린) 좌표. 글상자는 {왼쪽 위, 오른쪽 아래}
    bool dashed = false;     // 글상자 영역 미리보기용
    // 글상자
    std::vector<std::wstring> lines; // 화면에 보이는 줄 그대로
    std::wstring raw;                // 다시 고칠 때 쓰는 원문
    float lineH = 0, boxW = 0;
};
typedef std::shared_ptr<Shape> SP;

static std::vector<SP> g_shapes;
static std::vector<std::vector<SP>> g_undo, g_redo;

// ---------------------------------------------------------------------------
// 전역 상태
// ---------------------------------------------------------------------------
static HINSTANCE g_inst;
static HWND g_ov, g_tb, g_tip;
static int vx, vy, vw, vh;                 // 가상 스크린 영역
static HDC g_memDC;
static HBITMAP g_dib, g_oldBmp;
static uint32_t* g_front;                  // 창에 올라가는 버퍼 (DIB)
static std::vector<uint32_t> g_base;       // 확정된 도형만 그린 버퍼

static bool g_drawMode = false;
static Tool g_tool = T_PEN;
static int g_color = 0;
static int g_wIdx = 1;

static SP g_cur;                           // 그리는 중인 도형
static bool g_erasing = false, g_eraseSnap = false, g_tempErase = false;
static PointF g_lastPt;
static RECT g_prevDirty;
static bool g_hasPrevDirty = false;
static HWND g_prevFg;
static HCURSOR g_cursor, g_eraseCursor;
static bool g_inModal = false;

// 도구 막대
enum {
    B_TOGGLE = 1, B_PEN, B_HL, B_LINE, B_ARROW, B_RECT, B_ELLIPSE, B_ERASER, B_TEXT,
    B_C0, B_C1, B_C2, B_C3, B_C4, B_C5, B_W0, B_W1, B_W2,
    B_UNDO, B_REDO, B_CLEAR, B_COLLAPSE, B_EXIT
};
struct Btn { int id; RECT r; };
static std::vector<Btn> g_btns;
static std::vector<int> g_seps;
static std::vector<int> g_tipIds;
static float S = 1.f;
static int g_tbW, g_tbH, g_gripH;
static bool g_collapsed = false;
static int g_hover = -1;
static bool g_tracking = false;

static const wchar_t* OV_CLASS = L"PanseoOverlay";
static const wchar_t* TB_CLASS = L"PanseoToolbar";
static const wchar_t* APP_TITLE = L"화면 판서";
static const wchar_t* ED_CLASS = L"PanseoTextHost";

// 글상자 편집 상태
static HWND g_edHost, g_edit;
static HFONT g_edFont;
static HBRUSH g_edBrush;
static float g_edX, g_edY, g_edW, g_edLineH;
static bool g_edUndoPushed = false;
static bool g_edClosing = false;
static bool g_textDrag = false;
static WPARAM g_edGen = 0;
static COLORREF g_edBg;
static const int ED_PAD = 6;
#define WM_APP_COMMITTEXT (WM_APP + 1)

static const FontFamily* UiFamily() {
    static FontFamily* ff = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        ff = new FontFamily(L"Malgun Gothic");
        if (ff->GetLastStatus() != Ok) { delete ff; ff = nullptr; }
    }
    return ff ? ff : FontFamily::GenericSansSerif();
}
static float TextPx() { return roundf(TXT_PX[g_wIdx] * S); }

// ---------------------------------------------------------------------------
// 기하 계산
// ---------------------------------------------------------------------------
static float distPtSeg(PointF p, PointF a, PointF b) {
    float dx = b.X - a.X, dy = b.Y - a.Y;
    float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.X - a.X) * dx + (p.Y - a.Y) * dy) / l2 : 0.f;
    t = max(0.f, min(1.f, t));
    float x = a.X + t * dx - p.X, y = a.Y + t * dy - p.Y;
    return sqrtf(x * x + y * y);
}
static float cross3(PointF o, PointF a, PointF b) {
    return (a.X - o.X) * (b.Y - o.Y) - (a.Y - o.Y) * (b.X - o.X);
}
static float distSegSeg(PointF a, PointF b, PointF c, PointF d) {
    float d1 = cross3(c, d, a), d2 = cross3(c, d, b);
    float d3 = cross3(a, b, c), d4 = cross3(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return 0.f;
    return min(min(distPtSeg(a, c, d), distPtSeg(b, c, d)),
               min(distPtSeg(c, a, b), distPtSeg(d, a, b)));
}

static void ArrowGeom(const Shape& s, PointF& shaftEnd, PointF tri[3]) {
    PointF a = s.pts[0], b = s.pts[1];
    float dx = b.X - a.X, dy = b.Y - a.Y;
    float L = sqrtf(dx * dx + dy * dy);
    if (L < 0.01f) { shaftEnd = b; tri[0] = tri[1] = tri[2] = b; return; }
    float ux = dx / L, uy = dy / L;
    float h = max(s.width * 3.2f, 14.f);
    if (h > L * 0.7f) h = L * 0.7f;
    float half = h * 0.55f;
    PointF base(b.X - ux * h, b.Y - uy * h);
    tri[0] = b;
    tri[1] = PointF(base.X - uy * half, base.Y + ux * half);
    tri[2] = PointF(base.X + uy * half, base.Y - ux * half);
    shaftEnd = PointF(b.X - ux * h * 0.6f, b.Y - uy * h * 0.6f);
}

static void ShapeSegments(const Shape& s, std::vector<std::pair<PointF, PointF>>& out) {
    out.clear();
    const auto& p = s.pts;
    switch (s.type) {
    case T_PEN: case T_HL:
        if (p.size() == 1) out.push_back({p[0], p[0]});
        for (size_t i = 1; i < p.size(); i++) out.push_back({p[i - 1], p[i]});
        break;
    case T_LINE: case T_ARROW:
        out.push_back({p[0], p[1]});
        break;
    case T_RECT: case T_TEXT: {
        PointF a(p[0].X, p[0].Y), b(p[1].X, p[0].Y), c(p[1].X, p[1].Y), d(p[0].X, p[1].Y);
        out.push_back({a, b}); out.push_back({b, c}); out.push_back({c, d}); out.push_back({d, a});
        break;
    }
    case T_ELLIPSE: {
        float cx = (p[0].X + p[1].X) / 2, cy = (p[0].Y + p[1].Y) / 2;
        float rx = fabsf(p[1].X - p[0].X) / 2, ry = fabsf(p[1].Y - p[0].Y) / 2;
        const int N = 48;
        PointF prev(cx + rx, cy);
        for (int i = 1; i <= N; i++) {
            float t = (float)(i * 2 * M_PI / N);
            PointF q(cx + rx * cosf(t), cy + ry * sinf(t));
            out.push_back({prev, q});
            prev = q;
        }
        break;
    }
    default: break;
    }
}

static RectF ShapeBounds(const Shape& s) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto& q : s.pts) {
        x0 = min(x0, q.X); y0 = min(y0, q.Y); x1 = max(x1, q.X); y1 = max(y1, q.Y);
    }
    float pad = (s.type == T_TEXT) ? 3.f : s.width / 2 + 3;
    if (s.type == T_ARROW) pad += max(s.width * 3.2f, 14.f);
    return RectF(x0 - pad, y0 - pad, (x1 - x0) + 2 * pad, (y1 - y0) + 2 * pad);
}

static bool ShapeHit(const Shape& s, PointF a, PointF b, float r) {
    RectF sb = ShapeBounds(s);
    float ex0 = min(a.X, b.X) - r, ey0 = min(a.Y, b.Y) - r;
    float ex1 = max(a.X, b.X) + r, ey1 = max(a.Y, b.Y) + r;
    if (sb.X > ex1 || sb.Y > ey1 || sb.X + sb.Width < ex0 || sb.Y + sb.Height < ey0) return false;
    if (s.type == T_TEXT) return true; // 상자 범위에 닿으면 지움
    static std::vector<std::pair<PointF, PointF>> segs;
    ShapeSegments(s, segs);
    float th = r + s.width / 2;
    for (auto& sg : segs)
        if (distSegSeg(a, b, sg.first, sg.second) <= th) return true;
    return false;
}

static PointF SnapEnd(Tool t, PointF a, PointF p, bool shift) {
    if (!shift) return p;
    float dx = p.X - a.X, dy = p.Y - a.Y;
    if (t == T_RECT || t == T_ELLIPSE) {
        float s = max(fabsf(dx), fabsf(dy));
        return PointF(a.X + (dx < 0 ? -s : s), a.Y + (dy < 0 ? -s : s));
    }
    float ang = atan2f(dy, dx);
    float step = (float)(M_PI / 4);
    ang = roundf(ang / step) * step;
    float L = sqrtf(dx * dx + dy * dy);
    return PointF(a.X + cosf(ang) * L, a.Y + sinf(ang) * L);
}

// ---------------------------------------------------------------------------
// 그리기
// ---------------------------------------------------------------------------
static void DrawShape(Graphics& g, const Shape& s) {
    BYTE alpha = (s.type == T_HL) ? HL_ALPHA : 255;
    Color c(alpha, (BYTE)(s.argb >> 16), (BYTE)(s.argb >> 8), (BYTE)s.argb);
    if (s.type == T_TEXT) {
        Font f(UiFamily(), s.width, FontStyleBold, UnitPixel);
        SolidBrush br(c);
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
        for (size_t i = 0; i < s.lines.size(); i++) {
            if (s.lines[i].empty()) continue;
            g.DrawString(s.lines[i].c_str(), (INT)s.lines[i].size(), &f,
                         PointF(s.pts[0].X, s.pts[0].Y + i * s.lineH), &sf, &br);
        }
        return;
    }
    Pen pen(c, s.width);
    if (s.dashed) pen.SetDashStyle(DashStyleDash);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    const auto& p = s.pts;
    switch (s.type) {
    case T_PEN: case T_HL:
        if (p.size() == 1) {
            SolidBrush br(c);
            g.FillEllipse(&br, p[0].X - s.width / 2, p[0].Y - s.width / 2, s.width, s.width);
        } else {
            g.DrawLines(&pen, p.data(), (INT)p.size());
        }
        break;
    case T_LINE:
        g.DrawLine(&pen, p[0], p[1]);
        break;
    case T_ARROW: {
        PointF se, tri[3];
        ArrowGeom(s, se, tri);
        pen.SetEndCap(LineCapFlat);
        g.DrawLine(&pen, p[0], se);
        SolidBrush br(c);
        g.FillPolygon(&br, tri, 3);
        break;
    }
    case T_RECT: {
        float x = min(p[0].X, p[1].X), y = min(p[0].Y, p[1].Y);
        g.DrawRectangle(&pen, x, y, fabsf(p[1].X - p[0].X), fabsf(p[1].Y - p[0].Y));
        break;
    }
    case T_ELLIPSE: {
        float x = min(p[0].X, p[1].X), y = min(p[0].Y, p[1].Y);
        g.DrawEllipse(&pen, x, y, fabsf(p[1].X - p[0].X), fabsf(p[1].Y - p[0].Y));
        break;
    }
    default: break;
    }
}

static void PrepGraphics(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.TranslateTransform((REAL)-vx, (REAL)-vy);
}

// 판서 모드에서는 배경을 알파 1로 채워서 클릭을 받는다 (눈에는 안 보임)
static uint32_t BgValue() { return g_drawMode ? 0x01000000u : 0u; }

static void RenderBase() {
    std::fill(g_base.begin(), g_base.end(), BgValue());
    Bitmap bmp(vw, vh, vw * 4, PixelFormat32bppPARGB, (BYTE*)g_base.data());
    Graphics g(&bmp);
    PrepGraphics(g);
    for (auto& s : g_shapes) DrawShape(g, *s);
}

static void Present(const RECT* dirty) {
    POINT ptSrc = {0, 0};
    SIZE sz = {vw, vh};
    POINT ptDst = {vx, vy};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info = {};
    info.cbSize = sizeof(info);
    info.pptDst = &ptDst;
    info.psize = &sz;
    info.hdcSrc = g_memDC;
    info.pptSrc = &ptSrc;
    info.pblend = &bf;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = dirty;
    if (!UpdateLayeredWindowIndirect(g_ov, &info)) {
        info.prcDirty = NULL;
        UpdateLayeredWindowIndirect(g_ov, &info);
    }
}

static void PresentFull() {
    memcpy(g_front, g_base.data(), (size_t)vw * vh * 4);
    Present(NULL);
}

static bool ToClientRect(const RectF& rf, RECT& r) {
    r.left = max(0, (int)floorf(rf.X) - vx);
    r.top = max(0, (int)floorf(rf.Y) - vy);
    r.right = min(vw, (int)ceilf(rf.X + rf.Width) - vx + 1);
    r.bottom = min(vh, (int)ceilf(rf.Y + rf.Height) - vy + 1);
    return r.right > r.left && r.bottom > r.top;
}

// 진행 중인 도형을 dirty 영역만 다시 그려서 올린다
static void PresentCurrent(const RectF& changed) {
    RECT now;
    if (!ToClientRect(changed, now)) return;
    RECT d = now;
    if (g_hasPrevDirty) UnionRect(&d, &now, &g_prevDirty);
    for (int y = d.top; y < d.bottom; y++)
        memcpy(g_front + (size_t)y * vw + d.left, g_base.data() + (size_t)y * vw + d.left,
               (size_t)(d.right - d.left) * 4);
    if (g_cur) {
        Bitmap bmp(vw, vh, vw * 4, PixelFormat32bppPARGB, (BYTE*)g_front);
        Graphics g(&bmp);
        g.SetClip(Rect(d.left, d.top, d.right - d.left, d.bottom - d.top));
        PrepGraphics(g);
        DrawShape(g, *g_cur);
    }
    Present(&d);
    g_prevDirty = now;
    g_hasPrevDirty = true;
}

static void SetupSurface() {
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    vh = max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (g_dib) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_dib);
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = vw;
    bi.bmiHeader.biHeight = -vh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    g_dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    g_front = (uint32_t*)bits;
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);
    g_base.assign((size_t)vw * vh, 0);
    SetWindowPos(g_ov, NULL, vx, vy, vw, vh, SWP_NOZORDER | SWP_NOACTIVATE);
    RenderBase();
    PresentFull();
}

// ---------------------------------------------------------------------------
// 커서
// ---------------------------------------------------------------------------
enum CurKind { CK_DOT, CK_RING, CK_CROSS, CK_IBEAM };

static HCURSOR MakeCursor(CurKind kind, uint32_t argb, float diam) {
    int need = (int)ceilf(diam) + 6;
    int size = need <= 32 ? 32 : (need <= 48 ? 48 : 64);
    diam = min(diam, (float)size - 6);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    memset(bits, 0, size * size * 4);
    {
        Bitmap bmp(size, size, size * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        float c = size / 2.f;
        int r8 = (argb >> 16) & 255, g8 = (argb >> 8) & 255, b8 = argb & 255;
        bool light = (r8 * 299 + g8 * 587 + b8 * 114) / 1000 > 150;
        Color edge = light ? Color(255, 40, 40, 40) : Color(255, 255, 255, 255);
        if (kind == CK_DOT) {
            float d = max(diam, 6.f);
            SolidBrush br(Color(255, r8, g8, b8));
            g.FillEllipse(&br, c - d / 2, c - d / 2, d, d);
            Pen pe(edge, 1.2f);
            g.DrawEllipse(&pe, c - d / 2 - 0.6f, c - d / 2 - 0.6f, d + 1.2f, d + 1.2f);
        } else if (kind == CK_RING) {
            float d = max(diam, 8.f);
            Pen outer(Color(255, 255, 255, 255), 3.f);
            Pen inner(Color(255, 30, 30, 30), 1.4f);
            g.DrawEllipse(&outer, c - d / 2, c - d / 2, d, d);
            g.DrawEllipse(&inner, c - d / 2, c - d / 2, d, d);
            SolidBrush dot(Color(255, 30, 30, 30));
            g.FillEllipse(&dot, c - 1.5f, c - 1.5f, 3.f, 3.f);
        } else if (kind == CK_IBEAM) {
            Pen outer(Color(255, 255, 255, 255), 3.4f);
            Pen inner(Color(255, r8, g8, b8), 1.6f);
            float L = 10.f, sw = 4.f;
            for (Pen* pp : {&outer, &inner}) {
                g.DrawLine(pp, c, c - L, c, c + L);
                g.DrawLine(pp, c - sw, c - L, c + sw, c - L);
                g.DrawLine(pp, c - sw, c + L, c + sw, c + L);
            }
        } else {
            Pen outer(Color(255, 255, 255, 255), 3.f);
            Pen inner(Color(255, 30, 30, 30), 1.f);
            float L = 9.f;
            g.DrawLine(&outer, c - L, c, c + L, c);
            g.DrawLine(&outer, c, c - L, c, c + L);
            g.DrawLine(&inner, c - L, c, c + L, c);
            g.DrawLine(&inner, c, c - L, c, c + L);
            SolidBrush br(Color(255, r8, g8, b8));
            g.FillEllipse(&br, c - 3.f, c - 3.f, 6.f, 6.f);
        }
    }
    std::vector<BYTE> maskBits((size_t)size * size / 8, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = size / 2;
    ii.yHotspot = size / 2;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR cur = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return cur;
}

static void UpdateCursor() {
    HCURSOR old = g_cursor, oldE = g_eraseCursor;
    uint32_t col = PALETTE[g_color];
    switch (g_tool) {
    case T_PEN: g_cursor = MakeCursor(CK_DOT, col, PEN_W[g_wIdx]); break;
    case T_HL: g_cursor = MakeCursor(CK_DOT, col, HL_W[g_wIdx]); break;
    case T_ERASER: g_cursor = MakeCursor(CK_RING, 0, ER_R[g_wIdx] * 2); break;
    case T_TEXT: g_cursor = MakeCursor(CK_IBEAM, (col & 0xFFFFFF) == 0xFFFFFF ? 0xFF1B1B1B : col, 0); break;
    default: g_cursor = MakeCursor(CK_CROSS, col, 0); break;
    }
    g_eraseCursor = MakeCursor(CK_RING, 0, ER_R[g_wIdx] * 2);
    if (g_drawMode) {
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == g_ov) SetCursor(g_tempErase ? g_eraseCursor : g_cursor);
    }
    if (old) DestroyCursor(old);
    if (oldE) DestroyCursor(oldE);
}

// ---------------------------------------------------------------------------
// 편집 동작
// ---------------------------------------------------------------------------
static void TbRefresh() { if (g_tb) InvalidateRect(g_tb, NULL, FALSE); }

static void RaiseToolbar() {
    if (g_tb)
        SetWindowPos(g_tb, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void PushUndo() {
    g_undo.push_back(g_shapes);
    if (g_undo.size() > 300) g_undo.erase(g_undo.begin());
    g_redo.clear();
}

// ---------------------------------------------------------------------------
// 글상자 편집기 (Windows EDIT 컨트롤을 띄워서 한글 IME 입력을 그대로 받는다)
// ---------------------------------------------------------------------------
static COLORREF ToRGB(uint32_t a) { return RGB((a >> 16) & 255, (a >> 8) & 255, a & 255); }

static void EditorResize() {
    if (!g_edit) return;
    int lines = max(1, (int)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0));
    int h = (int)ceilf((lines + 1) * g_edLineH);
    int w = (int)ceilf(g_edW);
    SetWindowPos(g_edHost, NULL, 0, 0, w + 2 * ED_PAD, h + 2 * ED_PAD, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_edit, NULL, ED_PAD, ED_PAD, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(g_edit, EM_LINESCROLL, 0, -100000);
    InvalidateRect(g_edHost, NULL, TRUE);
}

static void UpdateEditorColors() {
    if (!g_edHost) return;
    uint32_t a = PALETTE[g_color];
    int r8 = (a >> 16) & 255, g8 = (a >> 8) & 255, b8 = a & 255;
    bool light = (r8 * 299 + g8 * 587 + b8 * 114) / 1000 > 150;
    g_edBg = light ? RGB(0x2B, 0x2D, 0x31) : RGB(0xFF, 0xFF, 0xFF);
    if (g_edBrush) DeleteObject(g_edBrush);
    g_edBrush = CreateSolidBrush(g_edBg);
    InvalidateRect(g_edHost, NULL, TRUE);
    InvalidateRect(g_edit, NULL, TRUE);
}

static void ApplyEditorFont() {
    if (!g_edit) return;
    HFONT f = CreateFontW(-(int)TextPx(), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, 0, L"Malgun Gothic");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)f, TRUE);
    if (g_edFont) DeleteObject(g_edFont);
    g_edFont = f;
    HDC dc = GetDC(g_edit);
    HFONT o = (HFONT)SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(g_edit, dc);
    g_edLineH = (float)tm.tmHeight;
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    EditorResize();
}

static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN &&
        (wp == VK_ESCAPE || (wp == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)))) {
        PostMessageW(g_ov, WM_APP_COMMITTEXT, g_edGen, 0);
        return 0;
    }
    if (m == WM_CHAR && (wp == 27 || wp == 10)) return 0;
    if (m == WM_KILLFOCUS) PostMessageW(g_ov, WM_APP_COMMITTEXT, g_edGen, 0);
    return DefSubclassProc(h, m, wp, lp);
}

static LRESULT CALLBACK EdHostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, ToRGB(PALETTE[g_color]));
        SetBkColor((HDC)wp, g_edBg);
        return (LRESULT)g_edBrush;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) EditorResize();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_edBrush);
        Graphics g(dc);
        Pen pen(Color(255, 0xF2, 0xC1, 0x4E), 2.f);
        pen.SetDashStyle(DashStyleDash);
        g.DrawRectangle(&pen, 1.f, 1.f, rc.right - 2.f, rc.bottom - 2.f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void OpenEditor(float x, float y, float w, const Shape* existing) {
    g_edGen++;
    g_edX = floorf(x);
    g_edY = floorf(y);
    g_edW = max(60.f, w);
    g_edHost = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, ED_CLASS, L"", WS_POPUP,
                               (int)g_edX - ED_PAD, (int)g_edY - ED_PAD, 10, 10, g_ov, NULL, g_inst, NULL);
    SetLayeredWindowAttributes(g_edHost, 0, 240, LWA_ALPHA);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                             ED_PAD, ED_PAD, 10, 10, g_edHost, (HMENU)100, g_inst, NULL);
    SetWindowSubclass(g_edit, EditSubclass, 1, 0);
    UpdateEditorColors();
    ApplyEditorFont();
    if (existing) {
        SetWindowTextW(g_edit, existing->raw.c_str());
        int len = GetWindowTextLengthW(g_edit);
        SendMessageW(g_edit, EM_SETSEL, len, len);
        EditorResize();
    }
    ShowWindow(g_edHost, SW_SHOW);
    SetForegroundWindow(g_edHost);
    SetFocus(g_edit);
}

static void CommitText() {
    if (!g_edHost || g_edClosing) return;
    g_edClosing = true;
    int len = GetWindowTextLengthW(g_edit);
    std::wstring all(len, L'\0');
    if (len > 0) GetWindowTextW(g_edit, &all[0], len + 1);
    std::vector<std::wstring> lines;
    int n = (int)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        int idx = (int)SendMessageW(g_edit, EM_LINEINDEX, i, 0);
        int L = (int)SendMessageW(g_edit, EM_LINELENGTH, idx, 0);
        if (idx < 0 || idx > len) break;
        lines.push_back(all.substr(idx, min(L, len - idx)));
    }
    auto blank = [](const std::wstring& t) { return t.find_first_not_of(L" \t　") == std::wstring::npos; };
    while (!lines.empty() && blank(lines.back())) lines.pop_back();
    while (!all.empty() && (all.back() == L'\n' || all.back() == L'\r')) all.pop_back();
    float px = TextPx(), lineH = g_edLineH;
    DestroyWindow(g_edHost);
    g_edHost = g_edit = NULL;

    if (!lines.empty()) {
        SP s = std::make_shared<Shape>();
        s->type = T_TEXT;
        s->argb = PALETTE[g_color];
        s->width = px;
        s->lines = lines;
        s->raw = all;
        s->lineH = lineH;
        s->boxW = g_edW;
        float maxW = 0;
        {
            HDC dc = GetDC(NULL);
            Graphics g(dc);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            Font f(UiFamily(), px, FontStyleBold, UnitPixel);
            StringFormat sf(StringFormat::GenericTypographic());
            sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
            for (auto& l : lines) {
                if (l.empty()) continue;
                RectF m;
                g.MeasureString(l.c_str(), (INT)l.size(), &f, PointF(0, 0), &sf, &m);
                maxW = max(maxW, m.Width);
            }
            ReleaseDC(NULL, dc);
        }
        s->pts = {PointF(g_edX, g_edY), PointF(g_edX + max(maxW, 4.f), g_edY + lines.size() * lineH)};
        if (!g_edUndoPushed) PushUndo();
        g_shapes.push_back(s);
    }
    g_edUndoPushed = false;
    RenderBase();
    PresentFull();
    g_edClosing = false;
    if (g_drawMode) {
        SetForegroundWindow(g_ov);
        SetFocus(g_ov);
    }
    TbRefresh();
}

// 글상자 도구로 이미 쓴 글을 누르면 다시 편집
static bool TryEditExisting(PointF pt) {
    for (int i = (int)g_shapes.size() - 1; i >= 0; i--) {
        const Shape& s = *g_shapes[i];
        if (s.type != T_TEXT) continue;
        if (pt.X < s.pts[0].X - 4 || pt.X > s.pts[1].X + 4 || pt.Y < s.pts[0].Y - 4 || pt.Y > s.pts[1].Y + 4)
            continue;
        SP keep = g_shapes[i];
        PushUndo();
        g_edUndoPushed = true;
        g_shapes.erase(g_shapes.begin() + i);
        for (int k = 0; k < 6; k++) if (PALETTE[k] == keep->argb) g_color = k;
        int best = 0;
        for (int k = 1; k < 3; k++)
            if (fabsf(TXT_PX[k] * S - keep->width) < fabsf(TXT_PX[best] * S - keep->width)) best = k;
        g_wIdx = best;
        RenderBase();
        PresentFull();
        OpenEditor(keep->pts[0].X, keep->pts[0].Y, keep->boxW, keep.get());
        TbRefresh();
        return true;
    }
    return false;
}

static void FinishTextDrag() {
    if (!g_textDrag) return;
    g_textDrag = false;
    SP c = g_cur;
    g_cur.reset();
    if (GetCapture() == g_ov) ReleaseCapture();
    if (!c) return;
    PresentCurrent(ShapeBounds(*c));
    g_hasPrevDirty = false;
    float x = min(c->pts[0].X, c->pts[1].X), y = min(c->pts[0].Y, c->pts[1].Y);
    float w = fabsf(c->pts[1].X - c->pts[0].X);
    if (w < 40 * S) {
        MONITORINFO mi = {sizeof(mi)};
        POINT p = {(LONG)x, (LONG)y};
        GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
        w = min(520 * S, (float)mi.rcWork.right - x - 2 * ED_PAD - 4);
    }
    OpenEditor(x, y, w, nullptr);
}

static void Undo() {
    CommitText();
    if (g_undo.empty()) return;
    g_redo.push_back(g_shapes);
    g_shapes = g_undo.back();
    g_undo.pop_back();
    RenderBase();
    PresentFull();
    TbRefresh();
}

static void Redo() {
    CommitText();
    if (g_redo.empty()) return;
    g_undo.push_back(g_shapes);
    g_shapes = g_redo.back();
    g_redo.pop_back();
    RenderBase();
    PresentFull();
    TbRefresh();
}

static void ClearAll() {
    CommitText();
    if (g_shapes.empty()) return;
    PushUndo();
    g_shapes.clear();
    RenderBase();
    PresentFull();
    TbRefresh();
}

static void CommitCurrent() {
    if (!g_cur) return;
    SP s = g_cur;
    g_cur.reset();
    bool keep = true;
    if (s->type != T_PEN && s->type != T_HL) {
        float dx = fabsf(s->pts[1].X - s->pts[0].X), dy = fabsf(s->pts[1].Y - s->pts[0].Y);
        if (dx < 3 && dy < 3) keep = false;
    }
    if (keep) {
        PushUndo();
        g_shapes.push_back(s);
        Bitmap bmp(vw, vh, vw * 4, PixelFormat32bppPARGB, (BYTE*)g_base.data());
        Graphics g(&bmp);
        PrepGraphics(g);
        DrawShape(g, *s);
    }
    RectF b = ShapeBounds(*s);
    PresentCurrent(b);
    g_hasPrevDirty = false;
    TbRefresh();
}

static void EraseAlong(PointF a, PointF b) {
    float r = ER_R[g_wIdx];
    std::vector<SP> keep;
    keep.reserve(g_shapes.size());
    bool removed = false;
    for (auto& s : g_shapes) {
        if (ShapeHit(*s, a, b, r)) { removed = true; continue; }
        keep.push_back(s);
    }
    if (!removed) return;
    if (!g_eraseSnap) { PushUndo(); g_eraseSnap = true; }
    g_shapes.swap(keep);
    RenderBase();
    PresentFull();
    TbRefresh();
}

static void EndStroke() {
    if (g_textDrag) {
        g_textDrag = false;
        SP c = g_cur;
        g_cur.reset();
        if (c) { PresentCurrent(ShapeBounds(*c)); g_hasPrevDirty = false; }
    }
    if (g_cur) CommitCurrent();
    if (g_erasing) {
        g_erasing = false;
        g_tempErase = false;
        g_eraseSnap = false;
    }
    if (GetCapture() == g_ov) ReleaseCapture();
}

static void SetDrawMode(bool on) {
    if (on == g_drawMode) return;
    CommitText();
    EndStroke();
    LONG_PTR ex = GetWindowLongPtr(g_ov, GWL_EXSTYLE);
    if (on) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != g_ov && fg != g_tb) g_prevFg = fg;
        ex &= ~WS_EX_TRANSPARENT;
    } else {
        ex |= WS_EX_TRANSPARENT;
    }
    SetWindowLongPtr(g_ov, GWL_EXSTYLE, ex);
    SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_drawMode = on;
    RenderBase();
    PresentFull();
    if (on) {
        SetForegroundWindow(g_ov);
        SetFocus(g_ov);
        UpdateCursor();
        RaiseToolbar();
    } else if (g_prevFg && IsWindow(g_prevFg)) {
        SetForegroundWindow(g_prevFg);
    }
    TbRefresh();
}

static void SelectTool(Tool t) {
    if (t != T_TEXT) CommitText();
    g_tool = t;
    UpdateCursor();
    if (!g_drawMode) SetDrawMode(true);
    TbRefresh();
}

static void SelectColor(int i) {
    g_color = i;
    if (g_tool == T_ERASER) g_tool = T_PEN;
    UpdateEditorColors();
    UpdateCursor();
    TbRefresh();
}

static void SelectWidth(int i) {
    g_wIdx = max(0, min(2, i));
    ApplyEditorFont();
    UpdateCursor();
    TbRefresh();
}

static void ConfirmExit() {
    CommitText();
    if (!g_shapes.empty()) {
        g_inModal = true;
        int r = MessageBoxW(NULL, L"판서한 내용이 모두 사라집니다. 종료할까요?", APP_TITLE,
                            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
        g_inModal = false;
        if (r != IDYES) return;
    }
    DestroyWindow(g_ov);
}

// ---------------------------------------------------------------------------
// 오버레이 창
// ---------------------------------------------------------------------------
static PointF MsgPoint(LPARAM lp) {
    return PointF((REAL)(GET_X_LPARAM(lp) + vx), (REAL)(GET_Y_LPARAM(lp) + vy));
}

static void UpdateShapePreview(PointF pt) {
    if (!g_cur) return;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    g_cur->pts[1] = SnapEnd(g_cur->type, g_cur->pts[0], pt, shift);
    PresentCurrent(ShapeBounds(*g_cur));
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SETCURSOR:
        if (g_drawMode && LOWORD(lp) == HTCLIENT) {
            SetCursor(g_tempErase ? g_eraseCursor : g_cursor);
            return TRUE;
        }
        break;
    case WM_MOUSEACTIVATE:
        return g_drawMode ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        if (g_edHost) { CommitText(); return 0; } // 입력 중이던 글상자 마무리
        if (!g_drawMode || g_cur || g_erasing) return 0;
        SetFocus(h);
        PointF pt = MsgPoint(lp);
        g_lastPt = pt;
        g_hasPrevDirty = false;
        if (g_tool == T_TEXT) {
            if (TryEditExisting(pt)) return 0;
            SetCapture(h);
            g_textDrag = true;
            g_cur = std::make_shared<Shape>();
            g_cur->type = T_RECT;
            g_cur->dashed = true;
            g_cur->argb = 0xFFF2C14E;
            g_cur->width = 1.5f;
            g_cur->pts = {pt, pt};
            PresentCurrent(ShapeBounds(*g_cur));
            return 0;
        }
        SetCapture(h);
        if (g_tool == T_ERASER) {
            g_erasing = true;
            g_eraseSnap = false;
            EraseAlong(pt, pt);
        } else {
            g_cur = std::make_shared<Shape>();
            g_cur->type = g_tool;
            g_cur->argb = PALETTE[g_color];
            g_cur->width = (g_tool == T_HL) ? HL_W[g_wIdx] : PEN_W[g_wIdx];
            g_cur->pts.push_back(pt);
            if (g_tool != T_PEN && g_tool != T_HL) g_cur->pts.push_back(pt);
            PresentCurrent(ShapeBounds(*g_cur));
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (g_edHost) { CommitText(); return 0; }
        if (!g_drawMode || g_cur || g_erasing) return 0;
        SetCapture(h);
        PointF pt = MsgPoint(lp);
        g_lastPt = pt;
        g_erasing = true;
        g_tempErase = true;
        g_eraseSnap = false;
        SetCursor(g_eraseCursor);
        EraseAlong(pt, pt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_drawMode) return 0;
        PointF pt = MsgPoint(lp);
        if (g_erasing) {
            EraseAlong(g_lastPt, pt);
            g_lastPt = pt;
        } else if (g_cur) {
            if (g_cur->type == T_PEN || g_cur->type == T_HL) {
                PointF last = g_cur->pts.back();
                float dx = pt.X - last.X, dy = pt.Y - last.Y;
                if (dx * dx + dy * dy < 2.25f) return 0;
                g_cur->pts.push_back(pt);
                float pad = g_cur->width / 2 + 3;
                RectF seg(min(last.X, pt.X) - pad, min(last.Y, pt.Y) - pad,
                          fabsf(pt.X - last.X) + 2 * pad, fabsf(pt.Y - last.Y) + 2 * pad);
                g_hasPrevDirty = false;
                PresentCurrent(seg);
            } else {
                UpdateShapePreview(pt);
            }
            g_lastPt = pt;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_textDrag) FinishTextDrag();
        else if (g_cur || (g_erasing && !g_tempErase)) EndStroke();
        return 0;
    case WM_RBUTTONUP:
        if (g_erasing && g_tempErase) {
            EndStroke();
            SetCursor(g_cursor);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h && (g_cur || g_erasing)) {
            if (g_textDrag) {
                g_textDrag = false;
                SP c = g_cur;
                g_cur.reset();
                PresentCurrent(ShapeBounds(*c));
                g_hasPrevDirty = false;
            }
            if (g_cur) CommitCurrent();
            g_erasing = g_tempErase = g_eraseSnap = false;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (g_drawMode) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            SelectWidth(g_wIdx + (d > 0 ? 1 : -1));
        }
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (wp == VK_SHIFT && g_cur && g_cur->type != T_PEN && g_cur->type != T_HL) {
            UpdateShapePreview(g_lastPt);
            return 0;
        }
        if (m == WM_KEYUP || !g_drawMode) break;
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl) {
                if (wp == 'Z' && !shift) Undo();
                else if (wp == 'Y' || (wp == 'Z' && shift)) Redo();
                return 0;
            }
            switch (wp) {
            case VK_ESCAPE: SetDrawMode(false); break;
            case VK_DELETE: ClearAll(); break;
            case 'P': SelectTool(T_PEN); break;
            case 'H': SelectTool(T_HL); break;
            case 'E': SelectTool(T_ERASER); break;
            case 'L': SelectTool(T_LINE); break;
            case 'A': SelectTool(T_ARROW); break;
            case 'R': SelectTool(T_RECT); break;
            case 'O': SelectTool(T_ELLIPSE); break;
            case 'T': SelectTool(T_TEXT); break;
            case VK_OEM_4: case VK_SUBTRACT: case VK_OEM_MINUS: SelectWidth(g_wIdx - 1); break;
            case VK_OEM_6: case VK_ADD: case VK_OEM_PLUS: SelectWidth(g_wIdx + 1); break;
            default:
                if (wp >= '1' && wp <= '6') SelectColor((int)(wp - '1'));
                else if (wp >= VK_NUMPAD1 && wp <= VK_NUMPAD6) SelectColor((int)(wp - VK_NUMPAD1));
                break;
            }
        }
        return 0;
    case WM_HOTKEY:
        if (wp == 1) SetDrawMode(!g_drawMode);
        else if (wp == 2) ClearAll();
        return 0;
    case WM_APP_COMMITTEXT:
        if (wp == g_edGen) CommitText();
        return 0;
    case WM_DISPLAYCHANGE:
        CommitText();
        EndStroke();
        SetupSurface();
        if (g_tb && !MonitorFromWindow(g_tb, MONITOR_DEFAULTTONULL)) {
            RECT wa;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            SetWindowPos(g_tb, NULL, wa.right - g_tbW - (int)(12 * S),
                         wa.top + (wa.bottom - wa.top - g_tbH) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_TIMER:
        // 슬라이드쇼 등 다른 창이 위로 올라와도 다시 맨 위로
        if (!g_inModal && !g_cur && !g_erasing)
        {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RaiseToolbar();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) RaiseToolbar();
        break;
    case WM_CLOSE:
        ConfirmExit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------------------
// 도구 막대
// ---------------------------------------------------------------------------
static int D(float v) { return (int)lroundf(v * S); }

static void BuildLayout() {
    g_btns.clear();
    g_seps.clear();
    int pad = D(5), cell = D(38), gap = D(2), sepH = D(9);
    int full = cell * 2 + gap;
    int x0 = pad, x1 = pad + cell + gap;
    g_tbW = pad * 2 + full;
    g_gripH = D(16);
    int y = g_gripH;
    auto add = [&](int id, int x, int yy, int w, int hh) {
        Btn b; b.id = id; SetRect(&b.r, x, yy, x + w, yy + hh); g_btns.push_back(b);
    };
    auto sep = [&]() { g_seps.push_back(y + sepH / 2 - gap / 2); y += sepH - gap; };

    add(B_TOGGLE, x0, y, full, D(40)); y += D(40) + gap;
    if (!g_collapsed) {
        sep();
        add(B_PEN, x0, y, cell, cell);   add(B_HL, x1, y, cell, cell);      y += cell + gap;
        add(B_LINE, x0, y, cell, cell);  add(B_ARROW, x1, y, cell, cell);   y += cell + gap;
        add(B_RECT, x0, y, cell, cell);  add(B_ELLIPSE, x1, y, cell, cell); y += cell + gap;
        add(B_TEXT, x0, y, cell, cell);  add(B_ERASER, x1, y, cell, cell);  y += cell + gap;
        sep();
        int ch = D(26);
        for (int i = 0; i < 6; i++)
            add(B_C0 + i, (i % 2) ? x1 : x0, y + (i / 2) * (ch + gap), cell, ch);
        y += 3 * (ch + gap);
        sep();
        int w3 = (full - 2 * gap) / 3;
        for (int i = 0; i < 3; i++) add(B_W0 + i, x0 + i * (w3 + gap), y, w3, D(28));
        y += D(28) + gap;
        sep();
        add(B_UNDO, x0, y, cell, D(32)); add(B_REDO, x1, y, cell, D(32)); y += D(32) + gap;
        add(B_CLEAR, x0, y, full, D(30)); y += D(30) + gap;
        sep();
        add(B_COLLAPSE, x0, y, cell, D(26)); add(B_EXIT, x1, y, cell, D(26)); y += D(26);
    } else {
        add(B_COLLAPSE, x0, y, full, D(22)); y += D(22);
    }
    g_tbH = y + pad;
}

static const wchar_t* TipFor(int id) {
    switch (id) {
    case B_TOGGLE: return L"판서 켜기/끄기 (Ctrl+Shift+D)\n끄면 그림은 남고 아래 화면을 클릭할 수 있습니다.";
    case B_PEN: return L"펜 (P)";
    case B_HL: return L"형광펜 (H)";
    case B_LINE: return L"직선 (L)\nShift를 누르고 그리면 45도 단위로 맞춰집니다.";
    case B_ARROW: return L"화살표 (A)\nShift를 누르고 그리면 45도 단위로 맞춰집니다.";
    case B_RECT: return L"사각형 (R)\nShift를 누르면 정사각형";
    case B_ELLIPSE: return L"원 (O)\nShift를 누르면 정원";
    case B_TEXT: return L"글상자 (T)\n끌어서 상자를 만들거나 클릭한 자리에 바로 입력합니다.\n입력을 마치려면 Esc, Ctrl+Enter, 또는 화면 다른 곳 클릭.\n이미 쓴 글을 누르면 다시 고칠 수 있습니다.";
    case B_ERASER: return L"지우개 (E)\n닿은 선을 통째로 지웁니다.\n판서 중에는 오른쪽 버튼으로 끌어도 지워집니다.";
    case B_W0: return L"가늘게\n글상자에서는 글자 크기입니다.\n마우스 휠이나 [ ] 키로도 바꿀 수 있습니다.";
    case B_W1: return L"보통\n글상자에서는 글자 크기입니다.\n마우스 휠이나 [ ] 키로도 바꿀 수 있습니다.";
    case B_W2: return L"굵게\n글상자에서는 글자 크기입니다.\n마우스 휠이나 [ ] 키로도 바꿀 수 있습니다.";
    case B_UNDO: return L"되돌리기 (Ctrl+Z)";
    case B_REDO: return L"다시 실행 (Ctrl+Y)";
    case B_CLEAR: return L"전체 삭제 (Delete, 판서가 꺼져 있을 때는 Ctrl+Shift+X)";
    case B_COLLAPSE: return g_collapsed ? L"막대 펴기" : L"막대 접기";
    case B_EXIT: return L"프로그램 종료";
    }
    if (id >= B_C0 && id <= B_C5) return PALETTE_TIP[id - B_C0];
    return L"";
}

static void RebuildTips() {
    if (!g_tip) return;
    for (int id : g_tipIds) {
        TTTOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = g_tb;
        ti.uId = id;
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    g_tipIds.clear();
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, D(340));
    for (auto& b : g_btns) {
        TTTOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_tb;
        ti.uId = b.id;
        ti.rect = b.r;
        ti.hinst = g_inst;
        ti.lpszText = (LPWSTR)TipFor(b.id);
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        g_tipIds.push_back(b.id);
    }
}

static void ApplyTbSize(bool keepPos) {
    BuildLayout();
    RECT wr;
    GetWindowRect(g_tb, &wr);
    if (keepPos)
        SetWindowPos(g_tb, NULL, 0, 0, g_tbW, g_tbH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RebuildTips();
    InvalidateRect(g_tb, NULL, FALSE);
}

// 팔레트
static const Color C_BG(255, 0x26, 0x29, 0x2E);
static const Color C_BORDER(255, 0x45, 0x4A, 0x52);
static const Color C_HOVER(255, 0x34, 0x38, 0x3F);
static const Color C_SEL(255, 0x3C, 0x41, 0x49);
static const Color C_ACCENT(255, 0xF2, 0xC1, 0x4E);
static const Color C_ICON(255, 0xE6, 0xE7, 0xE9);
static const Color C_DIM(255, 0x6B, 0x71, 0x78);
static const Color C_SEP(255, 0x3A, 0x3F, 0x46);

static void DrawGlyph(Graphics& g, int id, float cx, float cy, float s, const Color& col) {
    // s: 아이콘 한 변 길이. (u,v)는 0~1 좌표
    auto P = [&](float u, float v) { return PointF(cx - s / 2 + u * s, cy - s / 2 + v * s); };
    Pen pen(col, max(1.6f, s * 0.09f));
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush br(col);
    switch (id) {
    case B_PEN: {
        PointF body[5] = {P(0.80f, 0.08f), P(0.94f, 0.22f), P(0.36f, 0.80f), P(0.14f, 0.88f), P(0.22f, 0.66f)};
        g.DrawPolygon(&pen, body, 5);
        g.DrawLine(&pen, P(0.22f, 0.66f), P(0.36f, 0.80f));
        break;
    }
    case B_HL: {
        Pen wide(Color(150, col.GetR(), col.GetG(), col.GetB()), s * 0.28f);
        wide.SetStartCap(LineCapFlat);
        wide.SetEndCap(LineCapFlat);
        g.DrawLine(&wide, P(0.06f, 0.78f), P(0.94f, 0.78f));
        PointF body[5] = {P(0.40f, 0.62f), P(0.32f, 0.44f), P(0.68f, 0.08f), P(0.86f, 0.26f), P(0.50f, 0.62f)};
        g.DrawPolygon(&pen, body, 5);
        break;
    }
    case B_ERASER: {
        PointF body[4] = {P(0.08f, 0.62f), P(0.52f, 0.18f), P(0.92f, 0.56f), P(0.58f, 0.90f)};
        PointF half[4] = {P(0.08f, 0.62f), P(0.30f, 0.40f), P(0.70f, 0.78f), P(0.58f, 0.90f)};
        g.FillPolygon(&br, half, 4);
        g.DrawPolygon(&pen, body, 4);
        break;
    }
    case B_LINE:
        g.DrawLine(&pen, P(0.12f, 0.88f), P(0.88f, 0.12f));
        break;
    case B_ARROW: {
        g.DrawLine(&pen, P(0.12f, 0.88f), P(0.78f, 0.22f));
        PointF tri[3] = {P(0.92f, 0.08f), P(0.52f, 0.20f), P(0.80f, 0.48f)};
        g.FillPolygon(&br, tri, 3);
        break;
    }
    case B_RECT:
        g.DrawRectangle(&pen, cx - s * 0.42f, cy - s * 0.32f, s * 0.84f, s * 0.64f);
        break;
    case B_ELLIPSE:
        g.DrawEllipse(&pen, cx - s * 0.42f, cy - s * 0.42f, s * 0.84f, s * 0.84f);
        break;
    case B_UNDO: case B_REDO: {
        bool m = (id == B_REDO);
        auto Q = [&](float u, float v) { return m ? P(1 - u, v) : P(u, v); };
        g.DrawBezier(&pen, Q(0.34f, 0.36f), Q(0.72f, 0.36f), Q(0.90f, 0.50f), Q(0.74f, 0.84f));
        PointF tri[3] = {Q(0.10f, 0.36f), Q(0.38f, 0.16f), Q(0.38f, 0.56f)};
        g.FillPolygon(&br, tri, 3);
        break;
    }
    case B_CLEAR: {
        g.DrawLine(&pen, P(0.12f, 0.24f), P(0.88f, 0.24f));
        g.DrawLine(&pen, P(0.38f, 0.24f), P(0.40f, 0.10f));
        g.DrawLine(&pen, P(0.40f, 0.10f), P(0.60f, 0.10f));
        g.DrawLine(&pen, P(0.60f, 0.10f), P(0.62f, 0.24f));
        PointF can[4] = {P(0.22f, 0.32f), P(0.78f, 0.32f), P(0.70f, 0.92f), P(0.30f, 0.92f)};
        g.DrawPolygon(&pen, can, 4);
        g.DrawLine(&pen, P(0.42f, 0.44f), P(0.43f, 0.80f));
        g.DrawLine(&pen, P(0.58f, 0.44f), P(0.57f, 0.80f));
        break;
    }
    case B_COLLAPSE:
        if (g_collapsed) {
            g.DrawLine(&pen, P(0.22f, 0.36f), P(0.50f, 0.64f));
            g.DrawLine(&pen, P(0.50f, 0.64f), P(0.78f, 0.36f));
        } else {
            g.DrawLine(&pen, P(0.22f, 0.64f), P(0.50f, 0.36f));
            g.DrawLine(&pen, P(0.50f, 0.36f), P(0.78f, 0.64f));
        }
        break;
    case B_EXIT:
        g.DrawLine(&pen, P(0.24f, 0.24f), P(0.76f, 0.76f));
        g.DrawLine(&pen, P(0.76f, 0.24f), P(0.24f, 0.76f));
        break;
    case 1000: { // 마우스 포인터
        PointF arrow[7] = {P(0.24f, 0.06f), P(0.24f, 0.82f), P(0.42f, 0.64f), P(0.56f, 0.94f),
                           P(0.68f, 0.88f), P(0.54f, 0.58f), P(0.80f, 0.58f)};
        g.DrawPolygon(&pen, arrow, 7);
        break;
    }
    }
}

static bool ToolOf(int id, Tool& t) {
    switch (id) {
    case B_PEN: t = T_PEN; return true;
    case B_HL: t = T_HL; return true;
    case B_LINE: t = T_LINE; return true;
    case B_ARROW: t = T_ARROW; return true;
    case B_RECT: t = T_RECT; return true;
    case B_ELLIPSE: t = T_ELLIPSE; return true;
    case B_ERASER: t = T_ERASER; return true;
    case B_TEXT: t = T_TEXT; return true;
    }
    return false;
}


static void PaintToolbar(HDC hdc) {
    RECT rc;
    GetClientRect(g_tb, &rc);
    int W = rc.right, H = rc.bottom;
    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old = (HBITMAP)SelectObject(mdc, bmp);
    {
        Graphics g(mdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        SolidBrush bg(C_BG);
        g.FillRectangle(&bg, 0, 0, W, H);
        Pen border(C_BORDER, 1.f);
        g.DrawRectangle(&border, 0.5f, 0.5f, W - 1.f, H - 1.f);

        // 끌기 손잡이
        SolidBrush dots(C_DIM);
        float gy = g_gripH / 2.f;
        for (int i = -2; i <= 2; i++)
            for (int j = -1; j <= 1; j += 2)
                g.FillEllipse(&dots, W / 2.f + i * D(6) - D(1.2f), gy + j * D(2.5f) - D(1.2f), (REAL)D(2.4f), (REAL)D(2.4f));

        Pen sepPen(C_SEP, 1.f);
        for (int sy : g_seps) g.DrawLine(&sepPen, (REAL)D(8), sy + 0.5f, (REAL)(W - D(8)), sy + 0.5f);

        Font font(UiFamily(), (REAL)D(12), FontStyleBold, UnitPixel);
        Font fontR(UiFamily(), (REAL)D(11.5f), FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentNear);
        sf.SetLineAlignment(StringAlignmentCenter);

        for (auto& b : g_btns) {
            RectF r((REAL)b.r.left, (REAL)b.r.top, (REAL)(b.r.right - b.r.left), (REAL)(b.r.bottom - b.r.top));
            float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
            bool hover = (b.id == g_hover);
            Tool t;
            bool isTool = ToolOf(b.id, t);
            bool selTool = isTool && t == g_tool;

            if (b.id == B_TOGGLE) {
                SolidBrush fill(g_drawMode ? C_ACCENT : (hover ? C_SEL : C_HOVER));
                g.FillRectangle(&fill, r);
                Color fg = g_drawMode ? Color(255, 0x1E, 0x1F, 0x22) : C_ICON;
                float is = (REAL)D(18);
                float ix = r.X + D(10) + is / 2;
                DrawGlyph(g, g_drawMode ? B_PEN : 1000, ix, cy, is, fg);
                SolidBrush tb(fg);
                RectF tr(ix + is / 2 + D(5), r.Y, r.Width, r.Height);
                g.DrawString(g_drawMode ? L"판서 중" : L"마우스", -1, &font, tr, &sf, &tb);
                continue;
            }

            if (selTool) {
                SolidBrush fill(C_SEL);
                g.FillRectangle(&fill, r);
                SolidBrush acc(C_ACCENT);
                g.FillRectangle(&acc, r.X, r.Y, (REAL)D(3), r.Height);
            } else if (hover) {
                SolidBrush fill(C_HOVER);
                g.FillRectangle(&fill, r);
            }

            if (b.id >= B_C0 && b.id <= B_C5) {
                int i = b.id - B_C0;
                uint32_t a = PALETTE[i];
                float in = (REAL)D(5);
                RectF sw(r.X + in, r.Y + in * 0.8f, r.Width - 2 * in, r.Height - 1.6f * in);
                if (i == g_color) {
                    Pen ring(C_ACCENT, (REAL)D(2));
                    g.DrawRectangle(&ring, sw.X - D(2.5f), sw.Y - D(2.5f), sw.Width + D(5), sw.Height + D(5));
                }
                SolidBrush c(Color(255, (BYTE)(a >> 16), (BYTE)(a >> 8), (BYTE)a));
                g.FillRectangle(&c, sw);
                Pen e(Color(255, 0x5A, 0x60, 0x68), 1.f);
                g.DrawRectangle(&e, sw);
                continue;
            }
            if (b.id >= B_W0 && b.id <= B_W2) {
                int i = b.id - B_W0;
                if (i == g_wIdx) {
                    SolidBrush fill(C_SEL);
                    g.FillRectangle(&fill, r);
                    SolidBrush acc(C_ACCENT);
                    g.FillRectangle(&acc, r.X + D(4), r.Y + r.Height - D(3), r.Width - D(8), (REAL)D(2));
                }
                float d = (REAL)D(i == 0 ? 5.f : (i == 1 ? 8.5f : 13.f));
                uint32_t a = PALETTE[g_color];
                SolidBrush c(Color(255, (BYTE)(a >> 16), (BYTE)(a >> 8), (BYTE)a));
                g.FillEllipse(&c, cx - d / 2, cy - d / 2, d, d);
                Pen e(Color(255, 0x8A, 0x90, 0x98), 1.f);
                g.DrawEllipse(&e, cx - d / 2, cy - d / 2, d, d);
                continue;
            }

            Color ic = C_ICON;
            if ((b.id == B_UNDO && g_undo.empty()) || (b.id == B_REDO && g_redo.empty()) ||
                (b.id == B_CLEAR && g_shapes.empty()))
                ic = C_DIM;
            if (b.id == B_EXIT && hover) ic = Color(255, 0xFF, 0x8A, 0x80);

            if (b.id == B_TEXT) {
                Font tf(UiFamily(), (REAL)D(17), FontStyleBold, UnitPixel);
                StringFormat cf;
                cf.SetAlignment(StringAlignmentCenter);
                cf.SetLineAlignment(StringAlignmentCenter);
                SolidBrush tb(ic);
                g.DrawString(L"가", -1, &tf, r, &cf, &tb);
                continue;
            }
            if (b.id == B_CLEAR) {
                const wchar_t* label = L"전체 삭제";
                float is = (REAL)D(16);
                float gapT = (REAL)D(5);
                RectF m;
                g.MeasureString(label, -1, &fontR, PointF(0, 0), &m);
                float tw = min(m.Width, r.Width - is - gapT - D(4));
                float gx = r.X + (r.Width - (is + gapT + tw)) / 2;
                DrawGlyph(g, b.id, gx + is / 2, cy, is, ic);
                SolidBrush tb(ic);
                RectF tr(gx + is + gapT - D(2), r.Y, tw + D(6), r.Height);
                g.DrawString(label, -1, &fontR, tr, &sf, &tb);
            } else {
                float is = (REAL)D(b.id == B_COLLAPSE || b.id == B_EXIT ? 14.f : 19.f);
                DrawGlyph(g, b.id, cx, cy, is, ic);
            }
        }
    }
    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
}

static int BtnAt(int x, int y) {
    POINT p = {x, y};
    for (auto& b : g_btns)
        if (PtInRect(&b.r, p)) return b.id;
    return -1;
}

static void OnButton(int id) {
    Tool t;
    if (id == B_TOGGLE) SetDrawMode(!g_drawMode);
    else if (ToolOf(id, t)) SelectTool(t);
    else if (id >= B_C0 && id <= B_C5) SelectColor(id - B_C0);
    else if (id >= B_W0 && id <= B_W2) SelectWidth(id - B_W0);
    else if (id == B_UNDO) Undo();
    else if (id == B_REDO) Redo();
    else if (id == B_CLEAR) ClearAll();
    else if (id == B_COLLAPSE) { g_collapsed = !g_collapsed; g_hover = -1; ApplyTbSize(true); }
    else if (id == B_EXIT) ConfirmExit();
}

typedef UINT(WINAPI* PGetDpiForWindow)(HWND);
static UINT DpiOf(HWND h) {
    static PGetDpiForWindow fn = (PGetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) return fn(h);
    HDC dc = GetDC(NULL);
    UINT d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d;
}

static LRESULT CALLBACK ToolbarProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NCHITTEST: {
        LRESULT r = DefWindowProcW(h, m, wp, lp);
        if (r == HTCLIENT) {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            if (p.y < g_gripH || BtnAt(p.x, p.y) < 0) return HTCAPTION;
        }
        return r;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCLBUTTONDBLCLK:
        return 0;
    case WM_MOUSEMOVE: {
        int id = BtnAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (id != g_hover) { g_hover = id; InvalidateRect(h, NULL, FALSE); }
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        if (g_hover != -1) { g_hover = -1; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        int id = BtnAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (id > 0) OnButton(id);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        PaintToolbar(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        S = HIWORD(wp) / 96.f;
        RECT* r = (RECT*)lp;
        BuildLayout();
        SetWindowPos(h, NULL, r->left, r->top, g_tbW, g_tbH, SWP_NOZORDER | SWP_NOACTIVATE);
        RebuildTips();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        ConfirmExit();
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------------------
// 시작
// ---------------------------------------------------------------------------
typedef BOOL(WINAPI* PSetDpiCtx)(HANDLE);

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_inst = hInst;
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\PanseoScreenPenSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"화면 판서가 이미 실행 중입니다.", APP_TITLE, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return 0;
    }

    // 매니페스트가 무시되는 환경 대비
    PSetDpiCtx setCtx = (PSetDpiCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!setCtx || !setCtx((HANDLE)-4)) SetProcessDPIAware();

    GdiplusStartupInput gsi;
    ULONG_PTR gtoken;
    GdiplusStartup(&gtoken, &gsi, NULL);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    HICON icon = LoadIconW(hInst, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInst;
    wc.lpszClassName = OV_CLASS;
    wc.hIcon = icon;
    RegisterClassExW(&wc);

    WNDCLASSEXW tc = {};
    tc.cbSize = sizeof(tc);
    tc.style = CS_DROPSHADOW;
    tc.lpfnWndProc = ToolbarProc;
    tc.hInstance = hInst;
    tc.hCursor = LoadCursor(NULL, IDC_ARROW);
    tc.lpszClassName = TB_CLASS;
    tc.hIcon = icon;
    RegisterClassExW(&tc);

    WNDCLASSEXW ec = {};
    ec.cbSize = sizeof(ec);
    ec.lpfnWndProc = EdHostProc;
    ec.hInstance = hInst;
    ec.hCursor = LoadCursor(NULL, IDC_IBEAM);
    ec.lpszClassName = ED_CLASS;
    RegisterClassExW(&ec);

    g_ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
                           OV_CLASS, APP_TITLE, WS_POPUP, 0, 0, 1, 1, NULL, NULL, hInst, NULL);
    g_memDC = CreateCompatibleDC(NULL);
    SetupSurface();
    ShowWindow(g_ov, SW_SHOWNOACTIVATE);

    g_tb = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           TB_CLASS, APP_TITLE, WS_POPUP, 0, 0, 10, 10, g_ov, NULL, hInst, NULL);
    S = DpiOf(g_tb) / 96.f;
    BuildLayout();
    {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(g_tb, HWND_TOPMOST, wa.right - g_tbW - D(12),
                     wa.top + max(0L, (wa.bottom - wa.top - g_tbH) / 2), g_tbW, g_tbH, SWP_NOACTIVATE);
    }
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_tb, NULL, hInst, NULL);
    {
        HFONT f = CreateFontW(-D(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Malgun Gothic");
        SendMessageW(g_tip, WM_SETFONT, (WPARAM)f, TRUE);
    }
    RebuildTips();
    ShowWindow(g_tb, SW_SHOWNOACTIVATE);

    UpdateCursor();
    bool hk1 = RegisterHotKey(g_ov, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'D');
    RegisterHotKey(g_ov, 2, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'X');
    if (!hk1) {
        g_inModal = true;
        MessageBoxW(NULL, L"Ctrl+Shift+D 단축키를 다른 프로그램이 이미 쓰고 있어서 등록하지 못했습니다.\n"
                          L"판서 켜기/끄기는 도구 막대 맨 위 버튼으로 해 주세요.",
                    APP_TITLE, MB_OK | MB_ICONWARNING | MB_TOPMOST);
        g_inModal = false;
    }
    SetTimer(g_ov, 1, 2500, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_ov, 1);
    UnregisterHotKey(g_ov, 2);
    GdiplusShutdown(gtoken);
    if (mtx) CloseHandle(mtx);
    return 0;
}
