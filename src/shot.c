/* shot.c —— 原生 GTK4 截图标注工具（单窗口、portal 抓屏）
 *
 * 按快捷键 → portal 抓全屏 → 冻结画面窗口内框选 + 标注 → Enter 复制并保存
 * 保存: <base>/<软件名>/截图 YYYY-MM-DD HH-MM-SS.png
 */
#include <cairo.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#define APP_ID "io.github.suveng.shot"

/* snap 的 AppArmor 只给 StatusNotifierItem 这类名字 `dbus (bind)` 权限，GtkApplication
 * 默认会 RequestName 占一个 well-known name，于是启动直接死在
 * `Connection is not allowed to own the service ... due to AppArmor policy`。
 * 而且 `g_application_id_is_valid("shot-screenshot")` == 0（无点的 id 不合法），
 * 不能简单把 id 换成 snap 名。→ snap 里用 G_APPLICATION_NON_UNIQUE，不占名字。 */
static gboolean in_snap(void) {
  const char *snap = g_getenv("SNAP_INSTANCE_NAME");
  return snap && *snap;
}
#define UI 30.0
#define PAD 8.0

typedef enum { T_ARROW, T_RECT, T_PEN, T_TEXT, T_MOSAIC, T_NONE } Tool;

typedef struct {
  double x, y;
} Pt;

typedef struct {
  Tool t;
  GdkRGBA color;
  double w;
  GArray *pts;
  char *text;
  double size;
} Ann;

typedef struct {
  GtkApplication *app;
  GtkWidget *win;
  GtkWidget *area;
  GtkWidget *overlay;
  GtkWidget *entry;
  cairo_surface_t *img;
  int iw, ih;
  double scale, offx, offy;
  gboolean has_sel;
  double sx0, sy0, sx1, sy1;
  GArray *anns;
  Tool tool;
  int color_i;
  int mode; /* 0 idle, 1 selecting, 2 drawing */
  gboolean press_used;      /* 本次按下被工具栏/取色点消费，不再当双击 */
  gboolean press_moved;     /* 按下后是否拖动过（判定“点击” vs “拖”）*/
  double press_x, press_y;
  double last_click_x, last_click_y;
  gint64 last_click_us;
  gboolean has_last_click;
  Ann *preview;
  char *app_name;
  char *parent;
  gboolean smoke;
  gboolean test_dbl;   /* --test-dbl：自检双击判定（不进真实手势） */
} Shot;

static const char *COLORS[] = {"#ff3b30", "#ffcc00", "#34c759", "#0a84ff", "#ffffff"};
#define NCOLORS ((int)G_N_ELEMENTS(COLORS))

static void log_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  FILE *f = fopen("/tmp/shot.log", "a");
  if (f) {
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);
  }
  va_end(ap);
}

/* ------------------------------------------------------------ 抓屏 */

typedef struct {
  GMainLoop *loop;
  char *path;
  guint code;
  gboolean got;
} CapCtx;

static void on_portal_response(GDBusConnection *conn, const char *sender,
                               const char *path, const char *iface,
                               const char *signal, GVariant *params,
                               gpointer user) {
  (void)conn; (void)sender; (void)path; (void)iface; (void)signal;
  CapCtx *ctx = user;
  guint32 code = 2;
  GVariant *res = NULL;
  g_variant_get(params, "(u@a{sv})", &code, &res);
  ctx->code = code;
  if (code == 0 && res) {
    const char *uri = NULL;
    if (g_variant_lookup(res, "uri", "&s", &uri) && uri)
      ctx->path = g_filename_from_uri(uri, NULL, NULL);
  }
  if (res) g_variant_unref(res);
  ctx->got = TRUE;
  g_main_loop_quit(ctx->loop);
}

static gboolean on_cap_timeout(gpointer user) {
  CapCtx *ctx = user;
  if (!ctx->got) g_main_loop_quit(ctx->loop);
  return G_SOURCE_REMOVE;
}

static char *portal_capture(const char *parent) {
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    log_msg("bus: %s", err->message);
    g_clear_error(&err);
    return NULL;
  }

  CapCtx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  guint sub = g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
      "Response", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_portal_response,
      &ctx, NULL);

  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "interactive", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(FALSE));

  GVariant *ret = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Screenshot", "Screenshot",
      g_variant_new("(s@a{sv})", parent ? parent : "",
                    g_variant_builder_end(&opts)),
      NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
  if (!ret) {
    log_msg("Screenshot call failed: %s", err ? err->message : "?");
    g_clear_error(&err);
    g_main_loop_unref(ctx.loop);
    return NULL;
  }
  g_variant_unref(ret);

  guint to = g_timeout_add_seconds(20, on_cap_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_dbus_connection_signal_unsubscribe(bus, sub);
  g_main_loop_unref(ctx.loop);
  g_object_unref(bus);
  return ctx.path;
}

/* -------------------------------------------------- 焦点应用名 */

static const struct {
  const char *id;
  const char *name;
} FRIENDLY[] = {
    {"bytedance-feishu", "飞书"},  {"google-chrome", "Chrome"},
    {"chromium", "Chromium"},      {"code", "VSCode"},
    {"code-url-handler", "VSCode"},{"gnome-terminal-server", "终端"},
    {"org.gnome.console", "终端"}, {"org.gnome.ptyxis", "终端"},
    {"org.gnome.nautilus", "文件"},{"org.gnome.texteditor", "文本编辑器"},
    {"firefox", "Firefox"},        {"wechat", "微信"},
    {"utools", "uTools"},          {"kitty", "终端"},
    {"alacritty", "终端"},         {"org.gnome.calculator", "计算器"},
    {"apifox", "Apifox"},          {"snap-store", "软件"},
};

static char *sanitize(const char *in) {
  GString *s = g_string_new(NULL);
  for (const char *p = in; *p && s->len < 40; p++) {
    if ((guchar)*p < 0x20 || strchr("\\/:*?\"<>|", *p))
      continue;
    g_string_append_c(s, *p);
  }
  g_strstrip(s->str);
  if (s->len == 0) g_string_assign(s, "其他");
  return g_string_free(s, FALSE);
}

static char *focused_app(void) {
  char *raw = NULL;
  char *path = g_build_filename(g_get_user_runtime_dir(), "shot-focus", NULL);
  if (g_file_get_contents(path, &raw, NULL, NULL) && raw) {
    char *nl = strchr(raw, '\n');
    if (nl) *nl = '\0';
    g_strstrip(raw);
  }
  g_free(path);

  if (!raw || !*raw) {
    g_free(raw);
    raw = NULL;
    char *out = NULL;
    if (g_spawn_command_line_sync("sh -c 'xdotool getactivewindow 2>/dev/null'",
                                  &out, NULL, NULL, NULL) &&
        out && *out) {
      g_strstrip(out);
      char *cmd = g_strdup_printf("sh -c 'xprop -id %s WM_CLASS 2>/dev/null'", out);
      char *props = NULL;
      if (g_spawn_command_line_sync(cmd, &props, NULL, NULL, NULL) && props) {
        /* WM_CLASS(STRING) = "实例名", "类名"  取最后一个引号串 */
        char *last = NULL;
        for (char *q = props; (q = strchr(q, '"')) != NULL; q++) {
          char *close = strchr(q + 1, '"');
          if (!close) break;
          g_free(last);
          last = g_strndup(q + 1, close - q - 1);
          q = close;
        }
        if (last) raw = last;
        g_free(props);
      }
      g_free(cmd);
    }
    g_free(out);
  }
  if (!raw) return g_strdup("其他");

  char *low = g_ascii_strdown(raw, -1);
  char *result = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(FRIENDLY); i++) {
    if (g_strcmp0(low, FRIENDLY[i].id) == 0) {
      result = g_strdup(FRIENDLY[i].name);
      break;
    }
  }
  if (!result) result = sanitize(raw);
  g_free(low);
  g_free(raw);
  return result;
}

/* ------------------------------------------------------------ 标注 */

static Ann *ann_new(Tool t, const GdkRGBA *c, double w, double x, double y) {
  Ann *a = g_new0(Ann, 1);
  a->t = t;
  a->color = *c;
  a->w = w;
  a->pts = g_array_new(FALSE, FALSE, sizeof(Pt));
  Pt p = {x, y};
  g_array_append_val(a->pts, p);
  return a;
}

static void ann_free(Ann *a) {
  if (!a) return;
  if (a->pts) g_array_free(a->pts, TRUE);
  g_free(a->text);
  g_free(a);
}

static Pt ann_pt(Ann *a, guint i) { return g_array_index(a->pts, Pt, i); }

static void round_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI_2);
  cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI);
  cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
  cairo_close_path(cr);
}

static void set_color(cairo_t *cr, const GdkRGBA *c) {
  cairo_set_source_rgba(cr, c->red, c->green, c->blue, 1.0);
}

static void draw_mosaic(Shot *s, cairo_t *cr, Ann *a, double block) {
  Pt p0 = ann_pt(a, 0), p1 = ann_pt(a, a->pts->len - 1);
  double x0 = MIN(p0.x, p1.x), x1 = MAX(p0.x, p1.x);
  double y0 = MIN(p0.y, p1.y), y1 = MAX(p0.y, p1.y);
  int w = MAX(1, (int)(x1 - x0)), h = MAX(1, (int)(y1 - y0));

  cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                     MAX(1, w / (int)block),
                                                     MAX(1, h / (int)block));
  cairo_t *c = cairo_create(small);
  cairo_scale(c, 1.0 / block, 1.0 / block);
  cairo_translate(c, -x0, -y0);
  cairo_set_source_surface(c, s->img, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_BILINEAR);
  cairo_paint(c);
  cairo_destroy(c);

  cairo_save(cr);
  cairo_rectangle(cr, x0, y0, w, h);
  cairo_clip(cr);
  cairo_translate(cr, x0, y0);
  cairo_scale(cr, (double)w / cairo_image_surface_get_width(small),
              (double)h / cairo_image_surface_get_height(small));
  cairo_set_source_surface(cr, small, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_surface_destroy(small);
}

static void draw_ann(Shot *s, cairo_t *cr, Ann *a) {
  set_color(cr, &a->color);
  cairo_set_line_width(cr, a->w);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  if ((a->t == T_ARROW || a->t == T_PEN) && a->pts->len > 1) {
    Pt p0 = ann_pt(a, 0), pn = ann_pt(a, a->pts->len - 1);
    if (a->t == T_ARROW) {
      cairo_move_to(cr, p0.x, p0.y);
      cairo_line_to(cr, pn.x, pn.y);
      cairo_stroke(cr);
      double ang = atan2(pn.y - p0.y, pn.x - p0.x);
      double size = MAX(9.0, a->w * 4);
      for (int i = 0; i < 2; i++) {
        double d = i ? -0.45 : 0.45;
        cairo_move_to(cr, pn.x, pn.y);
        cairo_line_to(cr, pn.x - size * cos(ang - d), pn.y - size * sin(ang - d));
      }
      cairo_stroke(cr);
    } else {
      cairo_move_to(cr, p0.x, p0.y);
      for (guint i = 1; i < a->pts->len; i++) {
        Pt p = ann_pt(a, i);
        cairo_line_to(cr, p.x, p.y);
      }
      cairo_stroke(cr);
    }
  } else if (a->t == T_RECT && a->pts->len > 1) {
    Pt p0 = ann_pt(a, 0), p1 = ann_pt(a, a->pts->len - 1);
    cairo_rectangle(cr, MIN(p0.x, p1.x), MIN(p0.y, p1.y), fabs(p1.x - p0.x),
                    fabs(p1.y - p0.y));
    cairo_stroke(cr);
  } else if (a->t == T_TEXT && a->text) {
    cairo_set_font_size(cr, a->size);
    Pt p0 = ann_pt(a, 0);
    cairo_move_to(cr, p0.x, p0.y);
    cairo_show_text(cr, a->text);
  } else if (a->t == T_MOSAIC && a->pts->len > 1) {
    draw_mosaic(s, cr, a, 12.0);
  }
}

/* ------------------------------------------------------- 工具栏布局 */

/* 工具栏条目：kind 决定行为 */
#define K_TOOL 0
#define K_UNDO 1
#define K_COMMIT 2

typedef struct {
  int kind;
  Tool tool;
  const char *label;
  double x, y, size;
} Btn;

static void get_sel(Shot *s, double *x0, double *y0, double *x1, double *y1) {
  *x0 = MIN(s->sx0, s->sx1);
  *y0 = MIN(s->sy0, s->sy1);
  *x1 = MAX(s->sx0, s->sx1);
  *y1 = MAX(s->sy0, s->sy1);
}

static int buttons(Shot *s, Btn *out, int max) {
  if (!s->has_sel) return 0;
  double x0, y0, x1, y1;
  get_sel(s, &x0, &y0, &x1, &y1);
  double size = UI / (s->scale > 0 ? s->scale : 1.0);
  double gap = PAD / 2.0 / (s->scale > 0 ? s->scale : 1.0);
  static const int kinds[] = {K_TOOL, K_TOOL, K_TOOL, K_TOOL, K_TOOL, K_UNDO,
                              K_COMMIT};
  static const Tool tools[] = {T_ARROW, T_RECT, T_PEN, T_TEXT, T_MOSAIC};
  static const char *labels[] = {"箭", "框", "笔", "文", "马", "撤", "存"};
  int n = (int)G_N_ELEMENTS(kinds);
  if (n > max) n = max;
  double total = n * size + (n - 1) * gap;
  double bx = x1 - total;
  double by = y1 + gap * 2;
  if (by + size > s->ih) by = y1 - size - gap * 2;
  if (bx < 0) bx = 0;
  for (int i = 0; i < n; i++) {
    out[i].kind = kinds[i];
    out[i].tool = i < (int)G_N_ELEMENTS(tools) ? tools[i] : T_NONE;
    out[i].label = labels[i];
    out[i].x = bx + i * (size + gap);
    out[i].y = by;
    out[i].size = size;
  }
  return n;
}

static int hit_button(Shot *s, double ix, double iy) {  Btn b[8];
  int n = buttons(s, b, 8);
  for (int i = 0; i < n; i++)
    if (ix >= b[i].x && ix <= b[i].x + b[i].size && iy >= b[i].y &&
        iy <= b[i].y + b[i].size)
      return i;
  return -1;
}

static void do_commit(Shot *s, gboolean copy);

/* ------------------------------------------------------------ 绘制 */

static void queue_draw(Shot *s) { gtk_widget_queue_draw(s->area); }

static void draw_toolbar(Shot *s, cairo_t *cr) {
  Btn b[8];
  int n = buttons(s, b, 8);
  double sc = s->scale > 0 ? s->scale : 1.0;
  /* 本函数在 image 坐标系下调用（外面已 translate+scale），字号按 image 单位给 */
  cairo_set_font_size(cr, 13 / sc);
  for (int i = 0; i < n; i++) {
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 0.92);
    round_rect(cr, b[i].x, b[i].y, b[i].size, b[i].size, 6 / sc);
    cairo_fill(cr);
    gboolean active = (b[i].kind == K_TOOL && b[i].tool == s->tool);
    gboolean act = (b[i].kind != K_TOOL);
    if (active || act) {
      cairo_set_source_rgba(cr, active ? 0.24 : 0.3, active ? 0.55 : 0.3,
                            active ? 1.0 : 0.34, 0.95);
      round_rect(cr, b[i].x, b[i].y, b[i].size, b[i].size, 6 / sc);
      cairo_fill(cr);
    }
    cairo_text_extents_t ext;
    cairo_text_extents(cr, b[i].label, &ext);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr, b[i].x + (b[i].size - ext.width) / 2 - ext.x_bearing,
                  b[i].y + b[i].size / 2 + ext.height / 2);
    cairo_show_text(cr, b[i].label);
  }
  /* 颜色点 */
  for (int i = 0; i < NCOLORS; i++) {
    GdkRGBA c;
    gdk_rgba_parse(&c, COLORS[i]);
    double cx = b[0].x + i * b[0].size * 0.75;
    double cy = b[0].y - b[0].size * 0.45;
    set_color(cr, &c);
    cairo_arc(cr, cx, cy, 6 / sc, 0, 2 * G_PI);
    cairo_fill(cr);
    if (i == s->color_i) {
      cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
      cairo_set_line_width(cr, 1.6 / sc);
      cairo_arc(cr, cx, cy, 8 / sc, 0, 2 * G_PI);
      cairo_stroke(cr);
    }
  }
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user) {
  (void)area;
  Shot *s = user;

  cairo_set_source_rgb(cr, 0.08, 0.08, 0.09);
  cairo_paint(cr);
  if (!s->img || s->iw <= 0) {
    cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
    cairo_set_font_size(cr, 15);
    cairo_move_to(cr, w / 2.0 - 40, h / 2.0);
    cairo_show_text(cr, "准备中…");
    return;
  }

  s->scale = MIN((double)w / s->iw, (double)h / s->ih);
  s->offx = (w - s->iw * s->scale) / 2;
  s->offy = (h - s->ih * s->scale) / 2;

  cairo_save(cr);
  cairo_translate(cr, s->offx, s->offy);
  cairo_scale(cr, s->scale, s->scale);
  cairo_set_source_surface(cr, s->img, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  if (!s->has_sel) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_paint(cr);
    const char *msg = "拖动框选区域   ·   Esc 取消";
    cairo_set_font_size(cr, 15);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, msg, &ext);
    double x = (w - ext.width) / 2;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_rectangle(cr, x - 16, h / 2.0 - 26, ext.width + 32, 40);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr, x, h / 2.0);
    cairo_show_text(cr, msg);
    return;
  }

  double x0, y0, x1, y1;
  get_sel(s, &x0, &y0, &x1, &y1);
  double sc = s->scale;
  double dx0 = s->offx + x0 * sc, dy0 = s->offy + y0 * sc;
  double dx1 = s->offx + x1 * sc, dy1 = s->offy + y1 * sc;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
  cairo_rectangle(cr, 0, 0, w, dy0);
  cairo_rectangle(cr, 0, dy1, w, h - dy1);
  cairo_rectangle(cr, 0, dy0, dx0, dy1 - dy0);
  cairo_rectangle(cr, dx1, dy0, w - dx1, dy1 - dy0);
  cairo_fill(cr);

  cairo_set_line_width(cr, 1.5);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
  cairo_rectangle(cr, dx0 + 0.5, dy0 + 0.5, dx1 - dx0 - 1, dy1 - dy0 - 1);
  cairo_stroke(cr);

  char label[64];
  g_snprintf(label, sizeof label, "%d × %d", (int)(x1 - x0), (int)(y1 - y0));
  cairo_set_font_size(cr, 13);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, label, &ext);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
  cairo_rectangle(cr, dx0, dy0 - 22, ext.width + 12, 20);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
  cairo_move_to(cr, dx0 + 6, dy0 - 7);
  cairo_show_text(cr, label);

  /* 标注、预览、工具栏全都存在 image 坐标系里（to_img() 把鼠标位置换算成 image 坐标），
   * 画到屏幕上必须套同样的 translate+scale。之前漏了这一步：2x HiDPI 下
   * （截图 3840×2160、窗口 1920×1080、scale=0.5）标注被放大 2 倍甩到框外，
   * 工具栏更是画到屏幕右侧外面，根本看不见。 */
  cairo_save(cr);
  cairo_translate(cr, s->offx, s->offy);
  cairo_scale(cr, sc, sc);
  for (guint i = 0; i < s->anns->len; i++)
    draw_ann(s, cr, g_array_index(s->anns, Ann *, i));
  if (s->preview) draw_ann(s, cr, s->preview);
  draw_toolbar(s, cr);
  cairo_restore(cr);
}

/* ------------------------------------------------------------ 输出 */

static char *base_dir(void) {
  char *cfg = g_build_filename(g_get_user_config_dir(), "shot", "config.ini", NULL);
  GKeyFile *kf = g_key_file_new();
  char *base = NULL;
  if (g_key_file_load_from_file(kf, cfg, G_KEY_FILE_NONE, NULL))
    base = g_key_file_get_string(kf, "shot", "base", NULL);
  g_key_file_free(kf);
  g_free(cfg);
  if (!base || !*base) {
    g_free(base);
    base = g_build_filename(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES),
                            "截图", NULL);
  } else {
    char *exp = base;
    if (g_str_has_prefix(base, "~/"))
      base = g_build_filename(g_get_home_dir(), exp + 2, NULL);
    if (base != exp) g_free(exp);
  }
  return base;
}

static gboolean write_png(cairo_surface_t *surf, const char *path) {
  cairo_status_t st = cairo_surface_write_to_png(surf, path);
  if (st != CAIRO_STATUS_SUCCESS) {
    log_msg("write_png failed: %s", cairo_status_to_string(st));
    return FALSE;
  }
  return TRUE;
}

static void copy_clipboard(cairo_surface_t *surf) {
  GError *err = NULL;
  char *tmp = g_build_filename(g_get_tmp_dir(), "shot-clip.png", NULL);
  if (!write_png(surf, tmp)) {
    g_free(tmp);
    return;
  }
  /* g_spawn_command_line_sync 不经过 shell，`< file` 会被当成普通参数传给 wl-copy
   * （结果剪贴板里是字符串 "< /tmp/shot-clip.png"，根本不是图片），
   * 必须自己开 stdin 管道灌数据。wl-copy 会 fork 后常驻持有剪贴板，
   * 本进程退出后仍可粘贴。 */
  gchar *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(tmp, &data, &len, &err)) {
    log_msg("read clip png: %s", err->message);
    g_clear_error(&err);
    g_unlink(tmp);
    g_free(tmp);
    return;
  }
  /* 必须静默 stdout/stderr：wl-copy 会 fork 一个常驻进程持有剪贴板，
   * 若它继承了调用者的管道，启动 shot 的终端/脚本会一直等 EOF 不返回
   * （表现为“截图后命令卡住”，直到超时）。 */
  GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                        G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                        G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                    &err,
                                    "wl-copy", "--type", "image/png", NULL);
  if (!p) {
    log_msg("wl-copy spawn: %s", err->message);
    g_clear_error(&err);
  } else {
    GOutputStream *in = g_subprocess_get_stdin_pipe(p);
    if (!g_output_stream_write_all(in, data, len, NULL, NULL, &err)) {
      log_msg("wl-copy write: %s", err->message);
      g_clear_error(&err);
    }
    g_output_stream_close(in, NULL, NULL);
    g_subprocess_wait(p, NULL, NULL);
    if (g_subprocess_get_successful(p))
      log_msg("clipboard: %" G_GSIZE_FORMAT " bytes -> wl-copy", len);
    else
      log_msg("wl-copy exit=%d", g_subprocess_get_exit_status(p));
    g_object_unref(p);
  }
  g_free(data);
  g_unlink(tmp);
  g_free(tmp);
}

static void do_commit(Shot *s, gboolean copy);
static void commit_entry(Shot *s);
static void on_entry_activate(GtkEntry *e, gpointer user);

/* ------------------------------------------------------------ 交互 */

static void to_img(Shot *s, double x, double y, double *ix, double *iy) {
  *ix = (x - s->offx) / s->scale;
  *iy = (y - s->offy) / s->scale;
}

/* 双击选区内 = 复制到剪贴板并保存（对齐飞书/微信截图的肌肉记忆）
 * 用“按下→松开无位移 + 两次间隔 <400ms”自己判定，不依赖 GtkGestureClick：
 * 那个手势会和 GtkGestureDrag 抢同一个事件序列，还会把文字工具误触成输入框。 */
static gboolean try_double_click(Shot *s, double ix, double iy) {
  if (!s->has_sel) return FALSE;
  double x0, y0, x1, y1;
  get_sel(s, &x0, &y0, &x1, &y1);
  if (ix < x0 || ix > x1 || iy < y0 || iy > y1) return FALSE;
  gint64 now = g_get_monotonic_time();
  gboolean dbl = s->has_last_click && (now - s->last_click_us) <= 400000 &&
                 fabs(ix - s->last_click_x) < 24 &&
                 fabs(iy - s->last_click_y) < 24;
  if (dbl) {
    s->has_last_click = FALSE;
    log_msg("double-click -> copy+save");
    do_commit(s, TRUE);
    return TRUE;
  }
  s->has_last_click = TRUE;
  s->last_click_x = ix;
  s->last_click_y = iy;
  s->last_click_us = now;
  return FALSE;
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer user) {
  (void)g;
  Shot *s = user;
  if (s->entry) return;
  double ix, iy;
  to_img(s, x, y, &ix, &iy);
  s->press_x = ix;
  s->press_y = iy;
  s->press_moved = FALSE;
  s->press_used = FALSE;
  s->mode = 0;
  if (!s->has_sel) {
    s->mode = 1;
    s->has_sel = TRUE;
    s->sx0 = s->sx1 = ix;
    s->sy0 = s->sy1 = iy;
    return;
  }
  int idx = hit_button(s, ix, iy);
  if (idx >= 0) {
    s->press_used = TRUE;
    Btn b[8];
    buttons(s, b, 8);
    if (b[idx].kind == K_TOOL) {
      s->tool = b[idx].tool;
      queue_draw(s);
    } else if (b[idx].kind == K_UNDO) {
      if (s->anns->len > 0) {
        ann_free(g_array_index(s->anns, Ann *, s->anns->len - 1));
        g_array_set_size(s->anns, s->anns->len - 1);
        queue_draw(s);
      }
    } else if (b[idx].kind == K_COMMIT) {
      do_commit(s, TRUE);
    }
    return;
  }
  /* 颜色点 */
  Btn b[8];
  if (buttons(s, b, 8) > 0) {
    double cy = b[0].y - b[0].size * 0.45;
    if (fabs(iy - cy) <= b[0].size * 0.3)
      for (int i = 0; i < NCOLORS; i++) {
        double cx = b[0].x + i * b[0].size * 0.75;
        if (fabs(ix - cx) <= b[0].size * 0.3) {
          s->press_used = TRUE;
          s->color_i = i;
          queue_draw(s);
          return;
        }
      }
  }
  if (s->tool == T_TEXT) {
    GdkRGBA c;
    gdk_rgba_parse(&c, COLORS[s->color_i]);
    s->preview = ann_new(T_TEXT, &c, 3, ix, iy);
    s->preview->size = 22 / s->scale;
    GtkWidget *fixed = gtk_fixed_new();
    s->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->entry), "输入文字，回车确认");
    gtk_widget_set_size_request(s->entry, 240, -1);
    gtk_fixed_put(GTK_FIXED(fixed), s->entry, (int)(s->offx + ix * s->scale),
                  (int)(s->offy + iy * s->scale));
    gtk_overlay_add_overlay(GTK_OVERLAY(s->overlay), fixed);
    g_object_set_data(G_OBJECT(s->entry), "fixed", fixed);
    g_signal_connect(s->entry, "activate", G_CALLBACK(on_entry_activate), s);
    gtk_widget_grab_focus(s->entry);
    return;
  }
  GdkRGBA c;
  gdk_rgba_parse(&c, COLORS[s->color_i]);
  s->mode = 2;
  /* 线宽用 image 单位：3 个逻辑像素 = 3/scale 个 image 像素，
   * 保证屏幕预览与落盘 PNG 视觉粗细一致 */
  s->preview = ann_new(s->tool, &c, 3.0 / (s->scale > 0 ? s->scale : 1.0), ix, iy);
}

static void on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer user) {
  Shot *s = user;
  double sx, sy;
  gtk_gesture_drag_get_start_point(g, &sx, &sy);
  double ix, iy;
  to_img(s, sx + ox, sy + oy, &ix, &iy);
  if (s->press_moved == FALSE) {
    double dx = ix - s->press_x, dy = iy - s->press_y;
    if (dx * dx + dy * dy > 16) s->press_moved = TRUE; /* >4 image px 算拖拽 */
  }
  if (s->mode == 1) {
    s->sx1 = ix;
    s->sy1 = iy;
    queue_draw(s);
  } else if (s->mode == 2 && s->preview) {
    if (s->tool == T_PEN) {
      Pt p = {ix, iy};
      g_array_append_val(s->preview->pts, p);
    } else {
      Pt start = ann_pt(s->preview, 0);
      g_array_set_size(s->preview->pts, 0);
      g_array_append_val(s->preview->pts, start);
      Pt p = {ix, iy};
      g_array_append_val(s->preview->pts, p);
    }
    queue_draw(s);
  }
}

static void on_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer user) {
  Shot *s = user;
  /* 没拖动、也没点到工具栏/取色点 → 这是一次单击，交给双击判定 */
  if (!s->press_moved && !s->press_used) {
    double sx, sy, ix, iy;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    to_img(s, sx + ox, sy + oy, &ix, &iy);
    if (try_double_click(s, ix, iy)) return; /* 双击已提交并退出 */
  }
  if (s->mode == 2 && s->preview) {
    if (s->preview->pts->len > 1) {
      /* 越界裁剪到选区 */
      g_array_append_val(s->anns, s->preview);
    } else {
      ann_free(s->preview);
    }
    s->preview = NULL;
    queue_draw(s);
  } else if (s->mode == 1) {
    if (fabs(s->sx1 - s->sx0) < 8 || fabs(s->sy1 - s->sy0) < 8)
      s->has_sel = FALSE;
    queue_draw(s);
  }
  s->mode = 0;
}

static void commit_entry(Shot *s) {
  const char *txt = gtk_editable_get_text(GTK_EDITABLE(s->entry));
  if (txt && *txt) {
    g_free(s->preview->text);
    s->preview->text = g_strdup(txt);
    g_array_append_val(s->anns, s->preview);
    s->preview = NULL;
  } else if (s->preview) {
    ann_free(s->preview);
    s->preview = NULL;
  }
  GtkWidget *fixed = g_object_get_data(G_OBJECT(s->entry), "fixed");
  if (fixed) gtk_overlay_remove_overlay(GTK_OVERLAY(s->overlay), fixed);
  s->entry = NULL;
  queue_draw(s);
}

static void on_entry_activate(GtkEntry *e, gpointer user) {
  (void)e;
  commit_entry(user);
}


static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user) {
  (void)c; (void)keycode;
  Shot *s = user;
  if (s->entry) return FALSE;
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  switch (keyval) {
    case GDK_KEY_Escape:
      g_application_quit(G_APPLICATION(s->app));
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      do_commit(s, TRUE);
      return TRUE;
    case GDK_KEY_1: s->tool = T_ARROW; queue_draw(s); return TRUE;
    case GDK_KEY_2: s->tool = T_RECT; queue_draw(s); return TRUE;
    case GDK_KEY_3: s->tool = T_PEN; queue_draw(s); return TRUE;
    case GDK_KEY_4: s->tool = T_TEXT; queue_draw(s); return TRUE;
    case GDK_KEY_5: s->tool = T_MOSAIC; queue_draw(s); return TRUE;
    case GDK_KEY_c:
    case GDK_KEY_C:
      if (ctrl) { do_commit(s, TRUE); return TRUE; }
      s->color_i = (s->color_i + 1) % NCOLORS;
      queue_draw(s);
      return TRUE;
    case GDK_KEY_s:
    case GDK_KEY_S:
      if (ctrl) { do_commit(s, FALSE); return TRUE; }
      break;
    case GDK_KEY_z:
    case GDK_KEY_Z:
      if (ctrl && s->anns->len > 0) {
        ann_free(g_array_index(s->anns, Ann *, s->anns->len - 1));
        g_array_set_size(s->anns, s->anns->len - 1);
        queue_draw(s);
        return TRUE;
      }
      break;
    default:
      break;
  }
  return FALSE;
}

static void do_commit(Shot *s, gboolean copy) {
  if (!s->has_sel) return;
  double x0, y0, x1, y1;
  get_sel(s, &x0, &y0, &x1, &y1);
  int w = MAX(1, (int)(x1 - x0)), h = MAX(1, (int)(y1 - y0));
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(surf);
  cairo_set_source_surface(cr, s->img, -(int)x0, -(int)y0);
  cairo_paint(cr);
  cairo_translate(cr, -(int)x0, -(int)y0);
  for (guint i = 0; i < s->anns->len; i++)
    draw_ann(s, cr, g_array_index(s->anns, Ann *, i));
  cairo_destroy(cr);

  char *base = base_dir();
  char *dir = g_build_filename(base, s->app_name, NULL);
  g_mkdir_with_parents(dir, 0755);

  GDateTime *now = g_date_time_new_now_local();
  char *name = g_date_time_format(now, "截图 %Y-%m-%d %H-%M-%S.png");
  char *path = g_build_filename(dir, name, NULL);
  int i = 1;
  while (g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_free(path);
    char *suffix = g_strdup_printf("截图 %s-%d.png",
                                   g_date_time_format(now, "%Y-%m-%d %H-%M-%S"), i++);
    path = g_build_filename(dir, suffix, NULL);
    g_free(suffix);
  }
  write_png(surf, path);
  log_msg("saved %s", path);

  if (copy) copy_clipboard(surf);

  g_free(name);
  g_free(path);
  g_free(dir);
  g_free(base);
  g_date_time_unref(now);
  cairo_surface_destroy(surf);
  g_application_quit(G_APPLICATION(s->app));
}

/* ------------------------------------------------------------ 启动 */

static void shot_free(Shot *s) {
  for (guint i = 0; i < s->anns->len; i++)
    ann_free(g_array_index(s->anns, Ann *, i));
  g_array_free(s->anns, TRUE);
  if (s->img) cairo_surface_destroy(s->img);
  g_free(s->app_name);
  g_free(s->parent);
  g_free(s);
}

static gboolean capture_idle(gpointer user);
static void on_activate(GtkApplication *app, gpointer user) {
  Shot *s = user;
  if (s->win) return;

  s->win = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(s->win), "shot");
  gtk_window_set_decorated(GTK_WINDOW(s->win), FALSE);

  s->overlay = gtk_overlay_new();
  s->area = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->area), on_draw, s, NULL);
  gtk_widget_set_hexpand(s->area, TRUE);
  gtk_widget_set_vexpand(s->area, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(s->overlay), s->area);
  gtk_window_set_child(GTK_WINDOW(s->win), s->overlay);

  GtkGesture *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), s);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), s);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), s);
  gtk_widget_add_controller(s->area, GTK_EVENT_CONTROLLER(drag));

  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), s);
  gtk_widget_add_controller(s->win, keys);

  gtk_window_present(GTK_WINDOW(s->win));
  gtk_window_fullscreen(GTK_WINDOW(s->win));

  g_idle_add(capture_idle, s);
}

/* 窗口已 present 且获得焦点后再抓屏 —— portal 的授权对话框要求调用方在前台 */
static gboolean capture_idle(gpointer user) {
  Shot *s = user;
  char *png = portal_capture(NULL);
  if (!png) {
    g_printerr("[shot] 抓屏失败（若屏幕上有授权框，请点“允许”后重试）\n");
    log_msg("capture failed");
    g_application_quit(G_APPLICATION(s->app));
    return G_SOURCE_REMOVE;
  }
  s->img = cairo_image_surface_create_from_png(png);
  g_unlink(png);
  g_free(png);
  if (!s->img || cairo_surface_status(s->img) != CAIRO_STATUS_SUCCESS) {
    log_msg("load png failed");
    g_application_quit(G_APPLICATION(s->app));
    return G_SOURCE_REMOVE;
  }
  s->iw = cairo_image_surface_get_width(s->img);
  s->ih = cairo_image_surface_get_height(s->img);
  queue_draw(s);

  if (s->test_dbl) {
    s->has_sel = TRUE;
    s->sx0 = 200; s->sy0 = 200; s->sx1 = 1400; s->sy1 = 900;
    /* 选区外点击：不该记入双击 */
    gboolean outside = try_double_click(s, 10, 10);
    /* 选区内第一次：只记录；第二次（同一位置、间隔 <400ms）：应提交 */
    gboolean first = try_double_click(s, 600, 500);
    gboolean second = try_double_click(s, 600, 500);
    log_msg("test-dbl outside=%d first=%d second=%d", outside, first, second);
    return G_SOURCE_REMOVE;
  }
  if (s->smoke) {
    s->has_sel = TRUE;
    s->sx0 = 200; s->sy0 = 200; s->sx1 = 1400; s->sy1 = 900;
    GdkRGBA c;
    gdk_rgba_parse(&c, "#ff3b30");
    Ann *a = ann_new(T_ARROW, &c, 6, 320, 400);
    Pt p = {900, 760};
    g_array_append_val(a->pts, p);
    g_array_append_val(s->anns, a);
    g_object_set_data(G_OBJECT(s->win), "commit", GINT_TO_POINTER(1));
  }
  return G_SOURCE_REMOVE;
}

static gboolean smoke_commit(gpointer user) {
  do_commit(user, TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean smoke_timer(gpointer user) {
  Shot *sh = user;
  if (sh->smoke && sh->win) {
    smoke_commit(sh);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
  gboolean smoke = FALSE;
  for (int i = 1; i < argc; i++)
    if (g_strcmp0(argv[i], "--smoke") == 0) smoke = TRUE;
  gboolean test_dbl = FALSE;
  for (int i = 1; i < argc; i++)
    if (g_strcmp0(argv[i], "--test-dbl") == 0) test_dbl = TRUE;

  Shot *s = g_new0(Shot, 1);
  s->anns = g_array_new(FALSE, FALSE, sizeof(Ann *));
  s->tool = T_ARROW;
  s->smoke = smoke;
  s->test_dbl = test_dbl;
  s->app_name = focused_app();

  GtkApplication *app = gtk_application_new(
      APP_ID, G_APPLICATION_DEFAULT_FLAGS |
                  (in_snap() ? G_APPLICATION_NON_UNIQUE : 0));
  s->app = app;
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), s);

  /* 等窗口起来后自动提交（仅自检用） */
  g_timeout_add(1200, smoke_timer, s);

  int status = g_application_run(G_APPLICATION(app), 1, argv);
  g_object_unref(app);
  shot_free(s);
  return status;
}
