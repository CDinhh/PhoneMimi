#pragma once

// Pure helpers for the MCP tools: screenshot coordinate space, the
// uiautomator dump parser, Android key names.

#include <string>
#include <vector>

namespace opm::mcp {

struct Size { int w = 0, h = 0; };
struct Rect { int l = 0, t = 0, r = 0, b = 0; };

// The screenshot / tap coordinate space: same aspect, long side at most
// max_side, never upscaled.
Size shot_size(Size src, int max_side = 1280);
int map_x(int x, Size from, Size to);
int map_y(int y, Size from, Size to);

struct UiNode {
    std::string text, desc, id, cls, pkg;
    Rect bounds;
    bool clickable = false, scrollable = false, checked = false, focused = false,
         enabled = true;
    int depth = 0;
};

// False (and *err set) when the input is not a dump, e.g. uiautomator's
// "ERROR: could not get idle state." during animations.
bool parse_ui_dump(const std::string& xml, std::vector<UiNode>* out, std::string* err);
Size ui_root_size(const std::vector<UiNode>& nodes);
// One line per useful node, bounds mapped from the root size to `shot`.
std::string format_ui_tree(const std::vector<UiNode>& nodes, Size shot);

struct Selector { std::string text, id, desc; bool exact = false; };
std::vector<const UiNode*> find_nodes(const std::vector<UiNode>& nodes, const Selector& s);

int keycode_for(const std::string& name);   // -1 unknown; digits parse as a code
std::string truncate_output(const std::string& s, size_t max = 65536);
std::string base64(const std::string& bytes);

} // namespace opm::mcp
