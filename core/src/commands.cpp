#include "firn/commands.h"

#include "firn/raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace firn {

// --- CommandStack ------------------------------------------------------

void CommandStack::run(Document& doc, std::unique_ptr<Command> cmd) {
    // Running a new command discards any redo branch.
    done_.resize(cursor_);
    cmd->execute(doc);
    done_.push_back(std::move(cmd));
    cursor_ = done_.size();
}

void CommandStack::push_applied(std::unique_ptr<Command> cmd) {
    done_.resize(cursor_);
    done_.push_back(std::move(cmd));
    cursor_ = done_.size();
}

void CommandStack::undo(Document& doc) {
    if (!can_undo()) return;
    --cursor_;
    done_[cursor_]->undo(doc);
}

void CommandStack::redo(Document& doc) {
    if (!can_redo()) return;
    done_[cursor_]->execute(doc);
    ++cursor_;
}

void CommandStack::clear() {
    done_.clear();
    cursor_ = 0;
}

// --- LayerPixelCommand -------------------------------------------------

void LayerPixelCommand::execute(Document& doc) {
    Image& img = doc.layer(layer_).pixels;
    before_ = img;
    apply(img);
    raster::apply_through_mask(img, before_, doc.selection());
    doc.touch();
}

void LayerPixelCommand::undo(Document& doc) {
    doc.layer(layer_).pixels = before_;
    doc.touch();
}

// --- Pixel ops ---------------------------------------------------------

void InvertCommand::apply(Image& img) {
    uint8_t* p = img.data();
    const size_t n = img.size_bytes();
    for (size_t i = 0; i < n; i += 4) {
        p[i + 0] = 255 - p[i + 0];
        p[i + 1] = 255 - p[i + 1];
        p[i + 2] = 255 - p[i + 2];
    }
}

void FillCommand::apply(Image& img) { img.fill(color_); }

// Separable box blur on all four channels. Straight-alpha blur is not
// colour-correct at transparent edges; premultiplied comes later.
void BoxBlurCommand::apply(Image& img) {
    const int w = img.width(), h = img.height(), r = std::max(0, radius_);
    if (r == 0 || w == 0 || h == 0) return;

    std::vector<uint8_t> tmp(img.size_bytes());
    const uint8_t* src = img.data();
    uint8_t* dst = tmp.data();

    // Horizontal pass: src -> tmp
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0}, cnt = 0;
            for (int k = -r; k <= r; ++k) {
                int xx = std::clamp(x + k, 0, w - 1);
                const uint8_t* s = src + (static_cast<size_t>(y) * w + xx) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += s[c];
                ++cnt;
            }
            uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / cnt);
        }
    }
    // Vertical pass: tmp -> img
    uint8_t* out = img.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0}, cnt = 0;
            for (int k = -r; k <= r; ++k) {
                int yy = std::clamp(y + k, 0, h - 1);
                const uint8_t* s = dst + (static_cast<size_t>(yy) * w + x) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += s[c];
                ++cnt;
            }
            uint8_t* d = out + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / cnt);
        }
    }
}

void GreyscaleCommand::apply(Image& img) { raster::greyscale(img); }

void BrightnessContrastCommand::apply(Image& img) { raster::brightness_contrast(img, brightness_, contrast_); }

void GaussianBlurCommand::apply(Image& img) { raster::gaussian_blur(img, radius_); }

// --- Snapshot / geometry -----------------------------------------------

void LayerSnapshotCommand::execute(Document& doc) {
    doc.layer(layer_).pixels = after_;
    doc.touch();
}

void LayerSnapshotCommand::undo(Document& doc) {
    doc.layer(layer_).pixels = before_;
    doc.touch();
}

void FlipCommand::execute(Document& doc) {
    for (size_t i = 0; i < doc.layer_count(); ++i) raster::flip_vertical(doc.layer(i).pixels);
    doc.touch();
}

void MirrorCommand::execute(Document& doc) {
    for (size_t i = 0; i < doc.layer_count(); ++i) raster::mirror_horizontal(doc.layer(i).pixels);
    doc.touch();
}

void SelectionCommand::execute(Document& doc) {
    before_ = doc.selection();
    doc.set_selection(after_);
}

void SelectionCommand::undo(Document& doc) { doc.set_selection(before_); }

void PasteLayerCommand::execute(Document& doc) {
    prev_active_ = doc.active_layer();
    index_ = doc.active_layer() < 0 ? doc.layer_count() : static_cast<size_t>(doc.active_layer()) + 1;
    auto layer = std::make_unique<Layer>();
    layer->name = layer_name_;
    layer->pixels = pixels_;
    doc.insert_layer(std::move(layer), index_);
}

void PasteLayerCommand::undo(Document& doc) {
    doc.remove_layer(index_);
    doc.set_active_layer(prev_active_);
}

// --- Layer structure ops -----------------------------------------------

void AddLayerCommand::execute(Document& doc) {
    doc.add_layer(name_);
    index_ = doc.layer_count() - 1;
}

void AddLayerCommand::undo(Document& doc) { doc.remove_layer(index_); }

void DuplicateLayerCommand::execute(Document& doc) {
    auto copy = std::make_unique<Layer>(doc.layer(index_));
    copy->name = "Copy of " + copy->name;
    copy->background = false;
    doc.insert_layer(std::move(copy), index_ + 1);
}

void DuplicateLayerCommand::undo(Document& doc) {
    doc.remove_layer(index_ + 1);
    doc.set_active_layer(static_cast<int>(index_));
}

void PromoteBackgroundCommand::execute(Document& doc) {
    Layer& L = doc.layer(index_);
    old_name_ = L.name;
    L.background = false;
    L.name = "Raster 1";
    doc.touch();
}

void PromoteBackgroundCommand::undo(Document& doc) {
    Layer& L = doc.layer(index_);
    L.background = true;
    L.name = old_name_;
    doc.touch();
}

std::string MergeLayersCommand::name() const {
    switch (kind_) {
        case Kind::Down: return "Merge Down";
        case Kind::Visible: return "Merge Visible";
        default: return "Merge All (Flatten)";
    }
}

void MergeLayersCommand::execute(Document& doc) {
    before_ = doc.clone_layers();
    before_active_ = doc.active_layer();
    std::vector<Layer> out;

    if (kind_ == Kind::Down) {
        if (index_ == 0 || index_ >= doc.layer_count()) return;
        // Both layers composited against nothing; the lower keeps its identity.
        Layer merged = doc.layer(index_ - 1);
        merged.pixels = doc.composite_range(index_ - 1, index_);
        merged.opacity = 1.0f;
        merged.blend = BlendMode::Normal;
        merged.visible = true;
        for (size_t i = 0; i < doc.layer_count(); ++i) {
            if (i == index_) continue;
            out.push_back(i == index_ - 1 ? merged : doc.layer(i));
        }
        doc.replace_layers(out, static_cast<int>(index_ - 1));
        return;
    }

    Image flat = doc.composite();
    if (kind_ == Kind::All) {
        // A flattened image is a Background layer, which has no transparency.
        Layer bg;
        bg.name = "Background";
        bg.background = true;
        bg.pixels = Image(doc.width(), doc.height(), {255, 255, 255, 255});
        for (int y = 0; y < doc.height(); ++y)
            for (int x = 0; x < doc.width(); ++x) raster::blend_over(bg.pixels, x, y, flat.get(x, y), 1.0f);
        out.push_back(std::move(bg));
        doc.replace_layers(out, 0);
        return;
    }

    // Visible: the merged layer takes the place of the lowest visible layer.
    int first_visible = -1;
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const Layer& L = doc.layer(i);
        if (L.visible) {
            if (first_visible < 0) {
                first_visible = static_cast<int>(out.size());
                Layer merged;
                merged.name = "Merged";
                merged.pixels = flat;
                out.push_back(std::move(merged));
            }
        } else {
            out.push_back(L);
        }
    }
    if (first_visible < 0) return;  // nothing visible, nothing to do
    doc.replace_layers(out, first_visible);
}

// --- Geometry ----------------------------------------------------------

void GeometryCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    Document::State after;
    after.active = before_.active;
    transform(before_, after);
    doc.restore(after);
}

namespace {
// Fill fully transparent pixels of a Background layer with the fill colour.
void fill_transparent(Image& img, Color fill) {
    uint8_t* p = img.data();
    for (size_t i = 0; i < img.size_bytes(); i += 4)
        if (p[i + 3] == 0) { p[i] = fill.r; p[i + 1] = fill.g; p[i + 2] = fill.b; p[i + 3] = 255; }
}
}  // namespace

void CropCommand::transform(const Document::State& in, Document::State& out) {
    const raster::Rect r = rect_.clipped(in.width, in.height);
    out.width = r.x1 - r.x0;
    out.height = r.y1 - r.y0;
    for (const Layer& L : in.layers) {
        Layer n = L;
        n.pixels = raster::crop(L.pixels, r);
        out.layers.push_back(std::move(n));
    }
    if (!in.selection.empty()) {
        Mask m(out.width, out.height);
        for (int y = 0; y < out.height; ++y)
            std::memcpy(m.data() + static_cast<size_t>(y) * out.width, in.selection.data() + static_cast<size_t>(y + r.y0) * in.width + r.x0, out.width);
        out.selection = std::move(m);
    }
}

void ResizeCommand::transform(const Document::State& in, Document::State& out) {
    out.width = w_;
    out.height = h_;
    for (const Layer& L : in.layers) {
        Layer n = L;
        n.pixels = raster::resample(L.pixels, w_, h_, filter_);
        out.layers.push_back(std::move(n));
    }
    if (!in.selection.empty()) {
        Mask m(w_, h_);
        raster::resample_mask(in.selection.data(), in.width, in.height, m.data(), w_, h_);
        out.selection = std::move(m);
    }
}

void CanvasSizeCommand::transform(const Document::State& in, Document::State& out) {
    out.width = w_;
    out.height = h_;
    // Crop with a rect that may extend outside the source: outside is transparent.
    const raster::Rect r{-ox_, -oy_, -ox_ + w_, -oy_ + h_};
    for (const Layer& L : in.layers) {
        Layer n = L;
        n.pixels = raster::crop(L.pixels, r);
        if (L.background) fill_transparent(n.pixels, fill_);
        out.layers.push_back(std::move(n));
    }
    if (!in.selection.empty()) {
        Mask m(w_, h_);
        const raster::Rect c = r.clipped(in.width, in.height);
        for (int y = c.y0; y < c.y1; ++y)
            std::memcpy(m.data() + static_cast<size_t>(y - r.y0) * w_ + (c.x0 - r.x0),
                        in.selection.data() + static_cast<size_t>(y) * in.width + c.x0, c.x1 - c.x0);
        out.selection = std::move(m);
    }
}

void RotateCommand::transform(const Document::State& in, Document::State& out) {
    float d = std::fmod(degrees_, 360.0f);
    if (d < 0) d += 360.0f;
    const bool quarter = std::fmod(d, 90.0f) == 0.0f;
    const int q = static_cast<int>(d / 90.0f);
    if (quarter) {
        out.width = (q % 2) ? in.height : in.width;
        out.height = (q % 2) ? in.width : in.height;
    } else {
        raster::rotated_size(in.width, in.height, d, &out.width, &out.height);
    }
    for (const Layer& L : in.layers) {
        Layer n = L;
        n.pixels = quarter ? raster::rotate_quarter(L.pixels, q) : raster::rotate(L.pixels, d);
        if (!quarter && L.background) fill_transparent(n.pixels, fill_);
        out.layers.push_back(std::move(n));
    }
    if (!in.selection.empty()) {
        Image tmp(in.width, in.height);
        for (size_t i = 0; i < in.selection.size(); ++i) {
            uint8_t* p = tmp.data() + i * 4;
            p[0] = p[1] = p[2] = in.selection.data()[i]; p[3] = 255;
        }
        Image r = quarter ? raster::rotate_quarter(tmp, q) : raster::rotate(tmp, d);
        Mask m(out.width, out.height);
        for (size_t i = 0; i < m.size(); ++i) m.data()[i] = static_cast<uint8_t>(r.data()[i * 4] * r.data()[i * 4 + 3] / 255);
        out.selection = std::move(m);
    }
}

void RemoveLayerCommand::execute(Document& doc) { removed_ = doc.remove_layer(index_); }

void RemoveLayerCommand::undo(Document& doc) { doc.insert_layer(std::move(removed_), index_); }

}  // namespace firn
