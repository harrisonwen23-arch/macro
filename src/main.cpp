#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>
#include <array>
#include <type_traits>
#include <utility>
#include <filesystem>
#include <system_error>
#include <chrono>

// ===== MOD 1/7 (solver): worker thread + the isolated simulation engine =====
#include <thread>
#include <atomic>
#include <mutex>
#include <cfloat>
#include <memory>
#include "solver.hpp" // pure-C++ simulator/search: no Geode types, safe off the main thread
// ===== end MOD 1/7 =====

using namespace geode::prelude;


// sub = which processCommands call inside that step applied the input (0 = first). Presses and
// releases both carry it, so an input that the game applied in a later half-tick of a step is
// replayed in that same half-tick instead of at the start of the step.
struct MacroInput { int frame; int button; bool down; bool p1; int sub = 0; };

struct MacroAnchor {
	int frame;
	float x1, y1, v1;
	bool dual;
	float x2, y2, v2;
};

static int g_mode = 0; // 0 = off, 1 = record, 2 = play
static std::vector<MacroInput> g_inputs;
static std::vector<MacroAnchor> g_anchors;
static size_t g_cursor = 0;
static size_t g_anchorCursor = 0;
static int g_lastAnchorFrame = -1;
static int g_corrections = 0;
static int g_frame = 0;
static bool g_injecting = false;
static int g_macroTps = 0; // tick rate the loaded macro was recorded at (0 = unknown)
static float g_speed = 1.f;
static bool g_speedTouched = false;
static std::string g_selected;
static std::string g_filterText;
static std::string g_folder; // "" = all, "(none)" = untagged
static bool g_pathDirty = true;
static size_t g_pathKey = static_cast<size_t>(-1);

// step-exact recording: inputs are stamped with the step counter read at the START of
// processCommands (the same point playback injects them), not whatever the counter is when
// handleButton happens to run. Inputs that arrive outside processCommands are stamped with
// the step of the next processCommands call, which is the step that really applies them.
static bool g_inCmd = false;
static std::vector<size_t> g_pendStamp;
static int g_lastRawStep = -1;  // step counter seen by the previous processCommands call
static int g_callIdx = 0;       // which processCommands call of the current step this is (0 = first)
static std::string g_divNear;   // inputs close to the first desync, for the report

// divergence finder: where playback first stops matching the recording
static int g_divFrame = -1;     // first frame that differs by more than the limit (-1 = none yet)
static float g_divDx = 0.f;     // offset at that frame (live - recorded)
static float g_divDy = 0.f;
static int g_divWhich = 1;      // which player it was (1 or 2)
static int g_divCount = 0;      // how many checked frames were off
static int g_divChecked = 0;    // how many frames were compared
static float g_divMax = 0.f;    // biggest offset seen
static bool g_divReported = false;

static constexpr float kSpeedMin = 0.1f;
static constexpr float kSpeedMax = 5.f;

// ---------- designs ----------
// Every design is a gradient from a dark top colour to a lighter bottom colour, plus an
// accent colour, a panel shape and a default animated background.

struct ShapeDef { char const* name; char const* tex; };
static const ShapeDef kShapes[] = {
	{ "Rounded", "square02b_001.png" },
	{ "Soft", "square02_001.png" },
	{ "Classic", "GJ_square01.png" },
	{ "Dark", "GJ_square05.png" },
};
static const int kShapeCount = 4;

static const ccColor3B kAccents[] = {
	{ 70, 140, 255 }, { 70, 200, 110 }, { 240, 100, 170 },
	{ 255, 150, 60 }, { 160, 110, 240 }, { 240, 200, 60 },
};

// bg: 0 Meteors, 1 Stars, 2 Snow, 3 Rain, 4 Particles, 5 Plain
struct Design {
	char const* name;
	ccColor3B top;
	ccColor3B bot;
	ccColor3B accent;
	int shape;
	int bg;
};

static const Design kDesigns[] = {
	// dark -> light greys
	{ "Graphite", { 24, 24, 28 }, { 118, 120, 126 }, { 70, 140, 255 }, 0, 0 },
	{ "Steel", { 30, 34, 40 }, { 128, 136, 146 }, { 110, 190, 255 }, 0, 0 },
	{ "Ash", { 34, 34, 34 }, { 150, 150, 150 }, { 240, 200, 60 }, 1, 1 },
	{ "Smoke", { 20, 20, 22 }, { 110, 110, 115 }, { 160, 110, 240 }, 0, 0 },
	{ "Slate Fade", { 28, 32, 38 }, { 120, 130, 142 }, { 70, 200, 110 }, 1, 0 },
	{ "Charcoal", { 16, 16, 18 }, { 96, 96, 100 }, { 255, 150, 60 }, 0, 0 },
	{ "Silver", { 40, 42, 46 }, { 160, 162, 168 }, { 70, 140, 255 }, 1, 1 },
	{ "Pewter", { 36, 36, 40 }, { 136, 134, 140 }, { 240, 100, 170 }, 0, 4 },
	{ "Cloud", { 52, 54, 58 }, { 170, 172, 178 }, { 90, 170, 255 }, 1, 2 },
	{ "Moonstone", { 30, 30, 36 }, { 140, 142, 156 }, { 170, 150, 255 }, 0, 1 },
	{ "Gunmetal", { 18, 20, 24 }, { 104, 110, 120 }, { 70, 200, 200 }, 3, 0 },
	{ "Concrete", { 44, 44, 44 }, { 146, 142, 136 }, { 255, 170, 70 }, 2, 5 },
	{ "Fog", { 60, 62, 66 }, { 176, 178, 182 }, { 110, 170, 230 }, 1, 2 },
	{ "Obsidian", { 8, 8, 10 }, { 84, 84, 90 }, { 255, 90, 60 }, 3, 0 },
	// greys with a colour accent
	{ "Grey Ember", { 26, 24, 24 }, { 124, 118, 114 }, { 255, 120, 40 }, 0, 0 },
	{ "Grey Mint", { 24, 28, 28 }, { 116, 126, 124 }, { 90, 230, 170 }, 0, 4 },
	{ "Grey Rose", { 30, 26, 28 }, { 132, 120, 126 }, { 255, 110, 160 }, 1, 1 },
	{ "Grey Gold", { 28, 28, 24 }, { 130, 128, 116 }, { 255, 205, 70 }, 0, 0 },
	{ "Grey Violet", { 28, 26, 34 }, { 124, 120, 138 }, { 180, 120, 255 }, 0, 1 },
	{ "Grey Cyan", { 22, 28, 32 }, { 114, 126, 134 }, { 70, 220, 255 }, 1, 3 },
	// coloured
	{ "Midnight", { 12, 14, 40 }, { 70, 80, 150 }, { 110, 150, 255 }, 0, 1 },
	{ "Purple Haze", { 36, 16, 64 }, { 130, 90, 170 }, { 220, 140, 255 }, 1, 4 },
	{ "Deep Teal", { 6, 40, 48 }, { 60, 130, 140 }, { 70, 230, 210 }, 0, 3 },
	{ "Crimson Night", { 60, 10, 22 }, { 160, 70, 86 }, { 255, 90, 110 }, 0, 0 },
	{ "Forest", { 10, 40, 26 }, { 80, 130, 96 }, { 120, 230, 130 }, 1, 4 },
	{ "Rose Quartz", { 70, 24, 56 }, { 176, 110, 160 }, { 255, 150, 210 }, 1, 1 },
	{ "Sunset", { 50, 14, 24 }, { 210, 120, 70 }, { 255, 190, 80 }, 0, 0 },
	{ "Ocean", { 8, 28, 56 }, { 70, 150, 190 }, { 90, 210, 255 }, 0, 3 },
	{ "Aurora", { 10, 30, 40 }, { 90, 170, 140 }, { 110, 255, 190 }, 1, 4 },
	{ "Ember", { 30, 10, 8 }, { 190, 90, 40 }, { 255, 160, 60 }, 0, 0 },
	{ "Lava", { 24, 6, 6 }, { 170, 50, 30 }, { 255, 110, 40 }, 3, 0 },
	{ "Toxic", { 12, 24, 10 }, { 110, 170, 60 }, { 170, 255, 70 }, 3, 4 },
	{ "Royal", { 20, 14, 60 }, { 110, 90, 200 }, { 150, 130, 255 }, 0, 1 },
	{ "Plum", { 40, 14, 40 }, { 150, 90, 140 }, { 235, 120, 220 }, 1, 4 },
	{ "Mocha", { 30, 22, 18 }, { 150, 120, 100 }, { 240, 180, 110 }, 0, 5 },
	{ "Sand", { 50, 44, 36 }, { 190, 176, 150 }, { 255, 200, 100 }, 1, 5 },
	{ "Ice", { 26, 40, 56 }, { 170, 200, 224 }, { 140, 220, 255 }, 1, 2 },
	{ "Mint", { 14, 40, 36 }, { 130, 190, 170 }, { 120, 255, 200 }, 1, 2 },
	{ "Coral", { 60, 22, 26 }, { 220, 130, 120 }, { 255, 140, 110 }, 0, 4 },
	{ "Neon Pink", { 30, 6, 30 }, { 190, 50, 150 }, { 255, 70, 200 }, 3, 4 },
	{ "Cyber", { 6, 10, 24 }, { 0, 130, 160 }, { 0, 240, 255 }, 3, 3 },
	{ "Dusk", { 30, 20, 50 }, { 190, 120, 120 }, { 255, 170, 120 }, 0, 1 },
	{ "Storm", { 20, 24, 32 }, { 100, 116, 140 }, { 150, 190, 255 }, 0, 3 },
	{ "Eclipse", { 4, 4, 6 }, { 60, 40, 30 }, { 255, 150, 50 }, 3, 0 },
	{ "Monochrome", { 0, 0, 0 }, { 130, 130, 130 }, { 255, 255, 255 }, 2, 1 },
	{ "Daylight", { 70, 72, 76 }, { 190, 192, 196 }, { 40, 120, 240 }, 1, 5 },
};
static const int kDesignCount = sizeof(kDesigns) / sizeof(kDesigns[0]);

// ---------- settings table ----------

static char const* const kAccentSetNames[] = { "Design", "Blue", "Green", "Pink", "Orange", "Purple", "Gold" };
static char const* const kBgSetNames[] = { "Design", "Meteors", "Stars", "Snow", "Rain", "Particles", "Plain" };
static char const* const kAnimNames[] = { "None", "Fade", "Slide", "Bounce" };
static char const* const kAmountNames[] = { "Few", "Normal", "Many" };
static char const* const kSpeedNames[] = { "Slow", "Normal", "Fast" };
static char const* const kCornerNames[] = { "Bottom right", "Bottom left", "Top right", "Top left" };
static char const* const kOffOn[] = { "Off", "On" };
static char const* const kEveryNames[] = { "10 frames", "20 frames", "40 frames" };
static char const* const kNameFmtNames[] = { "Numbered", "Always numbered", "With date" };
static char const* const kPfModeNames[] = { "Off", "Route", "Trail", "Both" };
static char const* const kAheadNames[] = { "All", "100 frames", "300 frames", "600 frames" };
static char const* const kSortNames[] = { "Name", "Newest", "Longest" };

struct SettingDef {
	char const* section;
	char const* key;
	char const* label;
	int min, max, step, def;
	char const* const* names; // nullptr = numeric
	int div;
	char const* unit;
};

static const SettingDef kSettings[] = {
	// Look
	{ "Look", "design", "Design", 0, kDesignCount - 1, 1, 0, nullptr, 1, "" },
	{ "Look", "ui-size", "Window size", 85, 100, 5, 90, nullptr, 1, "%" },
	{ "Look", "ui-opacity", "Opacity", 30, 100, 10, 100, nullptr, 1, "%" },
	{ "Look", "accent", "Accent", 0, 6, 1, 0, kAccentSetNames, 1, "" },
	{ "Look", "anim", "Open animation", 0, 3, 1, 0, kAnimNames, 1, "" },
	{ "Look", "bg-style", "Background", 0, 6, 1, 0, kBgSetNames, 1, "" },
	{ "Look", "bg-amount", "Amount", 0, 2, 1, 1, kAmountNames, 1, "" },
	{ "Look", "bg-speed", "Bg speed", 0, 2, 1, 1, kSpeedNames, 1, "" },
	{ "Look", "font-size", "List text", 60, 100, 10, 100, nullptr, 1, "%" },
	// Layout
	{ "Layout", "compact", "Compact mode", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "Layout", "pause-corner", "Pause button", 0, 3, 1, 0, kCornerNames, 1, "" },
	{ "Layout", "confirm-delete", "Confirm delete", 0, 1, 1, 0, kOffOn, 1, "" },
	// HUD
	{ "HUD", "hud", "Mini HUD", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "HUD", "hud-pos", "HUD corner", 0, 3, 1, 3, kCornerNames, 1, "" },
	{ "HUD", "hud-frame", "Frame counter", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "HUD", "hud-corr", "Corrections", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "HUD", "hud-dot", "Recording dot", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "HUD", "counter", "Click counter", 0, 1, 1, 0, kOffOn, 1, "" },
	// Recording
	{ "Recording", "autosave", "Auto-save", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "Recording", "name-fmt", "Name format", 0, 2, 1, 0, kNameFmtNames, 1, "" },
	{ "Recording", "step-rec", "Step-exact rec", 0, 1, 1, 1, kOffOn, 1, "" },
	// Playback
	{ "Playback", "anchors", "Anchors", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "Playback", "anchor-tol", "Anchor strength", 5, 100, 5, 20, nullptr, 10, "" },
	{ "Playback", "anchor-every", "Anchor interval", 0, 2, 1, 1, kEveryNames, 1, "" },
	{ "Playback", "exact-route", "Exact route", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "Playback", "route-vel", "Restore speed", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "Playback", "snap", "Snap to route", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "Playback", "pitch", "Audio pitch", 0, 1, 1, 1, kOffOn, 1, "" },
	// Pathfinder
	{ "Pathfinder", "pf-mode", "Pathfinder", 0, 3, 1, 0, kPfModeNames, 1, "" },
	{ "Pathfinder", "pf-width", "Line width", 1, 4, 1, 2, nullptr, 1, "" },
	{ "Pathfinder", "pf-dots", "Anchor dots", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "Pathfinder", "pf-ahead", "Route ahead", 0, 3, 1, 1, kAheadNames, 1, "" },
	// Accuracy
	{ "Accuracy", "div-find", "Desync finder", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "Accuracy", "div-tol", "Desync limit", 1, 50, 1, 5, nullptr, 1, "/100" },
	// ===== MOD 2/7 (solver): settings =====
	{ "Solver", "sv-tick", "Decision ticks", 1, 16, 1, 4, nullptr, 1, "" },      // length of one hold/release choice
	{ "Solver", "sv-window", "Lookahead", 5, 60, 5, 20, nullptr, 1, " blocks" }, // search window per round
	{ "Solver", "sv-budget", "Search budget", 1, 20, 1, 4, nullptr, 1, "00k" },  // nodes per window
	{ "Solver", "sv-taps", "Prefer taps", 0, 1, 1, 1, kOffOn, 1, "" },           // release by default, press only when needed
	{ "Solver", "sv-anchors", "Add anchors", 0, 1, 1, 1, kOffOn, 1, "" },        // add the route's positions as anchors after solving
	// ===== end MOD 2/7 =====
};
static const int kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);

static int getInt(char const* key, int def) {
	return Mod::get()->getSavedValue<int>(key, def);
}

static void setInt(char const* key, int v) {
	Mod::get()->setSavedValue<int>(key, v);
}

static int getSetting(int idx) {
	auto& d = kSettings[idx];
	return std::clamp(getInt(d.key, d.def), d.min, d.max);
}

static int settingIndex(char const* key) {
	for (int i = 0; i < kSettingCount; i++) {
		if (std::strcmp(kSettings[i].key, key) == 0) return i;
	}
	return -1;
}

static int S(char const* key) {
	int i = settingIndex(key);
	return i < 0 ? 0 : getSetting(i);
}

// tick rate chosen by the user (1 - 10000, any value)
static int curTps() { return std::clamp(getInt("tps", 240), 1, 10000); }

static bool anchorsOn() { return S("anchors") != 0; }
static float anchorTol() { return S("anchor-tol") / 10.f; }
static int anchorEvery() {
	static const int v[] = { 10, 20, 40 };
	return v[std::clamp(S("anchor-every"), 0, 2)];
}

static Design const& curDesign() {
	return kDesigns[std::clamp(S("design"), 0, kDesignCount - 1)];
}

static ccColor3B accentColor() {
	int v = S("accent");
	if (v <= 0) return curDesign().accent;
	return kAccents[std::clamp(v - 1, 0, 5)];
}

// 0 Meteors, 1 Stars, 2 Snow, 3 Rain, 4 Particles, 5 Plain
static int bgKind() {
	int v = S("bg-style");
	return v <= 0 ? curDesign().bg : std::clamp(v - 1, 0, 5);
}

static ccColor3B mixColor(ccColor3B a, ccColor3B b, float t) {
	auto m = [&](GLubyte x, GLubyte y) { return static_cast<GLubyte>(x + (y - x) * t); };
	return { m(a.r, b.r), m(a.g, b.g), m(a.b, b.b) };
}

static CCScale9Sprite* makePanel(int shape, ccColor3B color, CCSize size) {
	auto panel = CCScale9Sprite::create(kShapes[std::clamp(shape, 0, kShapeCount - 1)].tex, { 0.f, 0.f, 80.f, 80.f });
	if (!panel) panel = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
	panel->setColor(color);
	panel->setContentSize(size);
	return panel;
}

// grey button: the active one is tinted with the accent colour
static CCMenuItemSpriteExtra* makeButton(char const* text, CCObject* target, SEL_MenuHandler cb, float scale) {
	auto spr = ButtonSprite::create(text, 0, false, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.8f);
	spr->setScale(scale);
	return CCMenuItemSpriteExtra::create(spr, target, cb);
}

// Shrinks a label so it never grows wider than maxW.
static void fitLabel(CCLabelBMFont* label, float maxW, float scale) {
	label->setScale(scale);
	float w = label->getContentSize().width;
	if (w > 0.f && w * scale > maxW) label->setScale(maxW / w);
}

// ---------- speed ----------

// GD resets the scheduler time scale on level restart, so re-apply every
// frame, multiplied by the level's own time warp.
// Slows / speeds the game's audio together with the speed hack. If the game's
// audio API looks different, this quietly does nothing instead of breaking the build.
static void applyPitch(float pitch) {
	auto eng = FMODAudioEngine::sharedEngine();
	if (!eng) return;
	[&](auto* e) {
		if constexpr (requires(FMOD::ChannelGroup* g) { e->m_system->getMasterChannelGroup(&g); g->setPitch(1.f); }) {
			FMOD::ChannelGroup* g = nullptr;
			e->m_system->getMasterChannelGroup(&g);
			if (g) g->setPitch(pitch);
		}
	}(eng);
}

static float pitchFor() {
	return S("pitch") != 0 ? g_speed : 1.f;
}

static void enforceSpeed(float timeWarp) {
	// custom TPS above 240: the level's time warp is replaced by the tps trick (see update()),
	// so the scheduler must run at normal speed
	bool tpsOn = curTps() > 240;
	if (!g_speedTouched && !tpsOn) return;
	if (tpsOn || timeWarp <= 0.f) timeWarp = 1.f;
	CCDirector::get()->getScheduler()->setTimeScale(g_speed * timeWarp);
	applyPitch(pitchFor());
}

static void applySpeed() {
	g_speedTouched = true;
	CCDirector::get()->getScheduler()->setTimeScale(g_speed);
	applyPitch(pitchFor());
}

// ---------- macro files ----------

static std::filesystem::path macroDir() {
	auto dir = Mod::get()->getSaveDir() / "macros";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	return dir;
}

static std::filesystem::path macroFile(std::string const& name) {
	return macroDir() / (name + ".macro");
}

static std::vector<std::string> listMacros() {
	std::vector<std::string> names;
	std::error_code ec;
	for (auto& e : std::filesystem::directory_iterator(macroDir(), ec)) {
		if (e.path().extension() == ".macro") names.push_back(e.path().stem().string());
	}
	std::sort(names.begin(), names.end());
	return names;
}

// Format (one item per line):
//   T <epoch>                                                  -> saved time
//   <frame> <button> <down 0/1> <p1 0/1> [sub-step]          -> input (press or release)
//   A <frame> <x1> <y1> <v1> <dual 0/1> <x2> <y2> <v2>        -> anchor
static void serializeMacro(std::ostream& out) {
	out << std::setprecision(9);
	out << "T " << static_cast<long long>(std::time(nullptr)) << '\n';
	out << "R " << (g_macroTps > 0 ? g_macroTps : curTps()) << '\n';
	for (auto& i : g_inputs) {
		out << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0);
		if (i.sub != 0) out << ' ' << i.sub;
		out << '\n';
	}
	for (auto& a : g_anchors) {
		out << "A " << a.frame << ' ' << a.x1 << ' ' << a.y1 << ' ' << a.v1 << ' '
			<< (a.dual ? 1 : 0) << ' ' << a.x2 << ' ' << a.y2 << ' ' << a.v2 << '\n';
	}
}

static void parseMacro(std::istream& in, std::vector<MacroInput>& inputs, std::vector<MacroAnchor>& anchors) {
	inputs.clear();
	anchors.clear();
	g_macroTps = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == 'T') continue;
		if (line[0] == 'R') { g_macroTps = std::atoi(line.c_str() + 1); continue; }
		if (line[0] == 'W') {
			continue; // old frame-window line from an earlier version: ignored
		} else if (line[0] == 'A') {
			std::istringstream s(line.substr(1));
			MacroAnchor a{};
			int dual = 0;
			if (s >> a.frame >> a.x1 >> a.y1 >> a.v1 >> dual >> a.x2 >> a.y2 >> a.v2) {
				a.dual = dual != 0;
				anchors.push_back(a);
			}
		} else {
			std::istringstream s(line);
			int frame, button, down, p1;
			int sub = 0;
			if (s >> frame >> button >> down >> p1) {
				if (!(s >> sub)) sub = 0;
				inputs.push_back({ frame, button, down != 0, p1 != 0, sub });
			}
		}
	}
}

static bool writeMacro(std::string const& name) {
	std::ofstream out(macroFile(name));
	if (!out) return false;
	serializeMacro(out);
	return true;
}

static bool readMacro(std::string const& name, std::vector<MacroInput>& inputs, std::vector<MacroAnchor>& anchors) {
	std::ifstream in(macroFile(name));
	if (!in) return false;
	parseMacro(in, inputs, anchors);
	return true;
}

struct MacroInfo {
	std::string name;
	int clicks = 0;
	int last = 0;
	long long stamp = 0;
};

static MacroInfo readInfo(std::string const& name) {
	MacroInfo info;
	info.name = name;
	std::ifstream in(macroFile(name));
	if (!in) return info;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty()) continue;
		if (line[0] == 'T') {
			info.stamp = std::atoll(line.c_str() + 1);
		} else if (line[0] != 'A' && line[0] != 'R' && line[0] != 'W') {
			std::istringstream s(line);
			int frame, button, down, p1;
			if (s >> frame >> button >> down >> p1) {
				if (down) info.clicks++;
				info.last = std::max(info.last, frame);
			}
		}
	}
	return info;
}

static std::string cleanName(std::string const& s) {
	std::string out;
	for (char c : s) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') out += c;
	}
	auto start = out.find_first_not_of(' ');
	if (start == std::string::npos) return "";
	out = out.substr(start);
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

static std::string lowerStr(std::string s) {
	for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

static std::string uniqueName(std::string const& base, bool startAtOne) {
	if (!startAtOne && !std::filesystem::exists(macroFile(base))) return base;
	for (int n = 1; n < 10000; n++) {
		auto name = base + " " + std::to_string(n);
		if (!std::filesystem::exists(macroFile(name))) return name;
	}
	return base + " " + std::to_string(10000);
}

static std::string nextMacroName() {
	std::string base = "Macro";
	if (auto pl = PlayLayer::get()) {
		if (pl->m_level) {
			auto cleaned = cleanName(std::string(pl->m_level->m_levelName));
			if (!cleaned.empty()) base = cleaned;
		}
	}
	int fmt = std::clamp(S("name-fmt"), 0, 2);
	if (fmt == 1) return uniqueName(base, true);
	if (fmt == 2) {
		std::time_t t = std::time(nullptr);
		char buf[16] = { 0 };
		if (auto tm = std::localtime(&t)) std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
		return uniqueName(base + " " + buf, false);
	}
	return uniqueName(base, false);
}

// ---------- favourites and tags ----------

static std::set<std::string> loadFavs() {
	std::set<std::string> s;
	std::istringstream in(Mod::get()->getSavedValue<std::string>("favs", ""));
	std::string l;
	while (std::getline(in, l)) if (!l.empty()) s.insert(l);
	return s;
}

static void saveFavs(std::set<std::string> const& s) {
	std::string out;
	for (auto& n : s) out += n + "\n";
	Mod::get()->setSavedValue<std::string>("favs", out);
}

static std::map<std::string, std::string> loadTags() {
	std::map<std::string, std::string> m;
	std::istringstream in(Mod::get()->getSavedValue<std::string>("tags", ""));
	std::string l;
	while (std::getline(in, l)) {
		auto p = l.find('\t');
		if (p == std::string::npos || p == 0) continue;
		m[l.substr(0, p)] = l.substr(p + 1);
	}
	return m;
}

static void saveTags(std::map<std::string, std::string> const& m) {
	std::string out;
	for (auto& [k, v] : m) if (!v.empty()) out += k + "\t" + v + "\n";
	Mod::get()->setSavedValue<std::string>("tags", out);
}

// ---------- animated background ----------

static ccColor4F starColor(float r, float g, float b, float a) {
	return ccc4f(r * a, g * a, b * a, a);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Burning colour ramp: h = 1 is white-hot, 0 is dull dark red.
static void fireColor(float h, float& r, float& g, float& b) {
	struct K { float h, r, g, b; };
	static const K ks[] = {
		{ 0.f, 110.f, 22.f, 8.f }, { 0.3f, 225.f, 70.f, 18.f }, { 0.6f, 255.f, 150.f, 55.f },
		{ 0.85f, 255.f, 215.f, 140.f }, { 1.f, 255.f, 250.f, 232.f }
	};
	h = std::clamp(h, 0.f, 1.f);
	for (int i = 0; i < 4; i++) {
		if (h <= ks[i + 1].h) {
			float t = (h - ks[i].h) / (ks[i + 1].h - ks[i].h);
			r = lerpf(ks[i].r, ks[i + 1].r, t) / 255.f;
			g = lerpf(ks[i].g, ks[i + 1].g, t) / 255.f;
			b = lerpf(ks[i].b, ks[i + 1].b, t) / 255.f;
			return;
		}
	}
	r = 1.f; g = 0.98f; b = 0.91f;
}

// type: 0 Meteors, 1 Stars, 2 Snow, 3 Rain, 4 Particles
class BgField : public CCNode {
protected:
	struct Shoot { float x0, y0, dx, dy, len, t, speed; };
	struct Meteor { float x0, y0, dx, dy, len, t, speed, size, heat, emit; };
	struct Ember { float x, y, vx, vy, life, maxLife, size, heat; };
	struct Twinkle { float x, y, phase, speed, size; };
	struct P { float x, y, vx, vy, size, phase; };

	CCDrawNode* m_glow = nullptr; // additive layer: bloom
	CCDrawNode* m_draw = nullptr; // normal layer
	CCSize m_area;
	float m_timeMul = 1.f;
	float m_amount = 1.f;
	int m_type = 0;
	ccColor3B m_accent = { 255, 255, 255 };
	std::vector<Shoot> m_shoots;
	std::vector<Meteor> m_meteors;
	std::vector<Ember> m_embers;
	std::vector<Twinkle> m_twinkles;
	std::vector<P> m_ps;

	static float rnd() {
		return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
	}

	void respawn(Shoot& s) {
		float ang = 0.55f + rnd() * 0.35f;
		float maxLen = std::min(m_area.width, m_area.height) * 0.7f;
		s.len = std::min(maxLen, 90.f + rnd() * 90.f);
		float lx = s.len * std::cos(ang);
		float ly = s.len * std::sin(ang);
		s.x0 = lx + rnd() * std::max(1.f, m_area.width - lx - 14.f);
		s.y0 = ly + rnd() * std::max(1.f, m_area.height - ly - 14.f);
		s.dx = -lx;
		s.dy = -ly;
		s.t = -rnd() * 2.f;
		s.speed = 0.4f + rnd() * 0.4f;
	}

	void respawnMeteor(Meteor& m) {
		float ang = 0.5f + rnd() * 0.5f;
		float maxLen = std::min(m_area.width, m_area.height) * 0.95f;
		m.len = std::min(maxLen, 150.f + rnd() * 160.f);
		float lx = m.len * std::cos(ang);
		float ly = m.len * std::sin(ang);
		m.x0 = lx + rnd() * std::max(1.f, m_area.width - lx - 10.f);
		m.y0 = ly + rnd() * std::max(1.f, m_area.height - ly - 10.f);
		m.dx = -lx;
		m.dy = -ly;
		m.t = -rnd() * 3.5f;
		m.speed = 0.28f + rnd() * 0.3f;
		m.size = 0.8f + rnd() * 0.9f;
		m.heat = 0.7f + rnd() * 0.3f;
		m.emit = 0.f;
	}

	bool init(CCSize area, int type, int amount, int speedIdx) {
		if (!CCNode::init()) return false;
		m_area = area;
		m_type = type;
		m_accent = accentColor();
		static const float tmul[] = { 0.5f, 1.f, 2.f };
		static const float amul[] = { 0.4f, 1.f, 2.f };
		m_timeMul = tmul[std::clamp(speedIdx, 0, 2)];
		float am = amul[std::clamp(amount, 0, 2)];
		m_amount = am;

		m_glow = CCDrawNode::create();
		// additive blending for the bloom; skipped quietly if this build has no such call
		[](auto* d) {
			if constexpr (requires { d->setBlendFunc(ccBlendFunc{ GL_ONE, GL_ONE }); }) {
				d->setBlendFunc(ccBlendFunc{ GL_ONE, GL_ONE });
			}
		}(m_glow);
		this->addChild(m_glow);
		m_draw = CCDrawNode::create();
		this->addChild(m_draw);

		if (type == 0 || type == 1) {
			int twinkles = static_cast<int>(40 * am);
			for (int i = 0; i < twinkles; i++) {
				m_twinkles.push_back({
					rnd() * area.width, rnd() * area.height,
					rnd() * 6.28f, 1.f + rnd() * 2.f, 0.5f + rnd() * 1.1f
				});
			}
		}
		if (type == 0) {
			int n = std::max(1, static_cast<int>(std::lround(3 * am)));
			for (int i = 0; i < n; i++) {
				Meteor m{};
				respawnMeteor(m);
				m.t -= static_cast<float>(i) * 0.7f;
				m_meteors.push_back(m);
			}
		} else if (type == 1) {
			int shoots = std::max(1, static_cast<int>(4 * am));
			for (int i = 0; i < shoots; i++) {
				Shoot s{};
				respawn(s);
				s.t -= static_cast<float>(i) * 0.6f;
				m_shoots.push_back(s);
			}
		} else {
			int n = static_cast<int>((type == 2 ? 60 : (type == 3 ? 50 : 40)) * am);
			for (int i = 0; i < n; i++) {
				P p{};
				p.x = rnd() * area.width;
				p.y = rnd() * area.height;
				p.phase = rnd() * 6.28f;
				if (type == 2) { p.vy = -(15.f + rnd() * 25.f); p.vx = 0.f; p.size = 0.8f + rnd() * 1.6f; }
				else if (type == 3) { p.vy = -(200.f + rnd() * 120.f); p.vx = -30.f; p.size = 0.7f; }
				else { p.vy = 10.f + rnd() * 15.f; p.vx = (rnd() - 0.5f) * 10.f; p.size = 1.f + rnd() * 2.f; }
				m_ps.push_back(p);
			}
		}
		this->scheduleUpdate();
		return true;
	}

	void updateTwinkles(float dt) {
		for (auto& tw : m_twinkles) {
			tw.phase += dt * tw.speed;
			float a = 0.25f + 0.55f * (0.5f + 0.5f * std::sin(tw.phase));
			m_draw->drawDot({ tw.x, tw.y }, tw.size, starColor(0.8f, 0.9f, 1.f, a));
		}
	}

	void updateShoots(float dt) {
		for (auto& s : m_shoots) {
			s.t += dt * s.speed;
			if (s.t >= 1.f) { respawn(s); continue; }
			if (s.t < 0.f) continue;

			float fade = std::sin(3.14159f * s.t);
			CCPoint head = { s.x0 + s.dx * s.t, s.y0 + s.dy * s.t };
			float ux = -s.dx / s.len;
			float uy = -s.dy / s.len;
			float trail = std::min(s.len * s.t, 80.f);

			const int segs = 8;
			for (int k = 0; k < segs; k++) {
				float a0 = static_cast<float>(k) / segs;
				float a1 = static_cast<float>(k + 1) / segs;
				CCPoint p0 = { head.x + ux * trail * a0, head.y + uy * trail * a0 };
				CCPoint p1 = { head.x + ux * trail * a1, head.y + uy * trail * a1 };
				float alpha = fade * std::pow(1.f - a0, 1.5f);
				m_draw->drawSegment(p0, p1, 1.4f * (1.f - a0) + 0.3f, starColor(0.75f, 0.88f, 1.f, alpha));
			}
			m_draw->drawDot(head, 2.2f, starColor(1.f, 1.f, 1.f, fade));
		}
	}

	// Burning meteors: a white-hot head with layered bloom, a long tail that cools from
	// yellow through orange to dark red (drawn in many thin overlapping segments so it is
	// smooth), and glowing embers that break off and fade. They burn hotter and shed more
	// sparks as they near the end of their path.
	void updateMeteors(float dt) {
		for (auto& m : m_meteors) {
			m.t += dt * m.speed;
			if (m.t >= 1.f) { respawnMeteor(m); continue; }
			if (m.t < 0.f) continue;

			float t = m.t;
			float env = std::pow(std::max(0.f, std::sin(3.14159f * t)), 0.7f);
			float burn = std::clamp((t - 0.35f) / 0.65f, 0.f, 1.f);
			float flick = 0.88f + 0.12f * std::sin(m.t * 90.f + m.size * 7.f);
			float bright = env * flick;
			CCPoint head = { m.x0 + m.dx * t, m.y0 + m.dy * t };
			float ux = -m.dx / m.len; // unit vector pointing back along the tail
			float uy = -m.dy / m.len;
			float trail = std::min(m.len * t, 70.f + 90.f * m.size) * (1.f + 0.25f * burn);
			float hs = m.size * (1.f - 0.45f * burn);
			float heat = m.heat * (1.f - 0.25f * burn);

			const int segs = 44;
			for (int k = 0; k < segs; k++) {
				float a0 = static_cast<float>(k) / segs;
				float a1 = static_cast<float>(k + 1) / segs;
				CCPoint p0 = { head.x + ux * trail * a0, head.y + uy * trail * a0 };
				CCPoint p1 = { head.x + ux * trail * a1, head.y + uy * trail * a1 };
				float fall = std::pow(1.f - a0, 1.6f);
				float r, g, b;
				fireColor(heat * (1.f - a0 * 0.85f), r, g, b);
				float w = hs * 1.9f * std::pow(1.f - a0, 1.1f) + 0.35f;
				float al = bright * fall;
				m_draw->drawSegment(p0, p1, w * 0.5f, starColor(r, g, b, al * 0.45f));
				m_glow->drawSegment(p0, p1, w * 1.6f + 1.f, starColor(r * 0.9f, g * 0.7f, b * 0.5f, al * 0.13f));
			}

			// head bloom
			m_glow->drawDot(head, 12.f * hs, starColor(1.f, 0.45f, 0.12f, 0.07f * bright));
			m_glow->drawDot(head, 7.f * hs, starColor(1.f, 0.6f, 0.2f, 0.16f * bright));
			m_glow->drawDot(head, 4.f * hs, starColor(1.f, 0.78f, 0.4f, 0.35f * bright));
			m_draw->drawDot(head, 1.7f * hs, starColor(1.f, 0.97f, 0.9f, bright));

			// embers
			m.emit += dt * (14.f + 40.f * burn) * m.size * m_amount;
			float sp = m.len * m.speed;
			while (m.emit >= 1.f && m_embers.size() < 260) {
				m.emit -= 1.f;
				Ember e{};
				e.x = head.x + (rnd() - 0.5f) * 2.f;
				e.y = head.y + (rnd() - 0.5f) * 2.f;
				float spread = 8.f + 22.f * burn;
				e.vx = ux * sp * 0.06f + (rnd() - 0.5f) * spread;
				e.vy = uy * sp * 0.06f + (rnd() - 0.5f) * spread;
				e.maxLife = e.life = 0.5f + rnd() * 0.8f;
				e.size = 0.5f + rnd() * 0.9f;
				e.heat = 0.6f + rnd() * 0.4f;
				m_embers.push_back(e);
			}
			if (m.emit >= 1.f) m.emit = 0.f;
		}

		for (size_t i = 0; i < m_embers.size();) {
			auto& e = m_embers[i];
			e.life -= dt;
			if (e.life <= 0.f) {
				e = m_embers.back();
				m_embers.pop_back();
				continue;
			}
			float drag = std::max(0.f, 1.f - 1.5f * dt);
			e.vx *= drag;
			e.vy = e.vy * drag - 14.f * dt;
			e.x += e.vx * dt;
			e.y += e.vy * dt;
			float k = e.life / e.maxLife;
			float r, g, b;
			fireColor(e.heat * k, r, g, b);
			float a = k * k;
			m_draw->drawDot({ e.x, e.y }, e.size * (0.6f + 0.4f * k), starColor(r, g, b, a));
			m_glow->drawDot({ e.x, e.y }, e.size * 3.f, starColor(r, g, b, a * 0.18f));
			i++;
		}
	}

	void updateParticles(float dt) {
		for (auto& p : m_ps) {
			p.phase += dt * 2.f;
			p.y += p.vy * dt;
			p.x += p.vx * dt;
			if (m_type == 2) p.x += std::sin(p.phase) * 12.f * dt;

			bool out = p.y < -6.f || p.y > m_area.height + 6.f || p.x < -6.f || p.x > m_area.width + 6.f;
			if (out) {
				p.x = rnd() * m_area.width;
				p.y = p.vy < 0.f ? m_area.height + 4.f : -4.f;
			}

			if (m_type == 2) {
				m_draw->drawDot({ p.x, p.y }, p.size, starColor(1.f, 1.f, 1.f, 0.7f));
			} else if (m_type == 3) {
				m_draw->drawSegment({ p.x, p.y }, { p.x - p.vx * 0.03f, p.y - p.vy * 0.03f }, 0.6f,
					starColor(0.65f, 0.8f, 1.f, 0.5f));
			} else {
				float a = 0.2f + 0.5f * (0.5f + 0.5f * std::sin(p.phase));
				m_draw->drawDot({ p.x, p.y }, p.size,
					starColor(m_accent.r / 255.f, m_accent.g / 255.f, m_accent.b / 255.f, a));
			}
		}
	}

	void update(float dt) override {
		dt = std::min(dt, 0.1f) * m_timeMul;
		m_draw->clear();
		m_glow->clear();
		if (m_type == 0) { updateTwinkles(dt); updateMeteors(dt); }
		else if (m_type == 1) { updateTwinkles(dt); updateShoots(dt); }
		else updateParticles(dt);
	}

public:
	static BgField* create(CCSize area, int type, int amount, int speedIdx) {
		auto ret = new BgField();
		if (ret->init(area, type, amount, speedIdx)) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

// ---------- manager popup ----------

class MacroPopup;
static MacroPopup* g_manager = nullptr;
static CCPoint g_dragOff = { 0.f, 0.f }; // where the compact window was dragged to

// Tabs, grouped by what they do:
//   0 Control  - record / play / off, speed
//   1 Files    - create, save, load, rename, copy, delete, import / export
//   2 Organize - favourites, tags, folders, sorting
//   3 Tools    - undo, merge, delete a range
static char const* const kMgrTabs[] = { "Control", "Files", "Organize", "Tools" };
static const int kMgrTabCount = 4;

class MacroPopup : public Popup {
protected:
	CCLabelBMFont* m_modeLabel = nullptr;
	CCLabelBMFont* m_selLabel = nullptr;
	CCLabelBMFont* m_speedLabel = nullptr;
	CCLabelBMFont* m_viewLabel = nullptr;
	Slider* m_speedSlider = nullptr;
	TextInput* m_input = nullptr;
	TextInput* m_search = nullptr;
	TextInput* m_tpsInput = nullptr;
	ScrollLayer* m_list = nullptr;
	std::vector<CCNode*> m_tabNodes;
	float m_listW = 0.f;
	float m_colW = 0.f;
	float m_rightX = 0.f;
	float m_top = 0.f;
	float m_bs = 0.55f; // button scale
	int m_tab = 0;
	bool m_compact = false;
	float m_pw = 0.f;
	float m_ph = 0.f;
	bool m_dragging = false;

	bool init() {
		auto ws = CCDirector::get()->getWinSize();
		m_compact = S("compact") != 0;
		int sizePct = S("ui-size");
		auto const& design = curDesign();
		int shape = design.shape;
		int opacity = S("ui-opacity");
		int bgType = bgKind();
		int anim = S("anim");

		float pw = ws.width * sizePct / 100.f;
		float ph = ws.height * sizePct / 100.f;
		if (m_compact) {
			pw = std::min(pw, ws.width * 0.62f);
			ph = ws.height * 0.78f;
		}
		if (anim >= 2) m_noElasticity = true;
		if (!Popup::init(pw, ph)) return false;

		m_bgSprite->setVisible(false);
		this->setTitle("Macro Manager");

		// remembered tab: stays the same after closing / re-opening the manager
		// (including when you come back from the settings popup)
		m_tab = std::clamp(getInt("mgr-tab", 0), 0, kMgrTabCount - 1);

		// dark -> light grey (or whichever design is picked): frame + vertical gradient
		auto top = design.top;
		auto bot = design.bot;
		auto panel = makePanel(shape, mixColor(top, bot, 0.5f), { pw, ph });
		panel->setOpacity(static_cast<GLubyte>(255 * opacity / 100));
		m_mainLayer->addChildAtPosition(panel, Anchor::Center);
		panel->setZOrder(-10);

		{
			auto g = CCLayerGradient::create(ccc4(top.r, top.g, top.b, 255), ccc4(bot.r, bot.g, bot.b, 255));
			g->setContentSize({ pw - 8.f, ph - 8.f });
			g->ignoreAnchorPointForPosition(false);
			g->setAnchorPoint({ 0.5f, 0.5f });
			g->setOpacity(static_cast<GLubyte>(255 * opacity / 100));
			m_mainLayer->addChildAtPosition(g, Anchor::Center);
			g->setZOrder(-9);
		}

		if (bgType < 5) {
			auto field = BgField::create({ pw, ph }, bgType, S("bg-amount"), S("bg-speed"));
			if (field) {
				m_mainLayer->addChildAtPosition(field, Anchor::Center, { -pw / 2.f, -ph / 2.f });
				field->setZOrder(-8);
			}
		}

		m_bs = m_compact ? 0.45f : 0.55f;
		float leftX = -pw * 0.245f;
		m_rightX = pw * 0.25f;
		m_colW = pw * 0.46f;
		m_listW = pw * 0.40f;
		m_top = ph / 2.f;
		float listH = ph - 108.f;
		float listY = -33.f;

		// ----- left side: search, view info, macro list -----
		m_search = TextInput::create(m_listW / 0.8f, "Search");
		m_search->setScale(0.8f);
		m_search->setString(g_filterText);
		m_search->setCallback([this](std::string const& s) {
			g_filterText = s;
			this->rebuildList();
		});
		m_mainLayer->addChildAtPosition(m_search, Anchor::Center, { leftX, m_top - 46.f });
		m_viewLabel = addLabel("", leftX, m_top - 70.f, 0.33f);

		auto listBg = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
		listBg->setColor({ 0, 0, 0 });
		listBg->setOpacity(110);
		listBg->setContentSize({ m_listW + 10.f, listH + 10.f });
		m_mainLayer->addChildAtPosition(listBg, Anchor::Center, { leftX, listY });

		m_list = ScrollLayer::create(CCSize{ m_listW, listH });
		m_list->ignoreAnchorPointForPosition(false);
		m_list->setAnchorPoint({ 0.5f, 0.5f });
		m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout());
		m_mainLayer->addChildAtPosition(m_list, Anchor::Center, { leftX, listY });

		// scroll bar for the macro list
		if (auto bar = Scrollbar::create(m_list)) {
			m_mainLayer->addChildAtPosition(bar, Anchor::Center, { leftX + m_listW / 2.f + 12.f, listY });
		}

		// ----- right side: shared text box (hidden on the Control tab) -----
		m_input = TextInput::create(m_colW / 0.75f, "Name / tag / macro / range");
		m_input->setScale(0.75f);
		m_input->setMaxCharCount(24);
		m_mainLayer->addChildAtPosition(m_input, Anchor::Center, { m_rightX, m_top - 84.f });

		// settings (gear) button, top right, level with the title
		auto gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn02_001.png");
		if (gear) {
			gear->setScale(0.6f);
			auto gearBtn = CCMenuItemSpriteExtra::create(gear, this, menu_selector(MacroPopup::onSettings));
			m_buttonMenu->addChildAtPosition(gearBtn, Anchor::TopRight, { -24.f, -24.f });
		}

		m_pw = pw;
		m_ph = ph;
		if (m_compact) m_mainLayer->setPosition(m_mainLayer->getPosition() + g_dragOff);
		playIntro(panel, opacity);
		buildTab();
		rebuildList();
		refreshLabels();
		g_manager = this;
		return true;
	}

	void playIntro(CCScale9Sprite* panel, int opacity) {
		int anim = S("anim");
		if (anim == 1) {
			panel->setOpacity(0);
			panel->runAction(CCFadeTo::create(0.3f, static_cast<GLubyte>(255 * opacity / 100)));
		} else if (anim == 2) {
			m_mainLayer->setPositionY(m_mainLayer->getPositionY() - 80.f);
			m_mainLayer->runAction(CCEaseExponentialOut::create(CCMoveBy::create(0.3f, ccp(0.f, 80.f))));
		} else if (anim == 3) {
			m_mainLayer->setScale(0.6f);
			m_mainLayer->runAction(CCEaseBackOut::create(CCScaleTo::create(0.35f, 1.f)));
		}
	}

	static float speedToSlider(float s) {
		return std::clamp((s - kSpeedMin) / (kSpeedMax - kSpeedMin), 0.f, 1.f);
	}

	static float sliderToSpeed(float v) {
		float s = kSpeedMin + std::clamp(v, 0.f, 1.f) * (kSpeedMax - kSpeedMin);
		s = std::round(s * 20.f) / 20.f;
		return std::clamp(s, kSpeedMin, kSpeedMax);
	}

	// permanent label (not removed when switching tabs)
	CCLabelBMFont* addLabel(char const* text, float x, float y, float scale) {
		auto label = CCLabelBMFont::create(text, "bigFont.fnt");
		label->setScale(scale);
		m_mainLayer->addChildAtPosition(label, Anchor::Center, { x, y });
		return label;
	}

	// label that belongs to the current tab
	CCLabelBMFont* tabLabel(char const* text, float y, float scale) {
		auto label = addLabel(text, m_rightX, y, scale);
		fitLabel(label, m_colW, scale);
		m_tabNodes.push_back(label);
		return label;
	}

	// fixed-width button, so neighbouring buttons can never overlap
	CCMenuItemSpriteExtra* addCell(char const* text, SEL_MenuHandler cb, float cx, float y, float w, float scale, bool active) {
		auto spr = ButtonSprite::create(
			text, static_cast<int>(w / scale), true,
			"bigFont.fnt", "GJ_button_04.png", 30.f, 0.8f
		);
		spr->setScale(scale);
		if (active) spr->setColor(accentColor());
		auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
		m_buttonMenu->addChildAtPosition(btn, Anchor::Center, { cx, y });
		m_tabNodes.push_back(btn);
		return btn;
	}

	// a row of equally sized buttons that exactly fills the right column
	void addRow(std::vector<std::pair<char const*, SEL_MenuHandler>> const& items, float y) {
		float gap = 5.f;
		int n = static_cast<int>(items.size());
		float w = (m_colW - gap * (n - 1)) / n;
		for (int i = 0; i < n; i++) {
			float cx = m_rightX - m_colW / 2.f + w / 2.f + i * (w + gap);
			addCell(items[i].first, items[i].second, cx, y, w, m_bs, false);
		}
	}

	void buildTab() {
		for (auto n : m_tabNodes) n->removeFromParent();
		m_tabNodes.clear();
		m_modeLabel = nullptr;
		m_selLabel = nullptr;
		m_speedLabel = nullptr;
		m_speedSlider = nullptr;
		m_tpsInput = nullptr;

		// tab bar
		float gap = 4.f;
		float tw = (m_colW - gap * (kMgrTabCount - 1)) / kMgrTabCount;
		float tabY = m_top - 56.f;
		for (int i = 0; i < kMgrTabCount; i++) {
			float cx = m_rightX - m_colW / 2.f + tw / 2.f + i * (tw + gap);
			auto b = addCell(kMgrTabs[i], menu_selector(MacroPopup::onTab), cx, tabY, tw, 0.5f, i == m_tab);
			b->setTag(i);
		}

		bool showInput = m_tab != 0;
		if (m_input) {
			m_input->setVisible(showInput);
			m_input->setEnabled(showInput);
		}

		float rowY = m_top - 112.f;
		float step = 30.f;

		if (m_tab == 0) {
			// Control
			m_modeLabel = tabLabel("", m_top - 86.f, 0.55f);
			addRow({
				{ "Record", menu_selector(MacroPopup::onRecord) },
				{ "Play", menu_selector(MacroPopup::onPlay) },
				{ "Off", menu_selector(MacroPopup::onOff) },
			}, rowY);
			m_selLabel = tabLabel("", rowY - 28.f, 0.4f);
			m_speedLabel = tabLabel("", rowY - 56.f, 0.5f);

			m_speedSlider = Slider::create(this, menu_selector(MacroPopup::onSpeedSlider), 0.7f);
			m_speedSlider->setValue(speedToSlider(g_speed));
			m_mainLayer->addChildAtPosition(m_speedSlider, Anchor::Center, { m_rightX, rowY - 76.f });
			m_tabNodes.push_back(m_speedSlider);

			float pg = 5.f;
			float pwid = (m_colW - pg * 2.f) / 3.f;
			float px = m_rightX - m_colW / 2.f + pwid / 2.f;
			addCell("0.5x", menu_selector(MacroPopup::onPreset), px, rowY - 102.f, pwid, 0.45f, false)->setTag(50);
			addCell("1x", menu_selector(MacroPopup::onPreset), px + (pwid + pg), rowY - 102.f, pwid, 0.45f, false)->setTag(100);
			addCell("2x", menu_selector(MacroPopup::onPreset), px + (pwid + pg) * 2.f, rowY - 102.f, pwid, 0.45f, false)->setTag(200);

			// tick rate: type any value from 1 to 10000
			m_tpsInput = TextInput::create(m_colW * 0.55f / 0.7f, "TPS 1-10000");
			m_tpsInput->setScale(0.7f);
			m_tpsInput->setFilter("0123456789");
			m_tpsInput->setMaxCharCount(5);
			m_tpsInput->setString(std::to_string(curTps()));
			m_mainLayer->addChildAtPosition(m_tpsInput, Anchor::Center, { m_rightX - m_colW * 0.2f, rowY - 130.f });
			m_tabNodes.push_back(m_tpsInput);
			addCell("Set TPS", menu_selector(MacroPopup::onSetTps), m_rightX + m_colW * 0.3f, rowY - 130.f, m_colW * 0.36f, 0.45f, false);
		} else if (m_tab == 1) {
			// Files
			addRow({
				{ "New", menu_selector(MacroPopup::onNew) },
				{ "Save", menu_selector(MacroPopup::onSave) },
				{ "Load", menu_selector(MacroPopup::onLoad) },
				{ "Delete", menu_selector(MacroPopup::onDelete) },
			}, rowY);
			addRow({
				{ "Rename", menu_selector(MacroPopup::onRename) },
				{ "Copy", menu_selector(MacroPopup::onCopy) },
			}, rowY - step);
			addRow({
				{ "Export", menu_selector(MacroPopup::onExport) },
				{ "Import", menu_selector(MacroPopup::onImport) },
			}, rowY - step * 2.f);
			tabLabel("Type a name, then tap Rename", rowY - step * 3.f + 6.f, 0.3f);
			tabLabel("Export / Import use the clipboard", rowY - step * 3.f - 8.f, 0.3f);
		} else if (m_tab == 2) {
			// Organize
			addRow({
				{ "Fav", menu_selector(MacroPopup::onFav) },
				{ "Tag", menu_selector(MacroPopup::onTag) },
				{ "Folder", menu_selector(MacroPopup::onFolder) },
				{ "Sort", menu_selector(MacroPopup::onSort) },
			}, rowY);
			tabLabel("Type a tag name, then tap Tag", rowY - step + 6.f, 0.3f);
			tabLabel("Folder cycles through your tags", rowY - step - 8.f, 0.3f);
		} else {
			// Tools
			addRow({
				{ "Undo", menu_selector(MacroPopup::onUndo) },
				{ "Merge", menu_selector(MacroPopup::onMerge) },
				{ "Del range", menu_selector(MacroPopup::onDelRange) },
			}, rowY);
			tabLabel("Undo removes the last recorded input", rowY - step + 6.f, 0.28f);
			tabLabel("Merge: type the macro to append", rowY - step - 8.f, 0.28f);
			tabLabel("Del range: type 100-500, then tap Save", rowY - step - 22.f, 0.28f);
		}
	}

	void rebuildList() {
		auto content = m_list->m_contentLayer;
		content->removeAllChildrenWithCleanup(true);

		auto favs = loadFavs();
		auto tags = loadTags();
		int sort = std::clamp(getInt("sort-mode", 0), 0, 2);
		std::string needle = lowerStr(g_filterText);

		std::vector<MacroInfo> infos;
		for (auto& name : listMacros()) {
			if (!needle.empty() && lowerStr(name).find(needle) == std::string::npos) continue;
			auto tagIt = tags.find(name);
			std::string tag = tagIt == tags.end() ? "" : tagIt->second;
			if (!g_folder.empty()) {
				if (g_folder == "(none)") { if (!tag.empty()) continue; }
				else if (tag != g_folder) continue;
			}
			infos.push_back(readInfo(name));
		}

		std::stable_sort(infos.begin(), infos.end(), [&](MacroInfo const& a, MacroInfo const& b) {
			bool fa = favs.count(a.name) > 0;
			bool fb = favs.count(b.name) > 0;
			if (fa != fb) return fa;
			if (sort == 1) return a.stamp > b.stamp;
			if (sort == 2) return a.last > b.last;
			return a.name < b.name;
		});

		if (infos.empty()) {
			auto label = CCLabelBMFont::create("No macros found.", "bigFont.fnt");
			label->setScale(0.5f);
			content->addChild(label);
		}

		float fs = S("font-size") / 100.f;
		for (auto& info : infos) {
			auto& name = info.name;
			auto row = CCMenu::create();
			row->ignoreAnchorPointForPosition(false);
			row->setAnchorPoint({ 0.5f, 0.5f });
			row->setContentSize({ m_listW, 42.f });

			std::string text;
			if (favs.count(name)) text += "* ";
			auto tagIt = tags.find(name);
			if (tagIt != tags.end() && !tagIt->second.empty()) text += "[" + tagIt->second + "] ";
			text += name;

			bool sel = (name == g_selected);
			float scale = 0.7f * fs;
			auto spr = ButtonSprite::create(
				text.c_str(), static_cast<int>((m_listW - 24.f) / scale), true,
				"bigFont.fnt", "GJ_button_04.png", 30.f, 0.8f
			);
			spr->setScale(scale);
			if (sel) spr->setColor(accentColor());
			auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroPopup::onPick));
			btn->setID(name);
			row->addChildAtPosition(btn, Anchor::Center, { 0.f, 6.f });

			char date[16] = "?";
			if (info.stamp > 0) {
				std::time_t t = static_cast<std::time_t>(info.stamp);
				if (auto tm = std::localtime(&t)) std::strftime(date, sizeof(date), "%Y-%m-%d", tm);
			}
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%d clicks | %d f | %s", info.clicks, info.last, date);
			auto infoLabel = CCLabelBMFont::create(buf, "chatFont.fnt");
			infoLabel->setScale(0.45f);
			infoLabel->setOpacity(190);
			row->addChildAtPosition(infoLabel, Anchor::Bottom, { 0.f, 7.f });

			content->addChild(row);
		}
		content->updateLayout();
		m_list->scrollToTop();
		refreshView();
	}

	void refreshView() {
		if (!m_viewLabel) return;
		int sort = std::clamp(getInt("sort-mode", 0), 0, 2);
		std::string f = g_folder.empty() ? "All" : g_folder;
		std::string s = std::string("Sort: ") + kSortNames[sort] + "  Folder: " + f;
		m_viewLabel->setString(s.c_str());
		fitLabel(m_viewLabel, m_listW, 0.33f);
	}

	void refreshLabels() {
		if (m_modeLabel) {
			char const* modes[] = { "Mode: Off", "Mode: Recording", "Mode: Playing" };
			m_modeLabel->setString(modes[std::clamp(g_mode, 0, 2)]);
			fitLabel(m_modeLabel, m_colW, 0.55f);
		}

		if (m_selLabel) {
			std::string shown = g_selected;
			if (shown.size() > 18) shown = shown.substr(0, 17) + "...";
			std::string sel = g_selected.empty() ? "Selected: none" : "Selected: " + shown;
			m_selLabel->setString(sel.c_str());
			fitLabel(m_selLabel, m_colW, 0.4f);
		}

		if (m_speedLabel) {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "Speed %.2fx", g_speed);
			m_speedLabel->setString(buf);
		}
	}

	void notify(char const* text) {
		Notification::create(text)->show();
	}

	std::string inputText() {
		return m_input ? std::string(m_input->getString()) : std::string();
	}

	void onTab(CCObject* sender) {
		int t = static_cast<CCNode*>(sender)->getTag();
		if (t == m_tab) return;
		m_tab = std::clamp(t, 0, kMgrTabCount - 1);
		setInt("mgr-tab", m_tab);
		buildTab();
		refreshLabels();
	}

	void onPick(CCObject* sender) {
		g_selected = static_cast<CCNode*>(sender)->getID();
		std::vector<MacroInput> loadedInputs;
		std::vector<MacroAnchor> loadedAnchors;
		if (readMacro(g_selected, loadedInputs, loadedAnchors)) {
			g_inputs = loadedInputs;
			g_anchors = loadedAnchors;
		}
		if (m_input) m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
	}

	void onRecord(CCObject*) {
		g_selected = nextMacroName();
		g_inputs.clear();
		g_anchors.clear();
		g_macroTps = curTps();
		writeMacro(g_selected);
		g_mode = 1;
		notify("Recording: restart the level");
		this->keyBackClicked();
	}

	void onPlay(CCObject*) {
		if (g_inputs.empty()) {
			notify("Select or load a macro first");
			return;
		}
		g_cursor = 0;
		g_anchorCursor = 0;
		g_corrections = 0;
		g_pathDirty = true;
		g_mode = 2;
		if (g_macroTps > 0 && g_macroTps != curTps()) {
			char tb[96];
			std::snprintf(tb, sizeof(tb), "Warning: macro is %d TPS, current is %d. It will desync", g_macroTps, curTps());
			notify(tb);
		} else {
			notify("Playing: restart the level");
		}
		this->keyBackClicked();
	}

	void onOff(CCObject*) {
		if (g_mode == 1 && !g_selected.empty() && !g_inputs.empty()) {
			writeMacro(g_selected);
			notify("Macro saved");
		}
		g_mode = 0;
		refreshLabels();
	}

	void onSave(CCObject*) {
		if (g_selected.empty()) return notify("Tap Record or pick a macro first");
		if (g_inputs.empty()) return notify("Nothing recorded yet");
		notify(writeMacro(g_selected) ? "Macro saved" : "Save failed");
		rebuildList();
	}

	void onLoad(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		std::vector<MacroInput> loadedInputs;
		std::vector<MacroAnchor> loadedAnchors;
		if (!readMacro(g_selected, loadedInputs, loadedAnchors)) return notify("Could not load");
		g_inputs = loadedInputs;
		g_anchors = loadedAnchors;
		notify(g_inputs.empty() ? "Macro is empty" : "Macro loaded");
	}

	void doDelete() {
		std::error_code ec;
		std::filesystem::remove(macroFile(g_selected), ec);
		auto favs = loadFavs();
		favs.erase(g_selected);
		saveFavs(favs);
		auto tags = loadTags();
		tags.erase(g_selected);
		saveTags(tags);
		g_selected.clear();
		g_inputs.clear();
		g_anchors.clear();
		rebuildList();
		refreshLabels();
		notify("Macro deleted");
	}

	void onDelete(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		if (S("confirm-delete")) {
			createQuickPopup(
				"Delete macro",
				"Delete <cy>" + g_selected + "</c>? This can't be undone.",
				"No", "Yes",
				[this](FLAlertLayer*, bool yes) {
					if (yes) this->doDelete();
				}
			);
		} else {
			doDelete();
		}
	}

	void onNew(CCObject*) {
		g_inputs.clear();
		g_anchors.clear();
		g_selected = nextMacroName();
		writeMacro(g_selected);
		if (m_input) m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
	}

	void onCopy(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		if (g_inputs.empty()) return notify("Nothing to copy");
		auto name = uniqueName(g_selected + " copy", false);
		if (!writeMacro(name)) return notify("Copy failed");
		auto tags = loadTags();
		if (tags.count(g_selected)) {
			tags[name] = tags[g_selected];
			saveTags(tags);
		}
		g_selected = name;
		if (m_input) m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
		notify("Macro duplicated");
	}

	void onExport(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		if (g_inputs.empty()) return notify("Nothing to export");
		std::ostringstream ss;
		serializeMacro(ss);
		if (geode::utils::clipboard::write(ss.str())) notify("Macro copied to clipboard");
		else notify("Could not copy to clipboard");
	}

	void onImport(CCObject*) {
		auto text = geode::utils::clipboard::read();
		std::istringstream ss(text);
		std::vector<MacroInput> inputs;
		std::vector<MacroAnchor> anchors;
		parseMacro(ss, inputs, anchors);
		if (inputs.empty()) return notify("Clipboard has no macro");
		g_inputs = inputs;
		g_anchors = anchors;
		g_selected = uniqueName("Imported", false);
		if (!writeMacro(g_selected)) return notify("Import failed");
		if (m_input) m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
		notify("Macro imported");
	}

	void onRename(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		auto newName = cleanName(inputText());
		if (newName.empty()) return notify("Type a valid name first");
		if (newName == g_selected) return;
		if (std::filesystem::exists(macroFile(newName))) return notify("That name is taken");
		std::error_code ec;
		std::filesystem::rename(macroFile(g_selected), macroFile(newName), ec);
		if (ec) return notify("Rename failed");

		auto favs = loadFavs();
		if (favs.erase(g_selected)) { favs.insert(newName); saveFavs(favs); }
		auto tags = loadTags();
		auto it = tags.find(g_selected);
		if (it != tags.end()) { tags[newName] = it->second; tags.erase(g_selected); saveTags(tags); }

		g_selected = newName;
		rebuildList();
		refreshLabels();
		notify("Renamed");
	}

	void onFav(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		auto favs = loadFavs();
		if (favs.count(g_selected)) { favs.erase(g_selected); notify("Removed from favourites"); }
		else { favs.insert(g_selected); notify("Added to favourites"); }
		saveFavs(favs);
		rebuildList();
	}

	void onTag(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		auto tag = cleanName(inputText());
		auto tags = loadTags();
		if (tag.empty() || tag == g_selected) { tags.erase(g_selected); notify("Tag cleared"); }
		else { tags[g_selected] = tag; notify("Tag set"); }
		saveTags(tags);
		rebuildList();
	}

	void onFolder(CCObject*) {
		auto tags = loadTags();
		std::set<std::string> uniq;
		for (auto& [k, v] : tags) if (!v.empty()) uniq.insert(v);
		std::vector<std::string> folders = { "" };
		for (auto& t : uniq) folders.push_back(t);
		folders.push_back("(none)");
		size_t cur = 0;
		for (size_t i = 0; i < folders.size(); i++) if (folders[i] == g_folder) cur = i;
		g_folder = folders[(cur + 1) % folders.size()];
		rebuildList();
	}

	void onSort(CCObject*) {
		setInt("sort-mode", (std::clamp(getInt("sort-mode", 0), 0, 2) + 1) % 3);
		rebuildList();
	}

	void onUndo(CCObject*) {
		if (g_inputs.empty()) return notify("Nothing to undo");
		g_inputs.pop_back();
		notify("Last input removed. Tap Save to keep it");
	}

	void onMerge(CCObject*) {
		if (g_selected.empty() || g_inputs.empty()) return notify("Select a macro first");
		auto second = cleanName(inputText());
		if (second.empty()) return notify("Type the macro to append");
		if (second == g_selected) return notify("Pick a different macro");
		std::vector<MacroInput> in2;
		std::vector<MacroAnchor> an2;
		int keepTps = g_macroTps;
		if (!readMacro(second, in2, an2)) return notify("Macro not found");
		g_macroTps = keepTps;

		int offset = 0;
		for (auto& i : g_inputs) offset = std::max(offset, i.frame);
		for (auto& a : g_anchors) offset = std::max(offset, a.frame);
		offset += 1;
		for (auto i : in2) { i.frame += offset; g_inputs.push_back(i); }
		for (auto a : an2) { a.frame += offset; g_anchors.push_back(a); }

		auto name = uniqueName(g_selected + " + " + second, false);
		if (!writeMacro(name)) return notify("Merge failed");
		g_selected = name;
		rebuildList();
		refreshLabels();
		notify("Macros merged");
	}

	void onDelRange(CCObject*) {
		if (g_inputs.empty()) return notify("Nothing loaded");
		int a = 0, b = 0;
		auto txt = inputText();
		if (std::sscanf(txt.c_str(), "%d-%d", &a, &b) != 2 && std::sscanf(txt.c_str(), "%d %d", &a, &b) != 2) {
			return notify("Type a range like 100-500");
		}
		if (a > b) std::swap(a, b);
		auto inRange = [&](int f) { return f >= a && f <= b; };
		size_t before = g_inputs.size();
		g_inputs.erase(std::remove_if(g_inputs.begin(), g_inputs.end(),
			[&](MacroInput const& i) { return inRange(i.frame); }), g_inputs.end());
		g_anchors.erase(std::remove_if(g_anchors.begin(), g_anchors.end(),
			[&](MacroAnchor const& x) { return inRange(x.frame); }), g_anchors.end());
		char buf[48];
		std::snprintf(buf, sizeof(buf), "Removed %d inputs. Tap Save", static_cast<int>(before - g_inputs.size()));
		notify(buf);
	}

	void onSpeedSlider(CCObject* sender) {
		auto thumb = static_cast<SliderThumb*>(sender);
		g_speed = sliderToSpeed(thumb->getValue());
		applySpeed();
		refreshLabels();
	}

	void onPreset(CCObject* sender) {
		g_speed = static_cast<CCNode*>(sender)->getTag() / 100.f;
		applySpeed();
		if (m_speedSlider) m_speedSlider->setValue(speedToSlider(g_speed));
		refreshLabels();
	}

	void onSettings(CCObject*);

	void onSetTps(CCObject*) {
		if (!m_tpsInput) return;
		int v = std::atoi(std::string(m_tpsInput->getString()).c_str());
		if (v < 1) return notify("Type a TPS from 1 to 10000");
		v = std::min(v, 10000);
		setInt("tps", v);
		m_tpsInput->setString(std::to_string(v));
		char buf[48];
		std::snprintf(buf, sizeof(buf), "TPS set to %d", v);
		notify(buf);
	}

public:
	// Compact mode: drag the window by its title bar, like a tab.
	bool ccTouchBegan(CCTouch* t, CCEvent* e) {
		m_dragging = false;
		if (m_compact && m_mainLayer && m_bgSprite) {
			auto local = m_mainLayer->convertToNodeSpace(t->getLocation());
			auto c = m_bgSprite->getPosition();
			float rx = local.x - c.x;
			float ry = local.y - c.y;
			if (std::fabs(rx) <= m_pw / 2.f && ry <= m_ph / 2.f && ry >= m_ph / 2.f - 38.f) m_dragging = true;
		}
		return Popup::ccTouchBegan(t, e);
	}

	void ccTouchMoved(CCTouch* t, CCEvent* e) {
		if (!m_dragging) return Popup::ccTouchMoved(t, e);
		auto ws = CCDirector::get()->getWinSize();
		auto d = t->getLocation() - t->getPreviousLocation();
		float lx = ws.width / 2.f - 30.f;
		float ly = ws.height / 2.f - 30.f;
		CCPoint next = { std::clamp(g_dragOff.x + d.x, -lx, lx), std::clamp(g_dragOff.y + d.y, -ly, ly) };
		CCPoint real = { next.x - g_dragOff.x, next.y - g_dragOff.y };
		g_dragOff = next;
		m_mainLayer->setPosition(m_mainLayer->getPosition() + real);
	}

	void ccTouchEnded(CCTouch* t, CCEvent* e) {
		if (m_dragging) { m_dragging = false; return; }
		Popup::ccTouchEnded(t, e);
	}

	void ccTouchCancelled(CCTouch* t, CCEvent* e) {
		if (m_dragging) { m_dragging = false; return; }
		Popup::ccTouchCancelled(t, e);
	}

	~MacroPopup() {
		if (g_manager == this) g_manager = nullptr;
	}

	void closeManager() {
		this->keyBackClicked();
	}

	static MacroPopup* create() {
		auto ret = new MacroPopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};



// ---------- settings popup ----------
// Two tabs: "Designs" (tap a swatch) and "Options" (scrollable list, grouped by section).
// Changes are saved as soon as they are made; Apply rebuilds the manager with the new look.

class SettingsPopup : public Popup {
protected:
	int m_tab = 0; // 0 = designs, 1 = options
	int m_sel = 0; // selected design
	float m_listW = 330.f;
	float m_listH = 160.f;
	float m_sw = 100.f; // swatch size
	float m_sh = 38.f;
	ScrollLayer* m_list = nullptr;
	CCLabelBMFont* m_nameLabel = nullptr;
	std::vector<CCLabelBMFont*> m_valLabels;
	std::vector<CCDrawNode*> m_borders;
	std::vector<CCNode*> m_tabNodes;

	bool init() {
		if (!Popup::init(380.f, 270.f)) return false;
		m_bgSprite->setVisible(false);
		this->setTitle("Settings");

		auto const& design = curDesign();
		auto panel = makePanel(design.shape, mixColor(design.top, design.bot, 0.5f), { 380.f, 270.f });
		m_mainLayer->addChildAtPosition(panel, Anchor::Center);
		panel->setZOrder(-10);
		{
			auto g = CCLayerGradient::create(
				ccc4(design.top.r, design.top.g, design.top.b, 255),
				ccc4(design.bot.r, design.bot.g, design.bot.b, 255));
			g->setContentSize({ 372.f, 262.f });
			g->ignoreAnchorPointForPosition(false);
			g->setAnchorPoint({ 0.5f, 0.5f });
			m_mainLayer->addChildAtPosition(g, Anchor::Center);
			g->setZOrder(-9);
		}

		m_tab = std::clamp(getInt("set-tab", 0), 0, 1);
		m_sel = std::clamp(getInt("design", 0), 0, kDesignCount - 1);

		auto listBg = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
		listBg->setColor({ 0, 0, 0 });
		listBg->setOpacity(110);
		listBg->setContentSize({ m_listW + 10.f, m_listH + 10.f });
		m_mainLayer->addChildAtPosition(listBg, Anchor::Center, { 0.f, -8.f });

		m_list = ScrollLayer::create(CCSize{ m_listW, m_listH });
		m_list->ignoreAnchorPointForPosition(false);
		m_list->setAnchorPoint({ 0.5f, 0.5f });
		m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout());
		m_mainLayer->addChildAtPosition(m_list, Anchor::Center, { 0.f, -8.f });

		if (auto bar = Scrollbar::create(m_list)) {
			m_mainLayer->addChildAtPosition(bar, Anchor::Center, { m_listW / 2.f + 14.f, -8.f });
		}

		m_nameLabel = CCLabelBMFont::create("", "bigFont.fnt");
		m_mainLayer->addChildAtPosition(m_nameLabel, Anchor::Center, { -80.f, -113.f });

		auto reset = makeButton("Reset", this, menu_selector(SettingsPopup::onReset), 0.5f);
		m_buttonMenu->addChildAtPosition(reset, Anchor::Center, { 80.f, -113.f });
		auto apply = makeButton("Apply", this, menu_selector(SettingsPopup::onApply), 0.6f);
		m_buttonMenu->addChildAtPosition(apply, Anchor::Center, { 140.f, -113.f });

		buildTabs();
		buildContent();
		return true;
	}

	void buildTabs() {
		for (auto n : m_tabNodes) n->removeFromParent();
		m_tabNodes.clear();
		char const* names[] = { "Designs", "Options" };
		for (int i = 0; i < 2; i++) {
			auto spr = ButtonSprite::create(names[i], 90, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.8f);
			spr->setScale(0.55f);
			if (i == m_tab) spr->setColor(accentColor());
			auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(SettingsPopup::onTab));
			btn->setTag(i);
			m_buttonMenu->addChildAtPosition(btn, Anchor::Center, { -50.f + i * 100.f, 92.f });
			m_tabNodes.push_back(btn);
		}
	}

	void buildContent() {
		m_list->m_contentLayer->removeAllChildrenWithCleanup(true);
		if (m_tab == 0) buildDesigns();
		else buildOptions();
		m_list->m_contentLayer->updateLayout();
		m_list->scrollToTop();
		refreshName();
	}

	void refreshName() {
		if (m_tab == 0) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%s  %d/%d", kDesigns[m_sel].name, m_sel + 1, kDesignCount);
			m_nameLabel->setString(buf);
			fitLabel(m_nameLabel, 150.f, 0.4f);
		} else {
			m_nameLabel->setString("");
		}
	}

	void drawBorder(int i) {
		if (i < 0 || i >= static_cast<int>(m_borders.size()) || !m_borders[i]) return;
		auto d = m_borders[i];
		d->clear();
		bool sel = (i == m_sel);
		ccColor4F c = sel ? ccc4f(1.f, 1.f, 1.f, 1.f) : ccc4f(0.f, 0.f, 0.f, 0.55f);
		float w = sel ? 2.2f : 0.9f;
		float W = m_sw, H = m_sh;
		d->drawSegment({ 0.f, 0.f }, { W, 0.f }, w, c);
		d->drawSegment({ W, 0.f }, { W, H }, w, c);
		d->drawSegment({ W, H }, { 0.f, H }, w, c);
		d->drawSegment({ 0.f, H }, { 0.f, 0.f }, w, c);
	}

	void buildDesigns() {
		m_borders.assign(kDesignCount, nullptr);
		const int perRow = 3;
		float gap = 6.f;
		m_sw = (m_listW - 8.f - gap * (perRow - 1)) / perRow;
		m_sh = 38.f;
		for (int r = 0; r * perRow < kDesignCount; r++) {
			auto row = CCMenu::create();
			row->ignoreAnchorPointForPosition(false);
			row->setAnchorPoint({ 0.5f, 0.5f });
			row->setContentSize({ m_listW, m_sh + 8.f });
			for (int c = 0; c < perRow; c++) {
				int i = r * perRow + c;
				if (i >= kDesignCount) break;
				auto const& d = kDesigns[i];

				auto holder = CCNode::create();
				holder->setContentSize({ m_sw, m_sh });
				holder->setAnchorPoint({ 0.5f, 0.5f });

				auto g = CCLayerGradient::create(ccc4(d.top.r, d.top.g, d.top.b, 255), ccc4(d.bot.r, d.bot.g, d.bot.b, 255));
				g->setContentSize({ m_sw, m_sh });
				g->setPosition({ 0.f, 0.f });
				holder->addChild(g);

				// accent strip along the bottom
				auto acc = CCDrawNode::create();
				acc->drawSegment({ 8.f, 5.f }, { m_sw - 8.f, 5.f }, 1.6f, ccc4f(d.accent.r / 255.f, d.accent.g / 255.f, d.accent.b / 255.f, 1.f));
				holder->addChild(acc, 2);

				auto border = CCDrawNode::create();
				holder->addChild(border, 3);
				m_borders[i] = border;

				auto lab = CCLabelBMFont::create(d.name, "bigFont.fnt");
				fitLabel(lab, m_sw - 12.f, 0.4f);
				lab->setPosition({ m_sw / 2.f, m_sh / 2.f + 3.f });
				holder->addChild(lab, 4);

				auto btn = CCMenuItemSpriteExtra::create(holder, this, menu_selector(SettingsPopup::onPickDesign));
				btn->setTag(i);
				row->addChildAtPosition(btn, Anchor::Center, { (c - (perRow - 1) / 2.f) * (m_sw + gap), 0.f });
				drawBorder(i);
			}
			m_list->m_contentLayer->addChild(row);
		}
	}

	std::string valueText(int idx) {
		auto& d = kSettings[idx];
		int v = std::clamp(getInt(d.key, d.def), d.min, d.max);
		if (d.names) return d.names[v - d.min];
		char buf[32];
		if (d.div > 1) std::snprintf(buf, sizeof(buf), "%.1f", static_cast<float>(v) / d.div);
		else std::snprintf(buf, sizeof(buf), "%d%s", v, d.unit);
		return buf;
	}

	void setValueLabel(CCLabelBMFont* label, int idx) {
		label->setString(valueText(idx).c_str());
		fitLabel(label, 90.f, 0.45f);
	}

	void buildOptions() {
		m_valLabels.assign(kSettingCount, nullptr);
		char const* lastSection = nullptr;
		for (int i = 0; i < kSettingCount; i++) {
			auto& d = kSettings[i];
			if (std::strcmp(d.key, "design") == 0) continue; // has its own tab
			if (!lastSection || std::strcmp(lastSection, d.section) != 0) {
				lastSection = d.section;
				auto holder = CCNode::create();
				holder->setContentSize({ m_listW, 24.f });
				auto hdr = CCLabelBMFont::create(d.section, "goldFont.fnt");
				hdr->setScale(0.55f);
				hdr->setAnchorPoint({ 0.f, 0.5f });
				hdr->setPosition({ 10.f, 12.f });
				holder->addChild(hdr);
				m_list->m_contentLayer->addChild(holder);
			}

			auto row = CCMenu::create();
			row->ignoreAnchorPointForPosition(false);
			row->setAnchorPoint({ 0.5f, 0.5f });
			row->setContentSize({ m_listW, 30.f });

			auto title = CCLabelBMFont::create(d.label, "bigFont.fnt");
			title->setAnchorPoint({ 0.f, 0.5f });
			fitLabel(title, 130.f, 0.42f);
			row->addChildAtPosition(title, Anchor::Left, { 10.f, 0.f });

			auto minus = makeButton("<", this, menu_selector(SettingsPopup::onStep), 0.5f);
			minus->setTag(i * 2);
			row->addChildAtPosition(minus, Anchor::Left, { 168.f, 0.f });

			auto value = CCLabelBMFont::create("", "bigFont.fnt");
			row->addChildAtPosition(value, Anchor::Left, { 232.f, 0.f });
			m_valLabels[i] = value;
			setValueLabel(value, i);

			auto plus = makeButton(">", this, menu_selector(SettingsPopup::onStep), 0.5f);
			plus->setTag(i * 2 + 1);
			row->addChildAtPosition(plus, Anchor::Left, { 296.f, 0.f });

			m_list->m_contentLayer->addChild(row);
		}
	}

	void onTab(CCObject* sender) {
		int t = static_cast<CCNode*>(sender)->getTag();
		if (t == m_tab) return;
		m_tab = std::clamp(t, 0, 1);
		setInt("set-tab", m_tab);
		buildTabs();
		buildContent();
	}

	void onPickDesign(CCObject* sender) {
		int i = static_cast<CCNode*>(sender)->getTag();
		int old = m_sel;
		m_sel = std::clamp(i, 0, kDesignCount - 1);
		setInt("design", m_sel);
		drawBorder(old);
		drawBorder(m_sel);
		refreshName();
	}

	void onStep(CCObject* sender) {
		int tag = static_cast<CCNode*>(sender)->getTag();
		int idx = tag / 2;
		int dir = (tag % 2 == 0) ? -1 : 1;
		auto& d = kSettings[idx];
		int v = std::clamp(getInt(d.key, d.def), d.min, d.max);
		if (d.names) {
			int n = d.max - d.min + 1;
			v = d.min + ((v - d.min + dir + n) % n);
		} else {
			v = std::clamp(v + dir * d.step, d.min, d.max);
		}
		setInt(d.key, v);
		if (idx >= 0 && idx < static_cast<int>(m_valLabels.size()) && m_valLabels[idx]) {
			setValueLabel(m_valLabels[idx], idx);
		}
	}

	void onReset(CCObject*) {
		for (int i = 0; i < kSettingCount; i++) setInt(kSettings[i].key, kSettings[i].def);
		m_sel = std::clamp(getInt("design", 0), 0, kDesignCount - 1);
		buildContent();
		Notification::create("Settings reset. Tap Apply")->show();
	}

	void onApply(CCObject*) {
		// the manager re-opens on the same tab (saved as "mgr-tab")
		auto old = g_manager;
		if (old) old->closeManager();
		if (auto popup = MacroPopup::create()) popup->show();
		this->keyBackClicked();
	}

public:
	static SettingsPopup* create() {
		auto ret = new SettingsPopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

void MacroPopup::onSettings(CCObject*) {
	if (auto popup = SettingsPopup::create()) popup->show();
}

// Compares the live player with the recorded spot. Called BEFORE any correction is applied,
// so with "Snap to route" off this shows the true drift of the replay.
static void divCheck(PlayerObject* p, int frame, float x, float y, int which) {
	if (!p) return;
	float dx = static_cast<float>(p->getPositionX()) - x;
	float dy = static_cast<float>(p->getPositionY()) - y;
	float err = std::max(std::fabs(dx), std::fabs(dy));
	g_divChecked++;
	if (err > g_divMax) g_divMax = err;
	if (err > S("div-tol") * 0.01f) {
		g_divCount++;
		if (g_divFrame < 0) {
			g_divFrame = frame;
			g_divDx = dx;
			g_divDy = dy;
			g_divWhich = which;
			// the position at this frame was decided by inputs just before it: list them
			g_divNear.clear();
			int shown = 0;
			for (auto& in : g_inputs) {
				if (in.frame < frame - 8 || in.frame > frame) continue;
				if (shown++ >= 6) break;
				g_divNear += std::string(g_divNear.empty() ? "" : " ") + (in.down ? "PRESS@" : "RELEASE@") + std::to_string(in.frame);
				if (in.sub) g_divNear += "." + std::to_string(in.sub);
				if (!in.p1) g_divNear += "(p2)";
			}
		}
	}
}

static void divReset() {
	g_divFrame = -1;
	g_divDx = 0.f;
	g_divDy = 0.f;
	g_divWhich = 1;
	g_divCount = 0;
	g_divChecked = 0;
	g_divMax = 0.f;
	g_divReported = false;
	g_divNear.clear();
}


// ===== MOD 8 (solver): measure the REAL physics tick rate =====
// The solver must simulate the same step length the game really uses. The mod's own "tps"
// setting can differ from what the game actually runs (another TPS mod, device, etc.), so
// the real value is measured: x advanced per step, divided into the portal speed (units/s).
static int speedIndexOf(PlayerObject* p) {
	static const float mult[5] = { 0.7f, 0.9f, 1.1f, 1.3f, 1.6f }; // nearest speed multiplier
	int best = 1;
	for (int i = 0; i < 5; i++)
		if (std::fabs(p->m_playerSpeed - mult[i]) < std::fabs(p->m_playerSpeed - mult[best])) best = i;
	return best;
}

static std::array<float, 32> g_tpsRing{};
static int g_tpsCount = 0, g_tpsPos = 0;
// measurement baseline: where the player was when the current measurement started
static int g_tpsBaseFrame = -1, g_tpsBaseCorr = 0, g_tpsBaseSpeed = -1;
static float g_tpsBaseX = 0.f;

// Called once per step (first processCommands call), in every mode. Instead of comparing two
// neighbouring steps (fragile: position may only update once per rendered frame), it measures
// over a baseline of >= 240 steps: tps = portal speed * steps / distance travelled.
// The baseline restarts on death/restart, a speed change, or when route-snapping moved the player.
static void sampleTickRate(PlayerObject* p, int frame) {
	float x = p->getPositionX();
	int corr = static_cast<int>(g_corrections);
	int sp = speedIndexOf(p);
	bool reset = g_tpsBaseFrame < 0 || frame < g_tpsBaseFrame || corr != g_tpsBaseCorr || sp != g_tpsBaseSpeed;
	if (!reset) {
		int df = frame - g_tpsBaseFrame;
		if (df >= 240) {
			float dist = x - g_tpsBaseX;
			if (dist > 1.f) {
				float tps = pf::kSpeedUps[sp] * static_cast<float>(df) / dist;
				if (tps > 30.f && tps < 3000.f) {
					// Forget old samples when the rate changed: the mod's TPS setting was edited, or the
					// new sample is >20% off the current median (stale samples must not outvote it).
					static int lastSetting = 0;
					bool changed = lastSetting != curTps();
					lastSetting = curTps();
					if (!changed && g_tpsCount >= 3) {
						std::vector<float> w(g_tpsRing.begin(), g_tpsRing.begin() + g_tpsCount);
						std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
						float med = w[w.size() / 2];
						if (std::fabs(tps - med) / med > 0.2f) changed = true;
					}
					if (changed) { g_tpsCount = 0; g_tpsPos = 0; }
					g_tpsRing[g_tpsPos] = tps;
					g_tpsPos = (g_tpsPos + 1) % 8;
					g_tpsCount = std::min(g_tpsCount + 1, 8);
				}
			}
			reset = true; // start the next baseline from here
		}
	}
	if (reset) { g_tpsBaseFrame = frame; g_tpsBaseX = x; g_tpsBaseCorr = corr; g_tpsBaseSpeed = sp; }
}

// median of the recent samples (snapped to a common rate when within 6%), 0 if not measured yet
static int measuredTps() {
	if (g_tpsCount < 3) return 0;
	std::vector<float> v(g_tpsRing.begin(), g_tpsRing.begin() + g_tpsCount);
	std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
	float m = v[v.size() / 2];
	for (int c : { 60, 120, 240, 360, 480, 600, 720, 960 })
		if (std::fabs(m - c) / c < 0.06f) return c;
	return static_cast<int>(std::lround(m));
}
// ===== end MOD 8 =====

// ===== MOD 3/7 (solver): job state, snapshot of the live game, launch / poll =====
//
// Threading model
//   main thread : copies the level into a pf::Level (read-only POD) and the player into a
//                 pf::State, starts the worker, then only does an atomic load per frame.
//   worker      : pf::solve() on that copy. It never touches cocos2d, GJBaseGameLayer or the
//                 live PlayerObject, so it cannot crash the game or stall rendering.
//   hand-off    : worker writes `result` under a mutex, then sets status = 2. The main
//                 thread sees status == 2 in update(), joins, and merges into g_inputs.
struct SolverJob {
	std::thread th;
	std::atomic<bool> cancel{ false };
	std::atomic<int> status{ 0 };        // 0 idle, 1 running, 2 finished (result waiting)
	std::atomic<float> prog{ 0.f };      // furthest x reached so far (for the HUD)
	float endX = 0.f;
	int startFrame = 0;                  // step counter the solve started on
	std::mutex m;
	pf::Result result;
	~SolverJob() { cancel = true; if (th.joinable()) th.join(); } // never destroy a joinable thread
};
static SolverJob g_solver;

static void cancelSolver() {
	g_solver.cancel = true;
	if (g_solver.th.joinable()) g_solver.th.join(); // returns fast: the search polls `cancel`
	g_solver.status = 0;
	g_solver.cancel = false;
}

// Reads the live player once, on the main thread.
static pf::State readPlayerState(PlayerObject* p) {
	pf::State st;
	st.x = p->getPositionX();
	st.y = p->getPositionY();
	st.vy = static_cast<float>(p->m_yVelocity);
	if (p->m_isShip) st.mode = pf::Mode::Ship;
	else if (p->m_isDart) st.mode = pf::Mode::Wave;
	else if (p->m_isBird || p->m_isBall || p->m_isRobot || p->m_isSpider || p->m_isSwing) st.mode = pf::Mode::Unsupported;
	else st.mode = pf::Mode::Cube;
	st.flip = p->m_isUpsideDown;
	st.mini = p->m_vehicleSize < 0.9f;
	st.onGround = p->m_isOnGround;
	st.held = false; // assumes the button is up when "Solve" is pressed (pause menu)
	st.speed = static_cast<uint8_t>(speedIndexOf(p));
	return st;
}

// Copies every relevant hitbox out of the live level into plain structs.
// Collision previewing deliberately does NOT call GJBaseGameLayer's own collision code
// (checkCollisions / collisionCheckObjects): those mutate the live player, fire triggers
// and kill the player, and they are not thread-safe. This snapshot is immutable instead.
static pf::Level snapshotLevel(GJBaseGameLayer* gl, pf::State const& st, int& slopes) {
	using pf::Kind; using pf::Eff;
	pf::Level L;
	slopes = 0;
	float maxX = st.x;
	bool needsCeiling = false;
	float minSolidY = FLT_MAX;

	for (auto obj : CCArrayExt<GameObject*>(gl->m_objects)) {
		if (!obj) continue;
		maxX = std::max(maxX, obj->getPositionX());
		if (obj->getPositionX() < st.x - 100.f) continue; // already behind the player

		CCRect r = obj->getObjectRect();
		pf::Box b{ r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(), Kind::Trigger, Eff::None, 0 };

		// portals / pads / orbs are matched by object ID (stable across bindings versions)
		switch (obj->m_objectID) {
			case 12:  b.eff = Eff::ToCube; break;
			case 13:  b.eff = Eff::ToShip; needsCeiling = true; break;
			case 660: b.eff = Eff::ToWave; needsCeiling = true; break;
			case 47: case 111: case 745: case 1331: case 1933: // ball, ufo, robot, spider, swing
			case 286:                                           // dual portal
				b.eff = Eff::ToUnsupported; break;
			case 10:  b.eff = Eff::GravNormal; break;
			case 11:  b.eff = Eff::GravFlip; needsCeiling = true; break;
			case 99:  b.eff = Eff::SizeNormal; break;
			case 101: b.eff = Eff::SizeMini; break;
			case 200: b.eff = Eff::Speed; b.arg = 0; break;
			case 201: b.eff = Eff::Speed; b.arg = 1; break;
			case 202: b.eff = Eff::Speed; b.arg = 2; break;
			case 203: b.eff = Eff::Speed; b.arg = 3; break;
			case 1334: b.eff = Eff::Speed; b.arg = 4; break;
			case 35:  b.eff = Eff::PadYellow; break;
			case 140: b.eff = Eff::PadPink; break;
			case 36:  b.eff = Eff::OrbYellow; break;
			case 141: b.eff = Eff::OrbPink; break;
			default: break;
		}
		if (b.eff == Eff::None) {
			// An alive player cannot be inside a hazard/solid. If the box overlaps the start position
			// it is a phantom (e.g. an object whose hitbox is not positioned yet): skip it and log its ID.
			float ph = st.mini ? 8.f : 14.f;
			bool onPlayer = b.x1 > st.x - ph && b.x0 < st.x + ph && b.y1 > st.y - ph && b.y0 < st.y + ph;
			if (onPlayer && (obj->m_objectType == GameObjectType::Hazard || obj->m_objectType == GameObjectType::Solid)) {
				geode::log::warn("Solver: ignored phantom object id={} type={} box x{:.0f}..{:.0f} y{:.0f}..{:.0f}",
					obj->m_objectID, static_cast<int>(obj->m_objectType), b.x0, b.x1, b.y0, b.y1);
				continue;
			}
			// plain geometry: use the game's own hitbox type
			if (obj->m_objectType == GameObjectType::Hazard) b.kind = Kind::Hazard;
			else if (obj->m_objectType == GameObjectType::Solid) { b.kind = Kind::Solid; minSolidY = std::min(minSolidY, b.y0); }
			else if (obj->m_objectType == GameObjectType::Slope) { b.kind = Kind::Solid; slopes++; } // approximated as a box
			else continue; // decoration, triggers, coins ...
		}
		L.boxes.push_back(b);
	}

	// Ground height: lowest solid's bottom edge, or the player's bottom when standing on the floor.
	// VERIFY against your level: if your bindings expose the real ground Y, use that instead.
	float half = st.mini ? 9.f : pf::phys::kHalf;
	float floorY = minSolidY;
	if (st.onGround && !st.flip) floorY = std::min(floorY, st.y - half);
	L.floorY = (floorY == FLT_MAX) ? 0.f : floorY;
	// ceilings only matter for ship / wave / flipped gravity; keep cube levels uncapped (tall pads)
	L.ceilY = L.floorY + (needsCeiling ? 300.f : 100000.f);
	L.endX = maxX + 60.f; // VERIFY: PlayLayer's level length is more exact if your bindings have it
	L.build(st.x);
	return L;
}

// Starts (or stops, if already running) the solver on the live level.
static void launchSolver(GJBaseGameLayer* gl) {
	if (g_solver.status.load() == 1) { cancelSolver(); Notification::create("Solver stopped")->show(); return; }
	if (!gl || !gl->m_player1) return;
	cancelSolver(); // joins any finished-but-unclaimed worker

	// MOD 8: the route is only valid at the tick rate the game really steps at
	int realTps = measuredTps();
	if (realTps == 0) {
		Notification::create("Solver: play the level for a couple of seconds first so the tick rate can be measured", NotificationIcon::Warning)->show();
		return;
	}

	pf::State st = readPlayerState(gl->m_player1);
	if (st.mode == pf::Mode::Unsupported) {
		Notification::create("Solver: unsupported gamemode here (cube / ship / wave only)", NotificationIcon::Warning)->show();
		return;
	}
	int slopes = 0;
	auto level = std::make_shared<pf::Level>(snapshotLevel(gl, st, slopes));

	geode::log::info("Solver start: x={:.1f} y={:.1f} vy={:.2f} mode={} flip={} mini={} speed={} ground={} boxes={} floor={:.1f} ceil={:.0f} end={:.0f}",
		st.x, st.y, st.vy, static_cast<int>(st.mode), st.flip, st.mini, st.speed, st.onGround,
		level->boxes.size(), level->floorY, level->ceilY, level->endX);

	geode::log::info("Solver: measured {} TPS (mod setting says {})", realTps, curTps());
	pf::Cfg cfg;
	cfg.tps = realTps;
	cfg.decisionTicks = std::max(1, S("sv-tick") * realTps / 240); // "4" always means 1/60 s
	cfg.window = S("sv-window") * 30.f;           // blocks -> units
	cfg.budget = static_cast<size_t>(S("sv-budget")) * 100000;
	cfg.preferTaps = S("sv-taps") != 0;
	cfg.traceEvery = S("exact-route") ? 1 : std::max(1, anchorEvery()); // same spacing recordAnchor() uses

	g_solver.startFrame = static_cast<int>(gl->m_gameState.m_currentProgress);
	g_solver.endX = level->endX;
	g_solver.prog = st.x;
	g_solver.status = 1;
	g_solver.th = std::thread([level, st, cfg]() {
		pf::Result r = pf::solve(*level, st, cfg, g_solver.cancel, g_solver.prog);
		{ std::lock_guard<std::mutex> lk(g_solver.m); g_solver.result = std::move(r); }
		g_solver.status = 2; // publish last
	});
	char sbuf[96];
	std::snprintf(sbuf, sizeof(sbuf), "Solving at %d steps/s (mod TPS %d)%s", realTps, curTps(), slopes ? ", slopes approx." : "");
	Notification::create(sbuf)->show();
}

// Called once per frame from GJBaseGameLayer::update. One atomic load unless a result is waiting.
static void pollSolver() {
	if (g_solver.status.load() != 2) return;
	if (g_solver.th.joinable()) g_solver.th.join();
	pf::Result r;
	{ std::lock_guard<std::mutex> lk(g_solver.m); r = std::move(g_solver.result); }
	g_solver.status = 0;

	if (!r.ok && !r.partial) {
		// full diagnostic in a popup you can screenshot, and in the Geode log
		geode::log::warn("Solver failed (reached x={:.0f}): {}", r.reachedX, r.msg);
		FLAlertLayer::create("Solver", r.msg.c_str(), "OK")->show();
		return;
	}

	// ----- input logging: write the found path into the macro arrays -----
	int sf = g_solver.startFrame;
	// keep everything before the start frame (same rule resetLevel uses for practice checkpoints)
	g_inputs.erase(std::remove_if(g_inputs.begin(), g_inputs.end(),
		[&](MacroInput const& i) { return i.frame >= sf; }), g_inputs.end());
	g_anchors.erase(std::remove_if(g_anchors.begin(), g_anchors.end(),
		[&](MacroAnchor const& a) { return a.frame >= sf; }), g_anchors.end());

	for (auto& in : r.inputs) {
		// button 1 = jump, player 1, applied in the first processCommands call of that step
		g_inputs.push_back({ sf + in.tick, 1, in.down, true, 0 });
	}
	// Anchors: the route's predicted positions, at the same spacing the mod uses when recording
	// (every step with "exact-route", otherwise every anchorEvery() steps). During playback they
	// pull the cube back onto the solved path each time, so the small difference between this
	// simulator and the real game can never build up. Turn "Add anchors" off to skip them.
	size_t anchorCount = 0;
	if (S("sv-anchors")) {
		for (auto& t : r.trace) {
			MacroAnchor a{};
			a.frame = sf + t.tick;
			a.x1 = t.x; a.y1 = t.y; a.v1 = t.vy;
			g_anchors.push_back(a);
			anchorCount++;
		}
	}
	g_macroTps = r.tps > 0 ? r.tps : curTps(); // macro frames are steps at the rate the solver simulated (measured), not the settings value
	g_pathDirty = true;

	// write the finished macro (inputs + anchors) to its file if one is selected
	bool saved = !g_selected.empty() && writeMacro(g_selected);

	char buf[128];
	char const* tail = saved ? ", saved" : ", not saved (pick a macro first)";
	if (r.partial)
		std::snprintf(buf, sizeof(buf), "Route found up to x=%.0f: %zu inputs, %zu anchors%s",
			r.reachedX, r.inputs.size(), anchorCount, tail);
	else
		std::snprintf(buf, sizeof(buf), "Route found: %zu inputs, %zu anchors%s",
			r.inputs.size(), anchorCount, tail);
	Notification::create(buf, NotificationIcon::Success)->show();
}
// ===== end MOD 3/7 =====


// ---------- hooks ----------

class $modify(GJBaseGameLayer) {
	// CBF compatibility: run this hook before click-between-frames' own handleButton hook, so
	// live clicks can be blocked during playback. If playback still drifts with CBF on, try
	// Priority::Last instead (then replayed inputs reach the game without CBF touching them).
	static void onModify(auto& self) {
		(void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::First);
	}

	void update(float dt) {
		// Custom TPS (above 240): the game takes one step of (timeWarp / 240) seconds, so a time
		// warp of 240 / TPS gives steps of exactly 1 / TPS, and the scheduler is kept at normal
		// speed by enforceSpeed. Level time-warp triggers are ignored while this is on.
		int tpsNow = curTps();
		if (tpsNow > 240) m_gameState.m_timeWarp = 240.f / static_cast<float>(tpsNow);
		GJBaseGameLayer::update(dt);
		// keep user speed alive through deaths/restarts/time-warp resets
		enforceSpeed(m_gameState.m_timeWarp);
		updateHud();
		updatePath();
		pollSolver(); // MOD 4/7 (solver): pick up a finished route; cheap atomic check otherwise
	}

	// ----- mini HUD -----
	void updateHud() {
		if (!m_uiLayer || !PlayLayer::get()) return;
		auto root = m_uiLayer->getChildByID("macro-hud");
		if (!S("hud")) {
			if (root) root->removeFromParent();
			return;
		}
		if (!root) {
			root = CCNode::create();
			root->setID("macro-hud");

			auto bg = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
			bg->setColor({ 0, 0, 0 });
			bg->setOpacity(130);
			bg->setAnchorPoint({ 0.f, 1.f });
			bg->setID("bg");

			auto label = CCLabelBMFont::create("", "bigFont.fnt");
			label->setScale(0.32f);
			label->setAnchorPoint({ 0.f, 1.f });
			label->setPosition({ 16.f, -4.f });
			label->setID("label");

			auto dot = CCDrawNode::create();
			dot->drawDot({ 0.f, 0.f }, 4.f, ccc4f(1.f, 0.15f, 0.15f, 1.f));
			dot->setPosition({ 8.f, -9.f });
			dot->setID("dot");

			root->addChild(bg);
			root->addChild(label, 1);
			root->addChild(dot, 2);
			m_uiLayer->addChild(root, 150);
		}

		auto bg = static_cast<CCScale9Sprite*>(root->getChildByID("bg"));
		auto label = static_cast<CCLabelBMFont*>(root->getChildByID("label"));
		auto dot = root->getChildByID("dot");
		if (!bg || !label || !dot) return;

		std::string text = g_mode == 1 ? "REC" : (g_mode == 2 ? "PLAY" : "IDLE");
		if (S("hud-frame")) {
			// live step counter (not the value from the last processCommands call)
			int shown = static_cast<int>(m_gameState.m_currentProgress);
			text += "\nFrame " + std::to_string(std::max(0, shown));
			if (g_mode == 2) {
				int total = 0;
				for (auto& in : g_inputs) total = std::max(total, in.frame);
				if (!g_anchors.empty()) total = std::max(total, g_anchors.back().frame);
				if (total > 0) text += " / " + std::to_string(total);
			}
		}
		if (S("hud-corr") && g_mode == 2) text += "\nFixes " + std::to_string(g_corrections);
		if (S("div-find") && g_mode == 2) {
			char db[64];
			if (g_divFrame >= 0) std::snprintf(db, sizeof(db), "\nDESYNC f%d %.2f/%.2f", g_divFrame, g_divDx, g_divDy);
			else std::snprintf(db, sizeof(db), "\nSync ok (max %.2f)", g_divMax);
			text += db;
		}
		if (S("counter") && g_mode == 1) {
			int clicks = 0, releases = 0;
			for (auto& i : g_inputs) {
				if (i.down) clicks++;
				else releases++;
			}
			text += "\nClicks " + std::to_string(clicks) + " / " + std::to_string(releases);
		}

		// MOD 5/7 (solver): progress while the worker thread is searching
		if (measuredTps() == 0) { // MOD 8: shows why "Solve" may refuse; disappears once measured
			char tb[64];
			std::snprintf(tb, sizeof(tb), "\nTPS measuring: samples %d/3 base %d", g_tpsCount, g_tpsBaseFrame);
			text += tb;
		}
		if (g_solver.status.load() == 1) {
			char sb[64];
			std::snprintf(sb, sizeof(sb), "\nSOLVING x%.0f / %.0f", g_solver.prog.load(), g_solver.endX);
			text += sb;
		}

		if (text != std::string(label->getString())) {
			label->setString(text.c_str());
			auto size = label->getScaledContentSize();
			bg->setContentSize({ size.width + 22.f, size.height + 8.f });
		}

		dot->setVisible(g_mode == 1 && S("hud-dot") != 0);
		dot->setScale(0.85f + 0.25f * std::sin(g_frame * 0.08f));

		auto ws = CCDirector::get()->getWinSize();
		auto sz = bg->getContentSize();
		int corner = std::clamp(S("hud-pos"), 0, 3);
		bool left = (corner == 1 || corner == 3);
		bool topSide = corner >= 2;
		float x = left ? 6.f : ws.width - sz.width - 6.f;
		float y = topSide ? ws.height - 6.f : sz.height + 6.f;
		root->setPosition({ x, y });
	}

	// ----- pathfinder overlay -----
	void updatePath() {
		if (!m_objectLayer || !PlayLayer::get()) return;
		int mode = S("pf-mode");
		bool route = (mode == 1 || mode == 3) && g_mode == 2 && !g_anchors.empty();
		bool trail = (mode == 2 || mode == 3) && g_mode == 1 && !g_anchors.empty();
		auto node = static_cast<CCDrawNode*>(m_objectLayer->getChildByID("macro-path"));

		if (!route && !trail) {
			if (node) node->clear();
			g_pathKey = static_cast<size_t>(-1);
			return;
		}
		if (!node) {
			node = CCDrawNode::create();
			node->setID("macro-path");
			m_objectLayer->addChild(node, 500);
			g_pathDirty = true;
		}

		size_t key = (route ? g_anchorCursor : g_anchors.size()) * 2 + (route ? 1 : 0);
		if (key == g_pathKey && !g_pathDirty) return;
		g_pathKey = key;
		g_pathDirty = false;
		node->clear();

		auto ac = accentColor();
		ccColor4F col = ccc4f(ac.r / 255.f, ac.g / 255.f, ac.b / 255.f, 0.85f);
		ccColor4F col2 = ccc4f(ac.r / 255.f, ac.g / 255.f, ac.b / 255.f, 0.5f);
		float w = S("pf-width") * 0.6f;
		bool dots = S("pf-dots") != 0;
		static const int aheadVals[] = { 0, 100, 300, 600 };
		int ahead = aheadVals[std::clamp(S("pf-ahead"), 0, 3)];

		size_t start = (route && g_anchorCursor > 0) ? g_anchorCursor - 1 : 0;
		size_t stride = std::max<size_t>(1, g_anchors.size() / 600);
		for (size_t i = start + stride; i < g_anchors.size(); i += stride) {
			auto& a = g_anchors[i - stride];
			auto& b = g_anchors[i];
			if (route && ahead > 0 && b.frame > g_frame + ahead) break;
			node->drawSegment({ a.x1, a.y1 }, { b.x1, b.y1 }, w, col);
			if (a.dual && b.dual) node->drawSegment({ a.x2, a.y2 }, { b.x2, b.y2 }, w, col2);
			if (dots) node->drawDot({ b.x1, b.y1 }, w * 1.6f, col);
		}
	}

	void handleButton(bool down, int button, bool p1) {
		if (g_mode == 2 && !g_injecting) return;
		if (g_mode == 1) {
			MacroInput in;
			bool exact = S("step-rec") != 0;
			// step-exact: use the step read at the start of processCommands
			in.frame = exact ? g_frame : static_cast<int>(m_gameState.m_currentProgress);
			in.button = static_cast<int>(button);
			in.down = down;
			in.p1 = p1;
			in.sub = (exact && g_inCmd) ? g_callIdx : 0;
			g_inputs.push_back(in);
			// arrived outside processCommands: it is applied on the next step, so stamp it then
			if (exact && !g_inCmd) g_pendStamp.push_back(g_inputs.size() - 1);
		}
		GJBaseGameLayer::handleButton(down, button, p1);
	}

	void recordAnchor(int frame) {
		if (!m_player1) return;
		int every = S("exact-route") ? 1 : anchorEvery();
		if (frame % every != 0 || frame == g_lastAnchorFrame) return;
		g_lastAnchorFrame = frame;

		MacroAnchor a{};
		a.frame = frame;
		a.x1 = m_player1->getPositionX();
		a.y1 = m_player1->getPositionY();
		a.v1 = static_cast<float>(m_player1->m_yVelocity);
		a.dual = m_gameState.m_isDualMode && m_player2;
		if (a.dual) {
			a.x2 = m_player2->getPositionX();
			a.y2 = m_player2->getPositionY();
			a.v2 = static_cast<float>(m_player2->m_yVelocity);
		}
		g_anchors.push_back(a);
	}

	// Position only: velocity is left alone so clicks keep working.
	void alignPlayer(PlayerObject* p, float x, float y, float v) {
		if (!p) return;
		// "Snap to route" off: playback is left alone so the finder shows real drift
		if (!S("snap")) return;
		if (S("exact-route")) {
			// exact route: put the player on the recorded spot every frame
			float ex = p->getPositionX() - x;
			float ey = p->getPositionY() - y;
			if (std::fabs(ex) > 0.5f || std::fabs(ey) > 0.5f) g_corrections++;
			// don't touch the position when it already matches: re-setting it
			// every frame disturbs the wave trail, which is built from positions
			if (std::fabs(ex) > 0.02f || std::fabs(ey) > 0.02f) p->setPosition({ x, y });
			if (S("route-vel")) p->m_yVelocity = v;
			return;
		}
		if (!anchorsOn()) return;
		float tol = anchorTol();
		float dx = p->getPositionX() - x;
		float dy = p->getPositionY() - y;
		if (std::fabs(dx) > tol || std::fabs(dy) > tol) {
			p->setPosition({ x, y });
			g_corrections++;
		}
	}

	void applyAnchors(int frame) {
		while (g_anchorCursor < g_anchors.size() && g_anchors[g_anchorCursor].frame <= frame) {
			auto& a = g_anchors[g_anchorCursor++];
			if (a.frame != frame) continue;
			if (g_mode == 2 && S("div-find")) {
				divCheck(m_player1, frame, a.x1, a.y1, 1);
				if (a.dual && m_gameState.m_isDualMode) divCheck(m_player2, frame, a.x2, a.y2, 2);
			}
			alignPlayer(m_player1, a.x1, a.y1, a.v1);
			if (a.dual && m_gameState.m_isDualMode) alignPlayer(m_player2, a.x2, a.y2, a.v2);
		}
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		int frame = static_cast<int>(m_gameState.m_currentProgress);
		// which call of this step is this? (the step counter stays the same across half-ticks)
		if (frame != g_lastRawStep) { g_lastRawStep = frame; g_callIdx = 0; }
		else g_callIdx++;
		g_frame = frame;
		if (g_callIdx == 0 && m_player1) sampleTickRate(m_player1, frame); // MOD 8
		if (g_mode == 1 && !g_pendStamp.empty()) {
			for (auto idx : g_pendStamp) {
				if (idx < g_inputs.size()) {
					g_inputs[idx].frame = frame;
					g_inputs[idx].sub = g_callIdx;
				}
			}
			g_pendStamp.clear();
		}
		if (g_mode == 1) {
			recordAnchor(frame);
		}
		if (g_mode == 2) {
			applyAnchors(frame);
			while (g_cursor < g_inputs.size() && (g_inputs[g_cursor].frame < frame || (g_inputs[g_cursor].frame == frame && g_inputs[g_cursor].sub <= g_callIdx))) {
				auto i = g_inputs[g_cursor++];
				g_injecting = true;
				GJBaseGameLayer::handleButton(i.down, i.button, i.p1);
				g_injecting = false;
			}
		}
		g_inCmd = true;
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		g_inCmd = false;
	}
};

class $modify(PlayLayer) {
	void resetLevel() {
		// MOD 6/7 (solver): NOT cancelled on restart. Dying must not kill a running solve; the
		// route is stamped with its own start frame, so it stays valid after a respawn.
		if (g_mode == 1 && S("autosave") && !g_inputs.empty() && !g_selected.empty()) {
			writeMacro(g_selected);
		}
		PlayLayer::resetLevel();

		// frame we respawned at: 0 on a full restart, > 0 at a practice checkpoint
		int startFrame = static_cast<int>(m_gameState.m_currentProgress);

		if (g_mode == 1) {
			if (startFrame > 0) {
				// practice checkpoint: keep everything recorded before it
				g_inputs.erase(std::remove_if(g_inputs.begin(), g_inputs.end(),
					[&](MacroInput const& i) { return i.frame >= startFrame; }), g_inputs.end());
				g_anchors.erase(std::remove_if(g_anchors.begin(), g_anchors.end(),
					[&](MacroAnchor const& a) { return a.frame >= startFrame; }), g_anchors.end());
			} else {
				g_inputs.clear();
				g_anchors.clear();
			}
		}

		g_cursor = 0;
		g_anchorCursor = 0;
		if (g_mode == 2 && startFrame > 0) {
			// playback from a checkpoint: skip what already happened
			while (g_cursor < g_inputs.size() && g_inputs[g_cursor].frame < startFrame) g_cursor++;
			while (g_anchorCursor < g_anchors.size() && g_anchors[g_anchorCursor].frame < startFrame) g_anchorCursor++;
		}
		g_lastAnchorFrame = -1;
		g_corrections = 0;
		g_pathDirty = true;
		g_pendStamp.clear();
		g_lastRawStep = -1;
		g_callIdx = 0;
		if (g_mode == 2) divReset();
		enforceSpeed(m_gameState.m_timeWarp);
	}

	void levelComplete() {
		if (g_mode == 1 && !g_inputs.empty() && !g_selected.empty()) {
			writeMacro(g_selected);
		}
		PlayLayer::levelComplete();
	}

	void onQuit() {
		cancelSolver(); // MOD 6/7 (solver): never leave the worker running after the level is gone
		if (g_mode == 1 && !g_inputs.empty() && !g_selected.empty()) {
			writeMacro(g_selected);
		}
		PlayLayer::onQuit();
	}
};

class $modify(MacroPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto ws = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		menu->setPosition({ 0.f, 0.f });

		auto btn = makeButton("Macro", this, menu_selector(MacroPauseLayer::onMacroOpen), 0.6f);
		int corner = std::clamp(S("pause-corner"), 0, 3);
		bool left = (corner == 1 || corner == 3);
		bool top = corner >= 2;
		float x = left ? 50.f : ws.width - 50.f;
		float y = top ? ws.height - 25.f : 25.f;
		btn->setPosition({ x, y });
		menu->addChild(btn);

		// MOD 7/7 (solver): "Solve" button under/over the Macro button. Tap again to stop a running solve.
		auto solveBtn = makeButton("Solve", this, menu_selector(MacroPauseLayer::onSolve), 0.6f);
		solveBtn->setPosition({ x, top ? y - 32.f : y + 32.f });
		menu->addChild(solveBtn);

		this->addChild(menu, 100);

		// MOD 7/7 (solver): GJBaseGameLayer::update does not run while paused, so also poll from here
		this->schedule(schedule_selector(MacroPauseLayer::pollSolverTick), 0.25f);
	}

	void pollSolverTick(float) { pollSolver(); }

	void onMacroOpen(CCObject*) {
		if (auto popup = MacroPopup::create()) popup->show();
	}

	// MOD 7/7 (solver)
	void onSolve(CCObject*) {
		launchSolver(PlayLayer::get());
	}
};
