module;
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

export module Gui;

import Features;
import Logger;
import SkinConfig;

namespace gui_detail {

struct GuiState {
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::thread thread;
};

struct SkinEditorState {
    static constexpr std::size_t WeaponCount = 34;
    int selected = 0;
    int firstVisible = 0;
    int section = 0; // both, ct, t
    int activeField = 0; // paint kit, seed, wear
    std::uint32_t knifeDef = 0;
    bool loaded = false;
    std::string paintKit;
    std::string seed;
    std::string wear;
};

SkinEditorState& SkinEditor() {
    static SkinEditorState state;
    return state;
}

constexpr std::array<const char*, SkinEditorState::WeaponCount> SkinWeapons = {
    "ak47", "aug", "awp", "bizon", "cz75a", "deagle", "dual_berettas", "famas",
    "five_seven", "galilar", "g3sg1", "glock", "m4a1", "m4a4", "mag7", "m249",
    "mac10", "mp5sd", "mp7", "mp9", "negev", "nova", "p2000", "p250", "p90",
    "revolver", "sawedoff", "scar20", "sg556", "ssg08", "tec9", "ump45", "usp", "knife"};

constexpr std::array<const char*, 3> SkinSections = {"both", "ct", "t"};

struct KnifeChoice {
    const char* label;
    std::uint32_t def;
};

constexpr std::array<KnifeChoice, 20> KnifeChoices = {{
    {"Bayonet", 500}, {"Classic", 503}, {"Flip", 505}, {"Gut", 506},
    {"Karambit", 507}, {"M9", 508}, {"Huntsman", 509}, {"Falchion", 512},
    {"Bowie", 514}, {"Butterfly", 515}, {"Shadow Daggers", 516},
    {"Paracord", 517}, {"Survival", 518}, {"Ursus", 519}, {"Navaja", 520},
    {"Nomad", 521}, {"Stiletto", 522}, {"Talon", 523}, {"Skeleton", 525},
    {"Kukri", 526}}};

int KnifeChoiceIndex(std::uint32_t def) {
    for (std::size_t i = 0; i < KnifeChoices.size(); ++i)
        if (KnifeChoices[i].def == def)
            return static_cast<int>(i);
    return 0;
}

void LoadSkinEditor() {
    auto& editor = SkinEditor();
    const auto& table = skinconfig::SkinTable::Global();
    const auto config = FeatureRuntime::Global().Snapshot();
    if (editor.section == 1)
        editor.knifeDef = config.ctKnifeDef;
    else if (editor.section == 2)
        editor.knifeDef = config.tKnifeDef;
    else
        editor.knifeDef = 0;
    if (editor.knifeDef == 0)
        editor.knifeDef = editor.section == 1 ? 507u : 508u;
    skinconfig::SkinEntry entry;
    if (!table.GetExact(SkinSections[editor.section], SkinWeapons[editor.selected], entry)) {
        entry = {};
        entry.paint_kit = 0;
        entry.seed = 0;
        entry.wear = 0.0001f;
    }
    editor.paintKit = std::to_string(entry.paint_kit);
    editor.seed = std::to_string(entry.seed);
    char wear[32];
    std::snprintf(wear, sizeof(wear), "%.6f", static_cast<double>(entry.wear));
    editor.wear = wear;
    editor.loaded = true;
}

bool ParseIntField(const std::string& text, std::int32_t& value) {
    if (text.empty())
        return false;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < -2147483647L || parsed > 2147483647L)
        return false;
    value = static_cast<std::int32_t>(parsed);
    return true;
}

bool ParseWearField(const std::string& text, float& value) {
    if (text.empty())
        return false;
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(parsed))
        return false;
    value = std::clamp(parsed, 0.0f, 1.0f);
    return true;
}

void SaveSkinEditor() {
    auto& editor = SkinEditor();
    skinconfig::SkinEntry entry;
    if (!ParseIntField(editor.paintKit, entry.paint_kit) ||
        !ParseIntField(editor.seed, entry.seed) ||
        !ParseWearField(editor.wear, entry.wear))
        return;
    skinconfig::SkinTable::Global().Set(SkinSections[editor.section],
                                        SkinWeapons[editor.selected], entry);
    if (editor.selected == static_cast<int>(SkinEditorState::WeaponCount) - 1 &&
        editor.section != 0) {
        auto config = FeatureRuntime::Global().Snapshot();
        if (editor.section == 1)
            config.ctKnifeDef = editor.knifeDef;
        else
            config.tKnifeDef = editor.knifeDef;
        FeatureRuntime::Global().Update(config);
    }
    editor.loaded = false;
}

GuiState& State() {
    static GuiState state;
    return state;
}

Window ActiveTopLevel(Display* display, Window root) {
    Window focused = None;
    int revert = RevertToParent;
    XGetInputFocus(display, &focused, &revert);

    if (focused == None || focused == PointerRoot || focused == root) {
        const Atom active = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
        if (active != None) {
            Atom actual = None;
            int format = 0;
            unsigned long count = 0;
            unsigned long remaining = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display, root, active, 0, 1, False, XA_WINDOW,
                                   &actual, &format, &count, &remaining, &data) == Success) {
                if (data && count == 1)
                    focused = *reinterpret_cast<Window*>(data);
                if (data)
                    XFree(data);
            }
        }
    }

    if (focused == None || focused == PointerRoot || focused == root)
        return None;

    Window current = focused;
    for (;;) {
        Window parent = None;
        Window rootReturned = None;
        Window* children = nullptr;
        unsigned int childCount = 0;
        if (!XQueryTree(display, current, &rootReturned, &parent, &children, &childCount))
            return None;
        if (children)
            XFree(children);
        if (parent == None || parent == rootReturned)
            return current;
        current = parent;
    }
}

bool LooksLikeCounterStrike(Display* display, Window window) {
    std::string description;
    char* title = nullptr;
    if (XFetchName(display, window, &title) && title) {
        description += title;
        XFree(title);
    }

    XClassHint classHint{};
    if (XGetClassHint(display, window, &classHint)) {
        if (classHint.res_name) {
            description += ' ';
            description += classHint.res_name;
            XFree(classHint.res_name);
        }
        if (classHint.res_class) {
            description += ' ';
            description += classHint.res_class;
            XFree(classHint.res_class);
        }
    }

    for (char& character : description)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return description.find("counter-strike") != std::string::npos ||
           description.find("counter strike") != std::string::npos ||
           description.find("cs2") != std::string::npos;
}

Window FindGameWindow(Display* display, Window root) {
    const Window active = ActiveTopLevel(display, root);
    if (active && LooksLikeCounterStrike(display, active))
        return active;

    const Atom clientList = XInternAtom(display, "_NET_CLIENT_LIST", True);
    if (clientList != None) {
        Atom actual = None;
        int format = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, root, clientList, 0, 1024, False, XA_WINDOW,
                               &actual, &format, &count, &remaining, &data) == Success) {
            const auto* windows = reinterpret_cast<Window*>(data);
            for (unsigned long index = 0; windows && index < count; ++index) {
                if (LooksLikeCounterStrike(display, windows[index])) {
                    const Window result = windows[index];
                    XFree(data);
                    return result;
                }
            }
            if (data)
                XFree(data);
        }
    }

    return (active && LooksLikeCounterStrike(display, active)) ? active : None;
}

unsigned long Color(Display* display, int screen, const char* value) {
    XColor exact{};
    Colormap map = DefaultColormap(display, screen);
    if (XParseColor(display, map, value, &exact) && XAllocColor(display, map, &exact))
        return exact.pixel;
    return WhitePixel(display, screen);
}

void Text(Display* display, Window window, GC gc, int x, int y, const char* value,
          unsigned long color) {
    XSetForeground(display, gc, color);
    XDrawString(display, window, gc, x, y, value, static_cast<int>(std::strlen(value)));
}

void Checkbox(Display* display, Window window, GC gc, int x, int y, bool enabled,
              unsigned long accent, unsigned long textColor, const char* label) {
    XSetForeground(display, gc, enabled ? accent : textColor);
    XDrawRectangle(display, window, gc, x, y - 13, 16, 16);
    if (enabled)
        XFillRectangle(display, window, gc, x + 4, y - 9, 9, 9);
    Text(display, window, gc, x + 28, y, label, textColor);
}

void DrawCrosshair(Display* display, Window window, GC gc, int screen) {
    constexpr int center = 8;
    constexpr int arm = 5;
    constexpr int thickness = 1;
    XSetForeground(display, gc, Color(display, screen, "#ffffff"));
    XFillRectangle(display, window, gc, 0, center, arm, thickness);
    XFillRectangle(display, window, gc, center + 4, center, arm, thickness);
    XFillRectangle(display, window, gc, center, 0, thickness, arm);
    XFillRectangle(display, window, gc, center, center + 4, thickness, arm);
    XFillRectangle(display, window, gc, center, center, thickness, thickness);
}

void ShapeCrosshair(Display* display, Window window) {
    XRectangle rectangles[] = {
        {0, 8, 5, 1}, {12, 8, 5, 1}, {8, 0, 1, 5},
        {8, 12, 1, 5}, {8, 8, 1, 1}};
    XShapeCombineRectangles(display, window, ShapeBounding, 0, 0, rectangles,
                            static_cast<int>(std::size(rectangles)), ShapeSet, YXBanded);
}

void DrawSkinPage(Display* display, Window window, GC gc, int screen,
                  const FeatureConfig& config) {
    const auto panel = Color(display, screen, "#202936");
    const auto border = Color(display, screen, "#344153");
    const auto textColor = Color(display, screen, "#e7edf5");
    const auto muted = Color(display, screen, "#94a1b4");
    const auto accent = Color(display, screen, "#72b7ff");
    auto& editor = SkinEditor();
    if (!editor.loaded)
        LoadSkinEditor();

    Text(display, window, gc, 176, 48, "Skin manager", textColor);
    Text(display, window, gc, 176, 67, "Edit paint kit, seed, and wear", muted);
    Checkbox(display, window, gc, 328, 78, config.skinsEnabled, accent, textColor, "Skins enabled");
    Text(display, window, gc, 176, 88, "Weapon", muted);
    Text(display, window, gc, 340, 88, "Values", muted);

    constexpr int rows = 8;
    for (int row = 0; row < rows; ++row) {
        const int index = editor.firstVisible + row;
        if (index >= static_cast<int>(SkinWeapons.size()))
            break;
        const int y = 110 + row * 24;
        if (index == editor.selected) {
            XSetForeground(display, gc, panel);
            XFillRectangle(display, window, gc, 168, y - 17, 148, 22);
        }
        Text(display, window, gc, 176, y, SkinWeapons[index],
             index == editor.selected ? accent : textColor);
    }

    Text(display, window, gc, 328, 116, "Paint kit", muted);
    Text(display, window, gc, 410, 116, editor.paintKit.c_str(), textColor);
    Text(display, window, gc, 328, 148, "Seed", muted);
    Text(display, window, gc, 410, 148, editor.seed.c_str(), textColor);
    Text(display, window, gc, 328, 180, "Wear", muted);
    Text(display, window, gc, 410, 180, editor.wear.c_str(), textColor);
    XSetForeground(display, gc, border);
    XDrawRectangle(display, window, gc, 402, 101 + editor.activeField * 32, 92, 22);

    Text(display, window, gc, 328, 218, "Section", muted);
    for (int section = 0; section < 3; ++section) {
        const int x = 328 + section * 54;
        XSetForeground(display, gc, editor.section == section ? accent : border);
        XDrawRectangle(display, window, gc, x, 226, 48, 22);
        Text(display, window, gc, x + 10, 242, SkinSections[section],
             editor.section == section ? accent : textColor);
    }

    if (editor.selected == static_cast<int>(SkinEditorState::WeaponCount) - 1) {
        Text(display, window, gc, 328, 270, "Knife subclass", muted);
        const int choice = KnifeChoiceIndex(editor.knifeDef);
        Text(display, window, gc, 328, 290, KnifeChoices[choice].label, textColor);
        XSetForeground(display, gc, border);
        XDrawRectangle(display, window, gc, 412, 268, 38, 24);
        XDrawRectangle(display, window, gc, 456, 268, 38, 24);
        Text(display, window, gc, 424, 285, "<", textColor);
        Text(display, window, gc, 468, 285, ">", textColor);
    } else {
        Text(display, window, gc, 328, 278, "Select knife to edit its subclass", muted);
    }

    XSetForeground(display, gc, border);
    XDrawRectangle(display, window, gc, 176, 318, 54, 24);
    XDrawRectangle(display, window, gc, 238, 318, 54, 24);
    XDrawRectangle(display, window, gc, 412, 318, 80, 24);
    Text(display, window, gc, 188, 335, "Prev", textColor);
    Text(display, window, gc, 250, 335, "Next", textColor);
    Text(display, window, gc, 432, 335, "Save", accent);
    Text(display, window, gc, 176, 358, "Saved to ~/.config/hamzex/skins.json", muted);
}

void HandleSkinClick(Display* display, Window window, int x, int y) {
    auto& editor = SkinEditor();
    if (!editor.loaded)
        LoadSkinEditor();
    if (x >= 320 && x < 500 && y >= 62 && y < 100) {
        auto config = FeatureRuntime::Global().Snapshot();
        config.skinsEnabled = !config.skinsEnabled;
        FeatureRuntime::Global().Update(config);
        return;
    }
    if (x >= 168 && x < 320 && y >= 93 && y < 302) {
        const int row = (y - 93) / 24;
        const int index = editor.firstVisible + row;
        if (index >= 0 && index < static_cast<int>(SkinWeapons.size())) {
            editor.selected = index;
            LoadSkinEditor();
        }
        return;
    }
    if (x >= 402 && x < 500 && y >= 101 && y < 195) {
        editor.activeField = std::clamp((y - 101) / 32, 0, 2);
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        return;
    }
    if (y >= 226 && y < 250) {
        for (int section = 0; section < 3; ++section) {
            const int sectionX = 328 + section * 54;
            if (x >= sectionX && x < sectionX + 48) {
                editor.section = section;
                LoadSkinEditor();
                return;
            }
        }
    }
    if (editor.selected == static_cast<int>(SkinEditorState::WeaponCount) - 1 &&
        editor.section != 0 && y >= 264 && y < 300) {
        const int current = KnifeChoiceIndex(editor.knifeDef);
        if (x >= 412 && x < 450)
            editor.knifeDef = KnifeChoices[(current + static_cast<int>(KnifeChoices.size()) - 1) %
                                          static_cast<int>(KnifeChoices.size())].def;
        else if (x >= 456 && x < 500)
            editor.knifeDef = KnifeChoices[(current + 1) %
                                          static_cast<int>(KnifeChoices.size())].def;
        return;
    }
    if (y >= 318 && y < 346) {
        if (x >= 176 && x < 230) {
            editor.selected = std::max(0, editor.selected - 1);
            editor.firstVisible = std::min(editor.firstVisible, editor.selected);
            LoadSkinEditor();
        } else if (x >= 238 && x < 292) {
            editor.selected = std::min(static_cast<int>(SkinWeapons.size()) - 1, editor.selected + 1);
            editor.firstVisible = std::max(editor.firstVisible, editor.selected - 7);
            LoadSkinEditor();
        } else if (x >= 412 && x < 500) {
            SaveSkinEditor();
        }
    }
}

void HandleSkinKeyPress(XKeyEvent& event) {
    auto& editor = SkinEditor();
    char text[16] = {};
    KeySym key = 0;
    const int count = XLookupString(&event, text, sizeof(text) - 1, &key, nullptr);
    std::string* field = editor.activeField == 0 ? &editor.paintKit :
                         editor.activeField == 1 ? &editor.seed : &editor.wear;
    if (key == XK_BackSpace) {
        if (!field->empty())
            field->pop_back();
    } else if (key == XK_Tab) {
        editor.activeField = (editor.activeField + 1) % 3;
    } else if (key == XK_Return || key == XK_KP_Enter) {
        SaveSkinEditor();
    } else if (count > 0) {
        for (int i = 0; i < count; ++i) {
            const char ch = text[i];
            const bool digit = ch >= '0' && ch <= '9';
            const bool decimal = ch == '.' && editor.activeField == 2 &&
                                 field->find('.') == std::string::npos;
            const bool minus = ch == '-' && editor.activeField != 2 && field->empty();
            if (digit || decimal || minus)
                field->push_back(ch);
        }
    }
}

void HandlePrimaryClick(Display* display, Window window, int x, int y, int& page) {
    if (x >= 12 && x <= 124 && y >= 76 && y < 200) {
        page = y < 120 ? 0 : (y < 164 ? 1 : 2);
        return;
    }
    if (x < 136 || x > 520)
        return;

    if (page == 2) {
        HandleSkinClick(display, window, x, y);
        return;
    }

    auto config = FeatureRuntime::Global().Snapshot();
    bool changed = false;
    if (page == 0) {
        if (y >= 84 && y < 126) { config.bhopEnabled = !config.bhopEnabled; changed = true; }
        else if (y >= 126 && y < 168) { config.thirdpersonEnabled = !config.thirdpersonEnabled; changed = true; }
        else if (y >= 168 && y < 210) { config.customCharacterEnabled = !config.customCharacterEnabled; changed = true; }
        else if (y >= 210 && y < 252) { config.sniperCrosshair = !config.sniperCrosshair; changed = true; }
    } else {
        if (y >= 76 && y < 116) { config.noFlash = !config.noFlash; changed = true; }
        else if (y >= 116 && y < 148) { config.noSmoke = !config.noSmoke; changed = true; }
        else if (y >= 148 && y < 180) { config.noSkybox = !config.noSkybox; changed = true; }
        else if (y >= 180 && y < 212) { config.fullbright = !config.fullbright; changed = true; }
        else if (y >= 212 && y < 244) { config.onlyRenderOpaque = !config.onlyRenderOpaque; changed = true; }
        else if (y >= 244 && y < 276) { config.changeSmokeColor = !config.changeSmokeColor; changed = true; }
        else if (y >= 276 && y < 308) { config.glowChams = !config.glowChams; changed = true; }
        else if (y >= 308 && y < 340) { config.weaponGlowChams = !config.weaponGlowChams; changed = true; }
    }
    if (changed)
        FeatureRuntime::Global().Update(config);
}

void Draw(Display* display, Window window, GC gc, int screen, int page) {
    constexpr unsigned GUI_WIDTH = 520;
    constexpr unsigned GUI_HEIGHT = 380;
    const auto background = Color(display, screen, "#11151d");
    const auto sidebar = Color(display, screen, "#171d27");
    const auto panel = Color(display, screen, "#202936");
    const auto border = Color(display, screen, "#344153");
    const auto textColor = Color(display, screen, "#e7edf5");
    const auto muted = Color(display, screen, "#94a1b4");
    const auto accent = Color(display, screen, "#72b7ff");

    const auto config = FeatureRuntime::Global().Snapshot();
    XSetForeground(display, gc, background);
    XFillRectangle(display, window, gc, 0, 0, GUI_WIDTH, GUI_HEIGHT);
    XSetForeground(display, gc, sidebar);
    XFillRectangle(display, window, gc, 0, 0, 136, GUI_HEIGHT);

    Text(display, window, gc, 18, 34, "CEYROS", accent);
    Text(display, window, gc, 18, 52, "internal .so menu", muted);
    XSetForeground(display, gc, page == 0 ? panel : sidebar);
    XFillRectangle(display, window, gc, 12, 76, 112, 36);
    Text(display, window, gc, 24, 99, "Movement", textColor);
    XSetForeground(display, gc, page == 1 ? panel : sidebar);
    XFillRectangle(display, window, gc, 12, 120, 112, 36);
    Text(display, window, gc, 24, 143, "Unsafe", textColor);
    XSetForeground(display, gc, page == 2 ? panel : sidebar);
    XFillRectangle(display, window, gc, 12, 164, 112, 36);
    Text(display, window, gc, 24, 187, "Skins", textColor);
    Text(display, window, gc, 18, 350, "HOME: show / hide", muted);

    XSetForeground(display, gc, panel);
    XFillRectangle(display, window, gc, 152, 16, 352, 348);
    XSetForeground(display, gc, border);
    XDrawRectangle(display, window, gc, 152, 16, 351, 347);

    if (page == 0) {
        Text(display, window, gc, 176, 48, "Movement", textColor);
        Text(display, window, gc, 176, 67, "Internal feature controls", muted);
        Checkbox(display, window, gc, 176, 108, config.bhopEnabled, accent, textColor, "Bunny hop");
        Text(display, window, gc, 360, 108, "Mouse4", muted);
        Checkbox(display, window, gc, 176, 150, config.thirdpersonEnabled, accent, textColor, "Thirdperson");
        Text(display, window, gc, 360, 150, "V", muted);
        Checkbox(display, window, gc, 176, 192, config.customCharacterEnabled, accent, textColor, "Gura body + hands");
        Checkbox(display, window, gc, 176, 234, config.sniperCrosshair, accent, textColor, "Sniper crosshair");
        Text(display, window, gc, 176, 274, "Jump input", muted);
        Text(display, window, gc, 176, 294, "Mouse wheel up; A/D stay manual", textColor);
        Text(display, window, gc, 176, 326, "Runtime", muted);
        Text(display, window, gc, 176, 346, "Frame hook + live schema offsets", textColor);
    } else if (page == 1) {
        Text(display, window, gc, 176, 48, "Unsafe features", textColor);
        Text(display, window, gc, 176, 67, "Client-side visual controls", muted);
        Checkbox(display, window, gc, 176, 100, config.noFlash, accent, textColor, "No flash");
        Checkbox(display, window, gc, 176, 132, config.noSmoke, accent, textColor, "No smoke");
        Checkbox(display, window, gc, 176, 164, config.noSkybox, accent, textColor, "No skybox");
        Checkbox(display, window, gc, 176, 196, config.fullbright, accent, textColor, "Fullbright");
        Checkbox(display, window, gc, 176, 228, config.onlyRenderOpaque, accent, textColor, "Only opaque");
        Checkbox(display, window, gc, 176, 260, config.changeSmokeColor, accent, textColor, "Smoke color");
        Checkbox(display, window, gc, 176, 292, config.glowChams, accent, textColor, "Glow chams");
        Checkbox(display, window, gc, 176, 324, config.weaponGlowChams, accent, textColor, "Weapon glow");
    } else {
        DrawSkinPage(display, window, gc, screen, config);
    }
}

void Run(GuiState& state) {
    constexpr unsigned crosshairSize = 17;
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        static Logger log;
        log.warn("Gui: XOpenDisplay failed; HOME GUI is unavailable");
        return;
    }
    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);
    Window gameWindow = None;
    for (int attempt = 0; attempt < 25 && !gameWindow; ++attempt) {
        gameWindow = FindGameWindow(display, root);
        if (!gameWindow)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!gameWindow) {
        static Logger log;
        log.warn("Gui: no focused game window found; internal menu is disabled");
        XCloseDisplay(display);
        return;
    }

    XWindowAttributes parentAttributes{};
    XGetWindowAttributes(display, gameWindow, &parentAttributes);
    const int panelX = std::max(0, (parentAttributes.width - 520) / 2);
    const int panelY = std::max(0, (parentAttributes.height - 380) / 2);
    const Window window = XCreateSimpleWindow(display, gameWindow, panelX, panelY, 520, 380, 0,
                                               BlackPixel(display, screen),
                                               Color(display, screen, "#11151d"));
    XSetWindowAttributes crosshairAttributes{};
    crosshairAttributes.background_pixmap = None;
    crosshairAttributes.border_pixel = 0;
    crosshairAttributes.override_redirect = True;
    const Window crosshairWindow = XCreateWindow(
        display, gameWindow, 0, 0, crosshairSize, crosshairSize, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWBackPixmap | CWBorderPixel | CWOverrideRedirect,
        &crosshairAttributes);
    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask |
                                  ButtonReleaseMask | StructureNotifyMask);
    XSelectInput(display, crosshairWindow, ExposureMask);
    int shapeEvent = 0;
    int shapeError = 0;
    const bool hasShape = XShapeQueryExtension(display, &shapeEvent, &shapeError);
    if (hasShape)
        ShapeCrosshair(display, crosshairWindow);
    const KeyCode homeCode = XKeysymToKeycode(display, XK_Home);
    XGrabButton(display, 8, AnyModifier, root, True,
                ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None);
    XFlush(display);

    GC gc = XCreateGC(display, window, 0, nullptr);
    GC crosshairGc = XCreateGC(display, crosshairWindow, 0, nullptr);
    int page = 0;
    bool visible = false;
    bool lastHome = false;
    bool leftClickHandled = false;
    FeatureRuntime::Global().Input().homeDown.store(false);
    FeatureRuntime::Global().Input().mouse4Down.store(false);

    while (!state.stop.load()) {
        XGetWindowAttributes(display, gameWindow, &parentAttributes);
        const int currentX = std::max(0, (parentAttributes.width - 520) / 2);
        const int currentY = std::max(0, (parentAttributes.height - 380) / 2);
        XMoveWindow(display, window, currentX, currentY);
        const int crosshairX = std::max(0, (parentAttributes.width - static_cast<int>(crosshairSize)) / 2);
        const int crosshairY = std::max(0, (parentAttributes.height - static_cast<int>(crosshairSize)) / 2);
        XMoveWindow(display, crosshairWindow, crosshairX, crosshairY);
        char keymap[32] = {};
        XQueryKeymap(display, keymap);
        const bool home = homeCode != 0 && (keymap[homeCode / 8] & (1 << (homeCode % 8)));
        const KeyCode vCode = XKeysymToKeycode(display, XK_v);
        const bool vDown = vCode != 0 && (keymap[vCode / 8] & (1 << (vCode % 8)));
        FeatureRuntime::Global().Input().homeDown.store(home, std::memory_order_relaxed);
        FeatureRuntime::Global().Input().vDown.store(vDown, std::memory_order_relaxed);
        if (home && !lastHome) {
            visible = !visible;
            if (visible)
                XMapRaised(display, window);
            else {
                XUnmapWindow(display, window);
                XSetInputFocus(display, gameWindow, RevertToParent, CurrentTime);
            }
        }
        lastHome = home;

        while (XPending(display)) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type == ButtonPress) {
                if (event.xbutton.button == 8) {
                    FeatureRuntime::Global().Input().mouse4Down.store(true, std::memory_order_relaxed);
                }
                if (visible && event.xbutton.window == window && event.xbutton.button == Button1 &&
                    !leftClickHandled) {
                    HandlePrimaryClick(display, window, event.xbutton.x, event.xbutton.y, page);
                    leftClickHandled = true;
                }
            } else if (event.type == KeyPress && visible && page == 2) {
                HandleSkinKeyPress(event.xkey);
            } else if (event.type == ButtonRelease) {
                if (event.xbutton.button == 8)
                    FeatureRuntime::Global().Input().mouse4Down.store(false, std::memory_order_relaxed);
                if (event.xbutton.button == Button1)
                    leftClickHandled = false;
            }
        }

        // Some fullscreen/compositor combinations deliver the child-window
        // click to CS2 instead of this X11 child. Poll the pointer as a
        // fallback so the controls still toggle.
        if (visible) {
            Window pointerRoot = None;
            Window pointerChild = None;
            int pointerRootX = 0;
            int pointerRootY = 0;
            int pointerX = 0;
            int pointerY = 0;
            unsigned pointerMask = 0;
            const bool pointerOnMenu = XQueryPointer(display, window, &pointerRoot, &pointerChild,
                                                     &pointerRootX, &pointerRootY,
                                                     &pointerX, &pointerY, &pointerMask);
            const bool leftDown = pointerOnMenu && (pointerMask & Button1Mask) != 0;
            if (leftDown && !leftClickHandled) {
                HandlePrimaryClick(display, window, pointerX, pointerY, page);
                leftClickHandled = true;
            } else if (!leftDown) {
                leftClickHandled = false;
            }
        }

        if (visible) {
            Draw(display, window, gc, screen, page);
            XFlush(display);
        }
        const bool crosshairEnabled =
            FeatureRuntime::Global().SniperCrosshairVisible() && !visible;
        if (crosshairEnabled && hasShape) {
            XMapRaised(display, crosshairWindow);
            XClearWindow(display, crosshairWindow);
            DrawCrosshair(display, crosshairWindow, crosshairGc, screen);
        } else {
            XUnmapWindow(display, crosshairWindow);
        }
        XFlush(display);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    FeatureRuntime::Global().Input().homeDown.store(false);
    FeatureRuntime::Global().Input().vDown.store(false);
    FeatureRuntime::Global().Input().mouse4Down.store(false);
    XUngrabButton(display, 8, AnyModifier, root);
    if (visible)
        XUnmapWindow(display, window);
    XUnmapWindow(display, crosshairWindow);
    XFreeGC(display, crosshairGc);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XDestroyWindow(display, crosshairWindow);
    XCloseDisplay(display);
}

}  // namespace gui_detail

using namespace gui_detail;

export class Gui {
public:
    static Gui& Global();
    void Start();
    void Stop();
};

Gui& Gui::Global() {
    static Gui gui;
    return gui;
}

void Gui::Start() {
    auto& state = State();
    if (state.running.exchange(true))
        return;
    state.stop.store(false);
    state.thread = std::thread([&state] { Run(state); state.running.store(false); });
}

void Gui::Stop() {
    auto& state = State();
    state.stop.store(true);
    if (state.thread.joinable())
        state.thread.join();
    state.running.store(false);
}
