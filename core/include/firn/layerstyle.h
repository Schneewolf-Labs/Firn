#pragma once
#include <cstdint>

#include "firn/image.h"
#include "firn/json.h"
#include "firn/raster.h"

// Layer styles: effects rendered from a layer's shape at composite time
// and kept editable (Layers > Layer Styles). Ours only: the native format
// carries them in the Firn stash (docs/FORMAT.md) and the original shows
// the layer without them.
namespace firn {

struct LayerStyle {
    bool drop_shadow = false;
    Color shadow_color{0, 0, 0, 255};
    float shadow_opacity = 0.75f, shadow_offset_x = 5.0f, shadow_offset_y = 5.0f, shadow_blur = 5.0f;
    bool outer_glow = false;
    Color glow_color{255, 240, 160, 255};
    float glow_size = 8.0f, glow_opacity = 0.75f;
    bool inner_glow = false;
    Color inner_glow_color{255, 240, 160, 255};
    float inner_glow_size = 8.0f, inner_glow_opacity = 0.75f;
    bool stroke = false;
    Color stroke_color{255, 0, 0, 255};
    int stroke_width = 3;
    float stroke_opacity = 1.0f;
    bool bevel = false;
    float bevel_size = 5.0f, bevel_depth = 1.0f, bevel_angle = 315.0f;   // depth 0..3, light direction in degrees

    bool any() const { return drop_shadow || outer_glow || inner_glow || stroke || bevel; }
    // Pixels the style spreads beyond the layer's own shape.
    int reach() const;
    bool operator==(const LayerStyle& o) const;
    bool operator!=(const LayerStyle& o) const { return !(*this == o); }

    json::Value to_json() const;
    static LayerStyle from_json(const json::Value& v);
};

// Renders `src` with its style over `r` (document coordinates); `src`'s
// pixel (0, 0) sits at document (sox, soy), so a group's composite can be
// styled from a buffer covering only the part being drawn. The result is
// r-sized (pixel (0, 0) at r's origin), straight alpha. `src` must already
// cover `r` grown by st.reach() wherever those pixels exist.
Image render_layer_style(const Image& src, int sox, int soy, const LayerStyle& st, const raster::Rect& r);

}  // namespace firn
