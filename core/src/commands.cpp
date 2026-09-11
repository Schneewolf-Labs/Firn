#include "firn/commands.h"

#include "firn/raster.h"
#include "firn/raster16.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace firn {

// --- CommandStack ------------------------------------------------------

void CommandStack::run(Document& doc, std::unique_ptr<Command> cmd) {
    cmd->execute(doc);
    push_applied(std::move(cmd));
}

void CommandStack::push_applied(std::unique_ptr<Command> cmd) {
    done_.resize(cursor_);
    states_.resize(cursor_ + 1);
    states_.push_back(next_state_++);
    done_.push_back(std::move(cmd));
    cursor_ = done_.size();
    trim();
}

size_t state_bytes(const Document::State& s) {
    size_t n = s.selection.size();
    for (const Layer& L : s.layers) n += L.pixels.size_bytes() + L.mask.size() + (L.deep ? L.deep->size() * 2 : 0);
    for (const auto& a : s.alpha) n += a.mask.size();
    return n;
}

size_t CommandStack::memory_bytes() const {
    size_t n = 0;
    for (const auto& c : done_) n += c->memory_bytes();
    return n;
}

void CommandStack::trim() {
    size_t drop = 0;
    if (limit_ != 0 && done_.size() > limit_) drop = std::min(cursor_, done_.size() - limit_);
    if (memory_limit_ != 0) {
        size_t total = memory_bytes();
        for (size_t i = 0; i < drop; ++i) total -= done_[i]->memory_bytes();
        // Keep at least the newest applied entry for undo.
        while (total > memory_limit_ && drop + 1 < cursor_) { total -= done_[drop]->memory_bytes(); ++drop; }
    }
    if (drop) {
        done_.erase(done_.begin(), done_.begin() + static_cast<long>(drop));
        states_.erase(states_.begin(), states_.begin() + static_cast<long>(drop));
        cursor_ -= drop;
    }
    // Never drop the beginning of an unapplied redo chain: later commands
    // depend on those edits. Discard its far end when reducing the limit.
    if (limit_ != 0 && done_.size() > limit_) {
        done_.resize(limit_);
        states_.resize(limit_ + 1);
    }
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
    states_.assign(1, 0);
    next_state_ = 1;
    cursor_ = 0;
}

// --- LayerPixelCommand -------------------------------------------------

void LayerPixelCommand::execute(Document& doc) {
    Layer& L = doc.layer(layer_);
    if (!L.is_raster()) return;
    before_ = L.pixels;
    before_deep_ = L.deep;
    if (L.is_deep()) {
        Image16 work = *L.deep;
        if (apply16(work)) {
            if (doc.has_selection()) raster16::apply_through_mask(work, *L.deep, doc.selection());
            L.set_deep(std::move(work));
            doc.touch();
            return;
        }
        L.deep.reset();   // this operation runs at 8 bits
    }
    Image& img = L.pixels;
    apply(img);
    raster::apply_through_mask(img, before_, doc.selection());
    doc.touch();
}

void LayerPixelCommand::undo(Document& doc) {
    if (!doc.layer(layer_).is_raster()) return;
    doc.layer(layer_).pixels = before_;
    doc.layer(layer_).deep = before_deep_;
    doc.touch();
}

// --- Pixel ops ---------------------------------------------------------

bool InvertCommand::apply16(Image16& img) { raster16::invert(img); return true; }

void InvertCommand::apply(Image& img) {
    uint8_t* p = img.data();
    const size_t n = img.size_bytes();
    for (size_t i = 0; i < n; i += 4) {
        p[i + 0] = 255 - p[i + 0];
        p[i + 1] = 255 - p[i + 1];
        p[i + 2] = 255 - p[i + 2];
    }
}

bool FillCommand::apply16(Image16& img) { raster16::fill(img, color_); return true; }

void FillCommand::apply(Image& img) { img.fill(color_); }

void BoxBlurCommand::apply(Image& img) { raster::box_blur(img, radius_); }

void GrayscaleCommand::apply(Image& img) { raster::grayscale(img); }

void BrightnessContrastCommand::apply(Image& img) { raster::brightness_contrast(img, brightness_, contrast_); }

void GaussianBlurCommand::apply(Image& img) { raster::gaussian_blur(img, radius_); }


// --- Snapshot / geometry -----------------------------------------------

namespace {
// Pastes a crop back into a document-sized layer image.
void paste_rect(Image& dst, const Image& crop, const raster::Rect& r) {
    if (crop.width() == dst.width() && crop.height() == dst.height()) { dst = crop; return; }
    for (int y = r.y0; y < r.y1 && y < dst.height(); ++y)
        std::memcpy(dst.data() + (static_cast<size_t>(y) * dst.width() + r.x0) * 4, crop.data() + static_cast<size_t>(y - r.y0) * crop.width() * 4, static_cast<size_t>(crop.width()) * 4);
}
}  // namespace

LayerSnapshotCommand::LayerSnapshotCommand(size_t layer, std::string name, Image before, Image after)
    : layer_(layer), name_(std::move(name)) {
    const int w = after.width(), h = after.height();
    if (before.width() != w || before.height() != h || w == 0 || h == 0) {
        rect_ = {0, 0, w, h};
        before_ = std::move(before); after_ = std::move(after);
        return;
    }
    // Bounding box of the pixels that changed.
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    const size_t row_bytes = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y) {
        const uint8_t* a = before.data() + static_cast<size_t>(y) * row_bytes;
        const uint8_t* b = after.data() + static_cast<size_t>(y) * row_bytes;
        if (std::memcmp(a, b, row_bytes) == 0) continue;
        int lx = 0, rx = w - 1;
        while (lx < w && std::memcmp(a + static_cast<size_t>(lx) * 4, b + static_cast<size_t>(lx) * 4, 4) == 0) ++lx;
        while (rx > lx && std::memcmp(a + static_cast<size_t>(rx) * 4, b + static_cast<size_t>(rx) * 4, 4) == 0) --rx;
        x0 = std::min(x0, lx); x1 = std::max(x1, rx + 1);
        y0 = std::min(y0, y); y1 = y + 1;
    }
    if (x1 <= x0 || y1 <= y0) { rect_ = {0, 0, 0, 0}; return; }  // nothing changed
    rect_ = {x0, y0, x1, y1};
    if (x0 == 0 && y0 == 0 && x1 == w && y1 == h) { before_ = std::move(before); after_ = std::move(after); return; }
    before_ = raster::crop(before, rect_);
    after_ = raster::crop(after, rect_);
}

void LayerSnapshotCommand::execute(Document& doc) {
    Layer& L = doc.layer(layer_);
    if (L.is_deep() && !before_deep_) before_deep_ = L.deep;
    L.deep.reset();
    if (!rect_.empty()) paste_rect(L.pixels, after_, rect_);
    if (rect_.empty()) doc.touch(); else doc.touch(rect_);
}

void LayerSnapshotCommand::undo(Document& doc) {
    Layer& L = doc.layer(layer_);
    if (!rect_.empty()) paste_rect(L.pixels, before_, rect_);
    L.deep = before_deep_;
    if (rect_.empty()) doc.touch(); else doc.touch(rect_);
}

void LayerSnapshotCommand::capture_deep(Document& doc) {
    Layer& L = doc.layer(layer_);
    if (L.is_deep()) { before_deep_ = L.deep; L.deep.reset(); }
}

namespace {
void flip_mask(Mask& m, bool vertical) {
    Image tmp(m.width(), m.height());
    for (size_t i = 0; i < m.size(); ++i) tmp.data()[i * 4] = m.data()[i];
    if (vertical) raster::flip_vertical(tmp); else raster::mirror_horizontal(tmp);
    for (size_t i = 0; i < m.size(); ++i) m.data()[i] = tmp.data()[i * 4];
}
}  // namespace

void FlipCommand::execute(Document& doc) {
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        if (doc.layer(i).is_raster()) raster::flip_vertical(doc.layer(i).pixels);
        if (doc.layer(i).is_deep()) { Image16 d = *doc.layer(i).deep; raster16::flip_vertical(d); doc.layer(i).deep = std::make_shared<const Image16>(std::move(d)); }
        if (doc.layer(i).has_mask()) flip_mask(doc.layer(i).mask, true);
    }
    doc.touch();
}

void MirrorCommand::execute(Document& doc) {
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        if (doc.layer(i).is_raster()) raster::mirror_horizontal(doc.layer(i).pixels);
        if (doc.layer(i).is_deep()) { Image16 d = *doc.layer(i).deep; raster16::mirror_horizontal(d); doc.layer(i).deep = std::make_shared<const Image16>(std::move(d)); }
        if (doc.layer(i).has_mask()) flip_mask(doc.layer(i).mask, false);
    }
    doc.touch();
}

void SelectionCommand::execute(Document& doc) {
    before_ = doc.selection();
    doc.set_selection(after_);
}

void SelectionCommand::undo(Document& doc) { doc.set_selection(before_); }

void PasteLayerCommand::execute(Document& doc) {
    prev_active_ = doc.active_layer();
    int depth = 0;
    index_ = doc.layer_count();
    if (prev_active_ >= 0) {
        const Layer& a = doc.layer(prev_active_);
        index_ = a.type == LayerType::Group ? doc.group_end(prev_active_) : prev_active_ + 1;
        depth = a.depth;
    }
    auto layer = std::make_unique<Layer>();
    layer->name = layer_name_;
    layer->pixels = pixels_;
    layer->depth = depth;
    layer->floating = floating_;
    doc.insert_layer(std::move(layer), index_);
}

void PasteLayerCommand::undo(Document& doc) {
    doc.remove_layer(index_);
    doc.set_active_layer(prev_active_);
}

// --- Layer structure ops -----------------------------------------------

void AddLayerCommand::execute(Document& doc) {
    prev_active_ = doc.active_layer();
    int depth = 0;
    size_t at = doc.layer_count();
    if (prev_active_ >= 0) {
        const Layer& a = doc.layer(prev_active_);
        // Above the active layer; above a whole group if the active layer is a group.
        at = a.type == LayerType::Group ? doc.group_end(prev_active_) : prev_active_ + 1;
        depth = a.depth;
    }
    Layer& L = doc.add_layer(name_, static_cast<int>(at));
    L.depth = depth;
    index_ = at;
}

void AddLayerCommand::undo(Document& doc) {
    doc.remove_layer(index_);
    doc.set_active_layer(prev_active_);
}

void NewLayerGroupCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    if (index_ >= doc.layer_count()) return;
    const size_t end = doc.layer(index_).type == LayerType::Group ? doc.group_end(index_) : index_ + 1;
    const int depth = doc.layer(index_).depth;
    std::vector<Layer> layers = doc.clone_layers();
    for (size_t i = index_; i < end; ++i) ++layers[i].depth;
    Layer g;
    g.type = LayerType::Group;
    g.name = "Group";
    g.depth = depth;
    g.pixels = Image();
    layers.insert(layers.begin() + index_, std::move(g));
    doc.replace_layers(layers, static_cast<int>(index_));
}

void UngroupCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    if (index_ >= doc.layer_count() || doc.layer(index_).type != LayerType::Group) return;
    const size_t end = doc.group_end(index_);
    std::vector<Layer> layers = doc.clone_layers();
    for (size_t i = index_ + 1; i < end; ++i) --layers[i].depth;
    layers.erase(layers.begin() + index_);
    doc.replace_layers(layers, static_cast<int>(std::min(index_, layers.size() - 1)));
}

void VectorEditCommand::execute(Document& doc) {
    Layer& L = doc.layer(layer_);
    if (!L.is_vector()) return;
    if (!has_before_) { before_ = L.objects; has_before_ = true; }
    L.objects = after_;
    doc.rasterize_vector_layer(layer_);
}

void VectorEditCommand::undo(Document& doc) {
    Layer& L = doc.layer(layer_);
    if (!L.is_vector()) return;
    L.objects = before_;
    doc.rasterize_vector_layer(layer_);
}

void AddAdjustmentLayerCommand::execute(Document& doc) {
    prev_active_ = doc.active_layer();
    int depth = 0;
    size_t at = doc.layer_count();
    if (prev_active_ >= 0) {
        const Layer& a = doc.layer(prev_active_);
        at = a.type == LayerType::Group ? doc.group_end(prev_active_) : prev_active_ + 1;
        depth = a.depth;
    }
    Layer& L = doc.add_layer(name_, static_cast<int>(at));
    L.type = LayerType::Adjustment;
    L.depth = depth;
    L.adjustment = adj_;
    index_ = at;
    doc.set_active_layer(static_cast<int>(at));
    doc.touch();
}

void AddAdjustmentLayerCommand::undo(Document& doc) {
    doc.remove_layer(index_);
    doc.set_active_layer(prev_active_);
    doc.touch();
}

void SetAdjustmentCommand::execute(Document& doc) {
    if (layer_ < doc.layer_count() && doc.layer(layer_).is_adjustment()) { doc.layer(layer_).adjustment = after_; doc.touch(); }
}

void SetAdjustmentCommand::undo(Document& doc) {
    if (layer_ < doc.layer_count() && doc.layer(layer_).is_adjustment()) { doc.layer(layer_).adjustment = before_; doc.touch(); }
}

void AddVectorLayerCommand::execute(Document& doc) {
    prev_active_ = doc.active_layer();
    int depth = 0;
    size_t at = doc.layer_count();
    if (prev_active_ >= 0) {
        const Layer& a = doc.layer(prev_active_);
        at = a.type == LayerType::Group ? doc.group_end(prev_active_) : prev_active_ + 1;
        depth = a.depth;
    }
    Layer& L = doc.add_layer(name_, static_cast<int>(at));
    L.type = LayerType::Vector;
    L.depth = depth;
    L.pixels = Image(doc.width(), doc.height(), {0, 0, 0, 0});
    index_ = at;
}

void AddVectorLayerCommand::undo(Document& doc) {
    doc.remove_layer(index_);
    doc.set_active_layer(prev_active_);
}

void ConvertToRasterCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    Layer& L = doc.layer(index_);
    if (!L.is_vector()) return;
    doc.rasterize_vector_layer(index_);
    L.type = LayerType::Raster;
    L.objects.clear();
    doc.touch();
}

void SetMaskCommand::execute(Document& doc) {
    Layer& L = doc.layer(index_);
    before_ = L.mask;
    before_enabled_ = L.mask_enabled;
    L.mask = after_;
    L.mask_enabled = enabled_;
    doc.touch();
}

void SetMaskCommand::undo(Document& doc) {
    Layer& L = doc.layer(index_);
    L.mask = before_;
    L.mask_enabled = before_enabled_;
    doc.touch();
}

void DuplicateLayerCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    before_.active = static_cast<int>(std::min(index_, doc.layer_count() - 1));  // undo reselects the original
    if (index_ >= doc.layer_count()) return;
    const size_t end = doc.layer(index_).type == LayerType::Group ? doc.group_end(index_) : index_ + 1;
    std::vector<Layer> layers = doc.clone_layers();
    std::vector<Layer> block(layers.begin() + index_, layers.begin() + end);
    block[0].name = "Copy of " + block[0].name;
    for (Layer& l : block) l.background = false;
    layers.insert(layers.begin() + end, block.begin(), block.end());
    doc.replace_layers(layers, static_cast<int>(end));
}

namespace {
// Sibling blocks at the same depth as `index`, within its parent group.
struct Block { size_t begin, end; };
std::vector<Block> sibling_blocks(const Document& doc, size_t index, size_t* which) {
    const int depth = doc.layer(index).depth;
    const int parent = doc.parent_group(index);
    const size_t lo = parent < 0 ? 0 : parent + 1;
    const size_t hi = parent < 0 ? doc.layer_count() : doc.group_end(parent);
    std::vector<Block> out;
    for (size_t i = lo; i < hi;) {
        if (doc.layer(i).depth != depth) { ++i; continue; }
        const size_t end = doc.layer(i).type == LayerType::Group ? doc.group_end(i) : i + 1;
        if (i == index) *which = out.size();
        out.push_back({i, end});
        i = end;
    }
    return out;
}
}  // namespace

void MoveLayerCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    if (index_ >= doc.layer_count() || before_pos_ > doc.layer_count()) return;
    std::vector<Layer> layers = doc.clone_layers();
    // The block being moved: the layer, plus the run above it that belongs to
    // it when it is a group.
    size_t end = index_ + 1;
    if (layers[index_].type == LayerType::Group)
        while (end < layers.size() && layers[end].depth > layers[index_].depth) ++end;
    if (before_pos_ > index_ && before_pos_ < end) return;   // into itself
    std::vector<Layer> block(layers.begin() + index_, layers.begin() + end);
    const int shift = std::max(depth_, 0) - block.front().depth;
    for (Layer& L : block) L.depth = std::max(L.depth + shift, 0);
    layers.erase(layers.begin() + index_, layers.begin() + end);
    const size_t at = before_pos_ > index_ ? before_pos_ - (end - index_) : before_pos_;
    layers.insert(layers.begin() + at, block.begin(), block.end());
    doc.replace_layers(layers, static_cast<int>(at));
}

void ArrangeLayerCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    if (index_ >= doc.layer_count() || steps_ == 0) return;
    size_t which = 0;
    std::vector<Block> blocks = sibling_blocks(doc, index_, &which);
    const int target = std::clamp(static_cast<int>(which) + steps_, 0, static_cast<int>(blocks.size()) - 1);
    if (target == static_cast<int>(which)) return;
    std::vector<Layer> layers = doc.clone_layers();
    std::vector<Block> order = blocks;
    const Block moving = order[which];
    order.erase(order.begin() + which);
    order.insert(order.begin() + target, moving);
    std::vector<Layer> rebuilt(layers.begin(), layers.begin() + blocks.front().begin);
    size_t new_index = 0;
    for (const Block& b : order) {
        if (b.begin == moving.begin) new_index = rebuilt.size();
        rebuilt.insert(rebuilt.end(), layers.begin() + b.begin, layers.begin() + b.end);
    }
    rebuilt.insert(rebuilt.end(), layers.begin() + blocks.back().end, layers.end());
    doc.replace_layers(rebuilt, static_cast<int>(new_index));
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
        if (!doc.layer(index_).is_raster() || !doc.layer(index_ - 1).is_raster() ||
            doc.layer(index_).depth != doc.layer(index_ - 1).depth) return;
        // Both layers composited against nothing; the lower keeps its identity.
        Layer merged = doc.layer(index_ - 1);
        merged.deep.reset();   // merges are 8-bit
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
    // Members of hidden groups count as hidden; groups themselves vanish.
    int first_visible = -1;
    std::vector<bool> hidden(doc.layer_count(), false);
    for (size_t i = 0; i < doc.layer_count(); ++i)
        if (doc.layer(i).type == LayerType::Group && !doc.layer(i).visible)
            for (size_t j = i; j < doc.group_end(i); ++j) hidden[j] = true;
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const Layer& L = doc.layer(i);
        if (L.type == LayerType::Group) continue;
        if (L.visible && !hidden[i]) {
            if (first_visible < 0) {
                first_visible = static_cast<int>(out.size());
                Layer merged;
                merged.name = "Merged";
                merged.pixels = flat;
                out.push_back(std::move(merged));
            }
        } else {
            Layer kept = L;
            kept.depth = 0;
            out.push_back(std::move(kept));
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
    // Saved selections only survive size-preserving changes.
    if (after.width == before_.width && after.height == before_.height) after.alpha = before_.alpha;
    doc.restore(after);
}

namespace {
// Fill fully transparent pixels of a Background layer with the fill color.
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
        if (L.is_raster()) n.pixels = raster::crop(L.pixels, r);
        if (L.is_deep()) n.deep = std::make_shared<const Image16>(raster16::crop(*L.deep, r));
        if (L.has_mask()) { Mask m(out.width, out.height); for (int y = 0; y < out.height; ++y) for (int x = 0; x < out.width; ++x) { const int sx = x + r.x0, sy = y + r.y0; m.at(x, y) = (sx >= 0 && sy >= 0 && sx < in.width && sy < in.height) ? L.mask.at(sx, sy) : 255; } n.mask = std::move(m); }
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
        if (L.is_raster()) n.pixels = raster::resample(L.pixels, w_, h_, filter_);
        if (L.is_deep()) n.deep = std::make_shared<const Image16>(raster16::resample(*L.deep, w_, h_, filter_));
        if (L.has_mask()) { Mask m(w_, h_); raster::resample_mask(L.mask.data(), in.width, in.height, m.data(), w_, h_); n.mask = std::move(m); }
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
        if (L.is_raster()) {
            n.pixels = raster::crop(L.pixels, r);
            if (L.background) fill_transparent(n.pixels, fill_);
        }
        if (L.is_deep()) {
            Image16 d = raster16::crop(*L.deep, r);
            if (L.background) { uint16_t* q = d.data(); for (size_t i = 0; i < d.size(); i += 4) if (q[i + 3] == 0) { q[i] = static_cast<uint16_t>(fill_.r * 257); q[i + 1] = static_cast<uint16_t>(fill_.g * 257); q[i + 2] = static_cast<uint16_t>(fill_.b * 257); q[i + 3] = 65535; } }
            n.deep = std::make_shared<const Image16>(std::move(d));
        }
        if (L.has_mask()) { Mask m(w_, h_, 255); const raster::Rect c = r.clipped(in.width, in.height); for (int y = c.y0; y < c.y1; ++y) for (int x = c.x0; x < c.x1; ++x) m.at(x - r.x0, y - r.y0) = L.mask.at(x, y); n.mask = std::move(m); }
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

void WarpLayersCommand::transform(const Document::State& in, Document::State& out) {
    const raster::Rect crop = crop_.empty() ? raster::Rect{0, 0, in.width, in.height} : crop_.clipped(in.width, in.height);
    out.width = crop.x1 - crop.x0;
    out.height = crop.y1 - crop.y0;
    // Fold the crop into the homography: destination coordinates shift by the crop origin.
    float H[9];
    for (int i = 0; i < 9; ++i) H[i] = H_[i];
    H[2] -= crop.x0 * H[8]; H[5] -= crop.y0 * H[8];
    H[0] -= crop.x0 * H[6]; H[1] -= crop.x0 * H[7];
    H[3] -= crop.y0 * H[6]; H[4] -= crop.y0 * H[7];
    auto warp_mask = [&](const Mask& src, uint8_t outside) {
        Image tmp(in.width, in.height);
        for (size_t i = 0; i < src.size(); ++i) { uint8_t* p = tmp.data() + i * 4; p[0] = p[1] = p[2] = src.data()[i]; p[3] = 255; }
        Image r = raster::warp(tmp, H, out.width, out.height);
        Mask m(out.width, out.height);
        for (size_t i = 0; i < m.size(); ++i) m.data()[i] = r.data()[i * 4 + 3] ? r.data()[i * 4] : outside;
        return m;
    };
    for (size_t li = 0; li < in.layers.size(); ++li) {
        const Layer& L = in.layers[li];
        Layer n = L;
        const bool touch = layer_ < 0 || static_cast<int>(li) == layer_;
        if (touch) n.deep.reset();   // warps run at 8 bits
        else if (L.is_deep() && !crop_.empty()) n.deep = std::make_shared<const Image16>(raster16::crop(*L.deep, crop));
        if (L.is_raster() && !L.pixels.empty()) {
            if (touch) {
                n.pixels = raster::warp(L.pixels, H, out.width, out.height);
                if (L.background) fill_transparent(n.pixels, fill_);
            } else if (!crop_.empty()) {
                n.pixels = raster::crop(L.pixels, crop);
            }
        } else if (L.is_vector() || L.is_adjustment()) {
            n.pixels = L.pixels.empty() ? Image() : Image(out.width, out.height, {0, 0, 0, 0});
        }
        if (L.has_mask()) n.mask = (touch || layer_ < 0) ? warp_mask(L.mask, 255) : warp_mask(L.mask, 255);
        out.layers.push_back(std::move(n));
    }
    if (!in.selection.empty()) out.selection = warp_mask(in.selection, 0);
    for (const auto& a : in.alpha) out.alpha.push_back({a.name, warp_mask(a.mask, 0)});
    out.active = in.active;
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
    auto rotate_mask = [&](const Mask& src) {
        Image tmp(in.width, in.height);
        for (size_t i = 0; i < src.size(); ++i) { uint8_t* p = tmp.data() + i * 4; p[0] = p[1] = p[2] = src.data()[i]; p[3] = 255; }
        Image r = quarter ? raster::rotate_quarter(tmp, q) : raster::rotate(tmp, d);
        Mask m(out.width, out.height);
        for (size_t i = 0; i < m.size(); ++i) m.data()[i] = r.data()[i * 4 + 3] ? r.data()[i * 4] : 255;
        return m;
    };
    for (const Layer& L : in.layers) {
        Layer n = L;
        if (L.is_raster()) {
            n.pixels = quarter ? raster::rotate_quarter(L.pixels, q) : raster::rotate(L.pixels, d);
            if (!quarter && L.background) fill_transparent(n.pixels, fill_);
        }
        if (L.is_deep()) {
            Image16 dd = quarter ? raster16::rotate_quarter(*L.deep, q) : raster16::rotate(*L.deep, d);
            if (!quarter && L.background) { uint16_t* qq = dd.data(); for (size_t i = 0; i < dd.size(); i += 4) if (qq[i + 3] == 0) { qq[i] = static_cast<uint16_t>(fill_.r * 257); qq[i + 1] = static_cast<uint16_t>(fill_.g * 257); qq[i + 2] = static_cast<uint16_t>(fill_.b * 257); qq[i + 3] = 65535; } }
            n.deep = std::make_shared<const Image16>(std::move(dd));
        }
        if (L.has_mask()) n.mask = rotate_mask(L.mask);
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

void RemoveLayerCommand::execute(Document& doc) {
    before_ = doc.snapshot();
    if (index_ >= doc.layer_count()) return;
    const size_t end = doc.layer(index_).type == LayerType::Group ? doc.group_end(index_) : index_ + 1;
    std::vector<Layer> layers = doc.clone_layers();
    layers.erase(layers.begin() + index_, layers.begin() + end);
    doc.replace_layers(layers, layers.empty() ? -1 : static_cast<int>(std::min(index_, layers.size() - 1)));
}

}  // namespace firn
