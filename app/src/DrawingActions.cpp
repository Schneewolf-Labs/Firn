// Drawing commands use the same raster/vector engines and history as the tools.
#include "Actions.h"
#include "App.h"
#include "firn/commands.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

using firn::json::Value;
namespace {
const char* coordinate = R"({"minimum":-100000,"maximum":100000})";
const char* positive = R"({"exclusiveMinimum":0,"maximum":100000})";
const char* color_schema = R"({"pattern":"^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$"})";
const char* paint_schema = R"({"pattern":"^(none|#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?)$"})";
const char* points_schema = R"({"minItems":3,"maxItems":4096,"items":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number","minimum":-100000,"maximum":100000}}})";
const char* stroke_points_schema = R"({"minItems":1,"maxItems":4096,"items":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number","minimum":-100000,"maximum":100000}}})";
const char* nodes_schema = R"({"minItems":2,"maxItems":4096,"items":{"type":"object","required":["x","y"],"additionalProperties":false,"properties":{"x":{"type":"number","minimum":-100000,"maximum":100000},"y":{"type":"number","minimum":-100000,"maximum":100000},"in":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number","minimum":-100000,"maximum":100000}},"out":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number","minimum":-100000,"maximum":100000}}}}})";
float number(const Value& p, const char* key, double fallback = 0) { return static_cast<float>(p.get(key).as_number(fallback)); }
firn::Color color(const std::string& text) {
    const auto n = std::stoul(text.substr(1), nullptr, 16);
    if (text.size() == 9) return {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
    return {static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n), 255};
}
firn::vec::PaintStyle paint(const std::string& text) {
    firn::vec::PaintStyle p;
    if (text != "none") { p.kind = firn::vec::PaintStyle::Kind::Solid; p.color = color(text); }
    return p;
}
std::string fail(bool* ok, const std::string& why) { *ok = false; return why; }
std::string result(App& app, const char* target, int object = -1) {
    Value r = Value::object(); r.set("ok", Value::boolean(true));
    r.set("layer", Value::number(app.active_layer()));
    r.set("target", Value::string(target));
    if (object >= 0) r.set("object", Value::number(object));
    return firn::json::dump(r);
}
bool drawable(App& app, bool* ok, std::string& error) {
    if (!app.doc || app.active_layer() < 0) error = "no active image layer";
    else if (app.mask_edit || app.selection_edit) error = "exit mask/selection editing before drawing through the API";
    else return true;
    *ok = false; return false;
}
std::string draw_object(App& app, const Value& p, firn::vec::Object o, bool* ok) {
    std::string error;
    if (!drawable(app, ok, error)) return error;
    o.name = p.get("name").as_string("Draw shape");
    o.fill = paint(p.get("fill").as_string("#000000"));
    o.stroke = paint(p.get("stroke").as_string("none"));
    o.stroke_width = number(p, "stroke_width", 1);
    o.antialias = p.get("antialias").as_bool(true);
    o.line.first_cap = o.line.last_cap = 1;
    if (!o.fill.enabled() && !o.stroke.enabled()) return fail(ok, "at least one of fill or stroke must be enabled");
    auto& layer = app.doc->layer(app.active_layer());
    if (p.get("target").as_string("raster") == "vector") {
        if (!layer.is_vector()) return fail(ok, "target vector requires an active vector layer; use layer.new_vector");
        if (app.doc->has_selection()) return fail(ok, "vector drawing does not clip to raster selections; use select.none or target raster");
        auto objects = layer.objects;
        const int index = static_cast<int>(objects.size());
        objects.push_back(o);
        app.run(std::make_unique<firn::VectorEditCommand>(app.active_layer(), o.name, std::move(objects)));
        return result(app, "vector", index);
    }
    if (!layer.is_raster()) return fail(ok, "target raster requires an active raster layer");
    app.run(std::make_unique<firn::AdjustCommand>(app.active_layer(), o.name,
        [o](firn::Image& image) { firn::vec::rasterize({o}, image); }));
    return result(app, "raster");
}
std::vector<Action::Param> style() {
    return {{"fill", "string", "Fill color #RRGGBB or #RRGGBBAA, or none", false, nullptr, "#000000", paint_schema},
            {"stroke", "string", "Outline color #RRGGBB or #RRGGBBAA, or none", false, nullptr, "none", paint_schema},
            {"stroke_width", "number", "Outline width in image pixels", false, nullptr, "1", R"({"exclusiveMinimum":0,"maximum":1000})"},
            {"target", "string", "Draw on the active raster layer or add an editable object to the active vector layer", false, "raster,vector", "raster"},
            {"antialias", "bool", "Smooth shape edges", false, nullptr, "true"},
            {"name", "string", "Undo label and vector object name", false, nullptr, "Draw shape", R"({"minLength":1,"maxLength":200})"}};
}

// A batch stores only its boundary states; intermediate undo entries are
// discarded. Failed batches restore the old history including its redo tail.
class BatchSnapshot final : public firn::Command {
    std::string label_;
    firn::Document::State before_, after_;
public:
    BatchSnapshot(std::string label, firn::Document::State before, firn::Document::State after)
        : label_(std::move(label)), before_(std::move(before)), after_(std::move(after)) {}
    std::string name() const override { return label_; }
    void execute(firn::Document& doc) override { doc.restore(after_); }
    void undo(firn::Document& doc) override { doc.restore(before_); }
    size_t memory_bytes() const override { return firn::state_bytes(before_) + firn::state_bytes(after_); }
};
std::string batch(App& app, const Value& p, bool* ok) {
    std::string error;
    if (!drawable(app, ok, error)) return error;
    const auto& calls = p.get("actions");
    // Validate the entire request before executing any child action.
    for (size_t i = 0; i < calls.size(); ++i) {
        const auto& call = calls[i];
        const Action* a = find_action(call.get("action").as_string());
        const Value params = call.find("params") ? call.get("params") : Value::object();
        if (!a || !a->batch_safe) return fail(ok, "actions[" + std::to_string(i) + "]: action is not batch-safe");
        if (!validate_action(*a, params, error)) return fail(ok, "actions[" + std::to_string(i) + "]: " + error);
    }
    auto before = app.doc->snapshot();
    auto history = std::move(app.history);
    app.history = firn::CommandStack{};
    app.history.set_limit(1);
    const std::string old_status = app.status;
    Value results = Value::array();
    size_t index = 0;
    std::unique_ptr<BatchSnapshot> command;
    try {
        for (; index < calls.size(); ++index) {
            const auto& call = calls[index];
            bool child_ok = true;
            const std::string reply = app.do_command(call.get("action").as_string(), call.find("params") ? call.get("params") : Value::object(), &child_ok);
            if (!child_ok) throw std::runtime_error(reply);
            Value response;
            if (!firn::json::parse(reply, response)) throw std::runtime_error("invalid action response");
            results.push(std::move(response));
        }
        command = std::make_unique<BatchSnapshot>(p.get("name").as_string("API batch"), before, app.doc->snapshot());
    } catch (const std::exception& e) {
        app.doc->restore(before);
        app.history = std::move(history);
        app.status = old_status;
        return fail(ok, "actions[" + std::to_string(index) + "]: " + e.what() + " (batch rolled back)");
    }
    app.history = std::move(history);
    app.commit(std::move(command));
    Value r = Value::object(); r.set("ok", Value::boolean(true)); r.set("results", std::move(results));
    r.set("count", Value::number(static_cast<double>(calls.size())));
    return firn::json::dump(r);
}
}

void add_drawing_actions(std::vector<Action>& actions) {
    auto add = [&](const char* name, const char* summary, std::vector<Action::Param> params,
                   std::function<std::string(App&, const Value&, bool*)> run, const char* example) {
        Action a; a.name = name; a.summary = summary; a.params = std::move(params); a.run = std::move(run);
        a.batch_safe = true;
        firn::json::parse(example, a.examples);
        actions.push_back(std::move(a));
    };
    for (const char* kind : {"draw.rectangle", "draw.ellipse"}) {
        auto params = style();
        params.insert(params.begin(), {{"x", "number", "Left edge in image pixels", true, nullptr, nullptr, coordinate},
                                      {"y", "number", "Top edge in image pixels", true, nullptr, nullptr, coordinate},
                                      {"width", "number", "Bounding box width", true, nullptr, nullptr, positive},
                                      {"height", "number", "Bounding box height", true, nullptr, nullptr, positive}});
        const bool ellipse = std::string(kind) == "draw.ellipse";
        add(kind, ellipse ? "Draw an ellipse" : "Draw a rectangle", params,
            [ellipse](App& app, const Value& p, bool* ok) {
                const float x = number(p,"x"), y = number(p,"y"), w = number(p,"width"), h = number(p,"height");
                auto o = ellipse ? firn::vec::make_ellipse(x+w/2,y+h/2,w/2,h/2) : firn::vec::make_rectangle(x,y,x+w,y+h);
                return draw_object(app,p,std::move(o),ok);
            }, R"([{"x":20,"y":20,"width":80,"height":60,"fill":"#EEAA75","stroke":"#573C39","stroke_width":3}])");
    }
    auto polygon = style(); polygon.insert(polygon.begin(), {"points", "array", "Polygon vertices as [x,y] pairs; closure is automatic", true, nullptr, nullptr, points_schema});
    add("draw.polygon", "Draw a closed polygon", polygon, [](App& app, const Value& p, bool* ok) {
        std::vector<std::pair<float,float>> points;
        for (const auto& point : p.get("points").arr) points.emplace_back(static_cast<float>(point[0].num),static_cast<float>(point[1].num));
        return draw_object(app,p,firn::vec::make_polygon(points,true),ok);
    }, R"([{"points":[[20,100],[50,20],[100,100]],"fill":"#EEAA75"}])");
    auto path = style();
    path.insert(path.begin(), {{"nodes", "array", "Bezier anchors with optional absolute in/out control points", true, nullptr, nullptr, nodes_schema},
                              {"closed", "bool", "Close the final segment to the first node", false, nullptr, "false"}});
    add("draw.path", "Draw an editable Bezier path or rasterize it", path, [](App& app, const Value& p, bool* ok) {
        firn::vec::Path path; path.closed = p.get("closed").as_bool(false);
        for (const auto& v : p.get("nodes").arr) {
            firn::vec::Node n; n.x = number(v,"x"); n.y = number(v,"y");
            n.in_x = v.find("in") ? static_cast<float>(v.get("in")[0].num) : n.x; n.in_y = v.find("in") ? static_cast<float>(v.get("in")[1].num) : n.y;
            n.out_x = v.find("out") ? static_cast<float>(v.get("out")[0].num) : n.x; n.out_y = v.find("out") ? static_cast<float>(v.get("out")[1].num) : n.y;
            path.nodes.push_back(n);
        }
        if (!path.closed && p.get("fill").as_string("#000000") != "none") return fail(ok,"open paths require fill: none");
        firn::vec::Object o; o.paths.push_back(std::move(path)); return draw_object(app,p,std::move(o),ok);
    }, R"([{"nodes":[{"x":20,"y":80,"out":[40,10]},{"x":100,"y":80,"in":[80,10]}],"fill":"none","stroke":"#573C39","stroke_width":4}])");
    add("draw.stroke", "Paint one continuous brush stroke", {
        {"points","array","Ordered [x,y] pairs; one point makes a dot",true,nullptr,nullptr,stroke_points_schema},
        {"color","string","Brush color #RRGGBB or #RRGGBBAA",true,nullptr,nullptr,color_schema},
        {"size","number","Brush diameter in image pixels",false,nullptr,"16",R"({"minimum":1,"maximum":500})"},
        {"hardness","number","0 is soft, 1 is hard",false,nullptr,"1",R"({"minimum":0,"maximum":1})"},
        {"opacity","number","Coverage for the entire stroke, 0 to 1",false,nullptr,"1",R"({"minimum":0,"maximum":1})"}},
        [](App& app, const Value& p, bool* ok) {
            std::string error; if (!drawable(app,ok,error)) return error;
            if (!app.active_is_raster()) return fail(ok,"draw.stroke requires an active raster layer");
            firn::raster::Brush brush; brush.size=number(p,"size",16); brush.hardness=number(p,"hardness",1); brush.opacity=number(p,"opacity",1);
            auto points=p.get("points"); const auto c=color(p.get("color").str);
            app.run(std::make_unique<firn::AdjustCommand>(app.active_layer(),"Draw stroke",[points,brush,c](firn::Image& img) {
                firn::raster::Stroke stroke(img,brush,c,firn::raster::StrokeMode::Paint);
                for (const auto& pt : points.arr) stroke.add_point(static_cast<float>(pt[0].num),static_cast<float>(pt[1].num));
                stroke.render(img);
            })); return result(app,"raster");
        }, R"([{"points":[[20,20],[80,70],[120,20]],"color":"#573C39","size":5}])");
    add("edit.fill", "Replace the active raster layer through the selection with a color", {
        {"color","string","Replacement color #RRGGBB or #RRGGBBAA",true,nullptr,nullptr,color_schema}},
        [](App& app, const Value& p, bool* ok) {
            std::string error; if (!drawable(app,ok,error)) return error;
            if (!app.active_is_raster()) return fail(ok,"edit.fill requires an active raster layer");
            app.run(std::make_unique<firn::FillCommand>(app.active_layer(),color(p.get("color").str))); return result(app,"raster");
        }, R"([{"color":"#FFF4E9"}])");
    add("app.batch", "Apply document edits atomically as one undo step", {
        {"name","string","Undo label",false,nullptr,"API batch",R"({"minLength":1,"maxLength":200})"},
        {"actions","array","Ordered calls; only actions marked batch_safe are accepted",true,nullptr,nullptr,R"({"minItems":1,"maxItems":256,"items":{"type":"object","required":["action"],"additionalProperties":false,"properties":{"action":{"type":"string","minLength":1},"params":{"type":"object"}}}})"}},
        batch,R"([{"name":"Triangle","actions":[{"action":"layer.new_vector"},{"action":"draw.polygon","params":{"points":[[20,100],[50,20],[100,100]],"fill":"#EEAA75","target":"vector"}}]}])");
    actions.back().batch_safe = false;
    actions.back().detail = "Requires an open document. Validation or execution failure restores pixels, layers, selection, active layer, and the previous undo/redo history. Files, clipboard, tools, nested batches, and inherited commands are excluded.";
}
