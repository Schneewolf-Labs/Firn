#include "App.h"
#include "ui/MenuBuilder.h"
#include "imgui.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <map>
#include <chrono>
#include <cstdlib>

using namespace firn;
static void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
// Native menus retain callbacks after the rendering function returns.
struct Menu : MenuBuilder {
    std::map<std::string, std::function<void()>> callbacks;
    std::string fire;
    bool begin_menu(const char*, bool enabled) override { return enabled; }
    void end_menu() override {}
    void item(const char* label, const char*, bool enabled, const std::function<void()>& action, bool, const char*) override {
        if (!enabled) return;
        callbacks[label] = action;
        if (fire == label) { fire.clear(); action(); }
    }
    void separator() override {}
    void text(const char*) override {}
    void push_id(int) override {}
    void pop_id() override {}
    void imgui_only(const std::function<void()>&) override {}
};
static void edit(App& app, Color c) { app.run(std::make_unique<FillCommand>(app.active_layer(), c)); }
static void dirty_branch(const std::filesystem::path& dir) {
    App app; app.new_document(8, 8);
    edit(app, {255,0,0,255}); check(app.save_document((dir/"saved.ora").string()), "save");
    app.undo(); edit(app, {0,255,0,255});
    check(app.modified(), "different edit at saved cursor must stay unsaved");
}
static void dirty_trim(const std::filesystem::path& dir) {
    App app; app.new_document(8,8); app.history.set_limit(1);
    edit(app, {255,0,0,255}); check(app.save_document((dir/"trim.ora").string()), "save");
    edit(app, {0,255,0,255}); check(app.modified(), "history trimming must not mark new edits saved");
    app.undo(); check(!app.modified(), "undo after trimming should recognize the saved state");
}
static void switch_selection(const std::filesystem::path&) {
    App app; app.new_document(8,8); app.select_all(); app.set_selection_edit(true);
    app.new_document(4,4);
    check(!app.selection_edit && !app.mask_edit, "new document inherited selection edit mode");
    app.activate_document(0);
    check(app.doc->has_selection() && app.doc->selection().width()==8, "switching destroyed original selection");
}
static void close_mask(const std::filesystem::path&) {
    App app; app.new_document(8,8);
    app.layer_set_mask("Mask", Mask(8,8,255)); app.set_mask_edit(true);
    app.close_document(app.current_doc, true);
    check(!app.mask_edit && !app.selection_edit, "closed document left stale mask mode");
}
static void deferred_menu(const std::filesystem::path&) {
    App app; app.new_document(8,8); app.layer_new(); edit(app,{255,0,0,255});
    Menu menu; app.draw_menu(menu);
    // Force reuse of the menu renderer's old stack frames before dispatch.
    Menu other; app.draw_selections_menu(other);
    menu.callbacks.at("Grayscale")();
    const auto c = app.doc->layer(1).pixels.get(0,0);
    check(c.r==c.g && c.g==c.b, "retained grayscale callback used a stale layer index");
    menu.callbacks.at("Remove White Matte")();
}
static void trim_redo(const std::filesystem::path&) {
    App app; app.new_document(8,8); app.layer_new(); app.layer_new(); edit(app,{255,0,0,255});
    app.undo(); app.undo(); app.undo();
    app.history.set_limit(1); app.redo();
    check(app.doc->layer_count()==2, "lowering undo limit skipped dependencies in the redo chain");
}
static void parked_saved_state(const std::filesystem::path& dir) {
    App app; app.new_document(8,8); app.history.set_limit(1);
    edit(app,{255,0,0,255}); check(app.save_document((dir/"parked.ora").string()), "save");
    edit(app,{0,255,0,255}); app.new_document(4,4);
    check(app.document_modified(0), "inactive document lost its dirty state");
    app.activate_document(0); app.undo(); check(!app.modified(), "inactive history lost saved identity");
}
static void pending_selection_save(const std::filesystem::path& dir) {
    App app; app.new_document(8,8); app.set_selection_edit(true);
    app.mask_proxy.fill({255,255,255,255});
    const auto path=(dir/"selection.ora").string(); check(app.save_document(path), "save");
    check(!app.selection_edit && app.doc->has_selection(), "save omitted the selection being edited");
    app.new_document(4,4); check(app.open_document(path), "reopen");
    check(app.doc->has_selection() && app.doc->selection().width()==8, "selection did not survive save/reopen");
}
static void pending_selection_close(const std::filesystem::path& dir) {
    App app; app.new_document(8,8); check(app.save_document((dir/"close.ora").string()), "save");
    app.set_selection_edit(true); app.mask_proxy.fill({255,255,255,255});
    app.close_document(app.current_doc);
    check(app.doc && app.pending_close==0 && app.modified(), "close discarded pending selection without prompting");
}
static void context_flatten(const std::filesystem::path&) {
    App app; app.new_document(8,8); app.layer_new(); app.layer_new();
    Menu menu; menu.fire="Merge All (Flatten)"; app.draw_layer_menu_items(menu);
    check(app.doc->layer_count()==1, "context flatten");
    app.undo(); check(app.doc->layer_count()==3, "flatten undo");
    menu.fire="Delete"; app.draw_layer_menu_items(menu);
    check(app.doc->layer_count()==2, "context delete");
}
static void untouched_selection_redo(const std::filesystem::path&) {
    App app; app.new_document(8,8); edit(app,{255,0,0,255}); app.undo();
    app.set_selection_edit(true); app.set_mask_edit(false);
    check(app.mask_proxy.width()==8, "disabling layer mask destroyed selection proxy");
    app.redo(); check(app.doc->layer(0).pixels.get(0,0).g==0, "leaving untouched selection mode discarded redo");
}
static void ui_scaling(const std::filesystem::path&) {
    App app;
    app.config.ui_scale = 0;
    app.set_auto_ui_scale(2.0f);
    const ImGuiStyle scaled = ImGui::GetStyle();
    for (int i = 0; i < 20; ++i) app.apply_theme(app.config.theme);
    check(ImGui::GetStyle().IndentSpacing == scaled.IndentSpacing, "theme application compounded indentation");
    check(ImGui::GetStyle().GrabMinSize == scaled.GrabMinSize, "theme application compounded grab size");
    check(ImGui::GetStyle().CellPadding.x == scaled.CellPadding.x, "theme application compounded table padding");
    app.set_auto_ui_scale(1.0f);
    check(app.ui_scale == 1.0f, "returning to a 100% monitor did not restore scale");
    check(ImGui::GetStyle().IndentSpacing == ImGuiStyle().IndentSpacing, "unscaled style did not recover");
    app.config.ui_scale = 1.5f;
    app.set_auto_ui_scale(2.0f);
    check(app.ui_scale == 1.5f, "monitor change ignored the user's scale override");
    app.font_pending = false;
    app.set_auto_ui_scale(2.0f);
    check(!app.font_pending, "unchanged DPI queued a font rebuild");
}
// The server list: several addresses with names, one of them in use, and a
// config written by a Firn that only knew about a single address.
static void generate_servers(const std::filesystem::path&) {
    {
        Config c;
        c.generate_servers = {{"Qwen 2.1", "http://127.0.0.1:1234"}, {"SDXL", "http://127.0.0.1:1235"}};
        check(c.use_generate_server(1), "selecting the second server did nothing");
        check(!c.use_generate_server(1), "selecting the one already in use reported a change");
        check(!c.use_generate_server(9), "an index off the end was accepted");
        check(c.generate_url == "http://127.0.0.1:1235", "the address in use did not follow the selection");
        check(c.current_generate_server() == 1, "the selected server was not reported back");
        c.save();
    }
    {
        Config c;
        c.load();
        check(c.generate_servers.size() == 2, "the server list did not survive a save and load");
        check(c.generate_servers[0].name == "Qwen 2.1", "a name with a space did not round trip");
        check(c.generate_servers[1].url == "http://127.0.0.1:1235", "an address did not round trip");
        check(c.current_generate_server() == 1, "which server was in use did not round trip");
    }
    // What a config from before the list looks like: one address, no names.
    {
        std::ofstream f(std::filesystem::path(Config::directory()) / "firn.cfg");
        f << "generate_url=http://192.168.1.9:1234\n";
    }
    {
        Config c;
        c.load();
        check(c.generate_servers.size() == 1, "an older config's single address was dropped");
        check(c.generate_servers[0].url == "http://192.168.1.9:1234", "the older address was not kept");
        check(c.generate_servers[0].name == "192.168.1.9:1234", "the older address got no usable name");
        check(c.generate_url == "http://192.168.1.9:1234", "the older address stopped being the one in use");
    }
    // A list with no address in use falls back to the first, rather than
    // leaving the feature switched off with servers configured.
    {
        std::ofstream f(std::filesystem::path(Config::directory()) / "firn.cfg");
        f << "generate_server=A|http://a:1\ngenerate_server=B|http://b:2\ngenerate_url=\n";
    }
    {
        Config c;
        c.load();
        check(c.generate_url == "http://a:1", "no address in use did not fall back to the first server");
        check(c.generate_servers.size() == 2, "the list was not read without an address in use");
    }
    std::filesystem::remove(std::filesystem::path(Config::directory()) / "firn.cfg");
}

int main(int argc, char** argv) {
    const auto dir=std::filesystem::temp_directory_path()/("firn-state-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("XDG_CONFIG_HOME", dir.string().c_str());
#else
    setenv("XDG_CONFIG_HOME", dir.string().c_str(),1);
#endif
    ImGui::CreateContext();
    int failures=0;
    const std::map<std::string,void(*)(const std::filesystem::path&)> tests={
        {"ui_scaling",ui_scaling},{"generate_servers",generate_servers},
        {"dirty_branch",dirty_branch},{"dirty_trim",dirty_trim},{"switch_selection",switch_selection},{"close_mask",close_mask},{"deferred_menu",deferred_menu},{"trim_redo",trim_redo},{"parked_saved_state",parked_saved_state},{"pending_selection_save",pending_selection_save},{"pending_selection_close",pending_selection_close},{"context_flatten",context_flatten},{"untouched_selection_redo",untouched_selection_redo}};
    for (const auto& [name,test] : tests) {
        if (argc>1 && name!=argv[1]) continue;
        try { test(dir); std::cout<<"PASS "<<name<<std::endl; }
        catch(const std::exception& e) { ++failures; std::cerr<<"FAIL "<<name<<": "<<e.what()<<std::endl; }
    }
    ImGui::DestroyContext(); std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
