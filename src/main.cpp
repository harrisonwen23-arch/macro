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
static float g_tpsExtra = 0.f; // leftover time between steps for a custom tick rate
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

// solver diagnostics: where the player last died while solving
static int g_solDeathFrame = -1;
static float g_solDeathX = 0.f;
static int g_solDeathObj = -1;  // object id that killed the player
static int g_solSteps = 0;       // physics steps survived in the current attempt
static std::string g_diag;       // player state on the first step after a restart
static std::string g_diagDeath;  // player/object state at the last death
static bool g_diagPending = false;

// ---------- save points for the window test ----------
struct SavePoint { int frame; CheckpointObject* cp; };
static std::vector<SavePoint> g_sps;
static std::vector<std::array<float, 2>> g_baseTrace; // player x/y at every frame of the baseline run
static int g_frameBase = 0;      // frame = game progress + this (only while a save point is loaded)
static bool g_spNeedBase = false;
static int g_spExpect = 0;       // frame the loaded save point stands for
static int g_spVerify = 0;       // steps left to compare against the baseline
static int g_spStrategy = 0;     // 0 = plain load, 1 = also restore the held button

// window test runs at its own speed, then the old speed is put back
static float g_wtSavedSpeed = 1.f;
static bool g_wtSavedTouched = false;
static bool g_wtSpeedSet = false;

// window counter tally: how many clicks of each window size were passed this run
static int g_tally[7] = { 0, 0, 0, 0, 0, 0, 0 };
static size_t g_tallyCursor = 0;

// ---------- frame windows ----------
// For every click (input index) the number of frames it can be pressed
// earlier / later and still survive. Saved in the macro file as "W" lines.
using WinMap = std::map<int, std::pair<int, int>>;
static WinMap g_windows;

struct WinTest {
	bool active = false;
	std::vector<int> clicks; // indices into g_inputs of every press
	size_t ci = 0;
	int phase = 0;           // 0 = check macro, 1 = test early, 2 = test late
	int s = 0;               // current shift in frames
	int early = 0;
	int late = 0;
	int endFrame = 0x7fffffff;
	int baseEnd = 0x7fffffff; // frame where the unshifted macro dies (INT_MAX = it finishes)
	bool resolved = false;   // result of the current attempt already recorded
	bool lastOk = false;
	bool pendingReset = false;
	bool pendingAdvance = false;
	int sp = -1;         // save point used by the current attempt (-1 = none)
	bool spOk = true;    // false once save points proved unreliable for this test
	int maxE = 0;        // most frames this click can move earlier without crossing the previous release
	int maxL = 0;        // most frames this click (and its release) can move later without crossing the next press
};
static WinTest g_wt;
static std::vector<MacroInput> g_work; // macro with the tested click shifted

// ---------- auto solver ----------
// Builds a macro by itself, one chunk of frames at a time. The route found so
// far is replayed exactly (positions + clicks); past its end the solver tries
// click patterns. A chunk that survives is kept, a chunk that dies tries the
// next pattern, and when every pattern fails it backs up one chunk.
// Dead-end states are remembered so the search never re-explores them.
struct SolFrame { size_t inputCount; size_t anchorCount; int frame; int tried; std::string key; };
struct Solver {
	bool active = false;
	int chunk = 12;
	int frame = 0;        // end of the proven route
	int k = 0;            // pattern being tried
	int attempts = 0;
	int maxAttempts = 20000;
	int desync = 0;
	bool holding = false; // hold state at the end of the proven route
	std::vector<SolFrame> stack;
	std::vector<MacroAnchor> tmpAn;
	std::set<std::string> dead;
	std::string curKey = "start";
	bool resolved = false;
	int result = 0;       // 0 died, 1 survived the chunk, 2 lost sync
	bool pendingReset = false;
	bool pendingAdvance = false;
	float savedSpeed = 1.f;
	bool savedTouched = false;
};
static Solver g_sol;
static const int kSolCands = 13;

// solver checkpoints: practice-mode save points on proven frames, so a failed attempt
// reloads the nearest one instead of replaying the level from the start
static std::vector<SavePoint> g_solCps;
static SavePoint g_solPend = { 0, nullptr }; // checkpoint taken at the end of the chunk being tried
static int g_solLastCp = -1000000;

// ---------- frame stepper ----------
// Freezes the game and advances it one physics step at a time. Every step first takes a
// checkpoint, so a step can be undone. Driven by on-screen buttons (touch friendly).
struct StepEntry { int frame; CheckpointObject* cp; bool held; };
static bool g_stepOn = false;
static std::vector<StepEntry> g_stepHist;
static int g_stepPending = 0;   // steps requested by the buttons
static int g_stepUndoReq = 0;   // undo requests
static bool g_stepHoldReq = false;
static bool g_stepHeld = false; // button 1 state the stepper believes is held
static bool g_stepDead = false;   // the player died in the last step: Undo is needed
static std::string g_stepMsg;

static constexpr float kSpeedMin = 0.1f;
static constexpr float kSpeedMax = 5.f;

// ---------- palettes ----------

struct ColorPreset { char const* name; ccColor3B color; };
static const ColorPreset kColors[] = {
	{ "Midnight", { 15, 18, 48 } },
	{ "Purple", { 45, 20, 80 } },
	{ "Teal", { 10, 55, 65 } },
	{ "Crimson", { 75, 15, 30 } },
	{ "Forest", { 15, 55, 35 } },
	{ "Slate", { 38, 40, 48 } },
	{ "Rose", { 95, 30, 75 } },
};
static const int kColorCount = 7;

static const ccColor3B kAccents[] = {
	{ 70, 140, 255 }, { 70, 200, 110 }, { 240, 100, 170 },
	{ 255, 150, 60 }, { 160, 110, 240 }, { 240, 200, 60 },
};

struct ShapeDef { char const* name; char const* tex; };
static const ShapeDef kShapes[] = {
	{ "Rounded", "square02b_001.png" },
	{ "Soft", "square02_001.png" },
	{ "Classic", "GJ_square01.png" },
	{ "Dark", "GJ_square05.png" },
};
static const int kShapeCount = 4;

// ---------- settings table ----------

static char const* const kThemeNames[] = { "None", "Neon", "Sunset", "Ocean", "Minimal" };
static char const* const kShapeNames[] = { "Rounded", "Soft", "Classic", "Dark" };
static char const* const kColorNames[] = { "Midnight", "Purple", "Teal", "Crimson", "Forest", "Slate", "Rose", "Custom" };
static char const* const kEndNames[] = { "Midnight", "Purple", "Teal", "Crimson", "Forest", "Slate", "Rose" };
static char const* const kAccentNames[] = { "Blue", "Green", "Pink", "Orange", "Purple", "Gold" };
static char const* const kAnimNames[] = { "None", "Fade", "Slide", "Bounce" };
static char const* const kBgNames[] = { "Stars", "Snow", "Rain", "Particles", "Plain" };
static char const* const kAmountNames[] = { "Few", "Normal", "Many" };
static char const* const kSpeedNames[] = { "Slow", "Normal", "Fast" };
static char const* const kCornerNames[] = { "Bottom right", "Bottom left", "Top right", "Top left" };
static char const* const kOffOn[] = { "Off", "On" };
static char const* const kEveryNames[] = { "10 frames", "20 frames", "40 frames" };
static char const* const kNameFmtNames[] = { "Numbered", "Always numbered", "With date" };
static char const* const kPfModeNames[] = { "Off", "Route", "Trail", "Both" };
static char const* const kAheadNames[] = { "All", "100 frames", "300 frames", "600 frames" };
static char const* const kSortNames[] = { "Name", "Newest", "Longest" };
static char const* const kStyleNames[] = { "Classic", "Tally" };

struct SettingDef {
	char const* key;
	char const* label;
	int min, max, step, def;
	char const* const* names; // nullptr = numeric
	int div;
	char const* unit;
};

static const SettingDef kSettings[] = {
	// page 0: look
	{ "ui-theme", "Theme", 0, 4, 1, 0, kThemeNames, 1, "" },
	{ "ui-size", "Size", 85, 100, 5, 90, nullptr, 1, "%" },
	{ "ui-shape", "Shape", 0, 3, 1, 0, kShapeNames, 1, "" },
	{ "ui-opacity", "Opacity", 30, 100, 10, 100, nullptr, 1, "%" },
	// page 1: colours
	{ "ui-color", "Colour", 0, 7, 1, 0, kColorNames, 1, "" },
	{ "col-r", "Custom red", 0, 255, 15, 40, nullptr, 1, "" },
	{ "col-g", "Custom green", 0, 255, 15, 40, nullptr, 1, "" },
	{ "col-b", "Custom blue", 0, 255, 15, 90, nullptr, 1, "" },
	// page 2: gradient and accent
	{ "grad", "Gradient", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "grad-end", "Gradient end", 0, 6, 1, 1, kEndNames, 1, "" },
	{ "accent", "Accent", 0, 5, 1, 0, kAccentNames, 1, "" },
	{ "anim", "Open animation", 0, 3, 1, 0, kAnimNames, 1, "" },
	// page 3: background and text
	{ "bg-type", "Background", 0, 4, 1, 0, kBgNames, 1, "" },
	{ "bg-amount", "Amount", 0, 2, 1, 1, kAmountNames, 1, "" },
	{ "bg-speed", "Bg speed", 0, 2, 1, 1, kSpeedNames, 1, "" },
	{ "font-size", "List text", 60, 100, 10, 100, nullptr, 1, "%" },
	// page 4: layout
	{ "compact", "Compact mode", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "pause-corner", "Pause button", 0, 3, 1, 0, kCornerNames, 1, "" },
	{ "confirm-delete", "Confirm delete", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "keep-speed", "Keep speed", 0, 1, 1, 0, kOffOn, 1, "" },
	// page 5: HUD
	{ "hud", "Mini HUD", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "hud-pos", "HUD corner", 0, 3, 1, 3, kCornerNames, 1, "" },
	{ "hud-frame", "Frame counter", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "hud-corr", "Corrections", 0, 1, 1, 1, kOffOn, 1, "" },
	// page 6: recording
	{ "hud-dot", "Recording dot", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "counter", "Click counter", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "autosave", "Auto-save", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "name-fmt", "Name format", 0, 2, 1, 0, kNameFmtNames, 1, "" },
	// page 7: playback
	{ "anchors", "Anchors", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "anchor-tol", "Anchor strength", 5, 100, 5, 20, nullptr, 10, "" },
	{ "anchor-every", "Anchor interval", 0, 2, 1, 1, kEveryNames, 1, "" },
	{ "loop", "Loop playback", 0, 1, 1, 0, kOffOn, 1, "" },
	// page 8: pathfinder
	{ "pf-mode", "Pathfinder", 0, 3, 1, 0, kPfModeNames, 1, "" },
	{ "pf-width", "Line width", 1, 4, 1, 2, nullptr, 1, "" },
	{ "pf-dots", "Anchor dots", 0, 1, 1, 0, kOffOn, 1, "" },
	{ "pf-ahead", "Route ahead", 0, 3, 1, 1, kAheadNames, 1, "" },
	// page 9: frame window counter
	{ "win-hud", "Window counter", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "win-size", "Counter size", 50, 100, 10, 70, nullptr, 1, "%" },
	{ "win-max", "Test range", 1, 10, 1, 6, nullptr, 1, "f" },
	{ "win-after", "Test survive", 10, 120, 10, 40, nullptr, 1, "f" },
	// page 10: exact route
	{ "exact-route", "Exact route", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "route-vel", "Restore speed", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "solve-chunk", "Solve chunk", 1, 40, 1, 1, nullptr, 1, "f" },
	{ "solve-speed", "Solve speed", 1, 10, 1, 4, nullptr, 1, "x" },
	// page 11: auto solver
	{ "solve-tries", "Max tries", 500, 20000, 500, 20000, nullptr, 1, "" },
	{ "solve-tap", "Tap length", 2, 10, 1, 3, nullptr, 1, "f" },
	{ "win-style", "Counter style", 0, 1, 1, 1, kStyleNames, 1, "" },
	{ "win-ring", "Click rings", 0, 1, 1, 1, kOffOn, 1, "" },
	// page 12: test run
	{ "win-save", "Save points", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "win-gap", "Save gap", 60, 300, 20, 120, nullptr, 1, "f" },
	{ "win-speed", "Test speed", 1, 10, 1, 4, nullptr, 1, "x" },
	{ "pitch", "Audio pitch", 0, 1, 1, 1, kOffOn, 1, "" },
	// page 13: accuracy
	{ "step-rec", "Step-exact rec", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "div-find", "Desync finder", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "div-tol", "Desync limit", 1, 50, 1, 5, nullptr, 1, "/100" },
	{ "snap", "Snap to route", 0, 1, 1, 1, kOffOn, 1, "" },
	// page 14: fast solver + stepper
	{ "solve-back", "Back up by", 1, 60, 1, 10, nullptr, 1, "f" },
	{ "solve-cp", "Solve checkpoints", 0, 1, 1, 1, kOffOn, 1, "" },
	{ "step-max", "Undo history", 50, 1000, 50, 300, nullptr, 1, "" },
	{ "step-size", "Stepper size", 60, 120, 10, 90, nullptr, 1, "%" },
};
static const int kSettingCount = 60;
static const int kRowsPerPage = 4;
static const int kPageCount = 15;
static char const* const kPageTitles[] = {
	"Look", "Colours", "Gradient", "Background", "Layout", "HUD", "Recording", "Playback", "Pathfinder", "Window", "Route", "Solver / Window", "Test run", "Accuracy", "Fast solve / Step"
};

struct ThemeVal { char const* key; int val; };
static const ThemeVal kThemeNeon[] = {
	{ "ui-shape", 0 }, { "ui-color", 7 }, { "col-r", 15 }, { "col-g", 0 }, { "col-b", 45 },
	{ "grad", 1 }, { "grad-end", 1 }, { "accent", 2 }, { "bg-type", 3 }, { "ui-opacity", 100 }, { nullptr, 0 }
};
static const ThemeVal kThemeSunset[] = {
	{ "ui-shape", 0 }, { "ui-color", 3 }, { "grad", 1 }, { "grad-end", 6 }, { "accent", 5 },
	{ "bg-type", 0 }, { "ui-opacity", 100 }, { nullptr, 0 }
};
static const ThemeVal kThemeOcean[] = {
	{ "ui-shape", 1 }, { "ui-color", 2 }, { "grad", 1 }, { "grad-end", 0 }, { "accent", 0 },
	{ "bg-type", 2 }, { "ui-opacity", 100 }, { nullptr, 0 }
};
static const ThemeVal kThemeMinimal[] = {
	{ "ui-shape", 1 }, { "ui-color", 5 }, { "grad", 0 }, { "accent", 0 },
	{ "bg-type", 4 }, { "ui-opacity", 90 }, { nullptr, 0 }
};
static const ThemeVal* const kThemes[] = { nullptr, kThemeNeon, kThemeSunset, kThemeOcean, kThemeMinimal };

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

static ccColor3B accentColor() {
	return kAccents[std::clamp(S("accent"), 0, 5)];
}

static ccColor3B baseColor() {
	int c = S("ui-color");
	if (c >= kColorCount) {
		return { static_cast<GLubyte>(S("col-r")), static_cast<GLubyte>(S("col-g")), static_cast<GLubyte>(S("col-b")) };
	}
	return kColors[c].color;
}

static CCScale9Sprite* makePanel(int shape, ccColor3B color, CCSize size) {
	auto panel = CCScale9Sprite::create(kShapes[shape].tex, { 0.f, 0.f, 80.f, 80.f });
	if (!panel) panel = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
	panel->setColor(color);
	panel->setContentSize(size);
	return panel;
}

static CCMenuItemSpriteExtra* makeButton(char const* text, CCObject* target, SEL_MenuHandler cb, float scale) {
	auto spr = ButtonSprite::create(text);
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

// test and solver runs use their own speed, so the audio is left alone there
static float pitchFor() {
	return (S("pitch") != 0 && g_mode != 3 && g_mode != 4) ? g_speed : 1.f;
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

static void resetSpeed() {
	g_speed = 1.f;
	g_speedTouched = false;
	CCDirector::get()->getScheduler()->setTimeScale(1.f);
	applyPitch(1.f);
}

static void wtBeginSpeed() {
	if (!g_wtSpeedSet) {
		g_wtSavedSpeed = g_speed;
		g_wtSavedTouched = g_speedTouched;
		g_wtSpeedSet = true;
	}
	int x = S("win-speed");
	if (x > 1) {
		g_speed = static_cast<float>(x);
		applySpeed();
	}
}

static void wtRestoreSpeed() {
	if (!g_wtSpeedSet) return;
	g_wtSpeedSet = false;
	if (g_wtSavedTouched) {
		g_speed = g_wtSavedSpeed;
		applySpeed();
	} else {
		resetSpeed();
	}
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
//   W <input index> <early frames> <late frames>              -> click window
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
	for (auto& [idx, w] : g_windows) {
		out << "W " << idx << ' ' << w.first << ' ' << w.second << '\n';
	}
}

static void parseMacro(std::istream& in, std::vector<MacroInput>& inputs, std::vector<MacroAnchor>& anchors, WinMap* wins = nullptr) {
	inputs.clear();
	anchors.clear();
	if (wins) wins->clear();
	g_macroTps = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == 'T') continue;
		if (line[0] == 'R') { g_macroTps = std::atoi(line.c_str() + 1); continue; }
		if (line[0] == 'W') {
			std::istringstream s(line.substr(1));
			int idx, e, l;
			if (wins && (s >> idx >> e >> l)) (*wins)[idx] = { e, l };
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

static bool readMacro(std::string const& name, std::vector<MacroInput>& inputs, std::vector<MacroAnchor>& anchors, WinMap* wins = nullptr) {
	std::ifstream in(macroFile(name));
	if (!in) return false;
	parseMacro(in, inputs, anchors, wins);
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

static void showSavedBox(std::string const& text) {
	auto scene = CCDirector::get()->getRunningScene();
	if (!scene) return;
	auto ws = CCDirector::get()->getWinSize();

	auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
	label->setScale(0.45f);
	{
		float maxW = ws.width - 30.f;
		float w = label->getContentSize().width;
		if (w > 0.f && w * 0.45f > maxW) label->setScale(maxW / w);
	}
	auto size = label->getScaledContentSize();

	auto bg = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
	bg->setColor({ 0, 0, 0 });
	bg->setOpacity(153);
	bg->setContentSize({ size.width + 24.f, size.height + 16.f });

	auto node = CCNode::create();
	node->setPosition({ ws.width / 2.f, ws.height - 28.f });
	node->addChild(bg);
	node->addChild(label, 1);

	bg->runAction(CCSequence::create(CCDelayTime::create(2.f), CCFadeTo::create(0.5f, 0), nullptr));
	label->runAction(CCSequence::create(CCDelayTime::create(2.f), CCFadeTo::create(0.5f, 0), nullptr));
	node->runAction(CCSequence::create(
		CCDelayTime::create(2.6f),
		CCCallFunc::create(node, callfunc_selector(CCNode::removeFromParent)),
		nullptr
	));

	scene->addChild(node, 10000);
}

// ---------- window test engine ----------

// The game's checkpoint functions are looked up in a way that compiles even if they
// are named differently: then save points simply stay off.
static CheckpointObject* spCreate(PlayLayer* pl) {
	return [](auto* p) -> CheckpointObject* {
		if constexpr (requires { p->createCheckpoint(); }) {
			auto r = p->createCheckpoint();
			if constexpr (std::is_convertible_v<decltype(r), CheckpointObject*>) return r;
			else return nullptr;
		} else {
			return nullptr;
		}
	}(pl);
}

static bool spLoad(PlayLayer* pl, CheckpointObject* cp) {
	return [](auto* p, CheckpointObject* c) -> bool {
		if constexpr (requires { p->loadFromCheckpoint(c); }) {
			p->loadFromCheckpoint(c);
			return true;
		} else {
			return false;
		}
	}(pl, cp);
}

// Is the jump button currently held down on player 1? Falls back to false if the
// field is named differently, which just means the button is pressed again.
static bool playerHolding(PlayerObject* p) {
	return [](auto* q) -> bool {
		if (!q) return false;
		if constexpr (requires { q->m_holdingButton; }) return static_cast<bool>(q->m_holdingButton);
		else return false;
	}(p);
}

static void stepClear() {
	for (auto& e : g_stepHist) if (e.cp) e.cp->release();
	g_stepHist.clear();
	g_stepPending = 0;
	g_stepUndoReq = 0;
	g_stepHoldReq = false;
	g_stepHeld = false;
	g_stepDead = false;
	g_stepMsg.clear();
}

static void spClear() {
	for (auto& s : g_sps) if (s.cp) s.cp->release();
	g_sps.clear();
	g_baseTrace.clear();
	g_frameBase = 0;
	g_spNeedBase = false;
	g_spVerify = 0;
}

// A loaded save point did not match the baseline run. Try the other way of loading it
// once; if that fails too, stop using save points and redo the attempt from the start.
static void spFail() {
	g_spVerify = 0;
	if (g_spStrategy == 0) g_spStrategy = 1;
	else g_wt.spOk = false;
	g_wt.sp = -1;
	g_wt.resolved = true;
	g_wt.pendingReset = true;
	g_wt.pendingAdvance = false;
}

static void wtResolve(bool ok) {
	if (!g_wt.active || g_wt.resolved) return;
	g_wt.resolved = true;
	g_wt.lastOk = ok;
	g_wt.pendingReset = true;
	g_wt.pendingAdvance = true;
}

// Builds the macro for the current attempt: the tested click (and its
// release) is moved by the current shift.
static void wtBuildWork() {
	g_work = g_inputs;
	if (g_wt.phase == 0) {
		g_wt.endFrame = 0x7fffffff;
		return;
	}
	int shift = g_wt.phase == 1 ? -g_wt.s : g_wt.s;
	size_t idx = static_cast<size_t>(g_wt.clicks[g_wt.ci]);
	if (idx >= g_work.size()) return;
	int button = g_work[idx].button;
	bool p1 = g_work[idx].p1;
	g_work[idx].frame = std::max(0, g_work[idx].frame + shift);
	int clickFrame = g_work[idx].frame;
	for (size_t j = idx + 1; j < g_work.size(); j++) {
		if (g_work[j].button == button && g_work[j].p1 == p1 && !g_work[j].down) {
			g_work[j].frame = std::max(0, g_work[j].frame + shift);
			break;
		}
	}
	std::stable_sort(g_work.begin(), g_work.end(), [](MacroInput const& a, MacroInput const& b) {
		return a.frame < b.frame;
	});
	// Survive for at least "win-after" frames, and also through the next two presses
	// of the original macro (a shifted click can set up a death a few clicks later).
	// Never past where the macro itself dies (the latest point we can judge).
	int need = clickFrame + S("win-after");
	int seen = 0;
	int origFrame = g_inputs[idx].frame;
	for (size_t j = idx + 1; j < g_inputs.size() && seen < 2; j++) {
		if (g_inputs[j].down && g_inputs[j].frame > origFrame) {
			seen++;
			if (seen == 2) need = std::max(need, g_inputs[j].frame + 10);
		}
	}
	if (seen < 2 && !g_inputs.empty()) need = std::max(need, g_inputs.back().frame + 10);
	g_wt.endFrame = std::min(need, g_wt.baseEnd);
}

// How far the click at input index idx can be moved without crossing a neighbouring input
// of the same button. Moving past the previous release (or the next press) would change the
// order of inputs and test a macro that is not "the same macro, shifted", which gave wrong windows.
static void wtLimits(int idx, int& maxE, int& maxL) {
	auto& c = g_inputs[idx];
	int prevRel = -1;
	for (int j = idx - 1; j >= 0; j--) {
		if (g_inputs[j].button == c.button && g_inputs[j].p1 == c.p1 && !g_inputs[j].down) {
			prevRel = g_inputs[j].frame;
			break;
		}
	}
	maxE = (prevRel < 0) ? c.frame : (c.frame - prevRel - 1);
	maxL = 0x7fffffff;
	int rel = -1;
	for (size_t j = static_cast<size_t>(idx) + 1; j < g_inputs.size(); j++) {
		if (g_inputs[j].button == c.button && g_inputs[j].p1 == c.p1 && !g_inputs[j].down) { rel = static_cast<int>(j); break; }
	}
	if (rel >= 0) {
		for (size_t j = static_cast<size_t>(rel) + 1; j < g_inputs.size(); j++) {
			if (g_inputs[j].button == c.button && g_inputs[j].p1 == c.p1 && g_inputs[j].down) {
				maxL = g_inputs[j].frame - g_inputs[rel].frame - 1;
				break;
			}
		}
	}
	maxE = std::max(0, maxE);
	maxL = std::max(0, maxL);
}

// Starts testing the click at g_wt.ci (skipping clicks that cannot move at all).
// 1 = a test is set up, 0 = every click is done
static int wtBeginClick() {
	while (g_wt.ci < g_wt.clicks.size()) {
		int idx = g_wt.clicks[g_wt.ci];
		wtLimits(idx, g_wt.maxE, g_wt.maxL);
		g_wt.early = 0;
		g_wt.late = 0;
		if (g_wt.maxE >= 1) { g_wt.phase = 1; g_wt.s = 1; return 1; }
		if (g_wt.maxL >= 1) { g_wt.phase = 2; g_wt.s = 1; return 1; }
		g_windows[idx] = { 0, 0 };
		g_wt.ci++;
	}
	return 0;
}

static int wtFinishClick() {
	g_windows[g_wt.clicks[g_wt.ci]] = { g_wt.early, g_wt.late };
	g_wt.ci++;
	return wtBeginClick();
}

// 1 = keep testing, 0 = finished, -1 = macro does not finish on its own
static int wtAdvance() {
	int maxS = S("win-max");
	if (g_wt.phase == 0) {
		if (!g_wt.lastOk) return -1;
		// the macro doesn't have to finish the level: test only the clicks
		// that happen before the frame where it dies
		std::vector<int> keep;
		for (int ci : g_wt.clicks) {
			if (ci >= 0 && ci < static_cast<int>(g_inputs.size()) && g_inputs[ci].frame < g_wt.baseEnd) keep.push_back(ci);
		}
		g_wt.clicks = keep;
		if (g_wt.clicks.empty()) return -1;
		g_wt.ci = 0;
		return wtBeginClick();
	}
	if (g_wt.phase == 1) {
		if (g_wt.lastOk) g_wt.early = g_wt.s;
		if (g_wt.lastOk && g_wt.s < std::min(maxS, g_wt.maxE)) { g_wt.s++; return 1; }
		if (g_wt.maxL >= 1) { g_wt.phase = 2; g_wt.s = 1; return 1; }
		return wtFinishClick();
	}
	int clickIdx = g_wt.clicks[g_wt.ci];
	bool lateOk = g_wt.lastOk && g_inputs[clickIdx].frame + g_wt.s < g_wt.baseEnd;
	if (lateOk) g_wt.late = g_wt.s;
	if (lateOk && g_wt.s < std::min(maxS, g_wt.maxL)) { g_wt.s++; return 1; }
	return wtFinishClick();
}

static void wtFinish(bool aborted) {
	g_mode = 0;
	wtRestoreSpeed();
	spClear();
	g_wt.active = false;
	g_wt.pendingReset = false;
	g_wt.pendingAdvance = false;
	if (aborted) {
		g_windows.clear();
		showSavedBox("Test stopped: no clicks to test before the macro's death\n" + g_diag + "\n" + g_diagDeath);
		return;
	}
	if (!g_selected.empty()) writeMacro(g_selected);
	std::string note = (S("win-save") && !g_wt.spOk) ? "\nSave points did not match here, used full replay" : "";
	if (g_wt.baseEnd < 0x7fffffff) showSavedBox("Window test done (up to frame " + std::to_string(g_wt.baseEnd) + "), saved" + note);
	else showSavedBox("Window test done and saved" + note);
}

// ---------- solver engine ----------

// Click pattern number k, as (frame offset inside the chunk, button down?).
static std::vector<std::pair<int, bool>> solCandidate(int k) {
	int C = g_sol.chunk;
	bool hold = g_sol.holding;
	int offs[4] = { 0, C / 2, C / 4, (C * 3) / 4 };
	std::vector<std::pair<int, bool>> ev;
	if (k <= 0) return ev;
	if (k <= 4) {
		ev.push_back({ offs[k - 1], !hold });
		return ev;
	}
	int j = std::clamp(k - 5, 0, 7);
	int o = offs[j / 2];
	int tap = S("solve-tap");
	int w = (j % 2 == 0) ? tap : tap * 2;
	int o2 = std::min(o + w, C - 1);
	if (o2 <= o) o2 = o + 1;
	ev.push_back({ o, !hold });
	ev.push_back({ o2, hold });
	return ev;
}

static void solRecomputeHold() {
	bool h = false;
	for (auto& i : g_inputs) {
		if (i.p1 && i.button == 1) h = i.down;
	}
	g_sol.holding = h;
}

// first candidate number >= k that does not repeat an earlier candidate's clicks
// (tiny chunks make several patterns identical)
static int solNextK(int k) {
	while (k < kSolCands) {
		auto a = solCandidate(k);
		bool dup = false;
		for (int j = 0; j < k && !dup; j++) {
			if (solCandidate(j) == a) dup = true;
		}
		if (!dup) break;
		k++;
	}
	return k;
}

static void solCpClear() {
	for (auto& c : g_solCps) if (c.cp) c.cp->release();
	g_solCps.clear();
	if (g_solPend.cp) g_solPend.cp->release();
	g_solPend = { 0, nullptr };
	g_solLastCp = -1000000;
}

// Called at the start of the step that ends the chunk: the state here is exactly the end of
// the route if this chunk survives. Practice mode only (like the window test save points).
static void solMaybeCp(int frame) {
	if (!S("solve-cp") || g_solPend.cp) return;
	if (g_solCps.size() >= 400) return;
	if (frame - g_solLastCp < S("solve-back")) return;
	auto pl = PlayLayer::get();
	if (!pl || !pl->m_isPracticeMode) return;
	if (auto cp = spCreate(pl)) {
		cp->retain();
		g_solPend = { frame, cp };
	}
}

// proven route + the pattern being tried
static void solBuildWork() {
	g_work = g_inputs;
	for (auto& e : solCandidate(g_sol.k)) {
		g_work.push_back({ g_sol.frame + e.first, 1, e.second, true });
	}
	g_sol.tmpAn.clear();
}

static void solResolve(int kind) {
	if (!g_sol.active || g_sol.resolved) return;
	g_sol.resolved = true;
	g_sol.result = kind;
	g_sol.pendingReset = true;
	g_sol.pendingAdvance = true;
}

// keep the chunk that was just played
static void solCommit() {
	g_sol.stack.push_back({ g_inputs.size(), g_anchors.size(), g_sol.frame, g_sol.k, g_sol.curKey });
	auto ev = solCandidate(g_sol.k);
	for (auto& e : ev) {
		g_inputs.push_back({ g_sol.frame + e.first, 1, e.second, true });
		g_sol.holding = e.second;
	}
	for (auto& a : g_sol.tmpAn) g_anchors.push_back(a);
	g_sol.tmpAn.clear();
}

// compact description of the player state at the end of a chunk
static std::string solKey(MacroAnchor const& a, bool hold, int frame) {
	char buf[160];
	std::snprintf(buf, sizeof(buf), "%d|%.2f|%.2f|%.3f|%d|%.2f|%.2f|%.3f|%d",
		frame, a.x1, a.y1, a.v1, a.dual ? 1 : 0, a.x2, a.y2, a.v2, hold ? 1 : 0);
	return buf;
}

// 1 = keep going, -1 = no route, -2 = try limit, -3 = lost sync
static int solAdvanceCore() {
	g_sol.attempts++;
	if (g_sol.attempts > g_sol.maxAttempts) return -2;
	if (g_sol.result == 2) {
		g_sol.desync++;
		return g_sol.desync > 5 ? -3 : 1;
	}
	g_sol.desync = 0;
	if (g_sol.result == 1) {
		bool h = g_sol.holding;
		for (auto& e : solCandidate(g_sol.k)) h = e.second;
		std::string endKey = g_sol.tmpAn.empty() ? std::string()
			: solKey(g_sol.tmpAn.back(), h, g_sol.frame + g_sol.chunk);
		if (endKey.empty() || !g_sol.dead.count(endKey)) {
			solCommit();
			g_sol.frame += g_sol.chunk;
			g_sol.k = 0;
			if (!endKey.empty()) g_sol.curKey = endKey;
			return 1;
		}
		// this chunk leads to a state already known to be a dead end: treat as failed
	}
	g_sol.k = solNextK(g_sol.k + 1);
	// Every pattern failed here: back up. The first step back goes to the previous chunk;
	// then keep going until we are "solve-back" frames before the place that failed, so the
	// next try restarts from a checkpoint about that far back instead of one chunk back.
	bool backing = false;
	int target = 0;
	while (true) {
		bool exhausted = g_sol.k >= kSolCands;
		if (!exhausted && !(backing && g_sol.frame > target && !g_sol.stack.empty())) break;
		if (g_sol.stack.empty()) return -1;
		if (exhausted) {
			if (!backing) { backing = true; target = g_sol.frame - S("solve-back"); }
			g_sol.dead.insert(g_sol.curKey);
		}
		auto sf = g_sol.stack.back();
		g_sol.stack.pop_back();
		g_inputs.resize(sf.inputCount);
		g_anchors.resize(sf.anchorCount);
		g_sol.frame = sf.frame;
		g_sol.curKey = sf.key;
		g_sol.k = solNextK(sf.tried + 1);
		solRecomputeHold();
	}
	return 1;
}

// Wraps the search step: keeps the checkpoint taken at the end of the chunk if that chunk
// was committed, and drops every checkpoint that lies past the (possibly backed-up) route end.
static int solAdvance() {
	SavePoint pend = g_solPend;
	g_solPend = { 0, nullptr };
	size_t before = g_sol.stack.size();
	int fb = g_sol.frame;
	int ck = g_sol.chunk;
	int r = solAdvanceCore();
	bool committed = r == 1 && g_sol.result == 1 && g_sol.frame == fb + ck && g_sol.stack.size() == before + 1;
	if (pend.cp) {
		if (committed && pend.frame == g_sol.frame) {
			g_solCps.push_back(pend);
			g_solLastCp = pend.frame;
		} else {
			pend.cp->release();
		}
	}
	while (!g_solCps.empty() && g_solCps.back().frame > g_sol.frame) {
		g_solCps.back().cp->release();
		g_solCps.pop_back();
	}
	g_solLastCp = g_solCps.empty() ? -1000000 : g_solCps.back().frame;
	return r;
}

static void solRestoreSpeed() {
	if (g_sol.savedTouched) {
		g_speed = g_sol.savedSpeed;
		applySpeed();
	} else {
		resetSpeed();
	}
}


#include "part2.inc"
#include "part3.inc"
