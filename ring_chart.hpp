#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

// Semantic colours. They are resolved while drawing from the current theme
// (accent colour, light/dark), so the app needs no CSS of its own.
enum class Tone {
    Accent,      // the system accent colour
    AccentSoft,  // the accent colour, translucent
    Neutral,     // the text colour, very translucent (an "empty" slice)
    Danger,      // the theme's error/destructive red
};

struct Segment {
    double value;
    Tone tone;
    std::string tooltip = {};  // shown in a balloon while hovering the slice
};

using Ring = std::vector<Segment>;

GdkRGBA tone_to_rgba(GtkWidget* widget, Tone tone);

// Small round colour swatch, used as a legend marker.
GtkWidget* color_dot_new(Tone tone);

// Concentric-ring ("wheel") chart. The first ring is the outermost one.
// The object is owned by its widget and deleted together with it.
class RingChart {
public:
    RingChart();

    GtkWidget* widget() const { return area_; }

    void set_rings(std::vector<Ring> rings);
    void set_center_text(std::string primary, std::string secondary);

private:
    static void draw_cb(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                        gpointer user_data);
    void draw(cairo_t* cr, int width, int height) const;

    struct Hit {
        int ring = -1;
        int segment = -1;
    };
    Hit hit_test(double x, double y) const;
    void set_hover(Hit hit);

    GtkWidget* area_;
    std::vector<Ring> rings_;
    std::string primary_;
    std::string secondary_;
    Hit hover_;
};
