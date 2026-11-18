#include <windows.h>

#include <shellapi.h>
#include <wincrypt.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/streamable_http_server.hpp"
#include "fastmcpp/tools/manager.hpp"

#include "capture.hpp"
#include "config.hpp"
#include "game_window.hpp"
#include "input.hpp"
#include "ui.hpp"

namespace {

using fastmcpp::Json;

Json text_result(const std::string& text)
{
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})}};
}

Json error_result(const std::string& text)
{
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
                {"isError", true}};
}

std::string base64(const std::string& bytes)
{
    DWORD len = 0;
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string out(len, '\0');
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &len);
    out.resize(len);
    return out;
}

// Input runs on a dedicated FIFO thread so an input that triggers a heavy game
// transition (world load, screen change) cannot block the HTTP worker thread and
// reset the client connection. Handlers enqueue the action and return "ok" at once.
std::mutex g_input_mutex;
std::condition_variable g_input_cv;
std::deque<std::function<void()>> g_input_queue;

void input_worker()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(g_input_mutex);
            g_input_cv.wait(lock, [] { return !g_input_queue.empty(); });
            task = std::move(g_input_queue.front());
            g_input_queue.pop_front();
        }
        task();
    }
}

Json enqueue_input(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_input_queue.push_back(std::move(task));
    }
    g_input_cv.notify_one();
    return text_result("ok");
}

input::KeyAction key_action(const Json& args)
{
    std::string a = args.value("action", "press");
    if (a == "down")
        return input::KeyAction::Down;
    if (a == "up")
        return input::KeyAction::Up;
    return input::KeyAction::Press;
}

input::Method method_of(const Json& args)
{
    std::string m = args.value("method", std::string());
    if (m == "post" || m == "postmessage")
        return input::Method::Post;
    if (m == "send" || m == "sendinput")
        return input::Method::Send;
    if (m == "feed")
        return input::Method::Feed;
    return input::default_method();
}

std::string url_encode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

Json open_minecraft_uri(const std::string& uri)
{
    if (uri.rfind("minecraft:", 0) != 0)
        return error_result("uri must start with minecraft:");
    int wlen = MultiByteToWideChar(CP_UTF8, 0, uri.c_str(), -1, nullptr, 0);
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, uri.c_str(), -1, wide.data(), wlen);
    auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32)
        return error_result("ShellExecute failed with code " + std::to_string(rc) + " for " + uri);
    return text_result("opened " + uri);
}

bool read_server_args(const Json& args, std::string& host, int& port, std::string& error)
{
    host = args.value("host", std::string());
    port = args.value("port", 19132);
    if (host.empty())
        error = "host is required";
    else if (port < 1 || port > 65535)
        error = "port must be 1-65535";
    return error.empty();
}

Json object_schema(Json properties = Json::object(), Json required = Json::array())
{
    Json schema{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
        schema["required"] = std::move(required);
    return schema;
}

Json method_prop()
{
    return Json{{"type", "string"}, {"enum", Json::array({"send", "post", "feed"})},
                {"description",
                 "Input delivery. Mouse: use 'feed' (calls MouseDevice::feed, no focus, the only "
                 "method the game accepts for mouse buttons; 'send'/'post' mouse clicks are ignored "
                 "by GameInput). Keyboard: 'post' (PostMessage, no focus). 'send' (SendInput) needs "
                 "window focus and is only a fallback. Omit to use the configured default (feed)."}};
}

// Run one input action synchronously (used by the batch tool so several actions
// execute back to back with precise timing in a single queued task).
void run_action(const std::string& tool, const Json& a)
{
    if (tool == "wait")
    {
        int ms = a.value("ms", 0);
        if (ms > 0 && ms <= 10000)
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return;
    }
    input::Method m = method_of(a);
    if (tool == "key")
        input::send_key(config::key_for(a.value("key", std::string())), key_action(a), m);
    else if (tool == "type")
        input::type_text(a.value("text", std::string()), m);
    else if (tool == "move")
        input::move_to(a.value("x", 0), a.value("y", 0), m);
    else if (tool == "look")
        input::move_relative(a.value("dx", 0), a.value("dy", 0), m);
    else if (tool == "scroll")
        input::scroll(a.value("amount", 0), m);
    else if (tool == "click")
    {
        std::string b = a.value("button", "left");
        input::MouseButton button = b == "right"    ? input::MouseButton::Right
                                    : b == "middle" ? input::MouseButton::Middle
                                                    : input::MouseButton::Left;
        std::string act = a.value("action", "click");
        if (act == "down")
            input::mouse_button(button, input::KeyAction::Down, m);
        else if (act == "up")
            input::mouse_button(button, input::KeyAction::Up, m);
        else
        {
            input::mouse_button(button, input::KeyAction::Down, m);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            input::mouse_button(button, input::KeyAction::Up, m);
        }
    }
}

void register_tools(fastmcpp::tools::ToolManager& tm)
{
    using fastmcpp::tools::Tool;
    auto str = Json{{"type", "string"}};
    auto num = Json{{"type", "number"}};

    tm.register_tool(Tool{"ping", object_schema(), Json(), [](const Json&) {
                              return text_result("pong from pid " +
                                                 std::to_string(GetCurrentProcessId()));
                          }});

    tm.register_tool(Tool{"state", object_schema(), Json(), [](const Json&) -> Json {
                              HWND hwnd = find_game_window();
                              if (!hwnd)
                                  return error_result("game window not found");
                              RECT rc{};
                              GetClientRect(hwnd, &rc);
                              wchar_t title[256] = {};
                              GetWindowTextW(hwnd, title, 256);
                              char utf8[512] = {};
                              WideCharToMultiByte(CP_UTF8, 0, title, -1, utf8, sizeof(utf8), nullptr,
                                                  nullptr);
                              Json s{{"title", utf8},
                                     {"client_width", rc.right},
                                     {"client_height", rc.bottom},
                                     {"foreground", GetForegroundWindow() == hwnd},
                                     {"minimized", IsIconic(hwnd) != 0},
                                     {"feed_ready", input::feed_ready()}};
                              return text_result(s.dump());
                          }});

    tm.register_tool(Tool{
        "screenshot", object_schema({{"width", num}, {"height", num}}), Json(),
        [](const Json& args) -> Json {
            int w = args.value("width", 1280);
            int h = args.value("height", 720);
            auto r = capture::screenshot(w, h);
            if (!r.ok)
                return error_result("screenshot: " + r.error);
            std::string map = "image " + std::to_string(r.width) + "x" + std::to_string(r.height) +
                              " of client " + std::to_string(r.client_width) + "x" +
                              std::to_string(r.client_height) + "; for move/click: client_x = round(image_x * " +
                              std::to_string(r.client_width) + " / " + std::to_string(r.width) +
                              "), client_y = round(image_y * " + std::to_string(r.client_height) + " / " +
                              std::to_string(r.height) + ")";
            return Json{{"content", Json::array({Json{{"type", "image"},
                                                      {"data", base64(r.png)},
                                                      {"mimeType", "image/png"}},
                                                 Json{{"type", "text"}, {"text", map}}})}};
        }});

    tm.register_tool(Tool{
        "key",
        object_schema({{"key", str},
                       {"action", Json{{"type", "string"},
                                       {"enum", Json::array({"press", "down", "up"})}}},
                       {"method", method_prop()}},
                      Json::array({"key"})),
        Json(),
        [](const Json& args) -> Json {
            std::string name = config::key_for(args.value("key", std::string()));
            input::KeyAction act = key_action(args);
            input::Method m = method_of(args);
            return enqueue_input([=] { input::send_key(name, act, m); });
        }});

    tm.register_tool(Tool{
        "hold_key",
        object_schema({{"key", str}, {"duration", num}, {"method", method_prop()}},
                      Json::array({"key", "duration"})),
        Json(),
        [](const Json& args) -> Json {
            std::string name = config::key_for(args.value("key", std::string()));
            input::Method m = method_of(args);
            int ms = args.value("duration", 0);
            if (ms < 1)
                ms = 1;
            if (ms > 10000)
                ms = 10000;
            return enqueue_input([=] {
                input::send_key(name, input::KeyAction::Down, m);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                input::send_key(name, input::KeyAction::Up, m);
            });
        }});

    tm.register_tool(Tool{"type", object_schema({{"text", str}, {"method", method_prop()}},
                                                Json::array({"text"})),
                          Json(),
                          [](const Json& args) -> Json {
                              std::string text = args.value("text", std::string());
                              input::Method m = method_of(args);
                              return enqueue_input([=] { input::type_text(text, m); });
                          }});

    tm.register_tool(Tool{
        "chat", object_schema({{"message", str}, {"method", method_prop()}},
                              Json::array({"message"})),
        Json(),
        [](const Json& args) -> Json {
            input::Method m = method_of(args);
            std::string msg = args.value("message", std::string());
            std::string chat_key = config::key_for("chat");
            return enqueue_input([=] {
                if (m == input::Method::Send)
                    input::focus_game();
                input::send_key(chat_key, input::KeyAction::Press, m);
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                input::type_text(msg, m);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                input::send_key("enter", input::KeyAction::Press, m);
            });
        }});

    tm.register_tool(Tool{
        "click",
        object_schema({{"button", Json{{"type", "string"},
                                       {"enum", Json::array({"left", "right", "middle"})}}},
                       {"action", Json{{"type", "string"},
                                       {"enum", Json::array({"click", "down", "up"})}}},
                       {"method", method_prop()}}),
        Json(),
        [](const Json& args) -> Json {
            std::string b = args.value("button", "left");
            input::MouseButton button = b == "right"    ? input::MouseButton::Right
                                        : b == "middle" ? input::MouseButton::Middle
                                                        : input::MouseButton::Left;
            input::Method m = method_of(args);
            std::string a = args.value("action", "click");
            return enqueue_input([=] {
                if (a == "down")
                    input::mouse_button(button, input::KeyAction::Down, m);
                else if (a == "up")
                    input::mouse_button(button, input::KeyAction::Up, m);
                else
                {
                    // A button that goes down and up in the same instant is often
                    // missed by the game's UI; hold briefly so it registers.
                    input::mouse_button(button, input::KeyAction::Down, m);
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    input::mouse_button(button, input::KeyAction::Up, m);
                }
            });
        }});

    tm.register_tool(Tool{
        "move", object_schema({{"x", num}, {"y", num}, {"method", method_prop()}},
                              Json::array({"x", "y"})),
        Json(),
        [](const Json& args) -> Json {
            int x = args.value("x", 0), y = args.value("y", 0);
            input::Method m = method_of(args);
            return enqueue_input([=] { input::move_to(x, y, m); });
        }});

    tm.register_tool(Tool{
        "look",
        object_schema({{"dx", num}, {"dy", num}, {"method", method_prop()}},
                      Json::array({"dx", "dy"})),
        Json(),
        [](const Json& args) -> Json {
            int dx = args.value("dx", 0), dy = args.value("dy", 0);
            input::Method m = method_of(args);
            return enqueue_input([=] { input::move_relative(dx, dy, m); });
        }});

    tm.register_tool(Tool{"scroll", object_schema({{"amount", num}, {"method", method_prop()}},
                                                  Json::array({"amount"})),
                          Json(),
                          [](const Json& args) -> Json {
                              int amount = args.value("amount", 0);
                              input::Method m = method_of(args);
                              return enqueue_input([=] { input::scroll(amount, m); });
                          }});

    tm.register_tool(Tool{
        "batch",
        object_schema({{"actions", Json{{"type", "array"}, {"items", Json{{"type", "object"}}}}}},
                      Json::array({"actions"})),
        Json(),
        [](const Json& args) -> Json {
            Json actions = args.value("actions", Json::array());
            return enqueue_input([actions] {
                for (const auto& a : actions)
                    run_action(a.value("tool", std::string()), a);
            });
        }});

    tm.register_tool(Tool{"wait", object_schema({{"ms", num}}, Json::array({"ms"})), Json(),
                          [](const Json& args) -> Json {
                              int ms = args.value("ms", 0);
                              if (ms > 0 && ms <= 10000)
                                  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                              return text_result("ok");
                          }});

    tm.register_tool(Tool{"focus", object_schema(), Json(), [](const Json&) -> Json {
                              if (!input::focus_game())
                                  return error_result("could not bring the game to the foreground");
                              return text_result("ok");
                          }});

    tm.register_tool(Tool{
        "open_uri", object_schema({{"uri", str}}, Json::array({"uri"})), Json(),
        [](const Json& args) -> Json {
            return open_minecraft_uri(args.value("uri", std::string()));
        }});

    tm.register_tool(Tool{
        "add_server", object_schema({{"name", str}, {"host", str}, {"port", num}},
                                    Json::array({"name", "host"})),
        Json(),
        [](const Json& args) -> Json {
            std::string host, error;
            int port = 0;
            if (!read_server_args(args, host, port, error))
                return error_result(error);
            std::string name = args.value("name", std::string());
            if (name.empty())
                return error_result("name is required");
            return open_minecraft_uri("minecraft://?addExternalServer=" + url_encode(name) + "|" +
                                      url_encode(host) + ":" + std::to_string(port));
        }});

    auto ui_json = [](const ui::Result& r) { return r.ok ? text_result(r.text) : error_result(r.text); };

    tm.register_tool(Tool{"ui_reload", object_schema(), Json(),
                          [ui_json](const Json&) { return ui_json(ui::reload()); }});

    tm.register_tool(Tool{"ui_screen", object_schema(), Json(),
                          [ui_json](const Json&) { return ui_json(ui::screen()); }});

    tm.register_tool(Tool{
        "ui_tree",
        object_schema({{"filter", str},
                       {"max_depth", num},
                       {"limit", num},
                       {"include_hidden", Json{{"type", "boolean"}}}}),
        Json(),
        [ui_json](const Json& args) {
            ui::TreeOptions options;
            options.filter = args.value("filter", std::string());
            options.max_depth = args.value("max_depth", options.max_depth);
            options.limit = args.value("limit", options.limit);
            options.include_hidden = args.value("include_hidden", false);
            return ui_json(ui::tree(options));
        }});

    tm.register_tool(Tool{"ui_at", object_schema({{"x", num}, {"y", num}}, Json::array({"x", "y"})),
                          Json(), [ui_json](const Json& args) {
                              return ui_json(ui::at(args.value("x", 0), args.value("y", 0)));
                          }});

    tm.register_tool(Tool{
        "connect_server", object_schema({{"host", str}, {"port", num}}, Json::array({"host"})),
        Json(),
        [](const Json& args) -> Json {
            std::string host, error;
            int port = 0;
            if (!read_server_args(args, host, port, error))
                return error_result(error);
            return open_minecraft_uri("minecraft://connect/?serverUrl=" + url_encode(host) +
                                      "&serverPort=" + std::to_string(port));
        }});
}

std::unordered_map<std::string, std::string> tool_descriptions()
{
    return {
        {"ping", "Check that the MCP server inside Minecraft is alive"},
        {"state", "Report window title, client size, focus state, and feed_ready. "
                  "feed_ready must be true before mouse (feed) works; it turns true a moment after "
                  "the game first runs MouseDevice::feed (the game decrypts it on first use)."},
        {"ui_reload", "Reload the legacy JSON UI definitions from the active resource packs, the same as "
                      "Ctrl+R in game (Conduit). Edit the pack's ui/*.json files first, then call this "
                      "and check with screenshot. Ore UI screens are not affected."},
        {"ui_screen", "Name of the active legacy JSON UI screen with its control, binding and factory "
                      "counts (Conduit diagnostics). Fails on Ore UI screens."},
        {"ui_tree", "Control tree of the active legacy JSON UI screen: name [type], rect in client "
                    "pixels (x,y wxh, the same units as move/click), effective layer. 'filter' keeps "
                    "controls whose name contains it and prints their full path; 'max_depth' (default "
                    "64), 'limit' lines (default 400), 'include_hidden' (default false). Types are "
                    "known only for controls created after the payload was injected; call ui_reload "
                    "to rebuild them."},
        {"ui_at", "The topmost visible legacy JSON UI control at client point (x,y) (the same pixels "
                  "as move/click): name [type], rect, layer and the path from the screen root."},
        {"screenshot", "Capture the game's client area as a PNG, scaled to fit within width x height "
                       "(default 1280x720, keeps aspect, never upscales; use width:0,height:0 for full "
                       "size). Works without focus. The image is the client area (no title bar); the "
                       "result's text line gives the exact formula to map image coordinates to "
                       "move/click client coordinates."},
        {"key", "Press a key by name or configured action (e.g. 'forward','chat','w'; press/down/up). "
                "Keyboard uses PostMessage and works WITHOUT focus, in menus and in-world (movement)."},
        {"hold_key", "Hold a key down for 'duration' milliseconds (1-10000), then release it. 'key' is "
                     "a name or configured action (e.g. 'forward','sprint'). Runs as one queued task. "
                     "To hold several keys at once, use 'batch' instead."},
        {"type", "Type a Unicode string. Keyboard, works without focus."},
        {"chat", "Open chat, type a message and send it (keyboard, no focus needed)."},
        {"click", "Mouse button (left/right/middle; click/down/up). IMPORTANT: the game reads mouse "
                  "buttons via GameInput and IGNORES 'send'/'post' clicks - use method 'feed' for "
                  "clicks that register. The click lands at the last 'move' position. Needs feed_ready. "
                  "Input is applied asynchronously in FIFO order, so 'ok' means queued; after a click "
                  "that triggers a heavy transition, wait then re-check with screenshot/state, do not "
                  "retry."},
        {"move", "Move the mouse to client (x,y). Use method 'feed' to position without focus; the "
                 "next 'click' lands here. Needs feed_ready (see state)."},
        {"look", "Relative camera look for in-world (dx,dy). Use method 'feed'. Only has effect while "
                 "the game is active - single-player pauses when the window is unfocused. Needs feed_ready."},
        {"scroll", "Scroll the mouse wheel (positive up). Works for scrollable menus only; the "
                   "in-world hotbar does not respond, because the game reads the wheel via GameInput "
                   "which ignores injected scroll. Use number keys for hotbar slots."},
        {"wait", "Sleep for a number of milliseconds (max 10000)."},
        {"focus", "Bring the Minecraft window to the foreground. Only needed for 'send' input; "
                  "'feed' mouse and 'post' keyboard do not need focus."},
        {"open_uri", "Open a raw minecraft: deep link. For servers prefer add_server/connect_server."},
        {"add_server", "Add an external server to the Servers tab via the addExternalServer deep link "
                       "(minecraft://?addExternalServer=name|host:port). 'port' defaults to 19132. "
                       "Check the result with screenshot."},
        {"connect_server", "Join a server directly via the connect deep link "
                           "(minecraft://connect/?serverUrl=host&serverPort=port), skipping the "
                           "menus. 'port' defaults to 19132. Leaves the current world. The join is "
                           "asynchronous: wait, then check with screenshot."},
        {"batch", "Run several input actions as one queued task, back to back with no HTTP round-trip "
                  "between them, for precise timing (double-tap to fly, holding two keys at once). "
                  "'actions' is an array of objects, each a 'tool' name plus that tool's args: "
                  "{\"tool\":\"key|type|click|move|look|scroll|wait\", ...}. Example (fly): [{\"tool\":"
                  "\"key\",\"key\":\"jump\",\"action\":\"down\"},{\"tool\":\"wait\",\"ms\":30},{\"tool\""
                  ":\"key\",\"key\":\"jump\",\"action\":\"up\"},{\"tool\":\"wait\",\"ms\":120},{\"tool\":"
                  "\"key\",\"key\":\"jump\",\"action\":\"down\"}]. Returns ok once queued."},
    };
}

DWORD WINAPI start_server(LPVOID)
{
    config::load();
    std::thread(input_worker).detach();
    input::init_feed(config::feed_signature().c_str());
    ui::init();
    std::string mode = config::input_mode();
    input::set_default_method(mode == "postmessage" ? input::Method::Post
                              : mode == "feed"       ? input::Method::Feed
                                                     : input::Method::Send);

    auto* tools = new fastmcpp::tools::ToolManager();
    register_tools(*tools);

    auto handler = fastmcpp::mcp::make_mcp_handler("bedrock-mcp", "0.1.0", *tools,
                                                   tool_descriptions());

    auto* server = new fastmcpp::server::StreamableHttpServerWrapper(
        handler, "127.0.0.1", config::port(), "/mcp", config::auth_token());
    if (!server->start())
        OutputDebugStringA("bedrock-mcp: failed to start HTTP server\n");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, start_server, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
