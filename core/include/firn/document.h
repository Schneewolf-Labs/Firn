#pragma once
#include <memory>
#include <string>
#include <vector>

#include "firn/image.h"

namespace firn {

struct Layer {
    std::string name;
    bool visible = true;
    // A Background layer has no transparency: the eraser paints the
    // background colour on it instead of clearing alpha.
    bool background = false;
    float opacity = 1.0f;  // 0..1
    Image pixels;
};

// The single source of truth. The UI and scripts both read this and
// mutate it only through Commands (see commands.h).
class Document {
public:
    Document(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    size_t layer_count() const { return layers_.size(); }
    Layer& layer(size_t i) { return *layers_.at(i); }
    const Layer& layer(size_t i) const { return *layers_.at(i); }

    int active_layer() const { return active_; }
    void set_active_layer(int i);

    // Layer ordering: index 0 is the bottom of the stack.
    Layer& add_layer(std::string name, int at = -1);
    std::unique_ptr<Layer> remove_layer(size_t i);
    void insert_layer(std::unique_ptr<Layer> layer, size_t at);

    // Flatten visible layers with normal blending into one image.
    Image composite() const;

    // Bumped on every mutation; the UI uses it to know when to re-upload.
    uint64_t revision() const { return revision_; }
    void touch() { ++revision_; }

private:
    int width_;
    int height_;
    std::vector<std::unique_ptr<Layer>> layers_;
    int active_ = -1;
    uint64_t revision_ = 0;
};

}  // namespace firn
