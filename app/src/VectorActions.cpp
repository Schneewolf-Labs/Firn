// Vector object and node actions. The node edit commands the original
// scripts (ConvertToPath, NodeEditAddPath) reach the program through these,
// and so does anything that wants to read a vector layer back out.
#include <algorithm>
#include <string>
#include <vector>

#include "Actions.h"
#include "App.h"
#include "firn/vector.h"

using firn::json::Value;

namespace {

const char* nodes_schema = R"({"minItems":2,"maxItems":4096,"items":{"type":"object","required":["x","y"],"additionalProperties":false,"properties":{"x":{"type":"number","minimum":-100000,"maximum":100000},"y":{"type":"number","minimum":-100000,"maximum":100000},"in":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number","minimum":-100000,"maximum":100000}},"out":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number","minimum":-100000,"maximum":100000}}}}})";
const char* index_schema = R"({"minimum":0,"maximum":100000})";

std::string fail(bool* ok, const std::string& why) { *ok = false; return why; }

// The vector layer these actions act on, or -1 with `error` set.
int vector_layer(App& app, std::string* error) {
    if (!app.doc || app.active_layer() < 0) { *error = "no active layer"; return -1; }
    if (!app.doc->layer(app.active_layer()).is_vector()) { *error = "the active layer is not a vector layer"; return -1; }
    return app.active_layer();
}

std::string ok_reply(App& app, const char* what) {
    Value r = Value::object();
    r.set("ok", Value::boolean(true));
    r.set("action", Value::string(what));
    r.set("layer", Value::number(app.active_layer()));
    r.set("node_object", Value::number(app.node_object));
    r.set("node_path", Value::number(app.node_path));
    r.set("node_index", Value::number(app.node_index));
    if (!app.status.empty()) r.set("status", Value::string(app.status));
    return firn::json::dump(r);
}

Value node_json(const firn::vec::Node& n) {
    Value v = Value::object();
    v.set("x", Value::number(n.x));
    v.set("y", Value::number(n.y));
    if (!n.is_corner()) {
        Value in = Value::array(); in.push(Value::number(n.in_x)); in.push(Value::number(n.in_y));
        Value out = Value::array(); out.push(Value::number(n.out_x)); out.push(Value::number(n.out_y));
        v.set("in", std::move(in));
        v.set("out", std::move(out));
    }
    return v;
}

firn::vec::Path path_from_json(const Value& v) {
    firn::vec::Path p;
    p.closed = v.get("closed").as_bool(false);
    const Value& nodes = v.find("nodes") ? v.get("nodes") : v;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Value& n = nodes[i];
        firn::vec::Node node;
        node.x = static_cast<float>(n.get("x").as_number(0));
        node.y = static_cast<float>(n.get("y").as_number(0));
        node.in_x = n.find("in") ? static_cast<float>(n.get("in")[0].num) : node.x;
        node.in_y = n.find("in") ? static_cast<float>(n.get("in")[1].num) : node.y;
        node.out_x = n.find("out") ? static_cast<float>(n.get("out")[0].num) : node.x;
        node.out_y = n.find("out") ? static_cast<float>(n.get("out")[1].num) : node.y;
        p.nodes.push_back(node);
    }
    firn::vec::fix_path_flags(p);
    return p;
}

}  // namespace

void add_vector_actions(std::vector<Action>& actions) {
    auto add = [&](const char* name, const char* summary, std::vector<Action::Param> params,
                   std::function<std::string(App&, const Value&, bool*)> run, const char* example) {
        Action a; a.name = name; a.summary = summary; a.params = std::move(params); a.run = std::move(run);
        a.batch_safe = true;
        firn::json::parse(example, a.examples);
        actions.push_back(std::move(a));
    };

    add("object.list", "Report the objects on the active vector layer with their paths", {
        {"paths", "bool", "Include every path and node, not just the names", false, nullptr, "true"}},
        [](App& app, const Value& p, bool* ok) {
            std::string error;
            const int layer = vector_layer(app, &error);
            if (layer < 0) return fail(ok, error);
            const bool want_paths = p.get("paths").as_bool(true);
            Value list = Value::array();
            const auto& objs = app.doc->layer(layer).objects;
            for (size_t i = 0; i < objs.size(); ++i) {
                const firn::vec::Object& o = objs[i];
                Value e = Value::object();
                e.set("index", Value::number(static_cast<double>(i)));
                e.set("name", Value::string(o.name));
                e.set("selected", Value::boolean(o.selected));
                e.set("visible", Value::boolean(o.visible));
                e.set("kind", Value::string(o.is_group ? "group" : o.is_text ? "text" : "shape"));
                if (o.is_text) e.set("text", Value::string(o.text.text));
                if (want_paths && !o.is_group) {
                    Value paths = Value::array();
                    for (const firn::vec::Path& path : o.paths) {
                        Value pv = Value::object();
                        pv.set("closed", Value::boolean(path.closed));
                        Value nodes = Value::array();
                        for (const firn::vec::Node& n : path.nodes) nodes.push(node_json(n));
                        pv.set("nodes", std::move(nodes));
                        paths.push(std::move(pv));
                    }
                    e.set("paths", std::move(paths));
                }
                list.push(std::move(e));
            }
            Value r = Value::object();
            r.set("ok", Value::boolean(true));
            r.set("layer", Value::number(layer));
            r.set("objects", std::move(list));
            return firn::json::dump(r);
        }, R"([{},{"paths":false}])");

    add("object.select", "Select vector objects by index", {
        {"index", "number", "Object index; leave it out with all or none", false, nullptr, nullptr, index_schema},
        {"mode", "string", "What to do with the selection", false, "set,add,all,none", "set"}},
        [](App& app, const Value& p, bool* ok) {
            std::string error;
            if (vector_layer(app, &error) < 0) return fail(ok, error);
            const std::string mode = p.get("mode").as_string("set");
            if (mode == "all") { app.object_select_all(); return ok_reply(app, "object.select"); }
            if (mode == "none") { app.object_select_none(); app.node_object = -1; return ok_reply(app, "object.select"); }
            if (!p.find("index")) return fail(ok, "object.select needs an index unless mode is all or none");
            const size_t i = static_cast<size_t>(p.get("index").as_number(0));
            if (i >= app.doc->layer(app.active_layer()).objects.size()) return fail(ok, "no object at that index");
            app.select_objects({i}, mode == "add");
            return ok_reply(app, "object.select");
        }, R"([{"index":0},{"mode":"all"}])");

    add("object.convert_to_path", "Turn the selected text objects into editable paths", {},
        [](App& app, const Value&, bool* ok) {
            std::string error;
            if (vector_layer(app, &error) < 0) return fail(ok, error);
            app.object_convert_to_path();
            return ok_reply(app, "object.convert_to_path");
        }, R"([{}])");

    add("object.node_select", "Pick the node the node edit actions act on", {
        {"object", "number", "Object index on the active vector layer", true, nullptr, nullptr, index_schema},
        {"path", "number", "Path index within the object", false, nullptr, "0", index_schema},
        {"node", "number", "Node index within the path", false, nullptr, "0", index_schema}},
        [](App& app, const Value& p, bool* ok) {
            std::string error;
            const int layer = vector_layer(app, &error);
            if (layer < 0) return fail(ok, error);
            const auto& objs = app.doc->layer(layer).objects;
            const size_t oi = static_cast<size_t>(p.get("object").as_number(0));
            const size_t pi = static_cast<size_t>(p.get("path").as_number(0));
            const size_t ni = static_cast<size_t>(p.get("node").as_number(0));
            if (oi >= objs.size() || objs[oi].is_group) return fail(ok, "no path object at that index");
            if (pi >= objs[oi].paths.size()) return fail(ok, "no path at that index");
            if (ni >= objs[oi].paths[pi].nodes.size()) return fail(ok, "no node at that index");
            app.select_objects({oi}, false);
            app.node_object = static_cast<int>(oi);
            app.node_path = static_cast<int>(pi);
            app.node_index = static_cast<int>(ni);
            return ok_reply(app, "object.node_select");
        }, R"([{"object":0,"node":2}])");

    struct Op { const char* name; const char* summary; int kind; };
    for (const Op& op : {Op{"object.node_break", "Break the selected node's path at that node", 0},
                         Op{"object.node_join", "Join the selected node's path to the nearest other open path", 1},
                         Op{"object.path_reverse", "Reverse the direction of the selected node's path", 2}}) {
        const int kind = op.kind;
        add(op.name, op.summary, {}, [kind](App& app, const Value&, bool* ok) {
            std::string error;
            if (vector_layer(app, &error) < 0) return fail(ok, error);
            if (app.node_object < 0) return fail(ok, "select a node first with object.node_select");
            const bool done = kind == 0 ? app.node_break() : kind == 1 ? app.node_join() : app.path_reverse();
            if (!done) return fail(ok, app.status);
            return ok_reply(app, "object.node_edit");
        }, R"([{}])");
    }

    add("object.path_closed", "Close or open the selected node's path", {
        {"closed", "bool", "True closes the path, false opens it", false, nullptr, "true"}},
        [](App& app, const Value& p, bool* ok) {
            std::string error;
            if (vector_layer(app, &error) < 0) return fail(ok, error);
            if (app.node_object < 0) return fail(ok, "select a node first with object.node_select");
            if (!app.path_set_closed(p.get("closed").as_bool(true))) return fail(ok, app.status);
            return ok_reply(app, "object.path_closed");
        }, R"([{"closed":true}])");

    add("object.add_path", "Append a path to the selected object", {
        {"nodes", "array", "Bezier anchors with optional absolute in/out control points", true, nullptr, nullptr, nodes_schema},
        {"closed", "bool", "Close the final segment to the first node", false, nullptr, "false"}},
        [](App& app, const Value& p, bool* ok) {
            std::string error;
            if (vector_layer(app, &error) < 0) return fail(ok, error);
            const firn::vec::Path path = path_from_json(p);
            if (path.nodes.size() < 2) return fail(ok, "a path needs at least two nodes");
            if (!app.object_add_path({path})) return fail(ok, app.status);
            return ok_reply(app, "object.add_path");
        }, R"([{"nodes":[{"x":10,"y":10},{"x":60,"y":10},{"x":60,"y":60}],"closed":true}])");
}
