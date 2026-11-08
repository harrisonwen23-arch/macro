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
#include <utility>
#include <filesystem>
#include <system_error>

using namespace geode::prelude;

struct MacroInput { int frame; int button; bool down; bool p1; };

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
static float g_speed = 1.f;
static bool g_speedTouched = false;
static std::string g_selected;
static std::string g_filterText;
static std::string g_folder; // "" = all, "(none)" = untagged
static bool g_pathDirty = true;
static size_t g_pathKey = static_cast<size_t>(-1);

// solver diagnostics: where the player last died while solving
static int g_solDeathFrame = -1;
static float g_solDeathX = 0.f;

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
	bool resolved = false;   // result of the current attempt already recorded
	bool lastOk = false;
	bool pendingReset = false;
	bool pendingAdvance = false;
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
	{ "solve-chunk", "Solve chunk", 6, 40, 2, 12, nullptr, 1, "f" },
	{ "solve-speed", "Solve speed", 1, 10, 1, 4, nullptr, 1, "x" },
	// page 11: auto solver
	{ "solve-tries", "Max tries", 500, 20000, 500, 20000, nullptr, 1, "" },
	{ "solve-tap", "Tap length", 2, 10, 1, 3, nullptr, 1, "f" },
};
static const int kSettingCount = 46;
static const int kRowsPerPage = 4;
static const int kPageCount = 12;
static char const* const kPageTitles[] = {
	"Look", "Colours", "Gradient", "Background", "Layout", "HUD", "Recording", "Playback", "Pathfinder", "Window", "Route", "Solver"
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
static void enforceSpeed(float timeWarp) {
	if (!g_speedTouched) return;
	if (timeWarp <= 0.f) timeWarp = 1.f;
	CCDirector::get()->getScheduler()->setTimeScale(g_speed * timeWarp);
}

static void applySpeed() {
	g_speedTouched = true;
	CCDirector::get()->getScheduler()->setTimeScale(g_speed);
}

static void resetSpeed() {
	g_speed = 1.f;
	g_speedTouched = false;
	CCDirector::get()->getScheduler()->setTimeScale(1.f);
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
//   <frame> <button> <down 0/1> <p1 0/1>                      -> input
//   A <frame> <x1> <y1> <v1> <dual 0/1> <x2> <y2> <v2>        -> anchor
//   W <input index> <early frames> <late frames>              -> click window
static void serializeMacro(std::ostream& out) {
	out << std::setprecision(9);
	out << "T " << static_cast<long long>(std::time(nullptr)) << '\n';
	for (auto& i : g_inputs) {
		out << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0) << '\n';
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
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == 'T') continue;
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
			if (s >> frame >> button >> down >> p1) {
				inputs.push_back({ frame, button, down != 0, p1 != 0 });
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
		} else if (line[0] != 'A') {
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
	g_wt.endFrame = clickFrame + S("win-after");
}

// 1 = keep testing, 0 = finished, -1 = macro does not finish on its own
static int wtAdvance() {
	int maxS = S("win-max");
	if (g_wt.phase == 0) {
		if (!g_wt.lastOk) return -1;
		g_wt.phase = 1;
		g_wt.s = 1;
		g_wt.early = 0;
		g_wt.late = 0;
		return 1;
	}
	if (g_wt.phase == 1) {
		if (g_wt.lastOk) g_wt.early = g_wt.s;
		if (g_wt.lastOk && g_wt.s < maxS) { g_wt.s++; return 1; }
		g_wt.phase = 2;
		g_wt.s = 1;
		return 1;
	}
	if (g_wt.lastOk) g_wt.late = g_wt.s;
	if (g_wt.lastOk && g_wt.s < maxS) { g_wt.s++; return 1; }
	g_windows[g_wt.clicks[g_wt.ci]] = { g_wt.early, g_wt.late };
	g_wt.ci++;
	if (g_wt.ci >= g_wt.clicks.size()) return 0;
	g_wt.phase = 1;
	g_wt.s = 1;
	g_wt.early = 0;
	g_wt.late = 0;
	return 1;
}

static void wtFinish(bool aborted) {
	g_mode = 0;
	g_wt.active = false;
	g_wt.pendingReset = false;
	g_wt.pendingAdvance = false;
	if (aborted) {
		g_windows.clear();
		showSavedBox("Test stopped: macro doesn't finish the level on its own");
		return;
	}
	if (!g_selected.empty()) writeMacro(g_selected);
	showSavedBox("Window test done and saved");
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
static int solAdvance() {
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
	g_sol.k++;
	while (g_sol.k >= kSolCands) {
		if (g_sol.stack.empty()) return -1;
		g_sol.dead.insert(g_sol.curKey);
		auto sf = g_sol.stack.back();
		g_sol.stack.pop_back();
		g_inputs.resize(sf.inputCount);
		g_anchors.resize(sf.anchorCount);
		g_sol.frame = sf.frame;
		g_sol.curKey = sf.key;
		g_sol.k = sf.tried + 1;
		solRecomputeHold();
	}
	return 1;
}

static void solRestoreSpeed() {
	if (g_sol.savedTouched) {
		g_speed = g_sol.savedSpeed;
		applySpeed();
	} else {
		resetSpeed();
	}
}

// code: 0 = route complete, 3 = stopped by the user, negative = gave up
static void solFinish(int code) {
	g_mode = 0;
	g_sol.active = false;
	g_sol.pendingReset = false;
	g_sol.pendingAdvance = false;
	solRestoreSpeed();
	bool have = !g_inputs.empty();
	if (have && code != -1 && !g_selected.empty()) writeMacro(g_selected);
	if (code == 0) showSavedBox("Route found: " + std::to_string(g_inputs.size()) + " inputs, saved");
	else if (code == 3) showSavedBox("Solver stopped. Partial route saved");
	else if (code == -1) showSavedBox(
		"No route. Last death: frame " + std::to_string(g_solDeathFrame) +
		", x " + std::to_string(static_cast<int>(g_solDeathX)) +
		", tries " + std::to_string(g_sol.attempts)
	);
	else if (code == -2) showSavedBox("Solver hit the try limit. Partial route saved");
	else showSavedBox("Solver lost sync with the level");
}

// ---------- animated background ----------

static ccColor4F starColor(float r, float g, float b, float a) {
	return ccc4f(r * a, g * a, b * a, a);
}

class BgField : public CCNode {
protected:
	struct Shoot { float x0, y0, dx, dy, len, t, speed; };
	struct Twinkle { float x, y, phase, speed, size; };
	struct P { float x, y, vx, vy, size, phase; };

	CCDrawNode* m_draw = nullptr;
	CCSize m_area;
	float m_timeMul = 1.f;
	int m_type = 0;
	ccColor3B m_accent = { 255, 255, 255 };
	std::vector<Shoot> m_shoots;
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

	bool init(CCSize area, int type, int amount, int speedIdx) {
		if (!CCNode::init()) return false;
		m_area = area;
		m_type = type;
		m_accent = accentColor();
		static const float tmul[] = { 0.5f, 1.f, 2.f };
		static const float amul[] = { 0.4f, 1.f, 2.f };
		m_timeMul = tmul[std::clamp(speedIdx, 0, 2)];
		float am = amul[std::clamp(amount, 0, 2)];
		m_draw = CCDrawNode::create();
		this->addChild(m_draw);

		if (type == 0) {
			int twinkles = static_cast<int>(40 * am);
			int shoots = std::max(1, static_cast<int>(4 * am));
			for (int i = 0; i < twinkles; i++) {
				m_twinkles.push_back({
					rnd() * area.width, rnd() * area.height,
					rnd() * 6.28f, 1.f + rnd() * 2.f, 0.5f + rnd() * 1.1f
				});
			}
			for (int i = 0; i < shoots; i++) {
				Shoot s{};
				respawn(s);
				s.t -= static_cast<float>(i) * 0.6f;
				m_shoots.push_back(s);
			}
		} else {
			int n = static_cast<int>((type == 1 ? 60 : (type == 2 ? 50 : 40)) * am);
			for (int i = 0; i < n; i++) {
				P p{};
				p.x = rnd() * area.width;
				p.y = rnd() * area.height;
				p.phase = rnd() * 6.28f;
				if (type == 1) { p.vy = -(15.f + rnd() * 25.f); p.vx = 0.f; p.size = 0.8f + rnd() * 1.6f; }
				else if (type == 2) { p.vy = -(200.f + rnd() * 120.f); p.vx = -30.f; p.size = 0.7f; }
				else { p.vy = 10.f + rnd() * 15.f; p.vx = (rnd() - 0.5f) * 10.f; p.size = 1.f + rnd() * 2.f; }
				m_ps.push_back(p);
			}
		}
		this->scheduleUpdate();
		return true;
	}

	void updateStars(float dt) {
		for (auto& tw : m_twinkles) {
			tw.phase += dt * tw.speed;
			float a = 0.25f + 0.55f * (0.5f + 0.5f * std::sin(tw.phase));
			m_draw->drawDot({ tw.x, tw.y }, tw.size, starColor(0.8f, 0.9f, 1.f, a));
		}
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

	void updateParticles(float dt) {
		for (auto& p : m_ps) {
			p.phase += dt * 2.f;
			p.y += p.vy * dt;
			p.x += p.vx * dt;
			if (m_type == 1) p.x += std::sin(p.phase) * 12.f * dt;

			bool out = p.y < -6.f || p.y > m_area.height + 6.f || p.x < -6.f || p.x > m_area.width + 6.f;
			if (out) {
				p.x = rnd() * m_area.width;
				p.y = p.vy < 0.f ? m_area.height + 4.f : -4.f;
			}

			if (m_type == 1) {
				m_draw->drawDot({ p.x, p.y }, p.size, starColor(1.f, 1.f, 1.f, 0.7f));
			} else if (m_type == 2) {
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
		dt *= m_timeMul;
		m_draw->clear();
		if (m_type == 0) updateStars(dt);
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

// Tabs, grouped by what they do:
//   0 Control  - record / play / off, speed
//   1 Files    - create, save, load, rename, copy, delete, import / export
//   2 Organize - favourites, tags, folders, sorting
//   3 Tools    - undo, merge, delete a range, window test, auto solver
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
	ScrollLayer* m_list = nullptr;
	std::vector<CCNode*> m_tabNodes;
	float m_listW = 0.f;
	float m_colW = 0.f;
	float m_rightX = 0.f;
	float m_top = 0.f;
	float m_bs = 0.55f; // button scale
	int m_tab = 0;
	bool m_compact = false;

	bool init() {
		auto ws = CCDirector::get()->getWinSize();
		m_compact = S("compact") != 0;
		int sizePct = S("ui-size");
		int shape = S("ui-shape");
		int opacity = S("ui-opacity");
		int bgType = S("bg-type");
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

		auto base = baseColor();
		auto panel = makePanel(shape, base, { pw, ph });
		panel->setOpacity(static_cast<GLubyte>(255 * opacity / 100));
		m_mainLayer->addChildAtPosition(panel, Anchor::Center);
		panel->setZOrder(-10);

		if (S("grad")) {
			auto end = kColors[std::clamp(S("grad-end"), 0, kColorCount - 1)].color;
			auto g = CCLayerGradient::create(ccc4(base.r, base.g, base.b, 255), ccc4(end.r, end.g, end.b, 255));
			g->setContentSize({ pw - 8.f, ph - 8.f });
			g->ignoreAnchorPointForPosition(false);
			g->setAnchorPoint({ 0.5f, 0.5f });
			g->setOpacity(static_cast<GLubyte>(255 * opacity / 100));
			m_mainLayer->addChildAtPosition(g, Anchor::Center);
			g->setZOrder(-9);
		}

		if (bgType < 4) {
			auto field = BgField::create({ pw, ph }, bgType, S("bg-amount"), S("bg-speed"));
			if (field) {
				m_mainLayer->addChildAtPosition(field, Anchor::Center, { -pw / 2.f, -ph / 2.f });
				field->setZOrder(-8);
			}
		}

		m_bs = m_compact ? 0.45f : 0.55f;
		float leftX = -pw * 0.23f;
		m_rightX = pw * 0.25f;
		m_colW = pw * 0.46f;
		m_listW = pw * 0.44f;
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
			"bigFont.fnt", active ? "GJ_button_02.png" : "GJ_button_01.png", 30.f, 0.8f
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
			// Edit
			addRow({
				{ "Undo", menu_selector(MacroPopup::onUndo) },
				{ "Merge", menu_selector(MacroPopup::onMerge) },
				{ "Del range", menu_selector(MacroPopup::onDelRange) },
			}, rowY);
			addRow({
				{ "Test windows", menu_selector(MacroPopup::onTestWindow) },
				{ "Stop test", menu_selector(MacroPopup::onStopTest) },
			}, rowY - step);
			addRow({
				{ "Auto solve", menu_selector(MacroPopup::onSolve) },
				{ "Stop solve", menu_selector(MacroPopup::onStopSolve) },
			}, rowY - step * 2.f);
			tabLabel("Merge: type macro to append. Del range: 100-500", rowY - step * 3.f + 6.f, 0.28f);
			tabLabel("Test windows: finds each click's frame window", rowY - step * 3.f - 8.f, 0.28f);
			tabLabel("Auto solve: finds clicks, replaces loaded macro", rowY - step * 3.f - 22.f, 0.28f);
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
				"bigFont.fnt", sel ? "GJ_button_02.png" : "GJ_button_01.png", 30.f, 0.8f
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
			m_modeLabel->setString(modes[g_mode]);
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
		WinMap loadedWins;
		if (readMacro(g_selected, loadedInputs, loadedAnchors, &loadedWins)) {
			g_inputs = loadedInputs;
			g_anchors = loadedAnchors;
			g_windows = loadedWins;
		}
		if (m_input) m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
	}

	void onRecord(CCObject*) {
		g_selected = nextMacroName();
		g_inputs.clear();
		g_anchors.clear();
		g_windows.clear();
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
		notify("Playing: restart the level");
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
		WinMap loadedWins;
		if (!readMacro(g_selected, loadedInputs, loadedAnchors, &loadedWins)) return notify("Could not load");
		g_inputs = loadedInputs;
		g_anchors = loadedAnchors;
		g_windows = loadedWins;
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
		g_windows.clear();
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
		g_windows.clear();
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
		WinMap wins;
		parseMacro(ss, inputs, anchors, &wins);
		if (inputs.empty()) return notify("Clipboard has no macro");
		g_inputs = inputs;
		g_anchors = anchors;
		g_windows = wins;
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
		for (auto it = g_windows.begin(); it != g_windows.end();) {
			if (it->first >= static_cast<int>(g_inputs.size())) it = g_windows.erase(it);
			else ++it;
		}
		notify("Last input removed. Tap Save to keep it");
	}

	void onMerge(CCObject*) {
		if (g_selected.empty() || g_inputs.empty()) return notify("Select a macro first");
		auto second = cleanName(inputText());
		if (second.empty()) return notify("Type the macro to append");
		if (second == g_selected) return notify("Pick a different macro");
		std::vector<MacroInput> in2;
		std::vector<MacroAnchor> an2;
		if (!readMacro(second, in2, an2)) return notify("Macro not found");

		int offset = 0;
		for (auto& i : g_inputs) offset = std::max(offset, i.frame);
		for (auto& a : g_anchors) offset = std::max(offset, a.frame);
		offset += 1;
		for (auto i : in2) { i.frame += offset; g_inputs.push_back(i); }
		for (auto a : an2) { a.frame += offset; g_anchors.push_back(a); }

		g_windows.clear();
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
		g_windows.clear();
		char buf[48];
		std::snprintf(buf, sizeof(buf), "Removed %d inputs. Tap Save", static_cast<int>(before - g_inputs.size()));
		notify(buf);
	}

	void onSolve(CCObject*) {
		if (g_mode == 4) return notify("Solver already running");
		g_inputs.clear();
		g_anchors.clear();
		g_windows.clear();
		g_sol = Solver();
		g_sol.active = true;
		g_sol.chunk = S("solve-chunk");
		g_sol.maxAttempts = S("solve-tries");
		g_sol.savedSpeed = g_speed;
		g_sol.savedTouched = g_speedTouched;
		g_speed = static_cast<float>(S("solve-speed"));
		applySpeed();
		g_selected = nextMacroName();
		g_cursor = 0;
		g_anchorCursor = 0;
		g_solDeathFrame = -1;
		g_solDeathX = 0.f;
		g_mode = 4;
		notify("Solver: restart the level");
		this->keyBackClicked();
	}

	void onStopSolve(CCObject*) {
		if (g_mode != 4) return notify("Solver is not running");
		solFinish(3);
		refreshLabels();
	}

	void onTestWindow(CCObject*) {
		if (g_inputs.empty()) return notify("Load a macro first");
		g_wt = WinTest();
		for (size_t i = 0; i < g_inputs.size(); i++) {
			if (g_inputs[i].down) g_wt.clicks.push_back(static_cast<int>(i));
		}
		if (g_wt.clicks.empty()) return notify("Macro has no clicks");
		g_windows.clear();
		g_wt.active = true;
		g_wt.phase = 0;
		g_cursor = 0;
		g_anchorCursor = 0;
		g_mode = 3;
		notify("Window test: restart the level");
		this->keyBackClicked();
	}

	void onStopTest(CCObject*) {
		if (g_mode != 3) return notify("No test running");
		g_mode = 0;
		g_wt.active = false;
		g_wt.pendingReset = false;
		g_wt.pendingAdvance = false;
		notify("Window test stopped");
		refreshLabels();
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

public:
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

class SettingsPopup : public Popup {
protected:
	int m_vals[kSettingCount];
	int m_page = 0;
	int m_profile = 1;
	std::vector<CCNode*> m_pageNodes;
	std::vector<CCLabelBMFont*> m_valueLabels;
	CCLabelBMFont* m_pageLabel = nullptr;
	CCLabelBMFont* m_profLabel = nullptr;
	CCScale9Sprite* m_preview = nullptr;

	bool init() {
		if (!Popup::init(350.f, 270.f)) return false;

		for (int i = 0; i < kSettingCount; i++) m_vals[i] = getSetting(i);

		// remembered page: the settings popup re-opens on the page you left it on
		m_page = std::clamp(getInt("set-page", 0), 0, kPageCount - 1);

		// profiles
		auto pPrev = makeButton("<", this, menu_selector(SettingsPopup::onProfPrev), 0.5f);
		m_buttonMenu->addChildAtPosition(pPrev, Anchor::Center, { -150.f, -68.f });
		m_profLabel = CCLabelBMFont::create("", "bigFont.fnt");
		m_profLabel->setScale(0.38f);
		m_mainLayer->addChildAtPosition(m_profLabel, Anchor::Center, { -105.f, -68.f });
		auto pNext = makeButton(">", this, menu_selector(SettingsPopup::onProfNext), 0.5f);
		m_buttonMenu->addChildAtPosition(pNext, Anchor::Center, { -60.f, -68.f });
		auto pSave = makeButton("Save", this, menu_selector(SettingsPopup::onProfSave), 0.55f);
		m_buttonMenu->addChildAtPosition(pSave, Anchor::Center, { -5.f, -68.f });
		auto pLoad = makeButton("Load", this, menu_selector(SettingsPopup::onProfLoad), 0.55f);
		m_buttonMenu->addChildAtPosition(pLoad, Anchor::Center, { 50.f, -68.f });

		// page nav + apply
		auto prev = makeButton("<", this, menu_selector(SettingsPopup::onPagePrev), 0.7f);
		m_buttonMenu->addChildAtPosition(prev, Anchor::Center, { -135.f, -105.f });
		m_pageLabel = CCLabelBMFont::create("", "bigFont.fnt");
		m_pageLabel->setScale(0.4f);
		m_mainLayer->addChildAtPosition(m_pageLabel, Anchor::Center, { -85.f, -105.f });
		auto next = makeButton(">", this, menu_selector(SettingsPopup::onPageNext), 0.7f);
		m_buttonMenu->addChildAtPosition(next, Anchor::Center, { -35.f, -105.f });

		auto apply = makeButton("Apply", this, menu_selector(SettingsPopup::onApply), 0.8f);
		m_buttonMenu->addChildAtPosition(apply, Anchor::Center, { 110.f, -105.f });

		updateProfLabel();
		buildPage();
		return true;
	}

	int val(char const* key) {
		int i = settingIndex(key);
		return i < 0 ? 0 : m_vals[i];
	}

	std::string valueText(int idx) {
		auto& d = kSettings[idx];
		int v = m_vals[idx];
		if (d.names) return d.names[v - d.min];
		char buf[32];
		if (d.div > 1) std::snprintf(buf, sizeof(buf), "%.1f", static_cast<float>(v) / d.div);
		else std::snprintf(buf, sizeof(buf), "%d%s", v, d.unit);
		return buf;
	}

	void setValueLabel(CCLabelBMFont* label, int idx) {
		label->setString(valueText(idx).c_str());
		fitLabel(label, 100.f, 0.45f);
	}

	void updateProfLabel() {
		char buf[24];
		std::snprintf(buf, sizeof(buf), "Profile %d", m_profile);
		m_profLabel->setString(buf);
	}

	void buildPage() {
		for (auto n : m_pageNodes) n->removeFromParent();
		m_pageNodes.clear();
		m_valueLabels.clear();

		char buf[64];
		std::snprintf(buf, sizeof(buf), "Settings: %s", kPageTitles[m_page]);
		this->setTitle(buf);
		std::snprintf(buf, sizeof(buf), "%d/%d", m_page + 1, kPageCount);
		m_pageLabel->setString(buf);

		for (int r = 0; r < kRowsPerPage; r++) {
			int idx = m_page * kRowsPerPage + r;
			if (idx >= kSettingCount) break;
			float y = 72.f - r * 36.f;

			auto title = CCLabelBMFont::create(kSettings[idx].label, "bigFont.fnt");
			title->setAnchorPoint({ 0.f, 0.5f });
			fitLabel(title, 100.f, 0.4f);
			m_mainLayer->addChildAtPosition(title, Anchor::Center, { -160.f, y });
			m_pageNodes.push_back(title);

			auto value = CCLabelBMFont::create("", "bigFont.fnt");
			value->setScale(0.45f);
			m_mainLayer->addChildAtPosition(value, Anchor::Center, { 10.f, y });
			m_pageNodes.push_back(value);
			m_valueLabels.push_back(value);

			auto minus = makeButton("<", this, menu_selector(SettingsPopup::onStep), 0.6f);
			minus->setTag(idx * 2);
			m_buttonMenu->addChildAtPosition(minus, Anchor::Center, { -55.f, y });
			m_pageNodes.push_back(minus);

			auto plus = makeButton(">", this, menu_selector(SettingsPopup::onStep), 0.6f);
			plus->setTag(idx * 2 + 1);
			m_buttonMenu->addChildAtPosition(plus, Anchor::Center, { 75.f, y });
			m_pageNodes.push_back(plus);

			setValueLabel(value, idx);
		}
		refreshPreview();
	}

	void refreshPreview() {
		if (m_preview) {
			m_preview->removeFromParent();
			m_preview = nullptr;
		}
		if (m_page > 2) return;
		ccColor3B c;
		int ci = val("ui-color");
		if (ci >= kColorCount) {
			c = { static_cast<GLubyte>(val("col-r")), static_cast<GLubyte>(val("col-g")), static_cast<GLubyte>(val("col-b")) };
		} else {
			c = kColors[ci].color;
		}
		m_preview = makePanel(val("ui-shape"), c, { 60.f, 46.f });
		m_preview->setOpacity(static_cast<GLubyte>(255 * val("ui-opacity") / 100));
		m_mainLayer->addChildAtPosition(m_preview, Anchor::Center, { 135.f, 20.f });
	}

	void applyTheme(int t) {
		if (t <= 0 || t > 4) return;
		for (auto p = kThemes[t]; p->key; p++) {
			int i = settingIndex(p->key);
			if (i >= 0) m_vals[i] = p->val;
		}
	}

	void onStep(CCObject* sender) {
		int tag = static_cast<CCNode*>(sender)->getTag();
		int idx = tag / 2;
		int dir = (tag % 2 == 0) ? -1 : 1;
		auto& d = kSettings[idx];
		int& v = m_vals[idx];

		if (d.names) {
			int n = d.max - d.min + 1;
			v = d.min + ((v - d.min + dir + n) % n);
		} else {
			v = std::clamp(v + dir * d.step, d.min, d.max);
		}

		if (std::strcmp(d.key, "ui-theme") == 0) {
			applyTheme(v);
			buildPage();
			return;
		}

		int row = idx - m_page * kRowsPerPage;
		if (row >= 0 && row < static_cast<int>(m_valueLabels.size())) {
			setValueLabel(m_valueLabels[row], idx);
		}
		refreshPreview();
	}

	void onPagePrev(CCObject*) {
		m_page = (m_page + kPageCount - 1) % kPageCount;
		setInt("set-page", m_page);
		buildPage();
	}

	void onPageNext(CCObject*) {
		m_page = (m_page + 1) % kPageCount;
		setInt("set-page", m_page);
		buildPage();
	}

	void onProfPrev(CCObject*) {
		m_profile = m_profile == 1 ? 3 : m_profile - 1;
		updateProfLabel();
	}

	void onProfNext(CCObject*) {
		m_profile = m_profile == 3 ? 1 : m_profile + 1;
		updateProfLabel();
	}

	void onProfSave(CCObject*) {
		std::string s;
		for (int i = 0; i < kSettingCount; i++) s += std::to_string(m_vals[i]) + ",";
		Mod::get()->setSavedValue<std::string>("profile-" + std::to_string(m_profile), s);
		Notification::create("Profile saved")->show();
	}

	void onProfLoad(CCObject*) {
		auto s = Mod::get()->getSavedValue<std::string>("profile-" + std::to_string(m_profile), "");
		std::vector<int> vals;
		std::istringstream in(s);
		std::string tok;
		while (std::getline(in, tok, ',')) {
			if (!tok.empty()) vals.push_back(std::atoi(tok.c_str()));
		}
		if (vals.empty() || static_cast<int>(vals.size()) > kSettingCount) {
			Notification::create("Profile is empty")->show();
			return;
		}
		for (int i = 0; i < kSettingCount; i++) {
			int v = i < static_cast<int>(vals.size()) ? vals[i] : kSettings[i].def;
			m_vals[i] = std::clamp(v, kSettings[i].min, kSettings[i].max);
		}
		buildPage();
		Notification::create("Profile loaded. Tap Apply")->show();
	}

	void onApply(CCObject*) {
		for (int i = 0; i < kSettingCount; i++) setInt(kSettings[i].key, m_vals[i]);

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

// ---------- hooks ----------

class $modify(GJBaseGameLayer) {
	void update(float dt) {
		GJBaseGameLayer::update(dt);
		// keep user speed alive through deaths/restarts/time-warp resets
		enforceSpeed(m_gameState.m_timeWarp);
		updateHud();
		updateWindowHud();
		updatePath();
		// window test: restart the level right after an attempt is decided
		if (g_wt.pendingReset) {
			g_wt.pendingReset = false;
			if (auto pl = PlayLayer::get()) pl->resetLevel();
		}
		if (g_sol.pendingReset) {
			g_sol.pendingReset = false;
			if (auto pl = PlayLayer::get()) pl->resetLevel();
		}
	}

	// ----- frame window counter -----
	// Shows the fixed window (in frames) of the next click while playing.
	void updateWindowHud() {
		if (!m_uiLayer || !PlayLayer::get()) return;
		auto label = static_cast<CCLabelBMFont*>(m_uiLayer->getChildByID("macro-wnd"));
		std::string text;
		char buf[96];
		if (g_mode == 3 && g_wt.active) {
			if (g_wt.phase == 0) {
				std::snprintf(buf, sizeof(buf), "Window test: checking macro");
			} else {
				std::snprintf(buf, sizeof(buf), "Testing click %d/%d  shift %c%d",
					static_cast<int>(g_wt.ci) + 1, static_cast<int>(g_wt.clicks.size()),
					g_wt.phase == 1 ? '-' : '+', g_wt.s);
			}
			text = buf;
		} else if (g_mode == 4 && g_sol.active) {
			std::snprintf(buf, sizeof(buf), "Solving  frame %d  tries %d  option %d/%d",
				g_sol.frame, g_sol.attempts, g_sol.k + 1, kSolCands);
			text = buf;
		} else if (g_mode == 2 && S("win-hud") && !g_windows.empty()) {
			for (size_t i = g_cursor; i < g_inputs.size(); i++) {
				if (!g_inputs[i].down) continue;
				auto it = g_windows.find(static_cast<int>(i));
				if (it != g_windows.end()) {
					int e = it->second.first;
					int l = it->second.second;
					std::snprintf(buf, sizeof(buf), "Window: %d  (-%d / +%d)", e + l + 1, e, l);
					text = buf;
				}
				break;
			}
		}
		if (text.empty()) {
			if (label) label->setVisible(false);
			return;
		}
		if (!label) {
			label = CCLabelBMFont::create("", "bigFont.fnt");
			label->setID("macro-wnd");
			label->setOpacity(225);
			m_uiLayer->addChild(label, 151);
		}
		label->setVisible(true);
		if (text != std::string(label->getString())) label->setString(text.c_str());
		label->setScale(S("win-size") / 100.f * 0.8f);
		auto ws = CCDirector::get()->getWinSize();
		label->setPosition({ ws.width / 2.f, ws.height - 50.f });
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

		std::string text = g_mode == 1 ? "REC" : (g_mode == 2 ? "PLAY" : (g_mode == 3 ? "TEST" : (g_mode == 4 ? "SOLVE" : "IDLE")));
		if (S("hud-frame")) text += "\nFrame " + std::to_string(g_frame);
		if (S("hud-corr") && g_mode == 2) text += "\nFixes " + std::to_string(g_corrections);
		if (S("counter") && g_mode == 1) {
			int clicks = 0, releases = 0;
			for (auto& i : g_inputs) {
				if (i.down) clicks++;
				else releases++;
			}
			text += "\nClicks " + std::to_string(clicks) + " / " + std::to_string(releases);
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
		if ((g_mode == 2 || g_mode == 3 || g_mode == 4) && !g_injecting) return;
		if (g_mode == 1) {
			MacroInput in;
			in.frame = static_cast<int>(m_gameState.m_currentProgress);
			in.button = static_cast<int>(button);
			in.down = down;
			in.p1 = p1;
			g_inputs.push_back(in);
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

	// solver: remember where the player is on every frame of the chunk
	void recordSolAnchor(int frame) {
		if (!m_player1 || frame == g_lastAnchorFrame) return;
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
		g_sol.tmpAn.push_back(a);
	}

	// Position only: velocity is left alone so clicks keep working.
	void alignPlayer(PlayerObject* p, float x, float y, float v) {
		if (!p) return;
		if (S("exact-route") || g_mode == 4) {
			// exact route: put the player on the recorded spot every frame
			float ex = p->getPositionX() - x;
			float ey = p->getPositionY() - y;
			if (std::fabs(ex) > 0.5f || std::fabs(ey) > 0.5f) g_corrections++;
			p->setPosition({ x, y });
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
			alignPlayer(m_player1, a.x1, a.y1, a.v1);
			if (a.dual && m_gameState.m_isDualMode) alignPlayer(m_player2, a.x2, a.y2, a.v2);
		}
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		int frame = static_cast<int>(m_gameState.m_currentProgress);
		g_frame = frame;
		if (g_mode == 1) {
			recordAnchor(frame);
		}
		if (g_mode == 2 || g_mode == 3 || g_mode == 4) {
			// anchors are off while testing: the test needs the real physics
			if (g_mode == 2) applyAnchors(frame);
			// solver: replay the proven route exactly, then explore past its end
			if (g_mode == 4 && frame < g_sol.frame) applyAnchors(frame);
			auto& src = (g_mode == 3 || g_mode == 4) ? g_work : g_inputs;
			while (g_cursor < src.size() && src[g_cursor].frame <= frame) {
				auto i = src[g_cursor++];
				g_injecting = true;
				GJBaseGameLayer::handleButton(i.down, i.button, i.p1);
				g_injecting = false;
			}
			if (g_mode == 3 && g_wt.active && g_wt.phase != 0 && !g_wt.resolved && frame >= g_wt.endFrame) {
				wtResolve(true);
			}
			if (g_mode == 4 && g_sol.active && !g_sol.resolved) {
				if (frame >= g_sol.frame + g_sol.chunk) solResolve(1);
				else if (frame >= g_sol.frame) recordSolAnchor(frame);
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}
};

class $modify(PlayLayer) {
	void resetLevel() {
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
				for (auto it = g_windows.begin(); it != g_windows.end();) {
					if (it->first >= static_cast<int>(g_inputs.size())) it = g_windows.erase(it);
					else ++it;
				}
			} else {
				g_inputs.clear();
				g_anchors.clear();
				g_windows.clear();
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
		enforceSpeed(m_gameState.m_timeWarp);

		if (g_mode == 3 && g_wt.active) {
			if (g_wt.pendingAdvance) {
				g_wt.pendingAdvance = false;
				int r = wtAdvance();
				if (r != 1) {
					wtFinish(r < 0);
					return;
				}
			}
			wtBuildWork();
			g_wt.resolved = false;
		}

		if (g_mode == 4 && g_sol.active) {
			if (g_sol.pendingAdvance) {
				g_sol.pendingAdvance = false;
				int r = solAdvance();
				if (r != 1) {
					solFinish(r);
					return;
				}
			}
			solBuildWork();
			g_sol.resolved = false;
		}
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		PlayLayer::destroyPlayer(player, object);
		if (g_mode == 3 && g_wt.active && !g_wt.resolved) {
			this->stopActionByTag(0x10); // cancel GD's slow delayed restart
			wtResolve(false);
		}
		if (g_mode == 4 && g_sol.active && !g_sol.resolved) {
			this->stopActionByTag(0x10);
			// remember where the death happened (shown if the solver gives up)
			g_solDeathFrame = g_frame;
			g_solDeathX = m_player1 ? m_player1->getPositionX() : 0.f;
			// dying inside the replayed part of the route means we lost sync
			solResolve(g_frame < g_sol.frame ? 2 : 0);
		}
	}

	void levelComplete() {
		if (g_mode == 3 && g_wt.active) {
			wtResolve(true);
			return;
		}
		if (g_mode == 4 && g_sol.active) {
			if (!g_sol.resolved || g_sol.result == 1) solCommit();
			solFinish(0);
			PlayLayer::levelComplete();
			return;
		}
		if (g_mode == 1 && !g_inputs.empty()) {
			if (g_selected.empty()) g_selected = nextMacroName();
			int clicks = 0, releases = 0;
			for (auto& i : g_inputs) {
				if (i.down) clicks++;
				else releases++;
			}
			if (writeMacro(g_selected)) {
				showSavedBox("Macro saved (" + std::to_string(clicks) + " clicks / " + std::to_string(releases) + " releases)");
			}
			g_mode = 0;
		} else if (g_mode == 2) {
			if (S("loop")) {
				g_cursor = 0;
				g_anchorCursor = 0;
				g_corrections = 0;
				this->resetLevel();
				return;
			}
			if (g_corrections > 0) {
				showSavedBox("Playback done (" + std::to_string(g_corrections) + " corrections)");
			}
			g_mode = 0;
		}
		PlayLayer::levelComplete();
	}

	void onQuit() {
		if (g_mode == 3) {
			g_mode = 0;
			g_wt.active = false;
			g_wt.pendingReset = false;
			g_wt.pendingAdvance = false;
		}
		if (g_mode == 4) {
			g_mode = 0;
			g_sol.active = false;
			g_sol.pendingReset = false;
			g_sol.pendingAdvance = false;
			solRestoreSpeed();
		}
		if (!S("keep-speed")) resetSpeed();
		g_pathKey = static_cast<size_t>(-1);
		PlayLayer::onQuit();
	}
};

class $modify(MacroPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		auto ws = CCDirector::get()->getWinSize();
		int corner = std::clamp(S("pause-corner"), 0, 3);
		float x = (corner == 1 || corner == 3) ? 60.f : ws.width - 60.f;
		float y = (corner >= 2) ? ws.height - 40.f : 40.f;

		auto menu = CCMenu::create();
		auto btn = makeButton("Macro", this, menu_selector(MacroPause::onOpenMacro), 1.f);
		menu->addChild(btn);
		menu->setPosition(x, y);
		this->addChild(menu);
	}

	void onOpenMacro(CCObject*) {
		if (auto popup = MacroPopup::create()) popup->show();
	}
};
