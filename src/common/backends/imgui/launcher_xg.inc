// launcher_xg.inc — the Xenogears launcher: the game's own menu look and a
// three-level, pad-first layout.
//
// Included by launcher_imgui.cpp inside its anonymous namespace, right before
// draw_ui(), so it reuses that file's static helpers (pickers, mod install
// queue, PLAY gating) instead of duplicating them.
//
// Layout: a header window (disc identity), a command window on the left
// (Game / Config / Mods / … / Exit) with the Start window under it, the page
// content on the right, a help window under the content and a button legend
// along the bottom. Navigation is our own state machine, not ImGui's nav:
//
//   MENU     ↑↓ moves the red arrow; → or ✕ enters the page
//   CONTENT  Game: windows (disc, players, cards) by position; ✕ opens options
//            Config: one list per section; ←→ changes in place; L1/R1 section
//            Mods: feature list; ←→ on/off; ✕ options; L1/R1 tab
//   POPUP    a small options window over the page; ↑↓ ←→ ✕, ○ closes
//
// START plays from anywhere; ○ always climbs one level. The mouse drives the
// same state: hovering marks, clicking selects/activates, the wheel changes a
// value, right click backs out. Pages that are still plain ImGui (Controller
// bindings, Assist Tools, Credits) render inside the content window with
// ImGui's own nav; netplay keeps its full legacy screen.
//
// Everything is sized from one scale factor taken from the window (the layout
// is designed on a 1120x760 canvas), and text is drawn from g_xg_font, a large
// raster of the UI face, so it stays sharp at any size.

/* ---- palette (sampled from the game's menu windows) ---------------------- */
static const ImU32 XG_WL   = IM_COL32(239, 240, 249, 255);  // window line + text
static const ImU32 XG_WG   = IM_COL32(60, 59, 91, 255);     // gap between the lines
static const ImU32 XG_OL   = IM_COL32(49, 33, 132, 255);    // text/line outline
static const ImU32 XG_RED  = IM_COL32(253, 1, 0, 255);
static const ImU32 XG_DIS  = IM_COL32(158, 158, 164, 255);  // greyed text (neutral, dark rim)
static const ImU32 XG_GOOD = IM_COL32(122, 224, 140, 255);
static const ImU32 XG_WARN = IM_COL32(245, 178, 60, 255);
static const ImU32 XG_BAD  = IM_COL32(255, 92, 80, 255);

static ImU32 xg_a(ImU32 c, float a) {
    const unsigned al = (unsigned)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | ((al & 0xFF) << IM_COL32_A_SHIFT);
}

/* ---- scale ----------------------------------------------------------------- */
static float g_xs = 1.0f;          // layout scale for this frame
static float g_xfs = 21.0f;        // body text size in pixels for this frame
static inline float xs(float v) { return v * g_xs; }
static inline ImVec2 xv(float x, float y) { return ImVec2(x * g_xs, y * g_xs); }

static void xg_set_scale(ImVec2 size) {
    float s = std::min(size.x / 1120.0f, size.y / 760.0f);
    if (s < 0.70f) s = 0.70f;
    if (s > 3.00f) s = 3.00f;
    g_xs = s;
    g_xfs = 20.0f * s;
}

/* ---- drawing primitives -------------------------------------------------- */
static ImFont* xg_font() { return g_xg_font ? g_xg_font : ImGui::GetFont(); }
static float xg_fs() { return g_xfs; }

// The game's text is a dot font: one-dot white strokes, a solid one-dot
// indigo rim all the way round (corners included) and generous spacing
// between letters. Glyphs are laid out one by one to get that spacing.
static const char* xg_utf8(const char* p, unsigned* cp) {
    const unsigned char c = (unsigned char)*p;
    if (c < 0x80) { *cp = c; return p + 1; }
    if ((c >> 5) == 0x6 && p[1]) { *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); return p + 2; }
    if ((c >> 4) == 0xE && p[1] && p[2]) {
        *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return p + 3;
    }
    if ((c >> 3) == 0x1E && p[1] && p[2] && p[3]) {
        *cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); return p + 4;
    }
    *cp = '?'; return p + 1;
}

// Menu labels drawn over the orbs use the game's black rim instead of indigo.
static bool g_xg_black_outline = false;

static float xg_spacing(float size) { return size * 0.10f; }
static float xg_line_h(float size) { return size * 1.32f; }

// Width of [s, e) on one line (stops at '\n').
static float xg_line_w(const char* s, const char* e, float size) {
    ImFont* f = xg_font();
    const float sc = size / f->FontSize;
    float w = 0.0f;
    int n = 0;
    for (const char* q = s; q < e && *q && *q != '\n';) {
        unsigned c; q = xg_utf8(q, &c);
        w += f->GetCharAdvance((ImWchar)(c <= 0xFFFF ? c : '?')) * sc;
        ++n;
    }
    return n ? w + xg_spacing(size) * (n - 1) : 0.0f;
}

// Split into display lines: honours '\n', and word-wraps at `wrap` (> 0).
static void xg_lines(const char* s, float size, float wrap, std::vector<std::pair<const char*, const char*>>& out) {
    const char* p = s;
    for (;;) {
        const char* eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        if (wrap <= 0.0f || xg_line_w(p, eol, size) <= wrap) {
            out.push_back({p, eol});
        } else {
            const char* ls = p;
            while (ls < eol) {
                const char* best = nullptr;
                const char* end = ls;
                for (;;) {
                    const char* we = end;
                    while (we < eol && *we == ' ') ++we;
                    while (we < eol && *we != ' ') ++we;
                    if (!best || xg_line_w(ls, we, size) <= wrap) {
                        best = we; end = we;
                        if (we >= eol) break;
                    } else {
                        break;
                    }
                }
                out.push_back({ls, best});
                ls = best;
                while (ls < eol && *ls == ' ') ++ls;
            }
        }
        if (!*eol) break;
        p = eol + 1;
    }
}

static ImVec2 xg_tsize(const char* s, float size = 0.0f, float wrap = 0.0f) {
    if (size <= 0.0f) size = xg_fs();
    if (!s || !s[0]) return ImVec2(0, 0);
    std::vector<std::pair<const char*, const char*>> lines;
    xg_lines(s, size, wrap, lines);
    float w = 0.0f;
    for (const auto& l : lines) w = std::max(w, xg_line_w(l.first, l.second, size));
    return ImVec2(w, lines.size() <= 1 ? size : xg_line_h(size) * (lines.size() - 1) + size);
}

static void xg_text(ImDrawList* dl, ImVec2 p, ImU32 c, const char* s,
                    float size = 0.0f, float wrap = 0.0f, float alpha = 1.0f) {
    if (!s || !s[0]) return;
    if (size <= 0.0f) size = xg_fs();
    ImFont* f = xg_font();
    const float sc = size / f->FontSize, sp = xg_spacing(size);
    // A solid rim of whole pixels: every glyph sits on a pixel and the rim is
    // drawn at every whole offset out to its width, so it never smears.
    const int o = std::max(1, (int)(size / 12.0f + 0.5f));
    // Greyed text keeps a dark neutral rim, like the game's inactive entries.
    const ImU32 ol = xg_a(g_xg_black_outline ? IM_COL32(8, 8, 8, 255)
                          : c == XG_DIS ? IM_COL32(34, 32, 44, 255) : XG_OL, alpha), fc = xg_a(c, alpha);
    std::vector<std::pair<const char*, const char*>> lines;
    xg_lines(s, size, wrap, lines);
    float y = (float)(int)(p.y + 0.5f);
    for (const auto& l : lines) {
        float x = p.x;
        for (const char* q = l.first; q < l.second && *q;) {
            unsigned cp; const char* nx = xg_utf8(q, &cp);
            if (cp != ' ') {
                const float gx = (float)(int)(x + 0.5f);
                for (int dy = -o; dy <= o; ++dy)
                    for (int dx = -o; dx <= o; ++dx) {
                        if (!dx && !dy) continue;
                        if (dx * dx + dy * dy > o * o + o) continue;   // round the corners a little
                        dl->AddText(f, size, ImVec2(gx + dx, y + dy), ol, q, nx);
                    }
                dl->AddText(f, size, ImVec2(gx, y), fc, q, nx);
            }
            x += f->GetCharAdvance((ImWchar)(cp <= 0xFFFF ? cp : '?')) * sc + sp;
            q = nx;
        }
        y += xg_line_h(size);
    }
}

static void xg_text_r(ImDrawList* dl, ImVec2 right_top, ImU32 c, const char* s,
                      float size = 0.0f, float alpha = 1.0f) {
    const ImVec2 ts = xg_tsize(s, size);
    xg_text(dl, ImVec2(right_top.x - ts.x, right_top.y), c, s, size, 0.0f, alpha);
}

// The game's window: neutral dark-grey fill, and a border that is one soft
// white line between two indigo bands.
static void xg_window(ImDrawList* dl, ImVec2 a, ImVec2 b, bool focus = false,
                      bool bar = false, float alpha = 1.0f) {
    const float r = xs(6.0f);
    const float band = std::max(3.0f, xs(6.0f)), core = std::max(1.0f, xs(1.8f));
    if (focus) {
        dl->AddRect(ImVec2(a.x - xs(5), a.y - xs(5)), ImVec2(b.x + xs(5), b.y + xs(5)),
                    xg_a(IM_COL32(253, 1, 0, 80), alpha), r + xs(5), 0, xs(6.0f));
        dl->AddRect(ImVec2(a.x - xs(3), a.y - xs(3)), ImVec2(b.x + xs(3), b.y + xs(3)),
                    xg_a(IM_COL32(253, 30, 20, 220), alpha), r + xs(3), 0, std::max(1.5f, xs(2.0f)));
    }
    dl->AddRectFilled(a, b, xg_a(IM_COL32(49, 49, 49, 240), alpha), r);
    const ImVec2 ia(a.x + band * 0.5f, a.y + band * 0.5f), ib(b.x - band * 0.5f, b.y - band * 0.5f);
    dl->AddRect(ia, ib, xg_a(IM_COL32(57, 57, 99, 255), alpha), r, 0, band);
    dl->AddRect(ia, ib, xg_a(IM_COL32(200, 200, 214, 110), alpha), r, 0, core * 2.2f);
    dl->AddRect(ia, ib, xg_a(IM_COL32(239, 239, 239, 255), alpha), r, 0, core);
    (void)bar;   // kept for the content inset only; the game's bar is a scroll bar, not decoration
}

// The game's menu orb: a shaded sphere lit from the upper left. Unlit it is
// a warm brown-grey, lit it is red with a pink sheen (sampled from the game).
static ImU32 xg_mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int sh) {
        const float x = ((a >> sh) & 0xFF) * (1 - t) + ((b >> sh) & 0xFF) * t;
        return (ImU32)(x + 0.5f) << sh;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (0xFFu << IM_COL32_A_SHIFT);
}

static void xg_orb(ImDrawList* dl, ImVec2 c, float r, bool red, float alpha = 1.0f) {
    // rim -> body -> sheen -> highlight
    const ImU32 rim   = red ? IM_COL32(74, 0, 0, 255)     : IM_COL32(24, 16, 8, 255);
    const ImU32 body  = red ? IM_COL32(178, 20, 16, 255)  : IM_COL32(70, 52, 44, 255);
    const ImU32 sheen = red ? IM_COL32(245, 84, 74, 255)  : IM_COL32(132, 108, 100, 255);
    const ImU32 hi    = red ? IM_COL32(255, 198, 206, 255) : IM_COL32(208, 200, 208, 255);
    const int seg = r > 14 ? 40 : 28;
    const int steps = std::max(8, std::min(22, (int)(r * 0.9f)));
    const ImVec2 lit(c.x - r * 0.28f, c.y - r * 0.36f);
    for (int i = 0; i <= steps; ++i) {
        const float t = (float)i / steps;              // 0 outer, 1 inner
        const float rr = r * (1.0f - t * 0.80f);
        const ImVec2 cc(c.x + (lit.x - c.x) * t, c.y + (lit.y - c.y) * t);
        ImU32 col = t < 0.25f ? xg_mix(rim, body, t / 0.25f)
                  : t < 0.72f ? xg_mix(body, sheen, (t - 0.25f) / 0.47f)
                              : xg_mix(sheen, hi, (t - 0.72f) / 0.28f);
        dl->AddCircleFilled(cc, rr, xg_a(col, alpha), seg);
    }
    // Small white glint.
    dl->AddCircleFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.46f), std::max(1.0f, r * 0.12f),
                        xg_a(IM_COL32(255, 255, 255, 230), alpha), 12);
}

// Red ▶ cursor; `p` is the left edge, vertically centred on the row.
static void xg_cursor(ImDrawList* dl, ImVec2 p, float alpha = 1.0f) {
    const float h = xs(7.0f), w = xs(11.0f), o = std::max(1.0f, xs(1.2f));
    dl->AddTriangleFilled(ImVec2(p.x - o, p.y - h - o), ImVec2(p.x + w + o * 1.5f, p.y), ImVec2(p.x - o, p.y + h + o), xg_a(XG_OL, alpha));
    dl->AddTriangleFilled(ImVec2(p.x, p.y - h), ImVec2(p.x + w, p.y), ImVec2(p.x, p.y + h), xg_a(XG_RED, alpha));
}

// The big red arrow of the main menu; `tip` touches the selected orb.
// The game's selector: a flat red arrowhead whose tip rests on the top of the
// lit orb, with a long shaft up and a notched wing to the right. `oc` and `r`
// are the orb's centre and radius.
static void xg_chevron(ImDrawList* dl, ImVec2 oc, float r) {
    const float k = 0.68f;   // the game's proportions, a little smaller to stay in its window
    const ImVec2 tip(oc.x, oc.y - r * 0.85f);
    auto P = [&](float dx, float dy) { return ImVec2(tip.x + dx * r * k, tip.y + dy * r * k); };
    const ImVec2 top = P(2.60f, -4.85f), notch = P(2.94f, -3.10f), wing = P(4.30f, -2.85f);
    const ImVec2 pts[4] = {tip, top, notch, wing};
    dl->AddConcavePolyFilled(pts, 4, IM_COL32(255, 0, 0, 255));   // one shape: no seam
}

enum XgGlyph { XGB_CROSS, XGB_CIRCLE, XGB_TRIANGLE, XGB_SQUARE, XGB_START, XGB_L1, XGB_R1, XGB_L1R1, XGB_DPAD, XGB_LR, XGB_UD };

// Which buttons the legend and the help speak of: the keyboard, or the
// family of the pad in use. Positions are what matter (SDL maps by place):
// "Cross" is the bottom face button, which is A on Xbox and B on Nintendo.
enum { XPS_KEYBOARD = 0, XPS_PLAYSTATION, XPS_XBOX, XPS_NINTENDO };
static int g_xg_style = XPS_KEYBOARD;
static bool g_xg_last_pad = false;

static bool xg_has(const char* hay, const char* needle) {
    if (!hay) return false;
    const size_t n = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && ((p[i] | 32) == (needle[i] | 32))) ++i;
        if (i == n) return true;
    }
    return false;
}

static int xg_pad_family(const char* name) {
    if (xg_has(name, "dualsense") || xg_has(name, "dualshock") || xg_has(name, "playstation") ||
        xg_has(name, "ps3") || xg_has(name, "ps4") || xg_has(name, "ps5") || xg_has(name, "sony"))
        return XPS_PLAYSTATION;
    if (xg_has(name, "nintendo") || xg_has(name, "switch") || xg_has(name, "joy-con") ||
        xg_has(name, "pro controller"))
        return XPS_NINTENDO;
    return XPS_XBOX;   // Xbox and the many pads that copy its layout
}

static const char* xg_bname(XgGlyph g) {
    switch (g_xg_style) {
        case XPS_KEYBOARD:
            switch (g) {
                case XGB_CROSS: return "Enter"; case XGB_CIRCLE: return "Esc";
                case XGB_TRIANGLE: return "Tab"; case XGB_SQUARE: return "Del";
                case XGB_START: return "Space"; case XGB_L1: return "Q"; case XGB_R1: return "E";
                default: return "";
            }
        case XPS_XBOX:
            switch (g) {
                case XGB_CROSS: return "A"; case XGB_CIRCLE: return "B";
                case XGB_TRIANGLE: return "Y"; case XGB_SQUARE: return "X";
                case XGB_START: return "Menu"; case XGB_L1: return "LB"; case XGB_R1: return "RB";
                default: return "";
            }
        case XPS_NINTENDO:
            switch (g) {
                case XGB_CROSS: return "B"; case XGB_CIRCLE: return "A";
                case XGB_TRIANGLE: return "X"; case XGB_SQUARE: return "Y";
                case XGB_START: return "+"; case XGB_L1: return "L"; case XGB_R1: return "R";
                default: return "";
            }
        default:
            switch (g) {
                case XGB_CROSS: return "Cross"; case XGB_CIRCLE: return "Circle";
                case XGB_TRIANGLE: return "Triangle"; case XGB_SQUARE: return "Square";
                case XGB_START: return "START"; case XGB_L1: return "L1"; case XGB_R1: return "R1";
                default: return "";
            }
    }
}

// Help text names buttons through tokens so it always matches the pad in use.
static std::string xg_expand(std::string s) {
    static const struct { const char* tok; XgGlyph g; } k[] = {
        {"{OK}", XGB_CROSS}, {"{BACK}", XGB_CIRCLE}, {"{ALT}", XGB_TRIANGLE},
        {"{START}", XGB_START}, {"{L1}", XGB_L1}, {"{R1}", XGB_R1},
    };
    for (const auto& t : k) {
        size_t at;
        while ((at = s.find(t.tok)) != std::string::npos) s.replace(at, strlen(t.tok), xg_bname(t.g));
    }
    return s;
}

// Ink box of a string (the pixels the glyphs actually cover), relative to
// the pen position, so small labels can be centred by what is drawn rather
// than by the font's line box.
static void xg_ink(const char* s, float size, ImVec2* mn, ImVec2* mx) {
    ImFont* f = xg_font();
    const float sc = size / f->FontSize;
    float x = 0.0f;
    *mn = ImVec2(FLT_MAX, FLT_MAX); *mx = ImVec2(-FLT_MAX, -FLT_MAX);
    for (const char* q = s; *q;) {
        unsigned c; q = xg_utf8(q, &c);
        const ImFontGlyph* g = f->FindGlyph((ImWchar)(c <= 0xFFFF ? c : '?'));
        if (!g) continue;
        if (g->Visible) {
            mn->x = std::min(mn->x, x + g->X0 * sc); mx->x = std::max(mx->x, x + g->X1 * sc);
            mn->y = std::min(mn->y, g->Y0 * sc);     mx->y = std::max(mx->y, g->Y1 * sc);
        }
        x += g->AdvanceX * sc;
    }
    if (mn->x > mx->x) { *mn = ImVec2(0, 0); *mx = ImVec2(0, 0); }
}

// Draw `s` so its ink is centred on `c`.
static void xg_text_centered(ImDrawList* dl, ImVec2 c, ImU32 col, const char* s, float size) {
    ImVec2 mn, mx; xg_ink(s, size, &mn, &mx);
    const ImVec2 p(c.x - (mn.x + mx.x) * 0.5f, c.y - (mn.y + mx.y) * 0.5f);
    dl->AddText(xg_font(), size, ImVec2((float)(int)(p.x + 0.5f), (float)(int)(p.y + 0.5f)), col, s);
}


// Top y at which text of `size` has its capitals centred on `cy`.
static float xg_cap_top(float cy, float size) {
    ImVec2 mn, mx; xg_ink("H", size, &mn, &mx);
    return cy - (mn.y + mx.y) * 0.5f;
}

static float xg_chip(ImDrawList* dl, ImVec2 p, const char* s) {
    const float h = xs(24.0f), fs = xg_fs() * 0.66f;
    ImVec2 mn, mx; xg_ink(s, fs, &mn, &mx);
    const float w = std::max(h, (mx.x - mn.x) + xs(14.0f));
    if (!dl) return w;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(26, 26, 34, 255), xs(4.0f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), XG_DIS, xs(4.0f), 0, std::max(1.0f, xs(1.0f)));
    xg_text_centered(dl, ImVec2(p.x + w * 0.5f, p.y + h * 0.5f), XG_WL, s, fs);
    return w;
}

// A face button drawn as the pad family shows it.
static float xg_face(ImDrawList* dl, ImVec2 p, XgGlyph g) {
    const float h = xs(24.0f);
    if (!dl) return h;
    const ImVec2 c(p.x + h * .5f, p.y + h * .5f);
    const float lw = std::max(1.5f, xs(2.0f));
    if (g_xg_style == XPS_XBOX || g_xg_style == XPS_NINTENDO) {
        ImU32 fill = IM_COL32(60, 60, 68, 255), ink = XG_WL;
        if (g_xg_style == XPS_XBOX) {
            fill = g == XGB_CROSS ? IM_COL32(80, 170, 60, 255) : g == XGB_CIRCLE ? IM_COL32(210, 50, 50, 255)
                 : g == XGB_SQUARE ? IM_COL32(50, 110, 210, 255) : IM_COL32(230, 180, 30, 255);
            ink = g == XGB_TRIANGLE ? IM_COL32(30, 30, 30, 255) : IM_COL32_WHITE;
        }
        dl->AddCircleFilled(c, h * .5f, fill, 24);
        dl->AddCircle(c, h * .5f, IM_COL32(20, 20, 26, 255), 24, std::max(1.0f, xs(1.0f)));
        xg_text_centered(dl, c, ink, xg_bname(g), xg_fs() * 0.70f);
        return h;
    }
    dl->AddCircleFilled(c, h * .5f, IM_COL32(26, 26, 34, 255), 24);
    dl->AddCircle(c, h * .5f, XG_DIS, 24, std::max(1.0f, xs(1.0f)));
    const float k = h * .22f;
    if (g == XGB_CROSS) {
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32(143, 176, 255, 255), lw);
        dl->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x - k, c.y + k), IM_COL32(143, 176, 255, 255), lw);
    } else if (g == XGB_CIRCLE) {
        dl->AddCircle(c, k * 1.15f, IM_COL32(255, 122, 122, 255), 20, lw);
    } else if (g == XGB_TRIANGLE) {
        dl->AddTriangle(ImVec2(c.x, c.y - k * 1.2f), ImVec2(c.x + k * 1.2f, c.y + k), ImVec2(c.x - k * 1.2f, c.y + k),
                        IM_COL32(95, 224, 160, 255), lw);
    } else {
        dl->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32(243, 160, 220, 255), 0, 0, lw);
    }
    return h;
}

// A button chip for the current input family; returns its width (height xs(24)).
static float xg_glyph(ImDrawList* dl, ImVec2 p, XgGlyph g) {
    switch (g) {
        case XGB_DPAD: return xg_chip(dl, p, "\xE2\x86\x91\xE2\x86\x93\xE2\x86\x90\xE2\x86\x92");
        case XGB_LR: return xg_chip(dl, p, "\xE2\x86\x90\xE2\x86\x92");
        case XGB_UD: return xg_chip(dl, p, "\xE2\x86\x91\xE2\x86\x93");
        case XGB_START: case XGB_L1: case XGB_R1: return xg_chip(dl, p, xg_bname(g));
        case XGB_L1R1: {
            const float w = xg_chip(dl, p, xg_bname(XGB_L1));
            return w + xs(4) + xg_chip(dl, ImVec2(p.x + w + xs(4), p.y), xg_bname(XGB_R1));
        }
        default: break;
    }
    if (g_xg_style == XPS_KEYBOARD) return xg_chip(dl, p, xg_bname(g));
    return xg_face(dl, p, g);
}

// Draw `glyph` then `label`, starting at x; returns the x after it.
static float xg_hint(ImDrawList* dl, float x, float y_top, XgGlyph g, const char* label, float alpha = 1.0f) {
    x += xg_glyph(dl, ImVec2(x, y_top), g) + xs(7);
    const float fs = xg_fs() * 0.8f;
    xg_text(dl, ImVec2(x, xg_cap_top(y_top + xs(12), fs)), XG_DIS, label, fs, 0, alpha);
    return x + xg_tsize(label, fs).x;
}

// The "Options" hint, right-aligned at `right` (its row top at y_top).
static void xg_hint_r(ImDrawList* dl, float right, float y_top, XgGlyph g, const char* label, float alpha = 1.0f) {
    const float fs = xg_fs() * 0.8f;
    const float w = xg_glyph(nullptr, ImVec2(0, 0), g) + xs(7) + xg_tsize(label, fs).x;
    xg_hint(dl, right - w, y_top, g, label, alpha);
}

// Box an image (keeping aspect) into [a, a+box]; returns the drawn rect.
static void xg_image(ImDrawList* dl, const LauncherTexture& t, ImVec2 a, ImVec2 box, float alpha = 1.0f,
                     bool frame = false) {
    if (!t.id || t.w <= 0 || t.h <= 0) return;
    const float s = std::min(box.x / t.w, box.y / t.h);
    const float iw = t.w * s, ih = t.h * s;
    const ImVec2 ia(a.x + (box.x - iw) * 0.5f, a.y + (box.y - ih) * 0.5f);
    dl->AddImage((ImTextureID)(intptr_t)t.id, ia, ImVec2(ia.x + iw, ia.y + ih), ImVec2(0, 0), ImVec2(1, 1),
                 xg_a(IM_COL32_WHITE, alpha));
    if (frame) {
        dl->AddRect(ImVec2(ia.x - 1, ia.y - 1), ImVec2(ia.x + iw + 1, ia.y + ih + 1), xg_a(XG_WL, alpha), 0, 0,
                    std::max(1.0f, xs(1.0f)));
        dl->AddRect(ImVec2(ia.x - 2, ia.y - 2), ImVec2(ia.x + iw + 2, ia.y + ih + 2), xg_a(XG_OL, alpha));
    }
}

/* ---- mouse hit areas ------------------------------------------------------ */
struct XgHit { bool hov = false, click = false, rclick = false; float wheel = 0.0f; };

static XgHit xg_hit(const char* id, ImVec2 a, ImVec2 b) {
    XgHit h;
    const ImVec2 sz(b.x - a.x < 1 ? 1 : b.x - a.x, b.y - a.y < 1 ? 1 : b.y - a.y);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    // Clicks act on RELEASE: an action that starts listening for input (a
    // binding capture) would otherwise swallow the button's own release and
    // leave the UI believing the mouse is still held down.
    const bool released = ImGui::InvisibleButton(id, sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImGui::PopItemFlag();
    h.hov = ImGui::IsItemHovered();
    if (h.hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    h.click = released && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    h.rclick = released && ImGui::IsMouseReleased(ImGuiMouseButton_Right);
    h.wheel = h.hov ? ImGui::GetIO().MouseWheel : 0.0f;
    return h;
}

// Hover only moves the cursor when the mouse actually moved, so a mouse
// resting over a window never fights the pad.
static bool xg_mouse_moved() {
    // Not while a test script drives the UI, and not in the first moments
    // after the window appears (the WM placing it under a resting pointer
    // reads as motion).
    if (launcher_debug_active() || ImGui::GetTime() < 0.75) return false;
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    return (d.x != 0.0f || d.y != 0.0f) && d.x > -1e5f;
}

/* ---- navigation state ----------------------------------------------------- */
enum { XZ_MENU = 0, XZ_CONTENT, XZ_POPUP };
enum XgCmd { XC_GAME, XC_CONFIG, XC_MODS, XC_NETPLAY, XC_ASSIST, XC_CREDITS, XC_EXIT, XC_START };

struct XgState {
    int zone = XZ_MENU;
    int menu = 0;            // index into the command list (Start is last)
    int win = 0;             // Game: focused window
    int opt = 0;             // POPUP: focused option
    int opt_scroll = 0;
    int cfg_sec = 0, cfg_row = 0, cfg_scroll = 0;
    int mod_tab = 0, mod_row = 0, mod_scroll = 0;
    std::string note;        // transient help override
    double note_until = 0.0;
    bool flash_start = false;
    double flash_until = 0.0;
    // TEXT mod options are edited in a small ImGui modal.
    bool text_edit = false;
    char text_buf[RECOMP_LAUNCHER_MOD_VALUE_MAX] = {};
    std::string text_pkg, text_feat, text_opt, text_label;
    // The frame a popup opened on: the click that opened it is not a click outside it.
    int popup_frame = -1;
    // Pages with an action bar under their list (Mods, Controls).
    bool bar = false;
    int bar_btn = 0;
    // Controls page.
    int ctl_sec = 0, ctl_row = 0, ctl_scroll = 0;
    bool rename_open = false;
    char rename_buf[64] = {};
    // Remove-package confirmation.
    bool confirm_remove = false;
    std::string rm_id, rm_version, rm_name;
};
static XgState X;

static void xg_open_popup() {
    X.zone = XZ_POPUP; X.opt = 0; X.opt_scroll = 0;
    X.popup_frame = ImGui::GetFrameCount();
}

static void xg_note(const char* s) {
    X.note = s ? s : "";
    X.note_until = ImGui::GetTime() + 5.0;
}

// Set while the mouse is over a row with a right-click action (last frame).
static bool g_xg_hover_alt = false, g_xg_hover_alt_next = false;

struct XgIn { bool up = false, down = false, left = false, right = false,
              ok = false, back = false, alt = false, l1 = false, r1 = false, start = false; };

static bool xg_input_allowed(const LauncherModel* m) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive()) return false;
    if (ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
    if (m->capturing || m->hk_capturing || m->camera_capturing || automap_in_progress()) return false;
    if (g_rom_picker.active || m->setup_wizard_open || m->skip_modal_open ||
        m->bios_play_modal_open || m->defaults_modal_open)
        return false;
    if (X.text_edit || X.confirm_remove || X.rename_open) return false;
    return true;
}

static XgIn xg_read_input(const LauncherModel* m) {
    XgIn in;
    if (!xg_input_allowed(m)) return in;
    auto k = [](ImGuiKey key, bool rep) { return ImGui::IsKeyPressed(key, rep); };
    in.up    = k(ImGuiKey_UpArrow, true);
    in.down  = k(ImGuiKey_DownArrow, true);
    in.left  = k(ImGuiKey_LeftArrow, true);
    in.right = k(ImGuiKey_RightArrow, true);
    in.ok    = k(ImGuiKey_Enter, false) || k(ImGuiKey_KeypadEnter, false);
    in.back  = k(ImGuiKey_Escape, false) || k(ImGuiKey_Backspace, false);
    in.l1    = k(ImGuiKey_Q, false) || k(ImGuiKey_PageUp, false);
    in.r1    = k(ImGuiKey_E, false) || k(ImGuiKey_PageDown, false);
    in.start = k(ImGuiKey_Space, false);
    in.alt   = k(ImGuiKey_Tab, false);
    // Right mouse button goes back too, except over a row whose right click
    // has its own meaning (an alternate binding).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !g_xg_hover_alt) in.back = true;
    const bool any_key = in.up || in.down || in.left || in.right || in.ok || in.back ||
                         in.l1 || in.r1 || in.start || in.alt;
    if (s_pad_nav_armed) {
        in.up    |= k(ImGuiKey_GamepadDpadUp, true) || k(ImGuiKey_GamepadLStickUp, true);
        in.down  |= k(ImGuiKey_GamepadDpadDown, true) || k(ImGuiKey_GamepadLStickDown, true);
        in.left  |= k(ImGuiKey_GamepadDpadLeft, true) || k(ImGuiKey_GamepadLStickLeft, true);
        in.right |= k(ImGuiKey_GamepadDpadRight, true) || k(ImGuiKey_GamepadLStickRight, true);
        in.ok    |= k(ImGuiKey_GamepadFaceDown, false);
        in.back  |= k(ImGuiKey_GamepadFaceRight, false);
        in.l1    |= k(ImGuiKey_GamepadL1, false);
        in.r1    |= k(ImGuiKey_GamepadR1, false);
        in.start |= k(ImGuiKey_GamepadStart, false);
        in.alt   |= k(ImGuiKey_GamepadFaceUp, false);
    }
    // The legend follows whatever was used last.
    if (in.up || in.down || in.left || in.right || in.ok || in.back || in.l1 || in.r1 || in.start || in.alt)
        g_xg_last_pad = !any_key;
    return in;
}

/* ---- generic option rows (popups and Config lists) ------------------------ */
enum XgRowKind { XR_VALUE, XR_ACTION, XR_INFO, XR_HEADER };

struct XgRow {
    std::string label;
    std::string value;
    std::string help;
    int kind = XR_VALUE;
    bool disabled = false;
    std::function<void(int)> change;   // ←→ (dir -1/+1); XR_VALUE
    std::function<void()> act;         // ✕; XR_ACTION (XR_VALUE: steps +1)
    std::function<void()> alt;         // △ / right click (optional)
};

static void xg_row_ok(XgRow& r) {
    if (r.disabled) return;
    if (r.kind == XR_ACTION && r.act) r.act();
    else if (r.kind == XR_VALUE && r.change) r.change(+1);
}

// Step a model setting that only exposes "cycle forward": walk the ring once
// to count it, then go round to the previous entry.
static void xg_cycle(int dir, const std::function<void()>& fwd,
                     const std::function<std::string()>& label) {
    if (dir >= 0) { fwd(); return; }
    const std::string start = label();
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        fwd(); ++n;
        if (label() == start) break;
    }
    for (int i = 0; i < n - 1; ++i) fwd();
}

static std::string xg_onoff(bool on) { return on ? ui_text("On") : ui_text("Off"); }

/* ---- PLAY ------------------------------------------------------------------ */
static const char* xg_play_block_reason(const LauncherModel* m) {
    if (launcher_model_can_launch(m) || launcher_model_bios_blocks_play(m)) return nullptr;
    if (m->has_bios && !m->setup_bios_ok) return "Select a valid BIOS first.";
    if (!m->rom_present || strcmp(m->rom_size, "--") == 0) return "Select a disc first.";
    if (m->profile && m->profile->verify.mode == 1 &&
        (m->verify.verdict == 0 || m->verify.verdict == 3))
        return "Select a verified disc first.";
    if (m->setup_preparing) return "Wait for the current setup job to finish.";
    return "Select a valid disc first.";
}

static void xg_play(LauncherModel* m) {
    const bool can_play = launcher_model_can_launch(m);
    const bool bios_block = launcher_model_bios_blocks_play(m);
    X.flash_start = true;
    X.flash_until = ImGui::GetTime() + 0.6;
    if (bios_block) {
        launcher_model_bios_play_prompt(m);
    } else if (can_play) {
        if (!launcher_model_prepare_rom_patch(m)) {
            if (m->rom_patch_supported && m->s.rom_patch_enabled)
                launcher_model_set_view(m, LNG_VIEW_MODS);
        } else if (mod_commit_launch(m)) {
            m->action = LNG_ACTION_LAUNCH;
        } else {
            xg_note(m->mod_status);
        }
    } else if (m->setup_wizard_supported) {
        m->setup_wizard_open = true;
    } else {
        xg_note(ui_text(xg_play_block_reason(m)));
    }
}

/* ---- Game page model -------------------------------------------------------- */
struct XgGameWin { int kind; int index; };   // kind: 0 disc, 1 player, 2 card
enum { XGW_DISC = 0, XGW_PLAYER, XGW_CARD };

static std::vector<XgGameWin> xg_game_windows(const LauncherModel* m) {
    std::vector<XgGameWin> w;
    w.push_back({XGW_DISC, 0});
    if (!m->lock_device) {
        int n = launcher_model_visible_player_count(m);
        if (n < 1) n = 1;
        if (n > LNG_MAX_PLAYERS) n = LNG_MAX_PLAYERS;
        for (int p = 0; p < n; ++p) w.push_back({XGW_PLAYER, p});
    }
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    int slots = prof ? prof->save.slots : 2;
    if (slots < 1) slots = 1;
    if (slots > 2) slots = 2;
    for (int s = 0; s < slots; ++s) w.push_back({XGW_CARD, s});
    return w;
}

static const char* xg_verdict_text(const LauncherModel* m, ImU32* colr) {
    const bool pending = !m->rom_present;
    if (pending) { *colr = XG_DIS; return "No disc selected"; }
    switch (m->verify.verdict) {
        case 1: *colr = XG_GOOD; return "Disc verified";
        case 2: *colr = XG_WARN; return "Disc verified (warnings)";
        case 3: *colr = XG_BAD;  return "Disc verification failed";
        default: *colr = XG_WARN; return "Disc not recognized";
    }
}

static std::string xg_disc_label(const LauncherModel* m) {
    if (launcher_model_disc_count(m) > 1)
        return launcher_model_disc_label(m, launcher_model_disc_selected(m));
    return ui_text("Disc");
}

static void xg_browse_disc(LauncherModel* m) {
    const char* noun = m->rom_noun ? m->rom_noun : "Disc";
    const int disc_no = launcher_model_disc_count(m) > 1
                            ? launcher_model_disc_number(m, launcher_model_disc_selected(m)) : 0;
    char title[64];
    if (disc_no > 0) snprintf(title, sizeof(title), "Select %s %d", noun, disc_no);
    else snprintf(title, sizeof(title), "Select %s%s", noun, m->import_sbi_cb ? " / SBI" : "");
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->rom_filter.patterns && prof->rom_filter.pattern_count > 0)
        request_rom_picker(m, title, prof->rom_filter.patterns, prof->rom_filter.pattern_count,
                           prof->rom_filter.desc, false);
    else
        request_rom_picker(m, title, NULL, 0, NULL, false);
}

static const char* xg_basename(const char* p) {
    if (!p) return "";
    const char* b = p;
    for (const char* s = p; *s; ++s) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

// Pad modes for player p (custom profile list, else PSX Analog / D-Pad).
struct XgMode { int mode; const char* label; };
static std::vector<XgMode> xg_pad_modes(const LauncherModel* m, int p) {
    std::vector<XgMode> v;
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->controller.modes && prof->controller.mode_count > 0) {
        for (int i = 0; i < prof->controller.mode_count && i < 8; ++i)
            v.push_back({prof->controller.modes[i].mode, prof->controller.modes[i].label});
    } else {
        if (m->s.player_src[p] != 1) v.push_back({1, "Analog"});   // no sticks on a keyboard
        v.push_back({2, "D-Pad"});
    }
    return v;
}

static const char* xg_pad_mode_label(const LauncherModel* m, int p) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    if (prof && prof->controller.modes && prof->controller.mode_count > 0) {
        for (int i = 0; i < prof->controller.mode_count; ++i)
            if (prof->controller.modes[i].mode == m->s.pad_mode[p]) return prof->controller.modes[i].label;
        return "";
    }
    return m->s.pad_mode[p] == 1 ? "Analog" : m->s.pad_mode[p] == 2 ? "D-Pad" : "Hybrid";
}

// Input sources for player p — same list, order and rules as the legacy combo
// (draw_source_selectables): None, Keyboard (+ Mouse), known + live pads, pads
// claimed by another player excluded.
struct XgSrc { std::string label; int kind; char guid[40]; char name[64]; uint32_t id; bool selected; };
static std::vector<XgSrc> xg_sources(const LauncherModel* m, int p) {
    std::vector<XgSrc> v;
    auto add = [&](const char* label, int kind, bool sel) {
        XgSrc s{}; s.label = label; s.kind = kind; s.selected = sel; v.push_back(s);
    };
    add(ui_text("None"), 0, m->s.player_src[p] == 0);
    if (m->has_mouse_controls && p == 0) {
        add(ui_text("Keyboard + Mouse"), 2, m->s.player_src[p] == 1 && m->s.mouse_enabled);
        add(ui_text("Keyboard"), 3, m->s.player_src[p] == 1 && !m->s.mouse_enabled);
    } else {
        add(ui_text("Keyboard"), 1, m->s.player_src[p] == 1);
    }
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const bool psx = prof && prof->id && !strcmp(prof->id, "psx");
    struct Pad { char guid[40]; char name[64]; uint32_t id; bool live; };
    std::vector<Pad> pads;
    auto push = [&](const char* guid, const char* name, uint32_t id, bool live) {
        if (!guid || !guid[0]) return;
        for (const Pad& q : pads) if (!strcmp(q.guid, guid)) return;
        Pad q{};
        snprintf(q.guid, sizeof(q.guid), "%s", guid);
        snprintf(q.name, sizeof(q.name), "%s",
                 (name && name[0] && strcmp(name, "Gamepad") != 0) ? name : "Controller");
        q.id = id; q.live = live;
        pads.push_back(q);
    };
    if (psx) {
        const int known = launcher_binds_psx_known_count();
        for (int i = 0; i < known; ++i) {
            char guid[40] = {}, name[64] = {};
            if (!launcher_binds_psx_known_at(i, guid, (int)sizeof(guid), name, (int)sizeof(name))) continue;
            uint32_t id = 0; bool live = false;
            const bool custom = launcher_binds_psx_name_is_custom(guid) != 0;
            for (int j = 0; j < g_pad_count; ++j)
                if (g_pads[j].guid[0] && !strcmp(g_pads[j].guid, guid)) {
                    live = true; id = g_pads[j].id;
                    if (!custom && g_pads[j].name[0] && strcmp(g_pads[j].name, "Gamepad") != 0)
                        snprintf(name, sizeof(name), "%s", g_pads[j].name);
                    break;
                }
            push(guid, name, id, live);
        }
        if (m->s.player_src[p] == 2 && m->s.player_gamepad_guid[p][0]) {
            bool live = false; uint32_t id = m->player_pad_id[p];
            for (int j = 0; j < g_pad_count; ++j)
                if (g_pads[j].guid[0] && !strcmp(g_pads[j].guid, m->s.player_gamepad_guid[p])) {
                    live = true; id = g_pads[j].id; break;
                }
            push(m->s.player_gamepad_guid[p], m->player_pad_name[p], id, live);
        }
    }
    for (int i = 0; i < g_pad_count; ++i) push(g_pads[i].guid, g_pads[i].name, g_pads[i].id, true);
    const int claim_n = launcher_model_visible_player_count(m);
    for (const Pad& q : pads) {
        bool claimed = false;
        for (int o = 0; o < claim_n; ++o)
            if (o != p && m->s.player_src[o] == 2 && m->s.player_gamepad_guid[o][0] &&
                !strcmp(m->s.player_gamepad_guid[o], q.guid)) { claimed = true; break; }
        if (claimed) continue;
        XgSrc s{};
        s.label = q.live ? std::string(q.name) : std::string(q.name) + " " + ui_text("(disconnected)");
        s.kind = 4;
        snprintf(s.guid, sizeof(s.guid), "%s", q.guid);
        snprintf(s.name, sizeof(s.name), "%s", q.name);
        s.id = q.id;
        s.selected = m->s.player_src[p] == 2 && m->s.player_gamepad_guid[p][0] &&
                     !strcmp(m->s.player_gamepad_guid[p], q.guid);
        v.push_back(s);
    }
    return v;
}

static void xg_apply_source(LauncherModel* m, int p, const XgSrc& s) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const bool psx = prof && prof->id && !strcmp(prof->id, "psx");
    switch (s.kind) {
        case 0: launcher_model_set_source(m, p, 0, 0, nullptr, nullptr); if (psx) launcher_binds_refresh(m); break;
        case 1: launcher_model_set_source(m, p, 1, 0, nullptr, nullptr); if (psx) launcher_binds_refresh(m); break;
        case 2: launcher_model_set_mouse_source(m, 1); break;
        case 3: launcher_model_set_mouse_source(m, 0); break;
        default:
            launcher_model_set_source(m, p, 2, s.id, s.name, s.guid);
            if (psx) { launcher_binds_apply_psx_pad_profile(m, p); launcher_binds_refresh(m); }
            break;
    }
}

static std::vector<XgRow> xg_game_options(LauncherModel* m, const XgGameWin& w) {
    std::vector<XgRow> rows;
    if (w.kind == XGW_DISC) {
        if (launcher_model_disc_count(m) > 1) {
            XgRow r; r.label = ui_text("Disc");
            r.value = xg_disc_label(m);
            r.help = ui_text("Which disc of the set to boot.");
            r.change = [m](int dir) {
                const int n = launcher_model_disc_count(m);
                launcher_model_select_disc(m, (launcher_model_disc_selected(m) + dir + n) % n);
            };
            rows.push_back(r);
        }
        XgRow b; b.kind = XR_ACTION;
        b.label = std::string(ui_text("Browse for disc")) + (m->import_sbi_cb ? " / SBI" : "") + "\xE2\x80\xA6";
        b.help = ui_text("Point this disc at an image on this computer (.cue / .bin). An .sbi file is imported for the selected disc.");
        b.act = [m]() { xg_browse_disc(m); };
        rows.push_back(b);
        if (m->setup_wizard_supported) {
            XgRow s; s.kind = XR_ACTION; s.label = ui_text("Setup wizard\xE2\x80\xA6");
            s.help = ui_text("Locate the discs and BIOS step by step.");
            s.act = [m]() { m->setup_wizard_open = true; };
            rows.push_back(s);
        }
    } else if (w.kind == XGW_PLAYER) {
        const int p = w.index;
        if (m->pad_mode_supported && m->pad_mode_selectable) {
            XgRow r; r.label = ui_text("Pad");
            r.value = ui_text(xg_pad_mode_label(m, p));
            r.help = ui_text("Analog uses the sticks (DualShock); D-Pad is the original digital pad.");
            r.change = [m, p](int dir) {
                const auto modes = xg_pad_modes(m, p);
                if (modes.empty()) return;
                int cur = 0;
                for (size_t i = 0; i < modes.size(); ++i) if (modes[i].mode == m->s.pad_mode[p]) cur = (int)i;
                const int n = (int)modes.size();
                launcher_model_set_pad_mode(m, p, modes[(cur + dir + n) % n].mode);
            };
            rows.push_back(r);
        }
        if (!m->lock_device) {
            XgRow r; r.label = ui_text("Device");
            r.value = launcher_model_player_src_label(m, p);
            r.help = ui_text("Which keyboard or controller plays as this player.");
            r.change = [m, p](int dir) {
                const auto src = xg_sources(m, p);
                if (src.empty()) return;
                int cur = 0;
                for (size_t i = 0; i < src.size(); ++i) if (src[i].selected) cur = (int)i;
                const int n = (int)src.size();
                xg_apply_source(m, p, src[(cur + dir + n) % n]);
            };
            rows.push_back(r);
        }
        XgRow c; c.kind = XR_ACTION; c.label = ui_text("Configure buttons\xE2\x80\xA6");
        c.help = ui_text("Open the button mapping for this player.");
        c.act = [m, p]() {
            launcher_model_open_config(m, p);
            X.zone = XZ_CONTENT; X.ctl_sec = 0; X.ctl_row = 0; X.ctl_scroll = 0; X.bar = false;
        };
        rows.push_back(c);
    } else {
        const int s = w.index;
        static const char* kCardPatterns[] = { "*.mcd", "*.mcr", "*.mc" };
        XgRow e; e.label = ui_text("Enabled");
        e.value = xg_onoff(m->s.memcard_enabled[s] != 0);
        e.help = ui_text("A disabled slot is empty in the game, like pulling the card out.");
        e.change = [m, s](int) { launcher_model_toggle_memcard(m, s); };
        rows.push_back(e);
        XgRow b; b.kind = XR_ACTION; b.label = ui_text("Browse for card\xE2\x80\xA6");
        b.help = ui_text("Use an existing memory card image (.mcd .mcr .mc).");
        b.act = [m, s]() {
            char buf[512];
            if (launcher_pick_file("Select memory card image", kCardPatterns, 3,
                                   "PS1 memory card (.mcd .mcr .mc)", buf, sizeof(buf)))
                launcher_model_set_memcard_path(m, s, buf);
        };
        rows.push_back(b);
        XgRow n; n.kind = XR_ACTION; n.label = ui_text("New card\xE2\x80\xA6");
        n.help = ui_text("Create a freshly formatted blank card and use it.");
        n.act = [m, s]() {
            char buf[512];
            if (launcher_pick_save_file("Create new memory card", kCardPatterns, 3,
                                        "PS1 memory card (.mcd)", buf, sizeof(buf)))
                launcher_model_new_memcard(m, s, buf);
        };
        rows.push_back(n);
    }
    return rows;
}

/* ---- Config page model ----------------------------------------------------- */
struct XgSection { std::string name; std::vector<XgRow> rows; };

static XgRow xg_toggle_row(const char* label, bool on, const char* help, std::function<void()> flip) {
    XgRow r; r.label = ui_text(label); r.value = xg_onoff(on); r.help = help ? ui_text(help) : "";
    r.change = [flip](int) { flip(); };
    return r;
}

static XgRow xg_cycle_row(const char* label, std::function<std::string()> lab, const char* help,
                          std::function<void()> fwd) {
    XgRow r; r.label = ui_text(label); r.value = lab(); r.help = help ? ui_text(help) : "";
    r.change = [fwd, lab](int dir) { xg_cycle(dir, fwd, lab); };
    return r;
}

static std::vector<XgSection> xg_config_sections(LauncherModel* m) {
    std::vector<XgSection> secs;
    const SystemProfile* prof = (const SystemProfile*)m->profile;

    /* Display: the window, timing and presentation. */
    {
        XgSection s; s.name = ui_text("Display");
        if (m->has_window_size)
            s.rows.push_back(xg_cycle_row("Window size", [m] { return std::string(launcher_model_window_size_label(m)); },
                "Size of the window when not fullscreen. The height follows the aspect ratio.",
                [m] { launcher_model_cycle_window_size(m); }));
        else {
            XgRow r; r.label = ui_text("Window scale"); r.value = launcher_model_scale_label(m);
            r.change = [m](int dir) {
                int v = m->s.window_scale + dir;
                if (v < 1) v = LNG_WINDOW_SCALE_MAX;
                if (v > LNG_WINDOW_SCALE_MAX) v = 1;
                launcher_model_set_scale(m, v);
            };
            s.rows.push_back(r);
        }
        {
            XgRow r; r.label = ui_text("Fullscreen"); r.value = ui_text(launcher_model_fullscreen_label(m));
            r.help = ui_text("Borderless covers the screen as a window; Exclusive takes over the display.");
            r.change = [m](int dir) { launcher_model_set_fullscreen(m, (m->s.fullscreen + dir + 3) % 3); };
            s.rows.push_back(r);
        }
        if (has_display_aspect_row(m)) {
            if (!m->aspect_mask && !(m->aspect_labels && m->num_aspect_labels > 0)) {
                s.rows.push_back(xg_toggle_row("Widescreen 16:9", m->s.widescreen != 0, nullptr,
                                               [m] { launcher_model_toggle_widescreen(m); }));
            } else {
                XgRow r = xg_cycle_row("Aspect ratio", [m] { return std::string(launcher_model_aspect_label(m)); },
                                       nullptr, [m] { launcher_model_cycle_aspect(m); });
                if (m->aspect_setting_label && m->aspect_setting_label[0]) r.label = m->aspect_setting_label;
                r.help = (m->aspect_setting_help && m->aspect_setting_help[0])
                             ? m->aspect_setting_help
                             : ui_text("Widescreen widens the 3D view; 4:3 is the original framing.");
                s.rows.push_back(r);
            }
        }
        if (m->has_frame_interp) {
            XgRow r; r.label = ui_text("FPS"); r.value = launcher_model_fps_label(m);
            r.help = ui_text("Target frame rate. Original keeps the game's own 30 FPS; higher rates interpolate.");
            r.change = [m](int dir) {
                const int n = launcher_model_fps_count();
                launcher_model_set_fps(m, (launcher_model_fps_index(m) + dir + n) % n);
            };
            s.rows.push_back(r);
        }
        if (m->has_vsync) {
            XgRow r; r.label = ui_text("VSync"); r.value = ui_text(launcher_model_vsync_label(m));
            r.help = ui_text("On: no tearing. Off: lowest latency, may tear. Adaptive: vsync while the game keeps up.");
            r.change = [m](int dir) {
                static const int order[] = {RECOMP_LAUNCHER_VSYNC_ON, RECOMP_LAUNCHER_VSYNC_OFF, RECOMP_LAUNCHER_VSYNC_ADAPTIVE};
                int cur = 0;
                for (int i = 0; i < 3; ++i) if (order[i] == m->s.vsync) cur = i;
                launcher_model_set_vsync(m, order[(cur + dir + 3) % 3]);
            };
            s.rows.push_back(r);
        }
        if (m->has_renderer) {
            XgRow r; r.label = ui_text("Renderer"); r.value = launcher_model_renderer_label(m);
            r.change = [m](int dir) {
                const int n = launcher_model_renderer_count(m);
                if (n > 0) launcher_model_set_renderer(m, (m->s.renderer + dir + n) % n);
            };
            s.rows.push_back(r);
        }
        if (m->has_screen_kind)
            s.rows.push_back(xg_cycle_row("Screen model", [m] { return std::string(ui_text(launcher_model_screen_kind_label(m))); },
                "Display simulation applied to the final image. Raw shows the pixels as they are.",
                [m] { launcher_model_cycle_screen_kind(m); }));
        if (m->has_skip_fmv)
            s.rows.push_back(xg_toggle_row("Skip FMVs", m->s.auto_skip_fmv != 0,
                "Skip full-motion video cutscenes automatically.", [m] { launcher_model_toggle_skip_fmv(m); }));
        if (m->has_fast_map_load)
            s.rows.push_back(xg_toggle_row("Fast map load", m->s.fast_map_load != 0,
                "Shorten map-loading waits while keeping gameplay and fades at their normal speed.",
                [m] { m->s.fast_map_load = m->s.fast_map_load ? 0 : 1; }));
        if (m->has_run_ahead) {
            XgRow r; r.label = ui_text("Run-ahead");
            char b[16]; if (m->s.run_ahead) snprintf(b, sizeof(b), "%d", m->s.run_ahead); else snprintf(b, sizeof(b), "%s", ui_text("Off"));
            r.value = b;
            r.help = ui_text("Emulate ahead to hide the game's own input lag. Each frame costs one extra emulated frame.");
            r.change = [m](int dir) {
                const int n = RECOMP_LAUNCHER_RUN_AHEAD_MAX + 1;
                launcher_model_set_run_ahead(m, (m->s.run_ahead + dir + n) % n);
            };
            s.rows.push_back(r);
        }
        secs.push_back(s);
    }

    /* Image: resolution, smoothing and the original look. */
    {
        XgSection s; s.name = ui_text("Image");
        if (m->has_supersampling)
            s.rows.push_back(xg_cycle_row("Supersampling", [m] { return std::string(launcher_model_supersampling_label(m)); },
                "Render at a higher internal resolution, then downscale. Sharper edges at the cost of GPU time.",
                [m] { launcher_model_cycle_supersampling(m); }));
        if (m->has_antialiasing) {
            if (launcher_model_has_aa_modes(m)) {
                XgRow r; r.label = ui_text("Antialiasing"); r.value = ui_text(launcher_model_aa_label(m));
                r.help = ui_text("Edge smoothing method.");
                r.change = [m](int dir) {
                    static const int order[] = {0, 1, 2, 3, 4, 5};
                    int cur = 0;
                    for (int i = 0; i < 6; ++i) if (order[i] == m->s.antialiasing) cur = i;
                    static int s_factor_before_taa = 0;
                    const int was = m->s.antialiasing;
                    m->s.antialiasing = order[(cur + dir + 6) % 6];
                    if (m->s.antialiasing == 3 && m->s.antialiasing_factor > 4) {
                        s_factor_before_taa = m->s.antialiasing_factor;   // TAA tops out at 4x
                        m->s.antialiasing_factor = 4;
                    } else if (was == 3 && s_factor_before_taa > 0) {
                        m->s.antialiasing_factor = s_factor_before_taa;  // give it back on the way out
                        s_factor_before_taa = 0;
                    }
                };
                s.rows.push_back(r);
                XgRow f; f.label = ui_text("AA multiplier");
                char b[16]; snprintf(b, sizeof(b), "%dx", m->s.antialiasing_factor); f.value = b;
                f.disabled = m->s.antialiasing == 0;
                f.help = f.disabled ? ui_text("Turn antialiasing on to change its strength.")
                                    : ui_text("Strength of the antialiasing pass.");
                f.change = [m](int dir) {
                    static const int factors[] = {1, 2, 4, 8, 16};
                    const int n = m->s.antialiasing == 3 ? 3 : 5;
                    int cur = 0;
                    for (int i = 0; i < n; ++i) if (factors[i] == m->s.antialiasing_factor) cur = i;
                    m->s.antialiasing_factor = factors[(cur + dir + n) % n];
                };
                s.rows.push_back(f);
            } else {
                s.rows.push_back(xg_cycle_row("Antialiasing", [m] { return std::string(ui_text(launcher_model_aa_label(m))); },
                                              nullptr, [m] { launcher_model_cycle_aa(m); }));
            }
        }
        if (m->has_anisotropic_filtering)
            s.rows.push_back(xg_cycle_row("Anisotropic filtering", [m] { return std::string(launcher_model_anisotropic_filtering_label(m)); },
                "Sharper textures on floors and walls seen at an angle. Sprites and menus are unchanged.",
                [m] { launcher_model_cycle_anisotropic_filtering(m); }));
        if (m->has_sharp_filter)
            s.rows.push_back(xg_cycle_row("Scaling filter", [m] { return std::string(ui_text(launcher_model_scaling_filter_label(m))); },
                                          nullptr, [m] { launcher_model_cycle_scaling_filter(m); }));
        else if (m->has_texture_filter)
            s.rows.push_back(xg_cycle_row("Texture filtering", [m] { return std::string(ui_text(launcher_model_texture_filter_label(m))); },
                "Nearest keeps the original pixels; Bilinear smooths the 3D textures.",
                [m] { launcher_model_toggle_texture_filter(m); }));
        if (m->has_sprite_filter)
            s.rows.push_back(xg_cycle_row("Sprite filtering", [m] { return std::string(ui_text(launcher_model_sprite_filter_label(m))); },
                "Same as texture filtering, for characters and 2D sprites.",
                [m] { launcher_model_toggle_sprite_filter(m); }));
        if (m->has_dithering)
            s.rows.push_back(xg_toggle_row("Dithering", m->s.dither_force_off == 0,
                "The PlayStation's dither pattern over 15-bit colour. Off gives a smoother, less grainy look.",
                [m] { launcher_model_toggle_dithering(m); }));
        if (m->has_frame_blend)
            s.rows.push_back(xg_toggle_row("Frame blending", m->s.frame_blend != 0,
                "Average each frame with the previous one, as a CRT would.", [m] { launcher_model_toggle_frame_blend(m); }));
        if (m->has_geometry_precision)
            s.rows.push_back(xg_toggle_row("Perspective textures", m->s.perspective_texturing != 0,
                "Stops large floors and walls from warping as the camera moves.",
                [m] { launcher_model_toggle_perspective_texturing(m); }));
        if (!s.rows.empty()) secs.push_back(s);
    }

    /* Audio. */
    {
        XgSection s; s.name = ui_text("Audio");
        if (!(prof && prof->hide_audio_freq))
            s.rows.push_back(xg_cycle_row("Sample rate", [m] { return std::string(launcher_model_freq_label(m)); },
                                          "Output sample rate.", [m] { launcher_model_cycle_freq(m); }));
        {
            XgRow r; r.label = ui_text("Volume");
            char b[16]; snprintf(b, sizeof(b), "%d%%", m->s.volume); r.value = b;
            r.help = ui_text("Master volume. Keypad + and - change it while playing.");
            r.change = [m](int dir) { launcher_model_volume_delta(m, dir * 5); };
            s.rows.push_back(r);
        }
        if (m->has_spu_hq)
            s.rows.push_back(xg_toggle_row("High-quality SPU", m->s.spu_hq != 0,
                "Higher-accuracy sound emulation.", [m] { launcher_model_toggle_spu_hq(m); }));
        if (m->num_languages > 0)
            s.rows.push_back(xg_cycle_row("Language", [m] { return std::string(launcher_model_language_label(m)); },
                                          nullptr, [m] { launcher_model_cycle_language(m); }));
        secs.push_back(s);
    }

    /* Input (multitap / stylus), only when the game has any of it. */
    if (launcher_model_multitap_available(m) || launcher_model_multitap_analog_available(m) ||
        launcher_model_virtual_stylus_available(m)) {
        XgSection s; s.name = ui_text("Input");
        if (launcher_model_multitap_available(m))
            s.rows.push_back(xg_toggle_row("Multitap", launcher_model_multitap_enabled(m) != 0,
                "Enable the SCPH-1070 multitap for 3+ player seats.", [m] { launcher_model_toggle_multitap(m); }));
        if (launcher_model_multitap_analog_available(m))
            s.rows.push_back(xg_toggle_row("Multitap analog (hack)", launcher_model_multitap_analog_enabled(m) != 0,
                "Allow DualShock sticks on multitap seats (not faithful).", [m] { launcher_model_toggle_multitap_analog(m); }));
        if (launcher_model_virtual_stylus_available(m))
            s.rows.push_back(xg_toggle_row("Virtual Stylus", launcher_model_virtual_stylus_enabled(m) != 0, nullptr,
                                           [m] { launcher_model_toggle_virtual_stylus(m); }));
        secs.push_back(s);
    }

    /* System: BIOS and the build jobs. */
    if (m->has_bios) {
        XgSection s; s.name = ui_text("System");
        const bool has_pick = m->s.bios_path[0] != 0;
        XgRow b; b.kind = XR_ACTION; b.label = ui_text("BIOS");
        b.value = has_pick ? xg_basename(m->s.bios_path) : "OpenBIOS";
        b.help = has_pick ? std::string(m->s.bios_path) + "\n" + ui_text("{OK} to pick another BIOS file.")
                          : ui_text("The bundled OpenBIOS is in use. {OK} to pick a BIOS file from your own console.");
        b.act = [m]() { request_bios_picker(m, "Select BIOS file", false); };
        s.rows.push_back(b);
        XgRow o; o.kind = XR_ACTION; o.label = ui_text("Use OpenBIOS");
        o.disabled = !has_pick;
        o.help = has_pick ? ui_text("Switch back to the bundled OpenBIOS (no rebuild).") : ui_text("OpenBIOS is already selected.");
        o.act = [m]() { launcher_model_request_bios_path(m, ""); };
        s.rows.push_back(o);
        const SystemProfile* p2 = prof;
        if (p2 && p2->id && !strcmp(p2->id, "psx") && m->pgo_optimize_with_progress_cb) {
            XgRow g; g.kind = XR_ACTION; g.label = ui_text("Optimize FMV playback\xE2\x80\xA6");
            g.disabled = !(m->rom_present && m->rom_full[0] && strcmp(m->rom_size, "--") != 0 && !m->setup_preparing);
            g.help = ui_text("Train this PC on the intro FMV, then rebuild with profile-guided optimization (about 3 minutes).");
            g.act = [m]() { launcher_model_request_pgo_optimize(m); };
            s.rows.push_back(g);
        }
        secs.push_back(s);
    }

    /* Hotkeys: each row rebinds on ✕. */
    {
        XgSection s; s.name = ui_text("Hotkeys");
        uint32_t mask = prof ? prof->hotkeys_mask : LNG_HOTKEYS_ALL;
        if (m->has_solar_sensor) mask |= LNG_HOTKEYS_SOLAR;
        for (int h = 0; h < LNG_HK_COUNT; ++h) {
            if (!(mask & (1u << h))) continue;
            XgRow r; r.kind = XR_ACTION; r.label = ui_text(launcher_hotkey_name((LngHotkey)h));
            const bool cap = m->hk_capturing && m->capture_hk == (LngHotkey)h;
            r.value = cap ? ui_text("[ press a key ]") : m->hotkeys[h][0] ? m->hotkeys[h] : ui_text("(unbound)");
            r.help = ui_text("{OK}, then press the new key. Esc cancels; Backspace clears the binding.");
            r.act = [m, h]() { launcher_model_begin_hk_capture(m, (LngHotkey)h); };
            s.rows.push_back(r);
        }
        if (!s.rows.empty()) secs.push_back(s);
    }

    /* Launcher. */
    {
        XgSection s; s.name = ui_text("Launcher");
        s.rows.push_back(xg_toggle_row("Skip launcher on boot", m->s.skip_launcher != 0,
            "Start the game directly next time. Hold the launcher key at boot to come back here.",
            [m] { launcher_model_request_skip_toggle(m); }));
        if (launcher_model_can_restore_defaults(m)) {
            XgRow r; r.kind = XR_ACTION; r.label = ui_text("Restore defaults\xE2\x80\xA6");
            r.help = ui_text("Reset display, audio, controller and launcher choices. Your disc and saves are not changed.");
            r.act = [m]() { launcher_model_request_restore_defaults(m); };
            s.rows.push_back(r);
        }
        secs.push_back(s);
    }
    return secs;
}

/* ---- Mods page model -------------------------------------------------------- */
static bool xg_mods_feature_provider(const LauncherModel* m) {
    const auto* mods = m->mods;
    return mods && mods->feature_count && mods->feature_get && mods->feature_option_get &&
           mods->feature_enable && mods->feature_set_option;
}

struct XgModRow {
    int kind = 0;        // 0 group header, 1 feature, 2 action, 3 package, 4 diagnostics
    std::string label;
    int index = -1;      // provider index (feature or package)
    RecompLauncherCModFeature f{};
    RecompLauncherCModPackage pkg{};
    std::string help;
    std::function<void()> act;
};

static std::vector<XgModRow> xg_mod_rows(LauncherModel* m, int tab) {
    std::vector<XgModRow> rows;
    const auto* mods = m->mods;
    if (!mods) return rows;
    if (tab == 0 && xg_mods_feature_provider(m)) {
        const int count = mods->feature_count(mods->ctx);
        std::vector<ModFeatureListItem> items;
        for (int i = 0; i < count; ++i) {
            ModFeatureListItem it{};
            it.index = i;
            if (!mods->feature_get(mods->ctx, i, &it.feature)) continue;
            if (it.feature.hidden && !it.feature.enabled) continue;
            items.push_back(it);
        }
        std::stable_sort(items.begin(), items.end(), mod_feature_less);
        std::string last = "\x01";
        for (const auto& it : items) {
            const std::string g = it.feature.group[0] ? it.feature.group : ui_text("General");
            if (g != last) { XgModRow h; h.kind = 0; h.label = g; rows.push_back(h); last = g; }
            XgModRow r; r.kind = 1; r.label = it.feature.name; r.index = it.index; r.f = it.feature;
            rows.push_back(r);
        }
        if (mods->catalog_diagnostic_count && mods->catalog_diagnostic_get &&
            mods->catalog_diagnostic_count(mods->ctx) > 0) {
            XgModRow d; d.kind = 4;
            char b[96]; snprintf(b, sizeof(b), "%d %s", mods->catalog_diagnostic_count(mods->ctx),
                                 ui_text("package(s) failed to load"));
            d.label = b;
            rows.push_back(d);
        }
    } else if (mods->package_count && mods->package_get) {
        const int count = mods->package_count(mods->ctx);
        for (int i = 0; i < count; ++i) {
            XgModRow r; r.kind = 3; r.index = i;
            if (!mods->package_get(mods->ctx, i, &r.pkg)) continue;
            r.label = r.pkg.name[0] ? r.pkg.name : r.pkg.id;
            rows.push_back(r);
        }
    }
    return rows;
}

// The always-visible action bar of the Mods page (not part of the list).
static std::vector<XgModRow> xg_mod_actions(LauncherModel* m) {
    std::vector<XgModRow> rows;
    const auto* mods = m->mods;
    if (!mods) return rows;
    const int count = mods->feature_count ? mods->feature_count(mods->ctx) : 0;
    const char* ext = mods->archive_extension && mods->archive_extension[0] ? mods->archive_extension : ".psxmod";
    const char* desc = mods->archive_description && mods->archive_description[0]
                           ? mods->archive_description : "PSXRecomp mod package (.psxmod)";
    XgModRow inst; inst.kind = 2;
    inst.label = std::string(ui_text("Install")) + " " + ext + "\xE2\x80\xA6";
    inst.help = ui_text("Install mod packages from files on this computer.");
    const std::string pattern = std::string("*") + ext;
    const std::string desc_s = desc;
    inst.act = [m, pattern, desc_s]() {
        const char* patterns[] = { pattern.c_str() };
        static char paths[64 * 1024];
        const int selected = launcher_pick_files(ui_text("Install Mod Packages"), patterns, 1,
                                                 desc_s.c_str(), paths, sizeof(paths));
        if (selected > 0) queue_mod_archive_list(m, paths, selected);
        else if (selected < 0) snprintf(m->mod_status, sizeof(m->mod_status), "No native multi-file picker is available.");
    };
    rows.push_back(inst);
    if (mods->texture_pack_add) {
        XgModRow tp; tp.kind = 2; tp.label = ui_text("Add texture pack\xE2\x80\xA6");
        tp.help = ui_text("Pick a folder of replacement textures made for Beetle PSX HW: the <name>-texture-replacements folder or the folder that contains it.");
        tp.act = [m, count]() { add_texture_pack(m, count); };
        rows.push_back(tp);
    }
    XgModRow on; on.kind = 2; on.label = ui_text("Enable all");
    on.help = ui_text("Enable every installed mod feature.");
    on.act = [m]() { set_all_mod_features(m, true); };
    rows.push_back(on);
    XgModRow off; off.kind = 2; off.label = ui_text("Disable all");
    off.help = ui_text("Disable every installed mod feature.");
    off.act = [m]() { set_all_mod_features(m, false); };
    rows.push_back(off);
    if (!xg_mods_feature_provider(m)) {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const XgModRow& r) {
            return r.label == ui_text("Enable all") || r.label == ui_text("Disable all"); }), rows.end());
    }
    return rows;
}

static std::string xg_feature_option_value(const LauncherModel* m, const RecompLauncherCModFeature& f,
                                           const RecompLauncherCModOption& o) {
    const auto* mods = m->mods;
    switch (o.type) {
        case RECOMP_MOD_OPTION_BOOLEAN: return xg_onoff(!strcmp(o.value, "true"));
        case RECOMP_MOD_OPTION_CHOICE:
            if (mods->feature_choice_get)
                for (int c = 0; c < o.choice_count; ++c) {
                    RecompLauncherCModChoice ch{};
                    if (mods->feature_choice_get(mods->ctx, f.package_id, f.id, o.id, c, &ch) &&
                        !strcmp(ch.value, o.value))
                        return ch.label[0] ? ch.label : ch.value;
                }
            return o.value;
        default: return o.value;
    }
}

static std::vector<XgRow> xg_mod_options(LauncherModel* m, const XgModRow& row) {
    std::vector<XgRow> rows;
    const auto* mods = m->mods;
    if (!mods) return rows;
    if (row.kind == 1) {
        const RecompLauncherCModFeature f = row.f;
        const std::string pkg = f.package_id, fid = f.id;
        XgRow en; en.label = ui_text("Enabled"); en.value = xg_onoff(f.enabled != 0);
        en.disabled = f.blocked && !f.enabled;
        if (en.disabled) en.help = std::string(ui_text("Unavailable while this is enabled: ")) + f.blocked_by;
        else en.help = ui_text("Turn this feature on or off. Changes apply when you press Start.");
        en.change = [m, pkg, fid, f](int) {
            if (!m->mods->feature_enable(m->mods->ctx, pkg.c_str(), fid.c_str(), f.enabled ? 0 : 1)) mod_note_error(m);
        };
        rows.push_back(en);
        for (int i = 0; i < f.option_count; ++i) {
            RecompLauncherCModOption o{};
            if (!mods->feature_option_get(mods->ctx, f.package_id, f.id, i, &o)) continue;
            XgRow r; r.label = o.label[0] ? o.label : o.id; r.help = o.description;
            r.value = xg_feature_option_value(m, f, o);
            r.disabled = o.disabled != 0;
            const std::string oid = o.id;
            auto set = [m, pkg, fid, oid](const std::string& v) {
                if (!m->mods->feature_set_option(m->mods->ctx, pkg.c_str(), fid.c_str(), oid.c_str(), v.c_str()))
                    mod_note_error(m);
            };
            if (o.type == RECOMP_MOD_OPTION_BOOLEAN) {
                const bool on = !strcmp(o.value, "true");
                r.change = [set, on](int) { set(on ? "false" : "true"); };
            } else if (o.type == RECOMP_MOD_OPTION_CHOICE) {
                const int n = o.choice_count;
                const std::string cur = o.value;
                r.change = [m, pkg, fid, oid, n, cur, set](int dir) {
                    if (n <= 0 || !m->mods->feature_choice_get) return;
                    int idx = 0;
                    std::vector<std::string> vals;
                    for (int c = 0; c < n; ++c) {
                        RecompLauncherCModChoice ch{};
                        if (m->mods->feature_choice_get(m->mods->ctx, pkg.c_str(), fid.c_str(), oid.c_str(), c, &ch)) {
                            if (cur == ch.value) idx = (int)vals.size();
                            vals.push_back(ch.value);
                        }
                    }
                    if (vals.empty()) return;
                    const int k = (int)vals.size();
                    set(vals[(idx + dir + k) % k]);
                };
            } else if (o.type == RECOMP_MOD_OPTION_INTEGER) {
                const long long mn = o.min_value, mx = o.max_value, st = o.step > 0 ? o.step : 1;
                const long long cur = strtoll(o.value, nullptr, 10);
                r.change = [set, mn, mx, st, cur](int dir) {
                    long long v = cur + dir * st;
                    if (mx > mn) { if (v < mn) v = mn; if (v > mx) v = mx; }
                    char b[32]; snprintf(b, sizeof(b), "%lld", v); set(b);
                };
            } else {
                r.kind = XR_ACTION;
                const std::string label = r.label, val = o.value;
                r.act = [pkg, fid, oid, label, val]() {
                    X.text_edit = true;
                    X.text_pkg = pkg; X.text_feat = fid; X.text_opt = oid; X.text_label = label;
                    snprintf(X.text_buf, sizeof(X.text_buf), "%s", val.c_str());
                };
            }
            rows.push_back(r);
        }
        if (mods->feature_resource_count && mods->feature_resource_get && mods->feature_resource_set_path) {
            const int nres = mods->feature_resource_count(mods->ctx, f.package_id, f.id);
            for (int i = 0; i < nres; ++i) {
                RecompLauncherCModResource res{};
                if (!mods->feature_resource_get(mods->ctx, f.package_id, f.id, i, &res)) continue;
                XgRow r; r.kind = XR_ACTION;
                r.label = std::string(res.label[0] ? res.label : res.id) + "\xE2\x80\xA6";
                r.value = res.path[0] ? xg_basename(res.path) : ui_text("Not selected");
                r.help = std::string(res.description) + (res.status[0] ? std::string("\n") + res.status : "");
                const std::string rid = res.id, rlabel = res.label, patterns = res.file_patterns,
                                  fdesc = res.file_description, format = res.format;
                r.act = [m, pkg, fid, rid, rlabel, patterns, fdesc, format]() {
                    char path[RECOMP_LAUNCHER_MOD_PATH_MAX] = {};
                    bool ok = false;
                    if (format == "directory" || format == "folder") {
                        ok = launcher_pick_folder(rlabel.c_str(), path, sizeof(path));
                    } else {
                        std::vector<std::string> pats;
                        size_t a = 0;
                        while (a <= patterns.size()) {
                            size_t b = patterns.find(',', a);
                            if (b == std::string::npos) b = patterns.size();
                            std::string t = patterns.substr(a, b - a);
                            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
                            while (!t.empty() && t.back() == ' ') t.pop_back();
                            if (!t.empty()) pats.push_back(t);
                            a = b + 1;
                        }
                        std::vector<const char*> pp;
                        for (auto& s : pats) pp.push_back(s.c_str());
                        ok = launcher_pick_file(rlabel.c_str(), pp.empty() ? nullptr : pp.data(), (int)pp.size(),
                                                fdesc.c_str(), path, sizeof(path));
                    }
                    if (!ok) return;
                    if (m->mods->feature_resource_set_path(m->mods->ctx, pkg.c_str(), fid.c_str(), rid.c_str(), path))
                        snprintf(m->mod_status, sizeof(m->mod_status), "%s verified. Changes apply on Start.", rlabel.c_str());
                    else
                        mod_note_error(m);
                };
                rows.push_back(r);
            }
        }
        if (f.texture_pack && mods->texture_pack_remove) {
            XgRow r; r.kind = XR_ACTION; r.label = ui_text("Remove from list");
            r.help = ui_text("Forget this texture pack. The folder and its images are kept on disk.");
            r.act = [m, pkg, fid]() {
                if (m->mods->texture_pack_remove(m->mods->ctx, pkg.c_str(), fid.c_str())) {
                    snprintf(m->mod_status, sizeof(m->mod_status), "Texture pack removed. Its folder was not touched.");
                    X.zone = XZ_CONTENT;
                } else mod_note_error(m);
            };
            rows.push_back(r);
        }
        if (f.camera_controls && f.enabled) {
            XgRow r; r.kind = XR_ACTION; r.label = ui_text("Review camera bindings\xE2\x80\xA6");
            r.help = ui_text("The 3D camera controls live on Player 1's button mapping.");
            r.act = [m]() {
                launcher_model_open_config(m, 0);
                X.zone = XZ_CONTENT; X.ctl_sec = 0; X.ctl_row = 0; X.ctl_scroll = 0; X.bar = false;
            };
            rows.push_back(r);
        }
    } else if (row.kind == 3) {
        const RecompLauncherCModPackage p = row.pkg;
        const std::string pid = p.id;
        if (mods->version_count && mods->version_get && mods->select_version &&
            mods->version_count(mods->ctx, p.id) > 1) {
            XgRow r; r.label = ui_text("Version"); r.value = p.version;
            r.help = ui_text("Switch between the installed versions of this package.");
            r.change = [m, pid](int dir) {
                const auto* md = m->mods;
                const int n = md->version_count(md->ctx, pid.c_str());
                int cur = 0;
                std::vector<std::string> vs;
                for (int i = 0; i < n; ++i) {
                    RecompLauncherCModVersion v{};
                    if (md->version_get(md->ctx, pid.c_str(), i, &v)) {
                        if (v.selected) cur = (int)vs.size();
                        vs.push_back(v.version);
                    }
                }
                if (vs.empty()) return;
                const int k = (int)vs.size();
                if (!md->select_version(md->ctx, pid.c_str(), vs[(cur + dir + k) % k].c_str())) mod_note_error(m);
            };
            rows.push_back(r);
        }
        if (!xg_mods_feature_provider(m) && mods->set_enabled) {
            rows.push_back(xg_toggle_row("Enabled", p.enabled != 0, nullptr, [m, pid, p] {
                if (!m->mods->set_enabled(m->mods->ctx, pid.c_str(), p.enabled ? 0 : 1)) mod_note_error(m);
            }));
        }
        {
            XgRow info; info.kind = XR_INFO; info.label = ui_text("Version"); info.value = p.version;
            info.help = std::string(p.name[0] ? p.name : p.id) + (p.source_name[0] ? std::string("  \xC2\xB7  ") + p.source_name : "");
            rows.push_back(info);
        }
        if (mods->remove_package) {
            XgRow r; r.kind = XR_ACTION; r.label = ui_text("Uninstall\xE2\x80\xA6");
            r.disabled = !p.removable;
            r.help = p.removable
                ? ui_text("Remove this package from the launcher. Its features stop working on the next start.")
                : ui_text("This package ships with the game and cannot be uninstalled. Turn its features off in the Features tab instead.");
            const std::string ver = p.version, name = p.name[0] ? p.name : p.id;
            r.act = [pid, ver, name]() {
                X.confirm_remove = true; X.rm_id = pid; X.rm_version = ver; X.rm_name = name;
            };
            rows.push_back(r);
        }
    }
    return rows;
}

/* ---- layout ------------------------------------------------------------------ */
struct XgLayout {
    ImVec2 hdr_a, hdr_b, cmd_a, cmd_b, start_a, start_b, cont_a, cont_b, help_a, help_b, leg_a, leg_b;
    float cmd_step;          // spacing of the commands
    float cmd_off;           // top offset that centres the commands in their window
};

// Command-window metrics, shared by the layout (its height) and the drawing.
// Command menu metrics (shared by the layout and the drawing). Every entry,
// Exit included, is the same size; the orbs step down and to the right like
// the game's menus, and the selector arrow needs headroom above the first.
static float xg_cmd_fs() { return xg_fs() * 1.22f; }
static float xg_cmd_r() { return xg_cmd_fs() * 0.70f; }
static float xg_cmd_step() { return xg_cmd_r() * 2.8f; }
static float xg_cmd_dx() { return xg_cmd_r() * 0.55f; }
static float xg_cmd_head() { return xg_cmd_r() * 2.0f; }      // room above the first orb's centre (the arrow may use the border)
static float xg_cmd_top() { return xg_cmd_head(); }
static float xg_cmd_height(int pages) {
    // `pages` entries plus Exit: arrow headroom, the steps, and a bottom margin.
    return xg_cmd_head() + pages * xg_cmd_step() + xg_cmd_r() + xs(20);
}

// The left column holds the commands (sized to fit them) and, under them,
// the Start button, which takes the rest of the column. The help window
// runs the full width under both.
static XgLayout xg_layout(ImVec2 o, ImVec2 sz, int pages) {
    XgLayout L;
    const float M = xs(20), G = xs(14), left_w = xs(178), hdr_h = xs(62), leg_h = xs(44),
                help_h = xs(104), start_min = xs(150);
    L.hdr_a = ImVec2(o.x + M, o.y + M);
    L.hdr_b = ImVec2(o.x + sz.x - M, L.hdr_a.y + hdr_h);
    L.leg_b = ImVec2(o.x + sz.x - M, o.y + sz.y - M);
    L.leg_a = ImVec2(o.x + M, L.leg_b.y - leg_h);
    const float body_top = L.hdr_b.y + G, body_bot = L.leg_a.y - G;
    L.help_a = ImVec2(o.x + M, body_bot - help_h);
    L.help_b = ImVec2(o.x + sz.x - M, body_bot);
    const float col_bot = L.help_a.y - G;
    const float natural = xg_cmd_height(pages);
    const float col_h = col_bot - body_top;
    // One grid for both columns: the column splits 56/44, the commands (four
    // entries) above, Start (one) below. The Game page uses the same line, so
    // the menu lines up with the Disc window and Start with Player / Memory.
    const float cmd_h = std::max(natural, std::max(xs(120), col_h * 0.56f - G * 0.5f));
    (void)start_min;
    L.cmd_step = xg_cmd_step();
    L.cmd_off = 0.0f;   // commands sit at the top, Exit right after them
    L.cmd_a = ImVec2(o.x + M, body_top);
    L.cmd_b = ImVec2(o.x + M + left_w, body_top + cmd_h);
    L.start_a = ImVec2(o.x + M, L.cmd_b.y + G);
    L.start_b = ImVec2(o.x + M + left_w, col_bot);
    L.cont_a = ImVec2(L.cmd_b.x + G, body_top);
    L.cont_b = ImVec2(o.x + sz.x - M, col_bot);
    return L;
}

struct XgCmdItem { XgCmd cmd; const char* label; };

static std::vector<XgCmdItem> xg_commands(const LauncherModel* m) {
    std::vector<XgCmdItem> v;
    v.push_back({XC_GAME, "Game"});
    v.push_back({XC_CONFIG, "Config"});
    if (m->mods) v.push_back({XC_MODS, "Mods"});
    if (m->netplay_supported) v.push_back({XC_NETPLAY, "Netplay"});
    if (m->has_assist_tools) v.push_back({XC_ASSIST, "Assist"});
    if (m->credits_text && m->credits_text[0]) v.push_back({XC_CREDITS, "Credits"});
    v.push_back({XC_EXIT, "Exit"});
    v.push_back({XC_START, "Start"});
    return v;
}

static XgCmd xg_page_of_view(LngView v) {
    switch (v) {
        case LNG_VIEW_SETTINGS: return XC_CONFIG;
        case LNG_VIEW_MODS: return XC_MODS;
        case LNG_VIEW_ASSIST_TOOLS: return XC_ASSIST;
        case LNG_VIEW_CREDITS: return XC_CREDITS;
        default: return XC_GAME;
    }
}

static void xg_enter_command(LauncherModel* m, XgCmd c) {
    X.bar = false;
    switch (c) {
        case XC_GAME:    launcher_model_set_view(m, LNG_VIEW_DASHBOARD); X.zone = XZ_CONTENT; X.win = 0; break;
        case XC_CONFIG:  launcher_model_set_view(m, LNG_VIEW_SETTINGS); X.zone = XZ_CONTENT; X.cfg_row = 0; X.cfg_scroll = 0; break;
        case XC_MODS:    launcher_model_set_view(m, LNG_VIEW_MODS); X.zone = XZ_CONTENT; X.mod_row = 0; X.mod_scroll = 0; break;
        case XC_NETPLAY: launcher_model_set_view(m, LNG_VIEW_NETPLAY_MODE); break;
        case XC_ASSIST:  launcher_model_set_view(m, LNG_VIEW_ASSIST_TOOLS); X.zone = XZ_CONTENT; break;
        case XC_CREDITS: launcher_model_set_view(m, LNG_VIEW_CREDITS); X.zone = XZ_CONTENT; break;
        case XC_EXIT:    m->action = LNG_ACTION_QUIT; break;
        case XC_START:   xg_play(m); break;
    }
}

/* ---- rows: shared by popups and lists ------------------------------------------ */
// One selectable row: label left, value right (◀ value ▶ when selected and
// changeable). Handles its own mouse. Returns true when the mouse claimed it.
static void xg_draw_row(ImDrawList* dl, XgRow& r, ImVec2 ra, ImVec2 rb, bool sel, bool hit_enabled,
                        int& sel_index, int my_index, float fs) {
    char id[32]; snprintf(id, sizeof(id), "##xgr%d_%d", (int)ra.y, my_index);
    const XgHit hit = hit_enabled ? xg_hit(id, ra, rb) : XgHit{};
    if (hit.hov && r.alt) g_xg_hover_alt_next = true;
    if (hit.hov && xg_mouse_moved()) { sel_index = my_index; X.zone = X.zone == XZ_MENU ? XZ_CONTENT : X.zone; }
    const float h = rb.y - ra.y, al = r.disabled ? 0.42f : 1.0f;
    if (sel) dl->AddRectFilled(ImVec2(ra.x, ra.y + xs(2)), ImVec2(rb.x, rb.y - xs(2)), IM_COL32(255, 255, 255, 24), xs(3));
    const float ty = ra.y + (h - fs) * 0.5f;
    if (sel) xg_cursor(dl, ImVec2(ra.x + xs(4), ra.y + h * 0.5f));
    const float right = rb.x - xs(10);
    // Value first, so the label can be clipped short of it.
    float value_left = right;
    if (r.kind == XR_VALUE) {
        const char* ar = "\xE2\x96\xB6"; const char* alr = "\xE2\x97\x80";
        const ImVec2 vs = xg_tsize(r.value.c_str(), fs);
        if (sel && !r.disabled) {
            const float aw = xg_tsize(ar, fs).x;
            xg_text(dl, ImVec2(right - aw, ty), XG_WL, ar, fs, 0, al);
            xg_text(dl, ImVec2(right - aw - xs(10) - vs.x, ty), XG_WL, r.value.c_str(), fs, 0, al);
            const float lx = right - aw - xs(20) - vs.x - aw;
            xg_text(dl, ImVec2(lx, ty), XG_WL, alr, fs, 0, al);
            value_left = lx;
            if (hit.click && r.change) {
                const float mx = ImGui::GetIO().MousePos.x;
                r.change(mx >= lx - xs(8) && mx <= lx + aw + xs(8) ? -1 : +1);
            }
        } else {
            xg_text(dl, ImVec2(right - vs.x, ty), XG_WL, r.value.c_str(), fs, 0, al);
            value_left = right - vs.x;
            if (hit.click) sel_index = my_index;
        }
    } else {
        if (!r.value.empty()) {
            const float vfs = fs * 0.88f;
            const ImVec2 vs = xg_tsize(r.value.c_str(), vfs);
            xg_text(dl, ImVec2(right - vs.x, ra.y + (h - vfs) * 0.5f), r.kind == XR_INFO ? XG_WL : XG_DIS,
                    r.value.c_str(), vfs, 0, al);
            value_left = right - vs.x;
        }
        if (hit.click && !r.disabled) { sel_index = my_index; xg_row_ok(r); }
        if (hit.rclick && !r.disabled && r.alt) { sel_index = my_index; r.alt(); }
    }
    // A label that does not fit before its value is set a little smaller
    // (down to 80%) before anything is clipped.
    const float room_w = std::max(xs(40), value_left - xs(14) - (ra.x + xs(24)));
    float lfs = fs;
    const float lw = xg_tsize(r.label.c_str(), fs).x;
    if (lw > room_w) lfs = std::max(fs * 0.8f, fs * room_w / lw);
    dl->PushClipRect(ra, ImVec2(std::max(ra.x + xs(40), value_left - xs(14)), rb.y), true);
    xg_text(dl, ImVec2(ra.x + xs(24), ra.y + (h - lfs) * 0.5f), XG_WL, r.label.c_str(), lfs, 0, al);
    dl->PopClipRect();
}


// Mouse wheel over [a, b): whole rows to scroll (positive = down).
static int xg_wheel_rows(ImVec2 a, ImVec2 b) {
    if (X.zone == XZ_POPUP) return 0;
    const float w = ImGui::GetIO().MouseWheel;
    if (w == 0.0f || !ImGui::IsMouseHoveringRect(a, b)) return 0;
    return w > 0 ? -std::max(1, (int)(w + 0.5f)) : std::max(1, (int)(-w + 0.5f));
}

/* ---- popup (options window) ---------------------------------------------------- */
// Draws `rows` as the game's small options window and handles its input.
// Returns false when the popup should close.
static bool xg_draw_popup(ImDrawList* dl, ImVec2 a, float w, const char* title,
                          std::vector<XgRow>& rows, const XgIn& in, const XgLayout& L) {
    const float row_h = xs(44), pad = xs(14), title_h = xs(30);
    const int max_vis = 7;
    const int n = (int)rows.size();
    if (n == 0) return false;
    if (X.opt >= n) X.opt = n - 1;
    if (X.opt < 0) X.opt = 0;
    if (in.up) X.opt = (X.opt + n - 1) % n;
    if (in.down) X.opt = (X.opt + 1) % n;
    XgRow& cur = rows[X.opt];
    if (in.left && cur.kind == XR_VALUE && !cur.disabled && cur.change) cur.change(-1);
    if (in.right && cur.kind == XR_VALUE && !cur.disabled && cur.change) cur.change(+1);
    if (in.ok) xg_row_ok(cur);
    if (in.back) return false;
    const int vis = n < max_vis ? n : max_vis;
    if (X.opt < X.opt_scroll) X.opt_scroll = X.opt;
    if (X.opt >= X.opt_scroll + max_vis) X.opt_scroll = X.opt - max_vis + 1;
    const float h = pad * 2 + title_h + vis * row_h;
    if (a.y + h > L.help_b.y) a.y = L.help_b.y - h;
    if (a.x + w > L.cont_b.x) a.x = L.cont_b.x - w;
    if (a.x < L.cont_a.x) a.x = L.cont_a.x;
    const ImVec2 b(a.x + w, a.y + h);
    {   // Wheel over the popup scrolls its options; the selection follows.
        const float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0.0f && n > vis && ImGui::IsMouseHoveringRect(a, b)) {
            const int wr = wh > 0 ? -std::max(1, (int)(wh + 0.5f)) : std::max(1, (int)(-wh + 0.5f));
            X.opt_scroll = std::max(0, std::min(n - vis, X.opt_scroll + wr));
            X.opt = std::max(X.opt_scroll, std::min(X.opt_scroll + vis - 1, X.opt));
        }
    }
    dl->AddRectFilled(ImVec2(a.x + xs(8), a.y + xs(10)), ImVec2(b.x + xs(12), b.y + xs(14)), IM_COL32(0, 0, 0, 130), xs(8));
    xg_window(dl, a, b, false, true);
    xg_text(dl, ImVec2(a.x + xs(22), a.y + pad), XG_DIS, title, xg_fs() * 0.8f);
    bool close = false;
    if (ImGui::GetFrameCount() != X.popup_frame && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) close = true;
    if (ImGui::GetFrameCount() != X.popup_frame && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseHoveringRect(a, b))
        close = true;
    for (int i = X.opt_scroll; i < X.opt_scroll + vis; ++i) {
        const float y = a.y + pad + title_h + (i - X.opt_scroll) * row_h;
        xg_draw_row(dl, rows[i], ImVec2(a.x + xs(14), y), ImVec2(b.x - xs(14), y + row_h), i == X.opt, true,
                    X.opt, i, xg_fs());
    }
    if (X.opt_scroll > 0) xg_text_r(dl, ImVec2(b.x - xs(18), a.y + pad), XG_DIS, "\xE2\x96\xB2", xg_fs() * 0.7f);
    if (X.opt_scroll + vis < n) xg_text_r(dl, ImVec2(b.x - xs(18), b.y - pad - xg_fs() * 0.7f), XG_DIS, "\xE2\x96\xBC", xg_fs() * 0.7f);
    return !close;
}

/* ---- pages -------------------------------------------------------------------- */
static std::string g_xg_help;      // what the help window says this frame
static std::vector<std::pair<XgGlyph, const char*>> g_xg_legend;

static void xg_kv(ImDrawList* dl, ImVec2 p, float kw, const char* k, const char* v, ImU32 vc = XG_WL,
                  float alpha = 1.0f) {
    xg_text(dl, p, XG_DIS, k, 0, 0, alpha);
    xg_text(dl, ImVec2(p.x + kw, p.y), vc, v, 0, 0, alpha);
}

static void xg_page_game(LauncherModel* m, ImDrawList* dl, const XgLayout& L, const XgIn& in) {
    const auto wins = xg_game_windows(m);
    const int nw = (int)wins.size();
    if (X.win >= nw) X.win = 0;
    const float G = xs(14);
    const float ch = L.cont_b.y - L.cont_a.y;
    // Shares the column split, so this window lines up with the command menu.
    const float disc_h = L.cmd_b.y - L.cont_a.y;
    std::vector<ImVec2> wa(nw), wb(nw);
    wa[0] = L.cont_a;
    wb[0] = ImVec2(L.cont_b.x, L.cont_a.y + disc_h);
    const int nb = nw - 1;
    const float bw = nb > 0 ? ((L.cont_b.x - L.cont_a.x) - G * (nb - 1)) / nb : 0;
    for (int i = 1; i < nw; ++i) {
        wa[i] = ImVec2(L.cont_a.x + (i - 1) * (bw + G), wb[0].y + G);
        wb[i] = ImVec2(wa[i].x + bw, L.cont_b.y);
    }

    /* Input. */
    bool opened = false;
    static int last_bottom = 1;
    if (X.zone == XZ_CONTENT) {
        if (X.win == 0) {
            if (in.down && nw > 1) X.win = last_bottom < nw ? last_bottom : 1;
            else if (in.left || in.back) X.zone = XZ_MENU;
        } else {
            if (in.up) { last_bottom = X.win; X.win = 0; }
            else if (in.left) { if (X.win > 1) X.win--; else X.zone = XZ_MENU; }
            else if (in.right) { if (X.win < nw - 1) X.win++; }
            else if (in.back) X.zone = XZ_MENU;
        }
        if (in.ok && X.zone == XZ_CONTENT) { xg_open_popup(); opened = true; }
    }
    const bool popup = X.zone == XZ_POPUP;

    /* Windows. */
    const bool moved = xg_mouse_moved();
    const float fs = xg_fs(), lh = fs + xs(9);
    for (int i = 0; i < nw; ++i) {
        const bool foc = X.zone != XZ_MENU && X.win == i;
        const float al = (popup && X.win != i) ? 0.45f : 1.0f;
        xg_window(dl, wa[i], wb[i], foc && !popup, wins[i].kind == XGW_PLAYER, al);
        if (!popup) {
            char id[24]; snprintf(id, sizeof(id), "##xgwin%d", i);
            const XgHit h = xg_hit(id, wa[i], wb[i]);
            if (h.hov && moved) { X.zone = XZ_CONTENT; X.win = i; }
            if (h.click) { X.win = i; xg_open_popup(); }
        }
        const float pad = xs(30);   // room for the ▶ cursor left of the title
        const float right = wb[i].x - pad;   // same margin on the right
        // The "Options" hint only on the window being pointed at.
        const bool show_hint = !popup && (foc || ImGui::IsMouseHoveringRect(wa[i], wb[i]));
        ImVec2 p(wa[i].x + pad, wa[i].y + xs(18));
        auto title = [&](const char* s) {
            if (foc) xg_cursor(dl, ImVec2(p.x - xs(20), p.y + fs * 0.5f), al);
            xg_text(dl, p, XG_WL, s, 0, 0, al);
        };
        dl->PushClipRect(wa[i], wb[i], true);
        if (wins[i].kind == XGW_DISC) {
            const float art = disc_h - xs(36);
            const ImVec2 aa(wa[i].x + xs(20), wa[i].y + xs(18));
            if (g_boxart.id) xg_image(dl, g_boxart, aa, ImVec2(art, art), al, true);
            else xg_orb(dl, ImVec2(aa.x + art * .5f, aa.y + art * .5f), art * .3f, false, al);
            p.x = aa.x + art + xs(30);
            const std::string dls = xg_disc_label(m);
            title(dls.c_str());
            ImU32 vc; const char* verdict = xg_verdict_text(m, &vc);
            xg_text_r(dl, ImVec2(right, p.y), vc, ui_text(verdict), 0, al);
            const float kw = std::min(xs(170), (wb[i].x - p.x) * 0.38f);
            float y = p.y + lh + xs(8);
            const char* dash = "\xE2\x80\x94";
            xg_kv(dl, ImVec2(p.x, y), kw, ui_text("Serial"), m->verify.serial[0] ? m->verify.serial : dash, XG_WL, al); y += lh;
            xg_kv(dl, ImVec2(p.x, y), kw, ui_text("Region"), m->verify.region[0] ? m->verify.region : dash, XG_WL, al); y += lh;
            if (m->rom_present) {
                xg_kv(dl, ImVec2(p.x, y), kw, ui_text("ISO header"), m->verify.iso_ok ? "OK" : ui_text("Mismatch"),
                      m->verify.iso_ok ? XG_WL : XG_WARN, al); y += lh;
                const char* sbi = m->verify.sbi_status == RECOMP_SBI_OK ? "OK"
                                  : m->verify.sbi_status == RECOMP_SBI_MISSING ? ui_text("Missing") : "N/A";
                xg_kv(dl, ImVec2(p.x, y), kw, "SBI", sbi, m->verify.sbi_status == RECOMP_SBI_MISSING ? XG_WARN : XG_WL, al); y += lh;
            }
            xg_kv(dl, ImVec2(p.x, y), kw, ui_text("File"), m->rom_file[0] ? m->rom_file : dash, XG_WL, al);
            if (m->setup_error[0])
                xg_text(dl, ImVec2(p.x, wb[i].y - xs(18) - fs * 0.85f * 2), XG_WARN, m->setup_error, fs * 0.85f,
                        wb[i].x - p.x - xs(20), al);
            if (show_hint) xg_hint_r(dl, right, wb[i].y - xs(40), XGB_CROSS, ui_text("Options"), al);
        } else if (wins[i].kind == XGW_PLAYER) {
            const int pl = wins[i].index;
            char t[32]; snprintf(t, sizeof(t), "%s %d", ui_text("Player"), pl + 1);
            title(t);
            float y = p.y + lh + xs(8);
            const float inner_w = right - p.x;
            const float kw = std::min(xs(110), inner_w * 0.42f);
            const char* dev = launcher_model_player_src_label(m, pl);
            const char* mode = ui_text(xg_pad_mode_label(m, pl));
            const int rows_n = m->pad_mode_supported ? 2 : 1;
            const float text_top = y;
            const float text_bot = y + rows_n * lh + xs(6);
            const LauncherTexture& pad_tex =
                (m->s.pad_mode[pl] == 2 || m->s.player_src[pl] == 1) && g_pad_digital.id ? g_pad_digital
                : g_pad_analog.id ? g_pad_analog : g_pad;
            // Two places the pad art can go; take whichever shows it bigger.
            //  below: under the text, full width;  side: right of the text,
            //  with the text column capped so a long device name cannot
            //  squeeze the art (the name is fitted to the column instead).
            const float hint_top = wb[i].y - xs(48);
            const float val_full = inner_w - kw;
            const float val_side = std::max(xs(60), val_full * 0.5f);
            const float side_x = p.x + kw + std::min(val_side, std::max(xg_tsize(mode).x, xg_tsize(dev).x)) + xs(20);
            // The Options hint sits bottom-right; "below" may also run down to
            // the window's bottom edge when it keeps clear of the hint's width.
            const float hint_w = xg_glyph(nullptr, ImVec2(0, 0), XGB_CROSS) + xs(7) +
                                 xg_tsize(ui_text("Options"), fs * 0.8f).x + xs(14);
            const float bottom_in = wb[i].y - xs(16);
            ImVec2 below_box(inner_w, hint_top - xs(4) - text_bot);
            const ImVec2 below_tall(inner_w - hint_w, bottom_in - text_bot);
            const ImVec2 side_box(right - side_x, hint_top - xs(4) - text_top);
            auto fit_scale = [&](ImVec2 box) {
                if (!pad_tex.id || box.x <= 0 || box.y <= 0) return 0.0f;
                return std::min(box.x / pad_tex.w, box.y / pad_tex.h);
            };
            if (fit_scale(below_tall) > fit_scale(below_box)) below_box = below_tall;
            const float sb = fit_scale(below_box), ss = fit_scale(side_box);
            const bool side = ss > sb * 1.05f;
            const float val_w = side ? side_x - xs(20) - (p.x + kw) : val_full;
            // Value text that fits its column: a little smaller first, then cut with "…".
            auto fitted = [&](ImVec2 at, const char* v) {
                float vfs = fs;
                const float w = xg_tsize(v, fs).x;
                if (w > val_w) vfs = std::max(fs * 0.8f, fs * val_w / w);
                std::string out = v;
                if (xg_tsize(out.c_str(), vfs).x > val_w) {
                    while (out.size() > 1 && xg_tsize((out + "\xE2\x80\xA6").c_str(), vfs).x > val_w) {
                        size_t cut = out.size() - 1;
                        while (cut > 0 && (out[cut] & 0xC0) == 0x80) --cut;   // whole UTF-8 characters
                        out.erase(cut);
                    }
                    out += "\xE2\x80\xA6";
                }
                xg_text(dl, ImVec2(at.x, at.y + (fs - vfs) * 0.7f), XG_WL, out.c_str(), vfs, 0, al);
            };
            if (m->pad_mode_supported) {
                xg_text(dl, ImVec2(p.x, y), XG_DIS, ui_text("Pad"), 0, 0, al);
                fitted(ImVec2(p.x + kw, y), mode);
                y += lh;
            }
            xg_text(dl, ImVec2(p.x, y), XG_DIS, ui_text("Device"), 0, 0, al);
            fitted(ImVec2(p.x + kw, y), dev);
            {   // Status on the title row, right-aligned.
                const bool conn = m->s.player_src[pl] != 0;
                const char* st = conn ? ui_text("Ready") : ui_text("Not assigned");
                const float sfs = fs * 0.8f;
                const float sx = right - xg_tsize(st, sfs).x;
                xg_text(dl, ImVec2(sx, xg_cap_top(p.y + fs * 0.5f, sfs)), conn ? XG_WL : XG_DIS, st, sfs, 0, al);
                xg_orb(dl, ImVec2(sx - xs(12), p.y + fs * 0.5f), xs(6), conn, al);
            }
            if (pad_tex.id) {
                if (side) xg_image(dl, pad_tex, ImVec2(side_x, text_top), side_box, al * 0.95f);
                else if (below_box.y > xs(24)) xg_image(dl, pad_tex, ImVec2(p.x, text_bot), below_box, al * 0.95f);
            }
            if (show_hint) xg_hint_r(dl, right, wb[i].y - xs(40), XGB_CROSS, ui_text("Options"), al);
        } else {
            const int s = wins[i].index;
            const bool on = m->s.memcard_enabled[s] != 0;
            char t[32]; snprintf(t, sizeof(t), "%s %d", ui_text("Memory"), s + 1);
            title(t);
            char fname[160];
            if (m->s.memcard_path[s][0]) snprintf(fname, sizeof(fname), "%s", xg_basename(m->s.memcard_path[s]));
            else snprintf(fname, sizeof(fname), "%s %d", ui_text("Memory Card"), s + 1);
            float y = p.y + lh;
            xg_text(dl, ImVec2(p.x, y), XG_DIS, on ? fname : ui_text("Disabled"), fs * 0.82f, 0, al);
            y += lh + xs(6);
            const uint32_t used = launcher_model_memcard_blocks_used(m, s);
            int cnt = 0; for (int b = 0; b < 15; ++b) if (used & (1u << b)) ++cnt;
            const float cal = on ? al : al * 0.35f;
            // Card art on the left, count + block map on the right.
            const float inner_w = right - p.x;
            float tx = p.x;
            // The block map sits just above the Options hint; the card art
            // takes what is left between the file name and the map.
            const float map_y = wb[i].y - xs(46) - xs(26);
            const float art_h = std::min(xs(96), map_y - y - xs(10));
            if (g_memcard.id && art_h > xs(26)) {
                const float art_w = art_h * (float)g_memcard.w / (float)std::max(1, g_memcard.h);
                xg_image(dl, g_memcard, ImVec2(p.x, y), ImVec2(art_w, art_h), cal);
                tx = p.x + art_w + xs(16);
            }
            char big[16]; snprintf(big, sizeof(big), "%d", cnt);
            const float bfs = fs * 1.7f;
            xg_text(dl, ImVec2(tx, y), XG_WL, big, bfs, 0, cal);
            xg_text(dl, ImVec2(tx + xg_tsize(big, bfs).x + xs(8), y + bfs - fs), XG_DIS, "/ 15", 0, 0, cal);
            const float gy = std::max(map_y, y + bfs + xs(8));
            const float gw = inner_w, cell = (gw - xs(3) * 14) / 15;
            for (int b = 0; b < 15; ++b) {
                const ImVec2 ca(p.x + b * (cell + xs(3)), gy), cb(ca.x + cell, gy + xs(14));
                const bool u = (used & (1u << b)) != 0;
                dl->AddRectFilled(ca, cb, xg_a(u ? XG_RED : IM_COL32(0, 0, 0, 140), cal));
                dl->AddRect(ca, cb, xg_a(u ? IM_COL32(255, 155, 144, 255) : XG_WG, cal));
            }
            if (show_hint) xg_hint_r(dl, right, wb[i].y - xs(40), XGB_CROSS, ui_text("Options"), al);
        }
        dl->PopClipRect();
    }

    /* Popup. */
    if (X.zone == XZ_POPUP) {
        auto rows = xg_game_options(m, wins[X.win]);
        const float pw = std::min(xs(500), L.cont_b.x - L.cont_a.x);
        ImVec2 pa;
        if (wins[X.win].kind == XGW_DISC) pa = ImVec2(L.cont_a.x + xs(40), wb[0].y - xs(40));
        else pa = ImVec2(wa[X.win].x, L.cont_a.y + xs(14));
        const char* titles[] = {"Disc", "Player", "Memory"};
        if (!xg_draw_popup(dl, pa, pw, ui_text(titles[wins[X.win].kind]), rows, opened ? XgIn{} : in, L))
            X.zone = XZ_CONTENT;
        if (X.zone == XZ_POPUP && X.opt < (int)rows.size()) g_xg_help = rows[X.opt].help;
        g_xg_legend = {{XGB_UD, "Move"}, {XGB_LR, "Change"}, {XGB_CROSS, "Confirm"}, {XGB_CIRCLE, "Close"}, {XGB_START, "Play"}};
    } else if (X.zone == XZ_CONTENT) {
        const auto& w = wins[X.win];
        if (w.kind == XGW_DISC) g_xg_help = std::string(xg_disc_label(m)) + " \xC2\xB7 " + (m->rom_file[0] ? m->rom_file : ui_text("no image selected")) + ".\n" + ui_text("Press {OK} to change the disc or locate an image.");
        else if (w.kind == XGW_PLAYER)
            g_xg_help = (m->pad_mode_supported && m->pad_mode_selectable)
                ? ui_text("How the game reads this player's controller.\nPress {OK} for the pad mode, the device and the button mapping.")
                : ui_text("How the game reads this player's controller.\nPress {OK} for the device and the button mapping.");
        else g_xg_help = ui_text("Memory card slot.\nPress {OK} to choose, create or disable the card.");
        g_xg_legend = {{XGB_DPAD, "Move"}, {XGB_CROSS, "Options"}, {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
    }
}

// Tab strip window: L1 · names · R1, the current one with the red cursor.
// Each name lights up under the mouse; the L1 / R1 chips step back and forth.
static void xg_tabs(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::vector<std::string>& names, int cur,
                    const char* right_note, const std::function<void(int)>& pick) {
    xg_window(dl, a, b);
    const int n = (int)names.size();
    const bool live = X.zone != XZ_POPUP;
    const float fs = xg_fs(), y = a.y + (b.y - a.y - fs) * 0.5f, gy = a.y + (b.y - a.y - xs(24)) * 0.5f;
    auto chip = [&](float x, XgGlyph g, const char* id, int target) {
        const XgHit h = live ? xg_hit(id, ImVec2(x - xs(4), gy - xs(4)), ImVec2(x + xs(40), gy + xs(28))) : XgHit{};
        const float w = xg_glyph(dl, ImVec2(x, gy), g);
        if (h.hov) {
            dl->AddRect(ImVec2(x - xs(2), gy - xs(2)), ImVec2(x + w + xs(2), gy + xs(26)), XG_WL, xs(5), 0,
                        std::max(1.0f, xs(1.5f)));
        }
        if (h.click && n > 0) pick(((target % n) + n) % n);
        return w;
    };
    float x = a.x + xs(20);
    x += chip(x, XGB_L1, "##xgtabl1", cur - 1) + xs(30);
    for (int i = 0; i < n; ++i) {
        const ImVec2 ts = xg_tsize(names[i].c_str(), fs);
        const ImVec2 ha(x - xs(10), a.y + xs(8)), hb(x + ts.x + xs(10), b.y - xs(8));
        char id[24]; snprintf(id, sizeof(id), "##xgtab%d", i);
        const XgHit h = live ? xg_hit(id, ha, hb) : XgHit{};
        if (h.hov) {
            dl->AddRectFilled(ha, hb, IM_COL32(255, 255, 255, i == cur ? 20 : 34), xs(4));
            dl->AddRectFilled(ImVec2(ha.x + xs(6), hb.y - xs(4)), ImVec2(hb.x - xs(6), hb.y - xs(2)),
                              i == cur ? XG_RED : XG_WL, xs(1));
        }
        if (i == cur) xg_cursor(dl, ImVec2(x - xs(18), y + fs * 0.5f));
        xg_text(dl, ImVec2(x, y), (i == cur || h.hov) ? XG_WL : XG_DIS, names[i].c_str(), fs);
        if (h.click) pick(i);
        x += ts.x + xs(36);
    }
    x -= xs(8);
    chip(x, XGB_R1, "##xgtabr1", cur + 1);
    if (right_note) xg_text_r(dl, ImVec2(b.x - xs(22), a.y + (b.y - a.y - fs * 0.85f) * 0.5f), XG_DIS, right_note, fs * 0.85f);
}

// A scrolling list of rows with the red bar; ←→ changes the selected value.
static void xg_list(ImDrawList* dl, ImVec2 a, ImVec2 b, std::vector<XgRow>& rows, int& sel, int& scroll,
                    bool active, const XgIn& in) {
    xg_window(dl, a, b, false, true);
    const float row_h = xs(46);
    const int n = (int)rows.size();
    const int vis = std::max(1, (int)((b.y - a.y - xs(24)) / row_h));
    if (n == 0) return;
    if (sel >= n) sel = n - 1;
    if (active) {
        if (in.up) sel = (sel + n - 1) % n;
        if (in.down) sel = (sel + 1) % n;
        XgRow& r = rows[sel];
        if (!r.disabled && r.kind == XR_VALUE && r.change) {
            if (in.left) r.change(-1);
            if (in.right) r.change(+1);
        }
        if (in.ok) xg_row_ok(r);
        if (in.alt && r.alt && !r.disabled) r.alt();
    }
    if (const int wr = xg_wheel_rows(a, b)) {
        // The wheel scrolls the view; the selection stays inside it.
        scroll = std::max(0, std::min(std::max(0, n - vis), scroll + wr));
        sel = std::max(scroll, std::min(scroll + vis - 1, sel));
    }
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + vis) scroll = sel - vis + 1;
    if (scroll > std::max(0, n - vis)) scroll = std::max(0, n - vis);
    for (int i = scroll; i < std::min(n, scroll + vis); ++i) {
        const float y = a.y + xs(12) + (i - scroll) * row_h;
        xg_draw_row(dl, rows[i], ImVec2(a.x + xs(14), y), ImVec2(b.x - xs(18), y + row_h),
                    X.zone != XZ_MENU && i == sel, X.zone != XZ_POPUP, sel, i, xg_fs());
    }
    if (scroll > 0) xg_text_r(dl, ImVec2(b.x - xs(22), a.y + xs(8)), XG_DIS, "\xE2\x96\xB2", xg_fs() * 0.7f);
    if (scroll + vis < n) xg_text_r(dl, ImVec2(b.x - xs(22), b.y - xs(10) - xg_fs() * 0.7f), XG_DIS, "\xE2\x96\xBC", xg_fs() * 0.7f);
}

// An always-visible row of actions under a page's list (Mods, Controls).
// It is reached with ↓ from the last row of the list; ←→ moves, ↑ returns.
struct XgBarItem { std::string label, help; std::function<void()> act; bool disabled = false; };

static float xg_bar_h() { return xs(60); }

static void xg_action_bar(ImDrawList* dl, ImVec2 a, ImVec2 b, std::vector<XgBarItem>& items, const XgIn& in) {
    const int n = (int)items.size();
    const bool focus = X.zone == XZ_CONTENT && X.bar && n > 0;
    if (X.bar_btn >= n) X.bar_btn = n > 0 ? n - 1 : 0;
    if (focus) {
        if (in.left) X.bar_btn = (X.bar_btn + n - 1) % n;
        if (in.right) X.bar_btn = (X.bar_btn + 1) % n;
        if (in.up) X.bar = false;
        if (in.ok && !items[X.bar_btn].disabled && items[X.bar_btn].act) items[X.bar_btn].act();
    }
    xg_window(dl, a, b, focus, false, X.zone == XZ_POPUP ? 0.45f : 1.0f);
    if (n == 0) return;
    const float fs = xg_fs() * 0.92f, cw = (b.x - a.x - xs(24)) / n;
    const bool moved = xg_mouse_moved();
    for (int i = 0; i < n; ++i) {
        XgBarItem& it = items[i];
        const ImVec2 ia(a.x + xs(12) + i * cw, a.y + xs(8)), ib(ia.x + cw, b.y - xs(8));
        char id[24]; snprintf(id, sizeof(id), "##xgbar%d", i);
        const XgHit h = X.zone == XZ_POPUP ? XgHit{} : xg_hit(id, ia, ib);
        if (h.hov && moved) { X.zone = XZ_CONTENT; X.bar = true; X.bar_btn = i; }
        const bool sel = focus && X.bar_btn == i;
        if (sel || h.hov) dl->AddRectFilled(ia, ib, IM_COL32(255, 255, 255, sel ? 26 : 16), xs(4));
        const float tw = xg_tsize(it.label.c_str(), fs).x;
        const float tx = ia.x + (cw - tw) * 0.5f, ty = ia.y + (ib.y - ia.y - fs) * 0.5f;
        if (sel) xg_cursor(dl, ImVec2(tx - xs(20), ty + fs * 0.5f));
        xg_text(dl, ImVec2(tx, ty), sel || h.hov ? XG_WL : XG_DIS, it.label.c_str(), fs, 0, it.disabled ? 0.42f : 1.0f);
        if (h.click && !it.disabled && it.act) { X.zone = XZ_CONTENT; X.bar = true; X.bar_btn = i; it.act(); }
    }
    if (focus) {
        g_xg_help = items[X.bar_btn].help;
        g_xg_legend = {{XGB_LR, "Move"}, {XGB_CROSS, "Select"}, {XGB_UD, "List"}, {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
    }
}

static void xg_page_config(LauncherModel* m, ImDrawList* dl, const XgLayout& L, const XgIn& in) {
    auto secs = xg_config_sections(m);
    if (secs.empty()) return;
    const int ns = (int)secs.size();
    if (X.cfg_sec >= ns) X.cfg_sec = 0;
    const bool active = X.zone == XZ_CONTENT;
    bool switched = false;
    if (active) {
        if (in.l1) { X.cfg_sec = (X.cfg_sec + ns - 1) % ns; X.cfg_row = 0; X.cfg_scroll = 0; switched = true; }
        if (in.r1) { X.cfg_sec = (X.cfg_sec + 1) % ns; X.cfg_row = 0; X.cfg_scroll = 0; switched = true; }
        if (in.back) X.zone = XZ_MENU;
    }
    std::vector<std::string> names;
    for (const auto& s : secs) names.push_back(s.name);
    const ImVec2 ta = L.cont_a, tb(L.cont_b.x, L.cont_a.y + xs(58));
    xg_tabs(dl, ta, tb, names, X.cfg_sec, nullptr, [](int i) { X.cfg_sec = i; X.cfg_row = 0; X.cfg_scroll = 0; X.zone = XZ_CONTENT; });
    auto& rows = secs[X.cfg_sec].rows;
    xg_list(dl, ImVec2(L.cont_a.x, tb.y + xs(14)), L.cont_b, rows, X.cfg_row, X.cfg_scroll,
            X.zone == XZ_CONTENT && !switched, in);
    if (X.zone == XZ_CONTENT && X.cfg_row < (int)rows.size()) {
        const XgRow& r = rows[X.cfg_row];
        g_xg_help = r.label + (r.value.empty() ? "" : "  \xC2\xB7  " + r.value) + "\n" + r.help;
        g_xg_legend = {{XGB_UD, "Move"}, {XGB_LR, "Change"}, {XGB_CROSS, "Select"}, {XGB_L1R1, "Section"},
                       {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
    }
}

static void xg_page_mods(LauncherModel* m, const LauncherTheme& th, ImDrawList* dl, const XgLayout& L, const XgIn& in) {
    process_mod_archive_install(m);
    const bool features = xg_mods_feature_provider(m);
    const bool packages = m->mods && m->mods->package_count && m->mods->package_get;
    std::vector<std::string> tabs;
    if (features) tabs.push_back(ui_text("Features"));
    if (packages) tabs.push_back(ui_text("Installed"));
    if (tabs.empty()) return;
    const int nt = (int)tabs.size();
    if (X.mod_tab >= nt) X.mod_tab = 0;
    const int tab_kind = features ? X.mod_tab : 1;   // 0 features, 1 packages
    const bool active = X.zone == XZ_CONTENT;
    bool switched = false;
    if (active) {
        if (in.l1 || in.r1) { X.mod_tab = (X.mod_tab + (in.r1 ? 1 : nt - 1)) % nt; X.mod_row = 0; X.mod_scroll = 0; switched = true; }
        else if (in.back) X.zone = XZ_MENU;
    }
    auto rows = xg_mod_rows(m, features ? X.mod_tab : 1);
    (void)tab_kind;
    std::vector<int> selectable;
    for (int i = 0; i < (int)rows.size(); ++i) if (rows[i].kind != 0) selectable.push_back(i);
    const int ns = (int)selectable.size();
    if (X.mod_row >= ns) X.mod_row = ns > 0 ? ns - 1 : 0;

    char note[64] = "";
    if (features && X.mod_tab == 0) {
        int on = 0, all = 0;
        for (const auto& r : rows) if (r.kind == 1) { ++all; if (r.f.enabled) ++on; }
        snprintf(note, sizeof(note), "%d / %d %s", on, all, ui_text("on"));
    }
    const ImVec2 ta = L.cont_a, tb(L.cont_b.x, L.cont_a.y + xs(58));
    xg_tabs(dl, ta, tb, tabs, X.mod_tab, note, [](int i) { X.mod_tab = i; X.mod_row = 0; X.mod_scroll = 0; X.zone = XZ_CONTENT; });

    const float G = xs(14);
    const float list_w = (L.cont_b.x - L.cont_a.x) * 0.54f;
    auto bar_rows = xg_mod_actions(m);
    std::vector<XgBarItem> bar;
    for (auto& r : bar_rows) bar.push_back({r.label, r.help, r.act, false});
    const float bottom = L.cont_b.y - xg_bar_h() - G;
    const ImVec2 la(L.cont_a.x, tb.y + G), lb(L.cont_a.x + list_w, bottom);
    const ImVec2 da(lb.x + G, la.y), db(L.cont_b.x, bottom);
    xg_window(dl, la, lb, false, true);
    const bool popup = X.zone == XZ_POPUP;
    if (switched) X.bar = false;

    /* List input (the action bar takes it while it has the focus). */
    bool opened = false;
    const bool list_live = active && ns > 0 && !switched && !X.bar;
    xg_action_bar(dl, ImVec2(L.cont_a.x, bottom + G), L.cont_b, bar, active && !switched ? in : XgIn{});
    if (list_live) {
        if (in.up) X.mod_row = std::max(0, X.mod_row - 1);
        if (in.down) { if (X.mod_row < ns - 1) ++X.mod_row; else if (!bar.empty()) { X.bar = true; X.bar_btn = 0; } }
        XgModRow& r = rows[selectable[X.mod_row]];
        if ((in.left || in.right) && r.kind == 1 && !(r.f.blocked && !r.f.enabled)) {
            if (!m->mods->feature_enable(m->mods->ctx, r.f.package_id, r.f.id, r.f.enabled ? 0 : 1)) mod_note_error(m);
            else r.f.enabled = !r.f.enabled;
        }
        if (in.ok && !X.bar) {
            if (r.kind == 2 && r.act) r.act();
            else if (r.kind == 1 || r.kind == 3) { xg_open_popup(); opened = true; }
        }
    }

    /* List drawing: group headers are not selectable. */
    const float fs = xg_fs(), row_h = xs(40), hdr_h = xs(28);
    // The wheel scrolls the view; the selection is pulled inside it after drawing.
    const int wheel_rows = xg_wheel_rows(la, lb);
    if (wheel_rows) X.mod_scroll = std::max(0, std::min((int)rows.size() - 1, X.mod_scroll + wheel_rows));
    if (!wheel_rows) {
        const float avail = lb.y - la.y - xs(24);
        const int sel_full = ns > 0 ? selectable[X.mod_row] : 0;
        if (sel_full < X.mod_scroll) X.mod_scroll = sel_full;
        for (;;) {
            float h = 0;
            for (int i = X.mod_scroll; i <= sel_full && i < (int)rows.size(); ++i) h += rows[i].kind == 0 ? hdr_h : row_h;
            if (h <= avail || X.mod_scroll >= sel_full) break;
            ++X.mod_scroll;
        }
    }
    const bool moved = xg_mouse_moved();
    dl->PushClipRect(la, lb, true);
    float y = la.y + xs(12);
    int sel_index = 0;
    bool more_below = false;
    int first_vis = -1, last_vis = -1;
    for (int i = 0; i < (int)rows.size(); ++i) {
        XgModRow& r = rows[i];
        const int si = r.kind == 0 ? -1 : sel_index++;
        if (i < X.mod_scroll) continue;
        const float h = r.kind == 0 ? hdr_h : row_h;
        if (y + h > lb.y - xs(8)) { more_below = true; break; }
        if (si >= 0) { if (first_vis < 0) first_vis = si; last_vis = si; }
        if (r.kind == 0) {
            if (!r.label.empty()) xg_text(dl, ImVec2(la.x + xs(24), y + (h - fs * 0.78f) * 0.6f), XG_DIS, r.label.c_str(), fs * 0.78f);
            y += h;
            continue;
        }
        const ImVec2 ra(la.x + xs(14), y), rb(lb.x - xs(16), y + h);
        const bool sel = X.zone != XZ_MENU && !X.bar && si == X.mod_row;
        char id[24]; snprintf(id, sizeof(id), "##xgmod%d", i);
        const XgHit hit = popup ? XgHit{} : xg_hit(id, ra, rb);
        if (hit.hov && moved) { X.zone = XZ_CONTENT; X.bar = false; X.mod_row = si; }
        if (sel) dl->AddRectFilled(ImVec2(ra.x, ra.y + xs(2)), ImVec2(rb.x, rb.y - xs(2)), IM_COL32(255, 255, 255, 24), xs(3));
        const float ty = y + (h - fs) * 0.5f;
        if (sel) xg_cursor(dl, ImVec2(ra.x + xs(4), y + h * 0.5f));
        const bool blocked = r.kind == 1 && r.f.blocked && !r.f.enabled;
        const float al = blocked ? 0.42f : 1.0f;
        float value_left = rb.x - xs(10);
        if (r.kind == 1) {
            const char* v = r.f.enabled ? ui_text("On") : ui_text("Off");
            std::string vs = sel && !popup && !blocked ? std::string("\xE2\x97\x80 ") + v + " \xE2\x96\xB6" : v;
            const ImVec2 sz = xg_tsize(vs.c_str(), fs);
            value_left = rb.x - xs(10) - sz.x;
            xg_text(dl, ImVec2(value_left, ty), r.f.enabled ? XG_WL : XG_DIS, vs.c_str(), fs, 0, al);
            if (hit.click) {
                const float mx = ImGui::GetIO().MousePos.x;
                if (mx >= value_left - xs(8) && !blocked) {
                    if (!m->mods->feature_enable(m->mods->ctx, r.f.package_id, r.f.id, r.f.enabled ? 0 : 1)) mod_note_error(m);
                } else { X.mod_row = si; xg_open_popup(); }
            }
        } else if (r.kind == 3) {
            const ImVec2 sz = xg_tsize(r.pkg.version, fs * 0.85f);
            value_left = rb.x - xs(10) - sz.x;
            xg_text(dl, ImVec2(value_left, y + (h - fs * 0.85f) * 0.5f), XG_DIS, r.pkg.version, fs * 0.85f);
            if (hit.click) { X.mod_row = si; xg_open_popup(); }
        } else if (r.kind == 2) {
            if (hit.click) { X.mod_row = si; if (r.act) r.act(); }
        } else if (hit.click) {
            X.mod_row = si;
        }
        std::string label = r.label;
        if (r.kind == 1 && r.f.channel == RECOMP_MOD_CHANNEL_EXPERIMENTAL) label += " [EXP]";
        if (r.kind == 1 && r.f.channel == RECOMP_MOD_CHANNEL_DEVELOPER) label += " [DEV]";
        if (r.kind == 1 && r.f.has_error) xg_text(dl, ImVec2(ra.x + xs(8), ty), XG_WARN, "!", fs, 0, al);
        dl->PushClipRect(ra, ImVec2(std::max(ra.x + xs(40), value_left - xs(14)), rb.y), true);
        xg_text(dl, ImVec2(ra.x + xs(24), ty), r.kind == 4 ? XG_WARN : XG_WL, label.c_str(), fs, 0, al);
        dl->PopClipRect();
        y += h;
    }
    dl->PopClipRect();
    if (wheel_rows && first_vis >= 0) X.mod_row = std::max(first_vis, std::min(last_vis, X.mod_row));
    if (wheel_rows && !more_below) {   // do not scroll past the end
        // Step back while the previous row would still fit.
        float used = 0;
        for (int i = X.mod_scroll; i < (int)rows.size(); ++i) used += rows[i].kind == 0 ? hdr_h : row_h;
        const float avail = lb.y - la.y - xs(20);
        while (X.mod_scroll > 0) {
            const float prev = rows[X.mod_scroll - 1].kind == 0 ? hdr_h : row_h;
            if (used + prev > avail) break;
            used += prev; --X.mod_scroll;
        }
    }
    if (X.mod_scroll > 0) xg_text_r(dl, ImVec2(lb.x - xs(20), la.y + xs(6)), XG_DIS, "\xE2\x96\xB2", fs * 0.7f);
    if (more_below) xg_text_r(dl, ImVec2(lb.x - xs(20), lb.y - xs(8) - fs * 0.7f), XG_DIS, "\xE2\x96\xBC", fs * 0.7f);

    /* Detail window (the options popup covers it). */
    xg_window(dl, da, db, false, false, popup ? 0.45f : 1.0f);
    if (!popup) {
        dl->PushClipRect(da, db, true);
        const float wrap = db.x - da.x - xs(44);
        ImVec2 p(da.x + xs(22), da.y + xs(18));
        auto line = [&](const char* s, ImU32 c, float scale = 1.0f) {
            if (!s || !s[0]) return;
            xg_text(dl, p, c, s, fs * scale, wrap);
            p.y += xg_tsize(s, fs * scale, wrap).y + xs(8);
        };
        if (ns > 0) {
            const XgModRow& r = rows[selectable[X.mod_row]];
            if (r.kind == 1) {
                const auto& f = r.f;
                line(f.name, XG_WL, 1.05f);
                char meta[256];
                snprintf(meta, sizeof(meta), "%s %s", f.package_name, f.package_version);
                line(meta, XG_DIS, 0.78f);
                if (f.author[0]) { char a[200]; snprintf(a, sizeof(a), "by %s", f.author); line(a, XG_DIS, 0.78f); }
                if (f.channel == RECOMP_MOD_CHANNEL_EXPERIMENTAL) line(ui_text("EXPERIMENTAL"), XG_WARN, 0.78f);
                if (f.channel == RECOMP_MOD_CHANNEL_DEVELOPER) line(ui_text("DEVELOPER"), XG_WARN, 0.78f);
                p.y += xs(6);
                line(f.description, XG_WL, 0.8f);
                if (f.texture_pack) {
                    char b[64]; snprintf(b, sizeof(b), "%d %s", f.texture_pack_images, ui_text("replacement images"));
                    line(b, XG_GOOD, 0.78f);
                    line(f.texture_pack_path, XG_DIS, 0.72f);
                }
                if (f.blocked && !f.enabled) {
                    char b[200]; snprintf(b, sizeof(b), "%s %s", ui_text("Unavailable while enabled:"), f.blocked_by);
                    line(b, XG_WARN, 0.78f);
                }
                if (f.has_error || f.status[0]) line(f.status, f.has_error ? XG_WARN : XG_DIS, 0.78f);
                if (f.option_count > 0) {
                    p.y += xs(6);
                    for (int i = 0; i < f.option_count && p.y + fs * 0.78f + xs(10) < db.y - xs(8); ++i) {
                        RecompLauncherCModOption o{};
                        if (!m->mods->feature_option_get(m->mods->ctx, f.package_id, f.id, i, &o)) continue;
                        const std::string v = xg_feature_option_value(m, f, o);
                        const float ofs = fs * 0.78f;
                        const float vw = xg_tsize(v.c_str(), ofs).x;
                        dl->PushClipRect(p, ImVec2(db.x - xs(30) - vw, p.y + ofs + xs(4)), true);
                        xg_text(dl, p, XG_DIS, o.label[0] ? o.label : o.id, ofs);
                        dl->PopClipRect();
                        xg_text_r(dl, ImVec2(db.x - xs(22), p.y), XG_WL, v.c_str(), ofs);
                        p.y += ofs + xs(10);
                    }
                }
            } else if (r.kind == 3) {
                const auto& pk = r.pkg;
                line(pk.name[0] ? pk.name : pk.id, XG_WL, 1.05f);
                char meta[200]; snprintf(meta, sizeof(meta), "%s %s", ui_text("Version"), pk.version); line(meta, XG_DIS, 0.78f);
                if (pk.author[0]) { char a[200]; snprintf(a, sizeof(a), "by %s", pk.author); line(a, XG_DIS, 0.78f); }
                p.y += xs(6);
                line(pk.description, XG_WL, 0.8f);
                if (pk.has_error) line(pk.status, XG_WARN, 0.78f);
            } else if (r.kind == 4 && m->mods->catalog_diagnostic_get) {
                const int nd = m->mods->catalog_diagnostic_count(m->mods->ctx);
                for (int i = 0; i < nd; ++i) {
                    RecompLauncherCModDiagnostic d{};
                    if (!m->mods->catalog_diagnostic_get(m->mods->ctx, i, &d)) continue;
                    line(d.message, XG_WARN, 0.78f);
                    line(d.resource, XG_DIS, 0.72f);
                }
            } else {
                line(r.label.c_str(), XG_WL, 1.05f);
                line(r.help.c_str(), XG_DIS, 0.8f);
            }
        }
        dl->PopClipRect();
    }

    /* Help + legend. */
    if (active && ns > 0 && !X.bar) {
        const XgModRow& r = rows[selectable[X.mod_row]];
        if (r.kind == 1) {
            g_xg_help = std::string(r.f.name) + "  \xC2\xB7  " + (r.f.enabled ? ui_text("On") : ui_text("Off")) + "\n" +
                        ui_text("Left/Right turns it on or off. {OK} opens its options. Changes apply when you press Start.");
            g_xg_legend = {{XGB_UD, "Move"}, {XGB_LR, "On/Off"}, {XGB_CROSS, "Options"}, {XGB_L1R1, "Tab"},
                           {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
        } else {
            if (r.kind == 3)
                g_xg_help = std::string(ui_text("Installed package. {OK} for its version and to uninstall it."));
            else g_xg_help = r.help;
            g_xg_legend = {{XGB_UD, "Move"}, {XGB_CROSS, "Select"}, {XGB_L1R1, "Tab"}, {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
        }
    }
    if (m->mod_status[0] && active && !X.bar) g_xg_help = m->mod_status;

    /* Options popup in place of the detail window. */
    if (popup && ns > 0) {
        const XgModRow r = rows[selectable[X.mod_row]];
        auto opts = xg_mod_options(m, r);
        if (opts.empty() || !xg_draw_popup(dl, da, db.x - da.x, r.label.c_str(), opts, opened ? XgIn{} : in, L))
            X.zone = XZ_CONTENT;
        else if (X.opt < (int)opts.size()) g_xg_help = opts[X.opt].help;
        g_xg_legend = {{XGB_UD, "Move"}, {XGB_LR, "Change"}, {XGB_CROSS, "Confirm"}, {XGB_CIRCLE, "Close"}, {XGB_START, "Play"}};
    } else if (popup) {
        X.zone = XZ_CONTENT;
    }

    draw_mod_archive_install_progress(m, th);
}

/* ---- confirmation dialog in the menu's own look ------------------------------ */
// Used for the launcher's yes/no questions (skip the launcher, restore
// defaults): ←→ picks, ✕ confirms, ○ cancels, the mouse clicks. Cancel is
// selected first. Returns 0 while open, 1 confirm, -1 cancel.
static int g_xg_confirm_sel = 0;
static int xg_confirm(ImDrawList* dl, const XgLayout& L, const char* title, const char* body,
                      const char* yes, bool first_frame) {
    if (first_frame) g_xg_confirm_sel = 0;
    const ImGuiIO& io = ImGui::GetIO();
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    const bool pad = s_pad_nav_armed;
    int result = 0;
    // The frame it opens on, the key that opened it is still "pressed": ignore input.
    if (!first_frame) {
        if (key(ImGuiKey_LeftArrow) || key(ImGuiKey_RightArrow) ||
            (pad && (key(ImGuiKey_GamepadDpadLeft) || key(ImGuiKey_GamepadDpadRight))))
            g_xg_confirm_sel ^= 1;
        if (key(ImGuiKey_Escape) || key(ImGuiKey_Backspace) || (pad && key(ImGuiKey_GamepadFaceRight))) result = -1;
    }
    if (!first_frame && (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter) || (pad && key(ImGuiKey_GamepadFaceDown))))
        result = g_xg_confirm_sel ? 1 : -1;
    // Dim everything, then the window in the middle of the content area.
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    (void)dl;
    const ImVec2 vp0 = ImGui::GetMainViewport()->Pos, vps = ImGui::GetMainViewport()->Size;
    fg->AddRectFilled(vp0, ImVec2(vp0.x + vps.x, vp0.y + vps.y), IM_COL32(0, 0, 0, 140));
    const float w = std::min(xs(560), L.help_b.x - L.help_a.x - xs(40));
    const float fs = xg_fs(), wrap = w - xs(48);
    const float body_h = xg_tsize(body, fs * 0.9f, wrap).y;
    const float h = xs(22) + fs + xs(16) + body_h + xs(26) + xs(44) + xs(20);
    const ImVec2 c((L.cont_a.x + L.cont_b.x) * 0.5f, (L.hdr_b.y + L.help_b.y) * 0.5f);
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    xg_window(fg, a, b);
    float y = a.y + xs(22);
    xg_text(fg, ImVec2(a.x + xs(24), y), XG_WL, title, fs);
    y += fs + xs(16);
    xg_text(fg, ImVec2(a.x + xs(24), y), XG_DIS, body, fs * 0.9f, wrap);
    y += body_h + xs(26);
    const char* labels[2] = {ui_text("Cancel"), yes};
    const float bw = (w - xs(24) * 2 - xs(16)) * 0.5f, bh = xs(44);
    for (int i = 0; i < 2; ++i) {
        const ImVec2 ba(a.x + xs(24) + i * (bw + xs(16)), y), bb(ba.x + bw, y + bh);
        const bool hov = ImGui::IsMouseHoveringRect(ba, bb);
        if (hov && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) g_xg_confirm_sel = i;
        const bool sel = g_xg_confirm_sel == i;
        fg->AddRectFilled(ba, bb, sel ? IM_COL32(255, 255, 255, 28) : IM_COL32(0, 0, 0, 60), xs(4));
        fg->AddRect(ba, bb, sel ? IM_COL32(253, 30, 20, 220) : XG_WG, xs(4), 0, std::max(1.0f, xs(1.5f)));
        const float tw = xg_tsize(labels[i], fs).x;
        const float tx = ba.x + (bw - tw) * 0.5f, ty = ba.y + (bh - fs) * 0.5f;
        if (sel) xg_cursor(fg, ImVec2(tx - xs(22), ty + fs * 0.5f));
        xg_text(fg, ImVec2(tx, ty), sel ? XG_WL : XG_DIS, labels[i], fs);
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !first_frame) result = i ? 1 : -1;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !first_frame) result = -1;
    return result;
}

// Text option editor and package removal confirmation (plain ImGui modals).
static void xg_modals(LauncherModel* m) {
    if (X.text_edit) { ImGui::OpenPopup("Edit value##xg"); X.text_edit = false; }
    if (ImGui::BeginPopupModal("Edit value##xg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(X.text_label.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(xs(420));
        const bool enter = ImGui::InputText("##xgtext", X.text_buf, sizeof(X.text_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(ui_text("OK"), ImVec2(xs(120), 0)) || enter) {
            if (m->mods && !m->mods->feature_set_option(m->mods->ctx, X.text_pkg.c_str(), X.text_feat.c_str(),
                                                        X.text_opt.c_str(), X.text_buf))
                mod_note_error(m);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(ui_text("Cancel"), ImVec2(xs(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (X.rename_open) { ImGui::OpenPopup("Rename gamepad##xg"); X.rename_open = false; }
    if (ImGui::BeginPopupModal("Rename gamepad##xg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(ui_text("Display name for this gamepad:"));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(xs(360));
        const bool enter = ImGui::InputText("##xgrename", X.rename_buf, sizeof(X.rename_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(ui_text("Cancel"), ImVec2(xs(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if ((ImGui::Button(ui_text("OK"), ImVec2(xs(120), 0)) || enter) && X.rename_buf[0]) {
            launcher_binds_rename_psx_gamepad(m, m->cfg_player + 1, X.rename_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* ---- Controls page (the button mapping of one player) ------------------------- */
static bool xg_is_psx(const LauncherModel* m) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    return prof && prof->id && !strcmp(prof->id, "psx");
}

static void xg_begin_pad_capture(LauncherModel* m, int p, int b) {
    m->map_all_active = false;
    m->map_all_wait_release = false;
    launcher_model_begin_pad_capture(m, b);
    // Starting a bind while still holding the previous input waits for a
    // full release first, as the classic page does.
    if (m->capturing && m->player_pad_id[p] && !launcher_input_gamepad_at_rest(m->player_pad_id[p]))
        m->map_all_wait_release = true;
}

static std::vector<XgSection> xg_controls_sections(LauncherModel* m, int p) {
    std::vector<XgSection> secs;
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const ControllerSpec& spec = prof->controller;
    const bool pad = m->s.player_src[p] == 2 && m->s.player_gamepad_guid[p][0];

    /* Buttons: every PSX input, in the order the classic page reads them. */
    {
        XgSection s; s.name = ui_text("Buttons");
        for (int i = 0; i < LNG_PSX_PAD_BUTTON_COUNT; ++i) {
            const int b = kPsxGamepadBindOrder[i];
            XgRow r; r.kind = XR_ACTION; r.label = spec.buttons[b].label;
            if (pad) {
                const bool cap = m->capturing && m->capture_pad && m->capture_btn == b;
                r.value = cap ? (m->map_all_wait_release ? ui_text("[ release ]") : ui_text("[ press a button ]"))
                              : (m->pad_binds[p][b][0] ? m->pad_binds[p][b] : ui_text("(unbound)"));
                r.help = std::string(spec.buttons[b].label) + "\n" +
                         ui_text("{OK}, then press the button to use. Esc cancels; Backspace clears it.");
                r.act = [m, p, b]() { xg_begin_pad_capture(m, p, b); };
            } else {
                const bool cap = m->capturing && !m->capture_pad && m->capture_btn == b;
                const char* key = m->binds[p][b][0] ? m->binds[p][b] : ui_text("(unbound)");
                const char* alt = m->binds_alt[p][b];
                const bool has_alt = alt && alt[0] && strcmp(alt, "(unbound)") != 0;
                r.value = cap ? (m->capture_slot == 1 ? ui_text("[ press an alternate ]") : ui_text("[ press a key ]"))
                              : has_alt ? std::string(key) + "  /  " + alt : std::string(key);
                r.help = std::string(spec.buttons[b].label) + "\n" +
                         ui_text("{OK} sets the key; {ALT} (or right click) sets an alternate key or mouse button. "
                                 "Esc cancels; Backspace clears it.");
                r.act = [m, b]() { m->map_all_active = false; launcher_model_begin_capture_slot(m, b, 0); };
                r.alt = [m, b]() { m->map_all_active = false; launcher_model_begin_capture_slot(m, b, 1); };
            }
            s.rows.push_back(r);
        }
        secs.push_back(s);
    }

    /* Device: source, the saved gamepad profile and the stick deadzone. */
    {
        XgSection s; s.name = ui_text("Device");
        XgRow src; src.label = ui_text("Input source"); src.value = launcher_model_player_src_label(m, p);
        src.help = ui_text("Which keyboard or controller plays as this player.");
        src.change = [m, p](int dir) {
            const auto v = xg_sources(m, p);
            if (v.empty()) return;
            int cur = 0;
            for (size_t i = 0; i < v.size(); ++i) if (v[i].selected) cur = (int)i;
            const int n = (int)v.size();
            xg_apply_source(m, p, v[(cur + dir + n) % n]);
        };
        s.rows.push_back(src);
        XgRow rn; rn.kind = XR_ACTION; rn.label = ui_text("Rename gamepad\xE2\x80\xA6");
        rn.disabled = !pad;
        rn.value = pad ? m->player_pad_name[p] : "";
        rn.help = pad ? ui_text("Give this controller a name of your own.") : ui_text("Choose a gamepad as the input source first.");
        rn.act = [m, p]() {
            snprintf(X.rename_buf, sizeof(X.rename_buf), "%s", m->player_pad_name[p]);
            X.rename_open = true;
        };
        s.rows.push_back(rn);
        XgRow dz; dz.label = ui_text("Deadzone");
        char b[16]; snprintf(b, sizeof(b), "%d%%", m->s.deadzone[p]); dz.value = b;
        dz.help = ui_text("How far a stick must move before it counts. Raise it if the stick drifts at rest.");
        dz.change = [m, p](int dir) {
            const int hi = m->s.deadzone[p] > 50 ? 100 : 50;
            launcher_model_set_deadzone(m, p, std::max(0, std::min(hi, m->s.deadzone[p] + dir)));
        };
        s.rows.push_back(dz);
        if (m->has_mouse_controls && p == 0) {
            XgRow ms; ms.label = ui_text("Mouse sensitivity");
            char mb[16]; snprintf(mb, sizeof(mb), "%.2f", m->s.mouse_sensitivity); ms.value = mb;
            ms.change = [m](int dir) {
                float v = m->s.mouse_sensitivity + dir * 0.01f;
                launcher_model_set_mouse_sensitivity(m, std::max(0.01f, std::min(0.50f, v)));
            };
            s.rows.push_back(ms);
            s.rows.push_back(xg_toggle_row("Invert mouse X", m->s.mouse_invert_x != 0, nullptr,
                                           [m] { launcher_model_toggle_mouse_invert_x(m); }));
            s.rows.push_back(xg_toggle_row("Invert mouse Y", m->s.mouse_invert_y != 0, nullptr,
                                           [m] { launcher_model_toggle_mouse_invert_y(m); }));
        }
        XgRow del; del.kind = XR_ACTION; del.label = ui_text("Forget this gamepad");
        del.disabled = !pad;
        del.help = pad ? ui_text("Delete the saved profile of this controller (name and mapping).")
                       : ui_text("Choose a gamepad as the input source first.");
        del.act = [m, p]() { launcher_binds_delete_psx_gamepad(m, p + 1); xg_note(ui_text("Gamepad profile deleted.")); };
        s.rows.push_back(del);
        secs.push_back(s);
    }

    /* Shortcuts: launcher-wide actions (fast-forward…), key and pad each. */
    if (m->assist_binding_count > 0) {
        XgSection s; s.name = ui_text("Shortcuts");
        for (int a = 0; a < m->assist_binding_count; ++a) {
            XgRow r; r.kind = XR_ACTION; r.label = m->assist_binding_labels[a];
            const bool ck = m->capturing && m->capture_assist && !m->capture_pad && m->capture_btn == a;
            const bool cp = m->capturing && m->capture_assist && m->capture_pad && m->capture_btn == a;
            char pad_l[48]; settings_pad_label(m->s.assist_pad_bind[a], pad_l, sizeof pad_l);
            r.value = std::string(ck ? ui_text("[ key ]") : settings_key_label(m->s.assist_key_bind[a])) + "  /  " +
                      (cp ? ui_text("[ button ]") : pad_l);
            r.help = ui_text("{OK} sets the key, {ALT} sets the controller button. Esc cancels; Backspace clears it.");
            r.act = [m, a]() { launcher_model_begin_assist_capture(m, a, false); };
            r.alt = [m, a]() { launcher_model_begin_assist_capture(m, a, true); };
            s.rows.push_back(r);
        }
        secs.push_back(s);
    }

    /* Camera: only while a mod with free-camera controls is enabled. */
    if (p == 0 && enabled_camera_controls(m)) {
        static const char* kActionLabels[LNG_CAMERA_BIND_COUNT] = {
            "Look up", "Look down", "Look left", "Look right",
            "Roll left", "Roll right", "Zoom in", "Zoom out",
            "Sprites smaller", "Sprites larger", "Reset view",
            "Toggle Voxel 3D",
        };
        launcher_binds_refresh_camera(m);
        XgSection s; s.name = ui_text("Camera");
        for (int a = 0; a < LNG_CAMERA_BIND_COUNT; ++a) {
            XgRow r; r.kind = XR_ACTION; r.label = ui_text(kActionLabels[a]);
            const bool cap = m->camera_capturing && m->capture_camera == a;
            r.value = cap ? ui_text("[ press a key ]") : m->camera_binds[a];
            r.help = ui_text("Keyboard key for this camera action. The right stick looks around on a gamepad.");
            r.act = [m, a]() { launcher_model_begin_camera_capture(m, a); };
            s.rows.push_back(r);
        }
        XgRow rs; rs.kind = XR_ACTION; rs.label = ui_text("Reset camera bindings");
        rs.act = [m]() { launcher_binds_reset_camera(m); };
        s.rows.push_back(rs);
        secs.push_back(s);
    }
    return secs;
}

static void xg_page_controls(LauncherModel* m, ImDrawList* dl, const XgLayout& L, const XgIn& in) {
    const int p = m->cfg_player;
    if (m->s.player_src[p] == 2 && m->s.player_gamepad_guid[p][0] && !m->player_pad_name[p][0])
        launcher_binds_hydrate_psx_pad_names(m);
    auto secs = xg_controls_sections(m, p);
    const int ns = (int)secs.size();
    if (X.ctl_sec >= ns) X.ctl_sec = 0;
    const bool pad = m->s.player_src[p] == 2 && m->s.player_gamepad_guid[p][0];
    const bool active = X.zone == XZ_CONTENT;
    bool switched = false;
    if (active) {
        if (in.l1 || in.r1) {
            X.ctl_sec = (X.ctl_sec + (in.r1 ? 1 : ns - 1)) % ns;
            X.ctl_row = 0; X.ctl_scroll = 0; X.bar = false; switched = true;
        } else if (in.back) {
            // Back to this player's window on the Game page.
            launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
            X.zone = XZ_CONTENT; X.win = 1 + p; X.bar = false;
            return;
        }
    }
    std::vector<std::string> names;
    for (const auto& s : secs) names.push_back(s.name);
    char note[96];
    snprintf(note, sizeof(note), "%s %d  \xC2\xB7  %s", ui_text("Player"), p + 1, launcher_model_player_src_label(m, p));
    const ImVec2 ta = L.cont_a, tb(L.cont_b.x, L.cont_a.y + xs(58));
    xg_tabs(dl, ta, tb, names, X.ctl_sec, note,
            [](int i) { X.ctl_sec = i; X.ctl_row = 0; X.ctl_scroll = 0; X.bar = false; X.zone = XZ_CONTENT; });

    /* Action bar: map everything in one go, defaults, and saving the profile. */
    std::vector<XgBarItem> bar;
    bar.push_back({ui_text("Map all"), ui_text("Set every button in turn: press the input for each one as it is asked for."),
                   [m, p, pad]() {
                       launcher_model_begin_map_all(m);
                       if (pad && m->capturing && m->player_pad_id[p] && !launcher_input_gamepad_at_rest(m->player_pad_id[p]))
                           m->map_all_wait_release = true;
                   }});
    bar.push_back({ui_text("Defaults"), ui_text("Put this player's buttons back to the defaults."),
                   [m, p]() { launcher_binds_reset_player(m, p + 1); xg_note(ui_text("Buttons reset to the defaults.")); }});
    bar.push_back({ui_text("Save profile"),
                   pad ? ui_text("Save the name, mapping and deadzone of this controller.")
                       : ui_text("Save the keyboard mapping."),
                   [m, p, pad]() {
                       if (pad) launcher_binds_save_psx_gamepad(m, p + 1);
                       else launcher_binds_save_psx_keyboard(m, p + 1);
                       xg_note(ui_text("Input profile saved."));
                   }});
    const float G = xs(14), bottom = L.cont_b.y - xg_bar_h() - G;
    if (switched) X.bar = false;
    xg_action_bar(dl, ImVec2(L.cont_a.x, bottom + G), L.cont_b, bar, active && !switched ? in : XgIn{});

    auto& rows = secs[X.ctl_sec].rows;
    const int n = (int)rows.size();
    XgIn lin = (active && !switched && !X.bar) ? in : XgIn{};
    // ↓ from the last row goes to the action bar instead of wrapping.
    if (lin.down && X.ctl_row >= n - 1) { X.bar = true; X.bar_btn = 0; lin.down = false; lin = XgIn{}; }
    if (lin.up && X.ctl_row == 0) lin.up = false;
    xg_list(dl, ImVec2(L.cont_a.x, tb.y + G), ImVec2(L.cont_b.x, bottom), rows, X.ctl_row, X.ctl_scroll,
            active && !switched && !X.bar, lin);

    if (active && !X.bar && X.ctl_row < n) {
        const XgRow& r = rows[X.ctl_row];
        g_xg_help = r.help;
        const bool has_alt = (bool)r.alt;
        const bool shortcuts = secs[X.ctl_sec].name == ui_text("Shortcuts");
        if (has_alt)
            g_xg_legend = {{XGB_UD, "Move"}, {XGB_CROSS, shortcuts ? "Key" : "Set"},
                           {XGB_TRIANGLE, shortcuts ? "Pad button" : "Alternate"}, {XGB_L1R1, "Section"},
                           {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
        else if (r.kind == XR_VALUE)
            g_xg_legend = {{XGB_UD, "Move"}, {XGB_LR, "Change"}, {XGB_L1R1, "Section"}, {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
        else
            g_xg_legend = {{XGB_UD, "Move"}, {XGB_CROSS, "Select"}, {XGB_L1R1, "Section"}, {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
    }
    // While a binding listens, say what for (input is with the capture).
    if (m->capturing || m->camera_capturing) {
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        const char* what = (!m->capture_assist && m->capture_btn >= 0 && m->capture_btn < LNG_PSX_PAD_BUTTON_COUNT)
                               ? prof->controller.buttons[m->capture_btn].label : "";
        char b[200];
        snprintf(b, sizeof(b), "%s %s%s\nEsc cancels; Backspace clears the binding.",
                 m->map_all_wait_release ? ui_text("Release the button to continue.") : ui_text("Press the new input for"),
                 m->map_all_wait_release ? "" : what, m->map_all_active ? "   (Map all)" : "");
        g_xg_help = b;
    }
}

/* ---- the frame ---------------------------------------------------------------- */
static bool xg_is_legacy_view(const LauncherModel* m) {
    return (m->view == LNG_VIEW_CONTROLLER && !xg_is_psx(m)) ||
           m->view == LNG_VIEW_ASSIST_TOOLS || m->view == LNG_VIEW_CREDITS;
}

void draw_ui_xg(LauncherModel* m, const LauncherTheme& th) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    xg_set_scale(vp->Size);
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const bool legacy = xg_is_legacy_view(m);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!legacy) wf |= ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##launcher", nullptr, wf);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The menu keeps the game's proportions: on a window taller than it
    // needs, it is centred with black above and below instead of stretched.
    ImVec2 lay_pos = vp->Pos, lay_size = vp->Size;
    {
        const float max_h = 790.0f * g_xs;
        if (lay_size.y > max_h) { lay_pos.y += (lay_size.y - max_h) * 0.5f; lay_size.y = max_h; }
    }
    const auto cmds = xg_commands(m);
    const int nc = (int)cmds.size();
    const XgLayout L = xg_layout(lay_pos, lay_size, nc - 2);
    if (X.menu >= nc) X.menu = 0;
    {   // A view change (from any path) never carries a popup or the bar focus over.
        static int s_last_view = -1;
        if (s_last_view != (int)m->view) {
            if (s_last_view != -1 && X.zone == XZ_POPUP) X.zone = XZ_CONTENT;
            X.bar = false;
            s_last_view = (int)m->view;
        }
    }
    const XgCmd page = xg_page_of_view(m->view);
    const float fs = xg_fs();
    g_xg_help.clear();
    g_xg_legend = {{XGB_UD, "Move"}, {XGB_CROSS, "Enter"}, {XGB_START, "Play"}};

    {   // First frame: assume a pad if one is connected; after that, the last used.
        static bool s_style_init = false;
        if (!s_style_init) { s_style_init = true; g_xg_last_pad = g_pad_count > 0; }
    }
    g_xg_hover_alt = g_xg_hover_alt_next;
    g_xg_hover_alt_next = false;
    XgIn in = xg_read_input(m);
    {
        const char* pad_name = nullptr;
        for (int j = 0; j < g_pad_count; ++j)
            if (m->s.player_src[0] == 2 && !strcmp(g_pads[j].guid, m->s.player_gamepad_guid[0])) pad_name = g_pads[j].name;
        if (!pad_name && g_pad_count > 0) pad_name = g_pads[0].name;
        g_xg_style = g_xg_last_pad && pad_name ? xg_pad_family(pad_name) : XPS_KEYBOARD;
    }
    if (legacy) {
        // ImGui's own nav drives the legacy page; ○ / Esc leaves it.
        const bool back = in.back;
        in = XgIn{};
        if (back) {
            // From the button mapping back to its Player window; from the
            // other plain pages back to the menu.
            X.zone = m->view == LNG_VIEW_CONTROLLER ? XZ_CONTENT : XZ_MENU;
            launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
        }
    }
    if (in.start) xg_play(m);

    /* Header. */
    xg_window(dl, L.hdr_a, L.hdr_b);
    {
        const float hh = L.hdr_b.y - L.hdr_a.y;
        const float tfs = fs * 1.25f;
        float x = L.hdr_a.x + xs(24);
        const char* name = m->game_name ? m->game_name : "Xenogears";
        xg_text(dl, ImVec2(x, L.hdr_a.y + (hh - tfs) * 0.5f), XG_WL, name, tfs);
        x += xg_tsize(name, tfs).x + xs(32);
        const float y2 = L.hdr_a.y + (hh - fs) * 0.5f;
        const std::string d = xg_disc_label(m);
        xg_text(dl, ImVec2(x, y2), XG_DIS, d.c_str()); x += xg_tsize(d.c_str()).x + xs(24);
        if (m->verify.serial[0]) { xg_text(dl, ImVec2(x, y2), XG_WL, m->verify.serial); x += xg_tsize(m->verify.serial).x + xs(24); }
        if (m->verify.region[0]) xg_text(dl, ImVec2(x, y2), XG_DIS, m->verify.region);
        ImU32 vc; const char* v = xg_verdict_text(m, &vc);
        const ImVec2 vs = xg_tsize(ui_text(v));
        xg_text(dl, ImVec2(L.hdr_b.x - xs(24) - vs.x, y2), vc, ui_text(v));
        xg_orb(dl, ImVec2(L.hdr_b.x - xs(40) - vs.x, y2 + fs * 0.5f), xs(8), m->verify.verdict == 1 || m->verify.verdict == 2);
    }

    /* Command window: the orbs step down and to the right, as in the game. */
    xg_window(dl, L.cmd_a, L.cmd_b);
    {
        const bool moved = xg_mouse_moved();
        const int ncmd = nc - 1;   // Start lives in its own window
        const float ifs = xg_cmd_fs(), r = xg_cmd_r(), step = xg_cmd_step(), dx = xg_cmd_dx();
        // Centre the staircase: its width is the widest "orb + label" plus
        // the drift of the steps; its height runs from the arrow's top to
        // the last orb's bottom.
        float block_w = 0.0f;
        for (int i = 0; i < ncmd; ++i)
            block_w = std::max(block_w, i * dx + r * 1.2f + xg_tsize(ui_text(cmds[i].label), ifs).x);
        // Centre the orbs themselves (first top to last bottom), nudged down a
        // little so the first one's arrow still clears the window's top edge.
        const float orbs_h = (ncmd - 1) * step + 2.0f * r;
        // A little left of centre: the labels trail off to the right.
        const float x0 = L.cmd_a.x + std::max(xs(16), (L.cmd_b.x - L.cmd_a.x - block_w) * 0.3f);
        const float free_h = (L.cmd_b.y - L.cmd_a.y) - orbs_h;
        // The orbs are centred; the selector may run over the window's edge, as in the game.
        const float cy0 = L.cmd_a.y + std::max(r * 1.6f, free_h * 0.5f + r);
        ImVec2 arrow_at(0, 0);
        bool arrow_on = false;
        for (int i = 0; i < ncmd; ++i) {
            const bool cur_page = cmds[i].cmd == page;
            const bool cursor = X.zone == XZ_MENU && X.menu == i;
            // The arrow stays on the page you are in while you work inside
            // it, and follows the cursor while you are in the menu.
            const bool arrow = X.zone == XZ_MENU ? cursor : cur_page;
            const ImVec2 oc(x0 + r + i * dx, cy0 + i * step);
            xg_orb(dl, oc, r, cur_page);
            if (arrow) { arrow_at = oc; arrow_on = true; }
            char id[24]; snprintf(id, sizeof(id), "##xgcmd%d", i);
            const XgHit h = X.zone == XZ_POPUP ? XgHit{}
                : xg_hit(id, ImVec2(L.cmd_a.x + xs(6), oc.y - step * 0.5f), ImVec2(L.cmd_b.x - xs(6), oc.y + step * 0.5f));
            if (h.hov && moved && !legacy) { X.zone = XZ_MENU; X.menu = i; }
            if (h.click) {
                // A click opens the page but leaves the cursor on the menu:
                // the mouse is still there, and the page takes focus when
                // the mouse (or the pad) goes into it.
                X.menu = i;
                xg_enter_command(m, cmds[i].cmd);
                if (cmds[i].cmd != XC_START && cmds[i].cmd != XC_EXIT) X.zone = XZ_MENU;
            }
        }
        // Orbs, labels, then the arrow on top of everything, as in the game.
        g_xg_black_outline = true;
        for (int i = 0; i < ncmd; ++i) {
            const ImVec2 oc(x0 + r + i * dx, cy0 + i * step);
            xg_text(dl, ImVec2(oc.x + r * 0.2f, oc.y - ifs * 0.5f), XG_WL, ui_text(cmds[i].label), ifs);
        }
        g_xg_black_outline = false;
        if (arrow_on) xg_chevron(dl, arrow_at, r);
    }

    /* Start window: the page's primary action. One flat, deliberate red
     * (the game keeps red for what matters) with a lighter top edge, and a
     * single centred group: the orb + "Start" in the menu's own scale, a
     * step larger, with the play button right under it. */
    {
        const int si = nc - 1;
        const bool cursor = X.zone == XZ_MENU && X.menu == si;
        const bool flash = X.flash_start && ImGui::GetTime() < X.flash_until;
        if (!flash) X.flash_start = false;
        const char* why = xg_play_block_reason(m);
        const XgHit h = X.zone == XZ_POPUP ? XgHit{} : xg_hit("##xgstart", L.start_a, L.start_b);
        const bool lit = cursor || h.hov;
        xg_window(dl, L.start_a, L.start_b);
        const ImVec2 ia(L.start_a.x + xs(6), L.start_a.y + xs(6)), ib(L.start_b.x - xs(6), L.start_b.y - xs(6));
        if (!why) {
            // A gradient of reds only (no grey in it, so it never turns brown):
            // deep crimson at the top to a live red at the bottom.
            const ImU32 top = lit ? IM_COL32(92, 8, 12, 255)   : IM_COL32(58, 6, 10, 255);
            const ImU32 bot = lit ? IM_COL32(200, 24, 16, 255) : IM_COL32(150, 16, 12, 255);
            dl->AddRectFilledMultiColor(ia, ib, top, top, bot, bot);
            // A soft glow behind the content so the label sits on light, not on a seam.
            const ImVec2 gc((ia.x + ib.x) * 0.5f, (ia.y + ib.y) * 0.5f);
            const float gr = std::min(ib.x - ia.x, ib.y - ia.y) * 0.55f;
            for (int k = 0; k < 6; ++k)
                dl->AddCircleFilled(gc, gr * (1.0f - k * 0.14f), IM_COL32(255, 60, 40, lit ? 10 : 7), 40);
            // Bevel: thin light top edge, dark bottom edge.
            dl->AddRectFilled(ia, ImVec2(ib.x, ia.y + std::max(1.0f, xs(1.5f))), IM_COL32(255, 110, 90, lit ? 150 : 90));
            dl->AddRectFilled(ImVec2(ia.x, ib.y - std::max(1.0f, xs(2))), ib, IM_COL32(40, 0, 0, 200));
            if (flash) dl->AddRectFilled(ia, ib, IM_COL32(255, 80, 60, 90), xs(3));
        }
        // The group: orb + label, then the button row.
        const float lfs = xg_cmd_fs() * 1.25f, orb_r = xg_cmd_r() * 1.25f;
        const char* label = ui_text("Start");
        const float tw = xg_tsize(label, lfs).x;
        const char* sub = why ? ui_text("Not ready") : ui_text("Play");
        const float sfs2 = fs * 0.85f;
        const float gw = xg_glyph(nullptr, ImVec2(0, 0), XGB_START);
        const float sw = gw + xs(8) + xg_tsize(sub, sfs2).x;
        const float head_h = orb_r * 2.0f, gap = xs(12), row_h = xs(24);
        const float group_h = head_h + gap + row_h;
        const float top = (L.start_a.y + L.start_b.y) * 0.5f - group_h * 0.5f;
        const float cx = (L.start_a.x + L.start_b.x) * 0.5f;
        const float head_w = orb_r * 1.2f + tw;
        const ImVec2 oc(cx - head_w * 0.5f + orb_r, top + orb_r);
        xg_orb(dl, oc, orb_r, cursor);
        g_xg_black_outline = true;
        xg_text(dl, ImVec2(oc.x + orb_r * 0.2f, oc.y - lfs * 0.5f), XG_WL, label, lfs);
        g_xg_black_outline = false;
        const float gx = std::max(L.start_a.x + xs(14), cx - sw * 0.5f), gy = top + head_h + gap;
        xg_glyph(dl, ImVec2(gx, gy), XGB_START);
        g_xg_black_outline = true;   // on red, the game's black rim reads cleaner than indigo
        xg_text(dl, ImVec2(gx + gw + xs(8), xg_cap_top(gy + xs(12), sfs2)), why ? XG_WARN : XG_WL, sub, sfs2);
        g_xg_black_outline = false;
        if (cursor) xg_chevron(dl, oc, orb_r);
        if (h.hov && xg_mouse_moved() && !legacy) { X.zone = XZ_MENU; X.menu = si; }
        if (h.click) xg_play(m);
    }

    /* Menu input. */
    if (X.zone == XZ_MENU && !legacy) {
        if (in.up) X.menu = (X.menu + nc - 1) % nc;
        if (in.down) X.menu = (X.menu + 1) % nc;
        // → only steps into a page; Start and Exit need the confirm button,
        // so a stray press on the d-pad never launches or quits.
        const XgCmd mc = cmds[X.menu].cmd;
        if (in.ok || (in.right && mc != XC_START && mc != XC_EXIT)) xg_enter_command(m, mc);
        const XgCmd c = cmds[X.menu].cmd;
        g_xg_help = c == XC_START ? (xg_play_block_reason(m) ? ui_text(xg_play_block_reason(m)) : ui_text("Start the game with these settings."))
                  : c == XC_GAME ? ui_text("Disc, controller and memory cards.")
                  : c == XC_CONFIG ? ui_text("Display, image, audio, system, hotkeys and launcher options.")
                  : c == XC_MODS ? ui_text("Turn mods on or off and adjust them.")
                  : c == XC_NETPLAY ? ui_text("Play online or over LAN.")
                  : c == XC_ASSIST ? ui_text("Assist tools and cheats.")
                  : c == XC_CREDITS ? ui_text("Credits.")
                  : c == XC_EXIT ? ui_text("Close the launcher.") : "";
        g_xg_legend = {{XGB_UD, "Move"}, {XGB_CROSS, "Enter"}, {XGB_START, "Play"}};
        in = XgIn{};   // consumed
    } else if (!legacy) {
        // Keep the arrow on the current page while inside it.
        for (int i = 0; i < nc; ++i) if (cmds[i].cmd == page) { X.menu = i; break; }
    }

    /* Page. */
    if (legacy) {
        xg_window(dl, L.cont_a, L.help_b);
        ImGui::SetCursorScreenPos(ImVec2(L.cont_a.x + xs(16), L.cont_a.y + xs(14)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##xglegacy", ImVec2(L.help_b.x - L.cont_a.x - xs(32), L.help_b.y - L.cont_a.y - xs(28)));
        // The plain pages keep their own fixed metrics; their text grows a
        // little with the window, never past what their buttons can hold.
        const float lfs = 18.0f * std::min(g_xs, 1.3f);
        if (g_xg_font) { ImGui::PushFont(g_xg_font); ImGui::SetWindowFontScale(lfs / g_xg_font->FontSize); }
        if (m->view == LNG_VIEW_CONTROLLER) draw_controller(m, th);
        else if (m->view == LNG_VIEW_ASSIST_TOOLS) draw_assist_tools(m, th);
        else draw_credits(m, th);
        if (g_xg_font) { ImGui::SetWindowFontScale(1.0f); ImGui::PopFont(); }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        g_xg_legend = {{XGB_DPAD, "Move"}, {XGB_CROSS, "Select"}, {XGB_CIRCLE, "Back"}, {XGB_START, "Play"}};
    } else {
        if (m->view == LNG_VIEW_CONTROLLER) xg_page_controls(m, dl, L, in);
        else if (page == XC_CONFIG) xg_page_config(m, dl, L, in);
        else if (page == XC_MODS) xg_page_mods(m, th, dl, L, in);
        else xg_page_game(m, dl, L, in);
    }

    /* Help window. */
    if (!legacy) {
        xg_window(dl, L.help_a, L.help_b);
        std::string help = xg_expand(g_xg_help);
        if (!X.note.empty() && ImGui::GetTime() < X.note_until) help = X.note;
        if (help.empty() && page == XC_GAME && X.zone == XZ_MENU) help = ui_text("Disc, controller and memory cards.");
        const float wrap = L.help_b.x - L.help_a.x - xs(48);
        dl->PushClipRect(L.help_a, L.help_b, true);
        xg_text(dl, ImVec2(L.help_a.x + xs(24), L.help_a.y + xs(18)), XG_WL, help.c_str(), fs * 0.9f, wrap);
        dl->PopClipRect();
    }

    /* Legend. */
    xg_window(dl, L.leg_a, L.leg_b);
    {
        float x = L.leg_a.x + xs(20);
        const float gy = L.leg_a.y + (L.leg_b.y - L.leg_a.y - xs(24)) * 0.5f;
        const float lfs = fs * 0.85f;
        for (const auto& it : g_xg_legend) {
            x += xg_glyph(dl, ImVec2(x, gy), it.first) + xs(8);
            xg_text(dl, ImVec2(x, xg_cap_top(gy + xs(12), lfs)), XG_WL, ui_text(it.second), lfs);
            x += xg_tsize(ui_text(it.second), lfs).x + xs(28);
        }
        const char* lvl = legacy ? "" : X.zone == XZ_MENU ? "Menu" : X.zone == XZ_POPUP ? "Options"
                        : page == XC_CONFIG ? "Settings" : page == XC_MODS ? "Mod list" : "Windows";
        if (lvl[0]) xg_text_r(dl, ImVec2(L.leg_b.x - xs(22), gy + (xs(24) - fs * 0.78f) * 0.5f), XG_DIS, ui_text(lvl), fs * 0.78f);
    }

    xg_modals(m);

    // ○ / Esc answers "Cancel" on the shared confirmation dialogs, which do
    // not handle it themselves.
    if (!ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
         (s_pad_nav_armed && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))) {
        if (m->skip_modal_open || m->defaults_modal_open) {}
        else if (m->bios_play_modal_open) launcher_model_bios_play_cancel(m);
        else if (m->bios_confirm_open) launcher_model_bios_confirm_cancel(m);
        else if (m->pgo_confirm_open) launcher_model_pgo_confirm_cancel(m);
        else if (m->fmv_timing_confirm_open) launcher_model_fmv_timing_confirm_cancel(m);
    }

    // The launcher's own yes/no questions, in the menu's look.
    {
        static bool s_skip_was = false, s_def_was = false;
        if (m->skip_modal_open) {
            const int r = xg_confirm(dl, L, ui_text("Skip the launcher on boot?"),
                ui_text("The game will start directly from now on. Run it with --launcher, or set SkipLauncher = 0 in config.ini, to see this menu again."),
                ui_text("Skip on boot"), !s_skip_was);
            if (r > 0) launcher_model_skip_confirm(m);
            else if (r < 0) launcher_model_skip_cancel(m);
        }
        s_skip_was = m->skip_modal_open;
        if (m->defaults_modal_open) {
            const int r = xg_confirm(dl, L, ui_text("Restore default settings?"),
                ui_text("Display, audio, controller and launcher choices go back to their defaults. Your disc, memory cards and saves are not changed."),
                ui_text("Restore"), !s_def_was);
            if (r > 0) launcher_model_restore_defaults(m);
            else if (r < 0) launcher_model_cancel_restore_defaults(m);
        }
        s_def_was = m->defaults_modal_open;
        static bool s_rm_was = false;
        if (X.confirm_remove) {
            const std::string body = X.rm_name + " " + X.rm_version + "\n" +
                ui_text("The package is removed from the launcher. You can install it again later.");
            const int r = xg_confirm(dl, L, ui_text("Uninstall this mod package?"), body.c_str(),
                                     ui_text("Uninstall"), !s_rm_was);
            if (r != 0) {
                X.confirm_remove = false;
                if (r > 0) {
                    if (m->mods && m->mods->remove_package &&
                        !m->mods->remove_package(m->mods->ctx, X.rm_id.c_str(), X.rm_version.c_str()))
                        mod_note_error(m);
                    else xg_note(ui_text("Package uninstalled."));
                    X.zone = XZ_CONTENT;
                    if (X.mod_row > 0) X.mod_row--;
                }
            }
        }
        s_rm_was = X.confirm_remove;
    }

    // Shared modals and pickers, exactly as the legacy frame draws them.
    draw_setup_wizard_modal(m, th);
    draw_bios_confirm_modal(m, th);
    draw_bios_play_modal(m, th);
    draw_pgo_confirm_modal(m, th);
    draw_fmv_timing_confirm_modal(m, th);
    draw_standalone_builtin_rom_picker(m, th);
    if (m->tpak_slots > 0) draw_tpak_modal(m, th);
    ImGui::End();
}
