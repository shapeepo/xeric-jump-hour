#include <pebble.h>

// Gauge path: stadium/pill shape sharing the inner pill's arc center.
//   Left side  (00→10): vertical line at x = cx - outer_R, going up
//   Top arc    (10→50): semicircle 270°→0°→90° (CW from left, over top, to right)
//   Right side (50→60): vertical line at x = cx + outer_R, going down
// All ticks point radially toward the arc center (cx, arc_cy).

#define NUM_LABELS 7

static Window *s_window;
static Layer  *s_canvas_layer;
static int     s_minutes = 0;
static int     s_hours   = 0;

static const char *s_labels[NUM_LABELS] = {"00","10","20","30","40","50","60"};

// Newton's-method integer sqrt — O(log n)
static int isqrt_i(int n) {
    if (n <= 0) return 0;
    int x = n, y = 1;
    while (x > y) { x = (x + y) / 2; y = n / x; }
    return x;
}

// Outer boundary point of the gauge path for minute index i (0–60)
static GPoint gauge_outer(int cx, int arc_cy, int outer_R, int straight_h, int i) {
    if (i <= 10) {
        // Left vertical section: i=0 is at the bottom, i=10 at the top
        return GPoint(cx - outer_R,
                      arc_cy + straight_h * (10 - i) / 10);
    } else if (i <= 50) {
        // Top semicircle: 270° (left) → 0° (top) → 90° (right), Pebble CW-from-north
        int32_t angle = DEG_TO_TRIGANGLE(270 + (i - 10) * 180 / 40);
        return GPoint(
            cx + (int)(sin_lookup(angle) * (int32_t)outer_R / TRIG_MAX_RATIO),
            arc_cy - (int)(cos_lookup(angle) * (int32_t)outer_R / TRIG_MAX_RATIO));
    } else {
        // Right vertical section: i=50 at the top, i=60 at the bottom
        return GPoint(cx + outer_R,
                      arc_cy + straight_h * (i - 50) / 10);
    }
}

#ifdef PBL_COLOR
// Pixel-perfect half-width of the pill at row ry (for rib texture)
static int pill_half_width(int R, int cy, int half_h, int ry) {
    int top_arc = cy - half_h + R;
    int bot_arc = cy + half_h - R;
    int dy;
    if      (ry < top_arc) { dy = top_arc - ry; }
    else if (ry > bot_arc) { dy = ry - bot_arc; }
    else                   { return R; }
    if (dy >= R) return -1;
    int r2 = R * R - dy * dy, x = R;
    while (x > 0 && x * x > r2) x--;
    return x;
}
#endif

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int w = bounds.size.w;
    int h = bounds.size.h;
    int cx = w / 2;

    // ── Pill geometry ──────────────────────────────────────────
    int oval_w = w * 35 / 100;
    int oval_h = h * 65 / 100;
    int oval_R = oval_w / 2;          // corner radius → true stadium
    int half_h = oval_h / 2;
    int cy     = h * 62 / 100;        // pill center / needle pivot

    // The pill's top-semicircle center — tick arc shares this point
    int arc_cy = cy - half_h + oval_R;

    // ── Gauge sizing ───────────────────────────────────────────
    // Keep "30" label on screen and clear of 10/50 labels: outer_R <= arc_cy - 32
    int outer_R    = arc_cy - 32;
    if (outer_R < 20) outer_R = 20;

    int straight_h = outer_R;

    int tick_major = 20;
    int tick_minor = 20;
    int label_gap  = 12;


    // Hour sub-dial
    int hour_r  = oval_w * 55 / 100;
    int hour_cy = cy + half_h / 2;

    // ═════════════════════════════════════════════════════════
    // 1. BACKGROUND
    // ═════════════════════════════════════════════════════════
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack));
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    // ═════════════════════════════════════════════════════════
    // 2. PILL — fill + ribs + single border
    // ═════════════════════════════════════════════════════════
    GRect oval_rect = GRect(cx - oval_w / 2, cy - half_h, oval_w, oval_h);

    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack));
    graphics_fill_rect(ctx, oval_rect, oval_R, GCornersAll);

#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorOxfordBlue);
    graphics_context_set_stroke_width(ctx, 1);
    for (int ry = oval_rect.origin.y + 2; ry < oval_rect.origin.y + oval_h - 2; ry += 3) {
        int xh = pill_half_width(oval_R, cy, half_h, ry);
        if (xh <= 1) continue;
        graphics_draw_line(ctx, GPoint(cx - xh + 1, ry), GPoint(cx + xh - 2, ry));
    }
#endif

    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorMediumAquamarine, GColorWhite));
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_round_rect(ctx, oval_rect, oval_R);

    // ═════════════════════════════════════════════════════════
    // 3. X LOGO — two thin lines, upper portion of pill
    // ═════════════════════════════════════════════════════════
    {
        int lcy = cy - half_h / 2;
        int sz  = oval_w / 7;
        if (sz < 3) sz = 3;
        graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
        graphics_context_set_stroke_width(ctx, 1);
        graphics_draw_line(ctx, GPoint(cx - sz, lcy - sz), GPoint(cx + sz, lcy + sz));
        graphics_draw_line(ctx, GPoint(cx + sz, lcy - sz), GPoint(cx - sz, lcy + sz));
    }

    // ═════════════════════════════════════════════════════════
    // 4. TICK MARKS — pill-shaped path, all pointing toward arc_cy
    // ═════════════════════════════════════════════════════════
    // Diagonal for straight sections: dy capped to (spacing-1) so no tick
    // inner-end ever overlaps the next tick's outer-end.
    int tick_sep  = (straight_h > 0) ? straight_h / 10 : 5;
    int diag_dy   = (tick_sep > 1) ? -(tick_sep - 1) : 0;
    int diag_dx   = isqrt_i(tick_major * tick_major - diag_dy * diag_dy);

    for (int i = 0; i <= 60; i++) {
        bool is_major = (i % 5 == 0);
        int  tlen     = is_major ? tick_major : tick_minor;

        GPoint outer = gauge_outer(cx, arc_cy, outer_R, straight_h, i);
        GPoint inner;

        if (i < 6) {
            // Left straight: pure diagonal
            inner = GPoint(outer.x + diag_dx, outer.y + diag_dy);
        } else if (i < 10) {
            // Left blend: interpolate from diagonal angle (i=5) to horizontal (i=10)
            int blend_dx = diag_dx + (tick_major - diag_dx) * (i - 5) / 5;
            int blend_dy = diag_dy * (10 - i) / 5;
            inner = GPoint(outer.x + blend_dx, outer.y + blend_dy);
        } else if (i <= 50) {
            // Arc: radial toward (cx, arc_cy)
            int ddx = cx     - outer.x;
            int ddy = arc_cy - outer.y;
            int mag = isqrt_i(ddx * ddx + ddy * ddy);
            if (mag == 0) continue;
            inner = GPoint(outer.x + tlen * ddx / mag,
                           outer.y + tlen * ddy / mag);
        } else if (i <= 54) {
            // Right blend: interpolate from horizontal (i=50) to diagonal angle (i=55)
            int blend_dx = -(tick_major - (tick_major - diag_dx) * (i - 50) / 5);
            int blend_dy = diag_dy * (i - 50) / 5;
            inner = GPoint(outer.x + blend_dx, outer.y + blend_dy);
        } else {
            // Right straight: pure diagonal (mirrored)
            inner = GPoint(outer.x - diag_dx, outer.y + diag_dy);
        }

        graphics_context_set_stroke_color(ctx, is_major ? PBL_IF_COLOR_ELSE(GColorMediumAquamarine, GColorWhite) : GColorWhite);
        graphics_context_set_stroke_width(ctx, is_major ? 2 : 1);
        graphics_draw_line(ctx, outer, inner);
    }

    // ═════════════════════════════════════════════════════════
    // 5. LABELS — placed radially outward from each major tick
    // ═════════════════════════════════════════════════════════
    GFont font_label = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    for (int i = 0; i < NUM_LABELS; i++) {
        int    min_i = i * 10;
        GPoint outer = gauge_outer(cx, arc_cy, outer_R, straight_h, min_i);

        // Vector from arc center to outer point (= outward direction)
        int dx = outer.x - cx;
        int dy = outer.y - arc_cy;
        int mag = isqrt_i(dx * dx + dy * dy);
        if (mag == 0) continue;

        int lcx = outer.x + label_gap * dx / mag;
        int lcy = outer.y + label_gap * dy / mag;

        // Clamp horizontally so text stays on screen
        int tx = lcx - 17;
        if (tx < 1)          tx = 1;
        if (tx + 34 > w - 1) tx = w - 1 - 34;

        GRect text_rect = GRect(tx, lcy - 11, 34, 22);
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, s_labels[i], font_label, text_rect,
                           GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }

    // ═════════════════════════════════════════════════════════
    // 6. MINUTE NEEDLE — pivot just below X logo, tip toward gauge boundary
    // ═════════════════════════════════════════════════════════
    {
        int sz       = oval_w / 7;
        if (sz < 3) sz = 3;
        int pivot_y  = cy - half_h / 2 + sz + 10;

        GPoint target = gauge_outer(cx, arc_cy, outer_R, straight_h, s_minutes);

        int dx   = target.x - cx;
        int dy   = target.y - pivot_y;
        int dist = isqrt_i(dx * dx + dy * dy);
        if (dist == 0) dist = 1;

        int reach = dist - 2;
        if (reach < 1) reach = 1;

        GPoint tip  = GPoint(cx + reach * dx / dist,
                             pivot_y + reach * dy / dist);
        GPoint tail = GPoint(cx - (reach / 6) * dx / dist,
                             pivot_y - (reach / 6) * dy / dist);

        graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorMediumAquamarine, GColorWhite));
        graphics_context_set_stroke_width(ctx, 3);
        graphics_draw_line(ctx, tail, tip);

        graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorMediumAquamarine, GColorWhite));
        graphics_fill_circle(ctx, GPoint(cx, pivot_y), 4);
    }

    // ═════════════════════════════════════════════════════════
    // 7. HOUR SUB-DIAL — metallic inset
    // ═════════════════════════════════════════════════════════
    {
        graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
        graphics_context_set_stroke_width(ctx, 4);
        graphics_draw_circle(ctx, GPoint(cx, hour_cy), hour_r);

        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, GPoint(cx, hour_cy), hour_r - 3);

        graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
        graphics_context_set_stroke_width(ctx, 1);
        graphics_draw_circle(ctx, GPoint(cx, hour_cy), hour_r - 4);

        static char hour_buf[4];
        int display_hour = s_hours % 12;
        if (display_hour == 0) display_hour = 12;
        snprintf(hour_buf, sizeof(hour_buf), "%d", display_hour);

        GFont font_hour = (hour_r >= 30)
            ? fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD)
            : (hour_r >= 17)
                ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD)
                : fonts_get_system_font(FONT_KEY_GOTHIC_14);
        int text_h = (hour_r >= 30) ? 30 : (hour_r >= 17) ? 20 : 16;
        GRect hour_rect = GRect(cx - hour_r + 4, hour_cy - text_h / 2,
                                (hour_r - 4) * 2, text_h);
        graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorMediumAquamarine, GColorWhite));
        graphics_draw_text(ctx, hour_buf, font_hour, hour_rect,
                           GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    s_minutes = tick_time->tm_min;
    s_hours   = tick_time->tm_hour;
    layer_mark_dirty(s_canvas_layer);
}

static void window_load(Window *window) {
    Layer *root   = window_get_root_layer(window);
    GRect  bounds = layer_get_bounds(root);
    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(root, s_canvas_layer);
    time_t    now = time(NULL);
    struct tm *t  = localtime(&now);
    s_minutes = t->tm_min;
    s_hours   = t->tm_hour;
}

static void window_unload(Window *window) {
    layer_destroy(s_canvas_layer);
}

static void init(void) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
        .load   = window_load,
        .unload = window_unload,
    });
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    window_stack_push(s_window, true);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
