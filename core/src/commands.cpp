#include "firn/commands.h"

#include "firn/raster.h"

#include <algorithm>
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

void RemoveLayerCommand::execute(Document& doc) { removed_ = doc.remove_layer(index_); }

void RemoveLayerCommand::undo(Document& doc) { doc.insert_layer(std::move(removed_), index_); }

}  // namespace firn
