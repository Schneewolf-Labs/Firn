#pragma once
// The program's actions as data. Everything the menus and tool options can
// do is registered here with its parameters, so the GUI, the driver socket
// and the command line all reach the same surface and a client can ask what
// exists instead of being told out of band.
#include <functional>
#include <string>
#include <vector>

#include "firn/json.h"

struct App;

struct Action {
    struct Param {
        const char* name;
        const char* type;         // number, string, bool
        const char* summary;
        bool required = false;    // the call is refused without it
        const char* choices = nullptr;   // comma separated, when the value is one of a set
        const char* fallback = nullptr;  // what is used when it is left out
        const char* schema_json = nullptr; // additional JSON Schema constraints (including nested arrays/objects)
    };
    const char* name;          // stable and dotted: "layer.new", "view.fit"
    const char* summary;
    const char* detail = nullptr;   // a sentence of context, where it earns one
    std::vector<Param> params;
    bool batch_safe = false;
    firn::json::Value examples = firn::json::Value::array();
    // Runs it and returns the JSON reply; `ok` false means the message is an error.
    std::function<std::string(App&, const firn::json::Value&, bool* ok)> run;
};

const std::vector<Action>& actions();
const Action* find_action(const std::string& name);
// The whole API as JSON: actions with their parameters, the tool names, and
// the option names `tool.set_option` accepts.
std::string describe_json(App& app, const std::string& name = "");
firn::json::Value action_schema(const Action& action);
bool validate_action(const Action& action, const firn::json::Value& params, std::string& error);
void add_drawing_actions(std::vector<Action>& actions);
void add_vector_actions(std::vector<Action>& actions);
