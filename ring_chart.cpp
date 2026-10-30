#include "ring_chart.hpp"

#include <adwaita.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kTwoPi = 2.0 * M_PI;

// Ring geometry shared by drawing and hit-testing.
constexpr double kThicknessRatio = 0.15;
constexpr double kRingGap = 6.0;     // space between rings, in pixels
constexpr double kSegmentGap = 4.0;  // space between slices of a ring, in pixels
constexpr double kHoverGrow = 4.0;   // extra stroke width of the hovered slice

void on_theme_changed(GObject*, GParamSpec*, gpointer widget) {
    gtk_widget_queue_draw(GTK_WIDGET(widget));
}

// Redraw when the dark mode or the accent colour changes.
// (The connection is dropped automatically when `widget` is destroyed.)
void redraw_on_theme_change(GtkWidget* widget) {
    AdwStyleManager* manager = adw_style_manager_get_default();
    g_signal_connect_object(manager, "notify::dark", G_CALLBACK(on_theme_changed), widget,
                            static_cast<GConnectFlags>(0));
    g_signal_connect_object(manager, "notify::accent-color", G_CALLBACK(on_theme_changed),
                            widget, static_cast<GConnectFlags>(0));
}

void draw_dot(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
    const Tone tone = static_cast<Tone>(GPOINTER_TO_INT(data));
    const GdkRGBA color = tone_to_rgba(GTK_WIDGET(area), tone);

    gdk_cairo_set_source_rgba(cr, &color);
    cairo_arc(cr, width / 2.0, height / 2.0, std::min(width, height) / 2.0 - 2.0, 0.0, kTwoPi);
    cairo_fill(cr);
}

}  // namespace

GdkRGBA tone_to_rgba(GtkWidget* widget, Tone tone) {
    GdkRGBA color = {0.208f, 0.518f, 0.894f, 1.0f};  // Adwaita blue, used as a fallback

    if (tone == Tone::Neutral) {
        gtk_widget_get_color(widget, &color);
        color.alpha *= 0.18f;
        return color;
    }

    // libadwaita exposes its colours as named colours of the stylesheet.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkStyleContext* context = gtk_widget_get_style_context(widget);
    GdkRGBA named;
    if (tone == Tone::Danger) {
        color = {0.753f, 0.110f, 0.157f, 1.0f};  // Adwaita red, used as a fallback
        if (gtk_style_context_lookup_color(context, "error_color", &named)) color = named;
        return color;
    }
    if (gtk_style_context_lookup_color(context, "accent_bg_color", &named)) color = named;
    G_GNUC_END_IGNORE_DEPRECATIONS

    if (tone == Tone::AccentSoft) color.alpha = 0.40f;
    return color;
}

GtkWidget* color_dot_new(Tone tone) {
    GtkWidget* area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 16);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 16);
    gtk_widget_set_halign(area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_dot,
                                   GINT_TO_POINTER(static_cast<int>(tone)), nullptr);
    redraw_on_theme_change(area);
    return area;
}

RingChart::RingChart() : area_(gtk_drawing_area_new()) {
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area_), 240);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area_), 240);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), draw_cb, this, nullptr);
    redraw_on_theme_change(area_);

    // Hovering a slice highlights it and shows its tooltip balloon.
    gtk_widget_set_has_tooltip(area_, TRUE);
    g_signal_connect(area_, "query-tooltip",
                     G_CALLBACK(+[](GtkWidget*, int x, int y, gboolean, GtkTooltip* tooltip, gpointer data) -> gboolean {
                         auto* self = static_cast<RingChart*>(data);
                         const Hit hit = self->hit_test(x, y);
                         if (hit.ring < 0) return FALSE;
                         const std::string& text = self->rings_[hit.ring][hit.segment].tooltip;
                         if (text.empty()) return FALSE;
                         gtk_tooltip_set_text(tooltip, text.c_str());
                         return TRUE;
                     }),
                     this);

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion",
                     G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, gpointer data) {
                         auto* self = static_cast<RingChart*>(data);
                         self->set_hover(self->hit_test(x, y));
                     }),
                     this);
    g_signal_connect(motion, "leave",
                     G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
                         static_cast<RingChart*>(data)->set_hover({});
                     }),
                     this);
    gtk_widget_add_controller(area_, motion);

    // The widget owns this object.
    g_object_set_data_full(G_OBJECT(area_), "ring-chart", this,
                           [](gpointer p) { delete static_cast<RingChart*>(p); });
}

void RingChart::set_rings(std::vector<Ring> rings) {
    rings_ = std::move(rings);
    hover_ = {};
    gtk_widget_queue_draw(area_);
}

void RingChart::set_hover(Hit hit) {
    if (hit.ring == hover_.ring && hit.segment == hover_.segment) return;
    hover_ = hit;
    gtk_widget_queue_draw(area_);
    gtk_widget_trigger_tooltip_query(area_);
}

// Finds the slice under (x, y), mirroring the layout used by draw().
RingChart::Hit RingChart::hit_test(double x, double y) const {
    const double width = gtk_widget_get_width(area_);
    const double height = gtk_widget_get_height(area_);
    const double dx = x - width / 2.0;
    const double dy = y - height / 2.0;
    const double distance = std::hypot(dx, dy);

    const double outer = std::min(width, height) / 2.0 - 2.0;
    const double thickness = outer * kThicknessRatio;

    // Angle clockwise from 12 o'clock, in [0, 2*pi).
    double angle = std::atan2(dy, dx) + M_PI / 2.0;
    if (angle < 0.0) angle += kTwoPi;

    for (std::size_t i = 0; i < rings_.size(); ++i) {
        const double radius = outer - thickness / 2.0 - static_cast<double>(i) * (thickness + kRingGap);
        if (std::abs(distance - radius) > thickness / 2.0) continue;

        double total = 0.0;
        for (const Segment& s : rings_[i])
            if (s.value > 0.0) total += s.value;
        if (total <= 0.0) return {};

        double start = 0.0;
        for (std::size_t j = 0; j < rings_[i].size(); ++j) {
            const Segment& s = rings_[i][j];
            if (s.value <= 0.0) continue;
            const double sweep = kTwoPi * s.value / total;
            if (angle >= start && angle < start + sweep) return {static_cast<int>(i), static_cast<int>(j)};
            start += sweep;
        }
        return {};
    }
    return {};
}

void RingChart::set_center_text(std::string primary, std::string secondary) {
    primary_ = std::move(primary);
    secondary_ = std::move(secondary);
    gtk_widget_queue_draw(area_);
}

void RingChart::draw_cb(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer user_data) {
    static_cast<const RingChart*>(user_data)->draw(cr, width, height);
}

void RingChart::draw(cairo_t* cr, int width, int height) const {
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    const double outer = std::min(width, height) / 2.0 - 2.0;

    const double thickness = outer * kThicknessRatio;

    cairo_set_line_width(cr, thickness);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);

    for (std::size_t i = 0; i < rings_.size(); ++i) {
        const Ring& ring = rings_[i];
        const double radius = outer - thickness / 2.0 - static_cast<double>(i) * (thickness + kRingGap);

        double total = 0.0;
        int visible = 0;
        for (const Segment& s : ring) {
            if (s.value > 0.0) {
                total += s.value;
                ++visible;
            }
        }

        // Empty ring: draw just the track.
        if (total <= 0.0) {
            const GdkRGBA track = tone_to_rgba(area_, Tone::Neutral);
            gdk_cairo_set_source_rgba(cr, &track);
            cairo_new_path(cr);
            cairo_arc(cr, cx, cy, radius, 0.0, kTwoPi);
            cairo_stroke(cr);
            continue;
        }

        const double gap = visible > 1 ? kSegmentGap / radius : 0.0;
        double start = -M_PI / 2.0;  // 12 o'clock

        for (std::size_t j = 0; j < ring.size(); ++j) {
            const Segment& s = ring[j];
            if (s.value <= 0.0) continue;

            const double sweep = kTwoPi * s.value / total;
            const double a0 = start + gap / 2.0;
            const double a1 = start + sweep - gap / 2.0;
            start += sweep;
            if (a1 <= a0) continue;

            const bool hovered = static_cast<int>(i) == hover_.ring && static_cast<int>(j) == hover_.segment;
            const GdkRGBA color = tone_to_rgba(area_, s.tone);
            gdk_cairo_set_source_rgba(cr, &color);
            cairo_set_line_width(cr, hovered ? thickness + kHoverGrow : thickness);
            cairo_new_path(cr);
            cairo_arc(cr, cx, cy, radius, a0, a1);
            cairo_stroke(cr);
        }
    }

    // Text in the middle.
    if (primary_.empty() && secondary_.empty()) return;

    gchar* primary = g_markup_escape_text(primary_.c_str(), -1);
    gchar* secondary = g_markup_escape_text(secondary_.c_str(), -1);
    gchar* markup = g_strdup_printf(
        "<span size=\"x-large\" weight=\"bold\">%s</span>\n<span size=\"small\" alpha=\"65%%\">%s</span>",
        primary, secondary);

    PangoLayout* layout = gtk_widget_create_pango_layout(area_, nullptr);
    pango_layout_set_markup(layout, markup, -1);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    GdkRGBA fg;
    gtk_widget_get_color(area_, &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    cairo_move_to(cr, cx - text_w / 2.0, cy - text_h / 2.0);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    g_free(markup);
    g_free(secondary);
    g_free(primary);
}
