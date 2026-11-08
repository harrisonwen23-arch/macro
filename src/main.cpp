#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

using namespace geode::prelude;

struct MacroInput { int frame; int button; bool down; bool p1; };

// A saved snapshot of the player(s) used to re-align during playback.
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
static bool g_injecting = false;
static float g_speed = 1.f;
static bool g_speedTouched = false; // true once the user changed speed
static std::string g_selected;

static constexpr float kSpeedMin = 0.1f;
static constexpr float kSpeedMax = 5.f;
static constexpr int kAnchorEvery = 20;     // frames between anchors
static constexpr float kAnchorTol = 0.25f;  // units of drift before correcting

// ---------- theme settings ----------

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

struct ShapeDef { char const* name; char const* tex; };
static const ShapeDef kShapes[] = {
	{ "Rounded", "square02b_001.png" },
	{ "Soft", "square02_001.png" },
	{ "Classic", "GJ_square01.png" },
	{ "Dark", "GJ_square05.png" },
};
static const int kShapeCount = 4;

static int getInt(char const* key, int def) {
	return Mod::get()->getSavedValue<int>(key, def);
}

static void setInt(char const* key, int v) {
	Mod::get()->setSavedValue<int>(key, v);
}

static CCScale9Sprite* makePanel(int shape, int color, CCSize size) {
	auto panel = CCScale9Sprite::create(kShapes[shape].tex, { 0.f, 0.f, 80.f, 80.f });
	if (!panel) panel = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
	panel->setColor(kColors[color].color);
	panel->setContentSize(size);
	return panel;
}

static CCMenuItemSpriteExtra* makeButton(char const* text, CCObject* target, SEL_MenuHandler cb, float scale) {
	auto spr = ButtonSprite::create(text);
	spr->setScale(scale);
	return CCMenuItemSpriteExtra::create(spr, target, cb);
}

// ---------- speed ----------

// GD itself resets the scheduler time scale when a level restarts, which is
// why speed used to vanish after dying. We re-apply it every frame,
// multiplied by the level's own time warp so speed triggers still work.
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

// File format (one item per line):
//   <frame> <button> <down 0/1> <p1 0/1>                      -> input
//   A <frame> <x1> <y1> <v1> <dual 0/1> <x2> <y2> <v2>        -> anchor
static bool writeMacro(std::string const& name) {
	std::ofstream out(macroFile(name));
	if (!out) return false;
	out << std::setprecision(9);
	for (auto& i : g_inputs) {
		out << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0) << '\n';
	}
	for (auto& a : g_anchors) {
		out << "A " << a.frame << ' ' << a.x1 << ' ' << a.y1 << ' ' << a.v1 << ' '
			<< (a.dual ? 1 : 0) << ' ' << a.x2 << ' ' << a.y2 << ' ' << a.v2 << '\n';
	}
	return true;
}

static bool readMacro(std::string const& name, std::vector<MacroInput>& inputs, std::vector<MacroAnchor>& anchors) {
	std::ifstream in(macroFile(name));
	if (!in) return false;
	inputs.clear();
	anchors.clear();
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty()) continue;
		if (line[0] == 'A') {
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
	return true;
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

// Names a new macro after the level being played. First one is "Level",
// then "Level 1", "Level 2", ... Falls back to "Macro" with no level.
static std::string nextMacroName() {
	std::string base = "Macro";
	if (auto pl = PlayLayer::get()) {
		if (pl->m_level) {
			auto cleaned = cleanName(std::string(pl->m_level->m_levelName));
			if (!cleaned.empty()) base = cleaned;
		}
	}
	if (!std::filesystem::exists(macroFile(base))) return base;
	for (int n = 1; n < 10000; n++) {
		auto name = base + " " + std::to_string(n);
		if (!std::filesystem::exists(macroFile(name))) return name;
	}
	return base + " " + std::to_string(10000);
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
	bg->setOpacity(153); // 60%
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

// ---------- shooting star background ----------

static ccColor4F starColor(float r, float g, float b, float a) {
	return ccc4f(r * a, g * a, b * a, a);
}

class StarField : public CCNode {
protected:
	struct Shoot { float x0, y0, dx, dy, len, t, speed; };
	struct Twinkle { float x, y, phase, speed, size; };

	CCDrawNode* m_draw = nullptr;
	CCSize m_area;
	std::vector<Shoot> m_shoots;
	std::vector<Twinkle> m_twinkles;

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

	bool init(CCSize area) {
		if (!CCNode::init()) return false;
		m_area = area;
		m_draw = CCDrawNode::create();
		this->addChild(m_draw);

		for (int i = 0; i < 40; i++) {
			m_twinkles.push_back({
				rnd() * area.width, rnd() * area.height,
				rnd() * 6.28f, 1.f + rnd() * 2.f, 0.5f + rnd() * 1.1f
			});
		}
		for (int i = 0; i < 4; i++) {
			Shoot s{};
			respawn(s);
			s.t -= static_cast<float>(i) * 0.6f;
			m_shoots.push_back(s);
		}
		this->scheduleUpdate();
		return true;
	}

	void update(float dt) override {
		m_draw->clear();

		for (auto& tw : m_twinkles) {
			tw.phase += dt * tw.speed;
			float a = 0.25f + 0.55f * (0.5f + 0.5f * std::sin(tw.phase));
			m_draw->drawDot({ tw.x, tw.y }, tw.size, starColor(0.8f, 0.9f, 1.f, a));
		}

		for (auto& s : m_shoots) {
			s.t += dt * s.speed;
			if (s.t >= 1.f) {
				respawn(s);
				continue;
			}
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

public:
	static StarField* create(CCSize area) {
		auto ret = new StarField();
		if (ret->init(area)) {
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

class MacroPopup : public Popup {
protected:
	CCLabelBMFont* m_modeLabel = nullptr;
	CCLabelBMFont* m_selLabel = nullptr;
	CCLabelBMFont* m_speedLabel = nullptr;
	Slider* m_speedSlider = nullptr;
	TextInput* m_input = nullptr;
	ScrollLayer* m_list = nullptr;
	float m_listW = 0.f;

	bool init() {
		auto ws = CCDirector::get()->getWinSize();
		int sizePct = std::clamp(getInt("ui-size", 90), 85, 100);
		int shape = std::clamp(getInt("ui-shape", 0), 0, kShapeCount - 1);
		int color = std::clamp(getInt("ui-color", 0), 0, kColorCount - 1);

		float pw = ws.width * sizePct / 100.f;
		float ph = ws.height * sizePct / 100.f;
		if (!Popup::init(pw, ph)) return false;

		m_bgSprite->setVisible(false);
		this->setTitle("Macro Manager");

		auto panel = makePanel(shape, color, { pw, ph });
		m_mainLayer->addChildAtPosition(panel, Anchor::Center);
		panel->setZOrder(-10);

		auto stars = StarField::create({ pw, ph });
		m_mainLayer->addChildAtPosition(stars, Anchor::Center, { -pw / 2.f, -ph / 2.f });
		stars->setZOrder(-9);

		float leftX = -pw * 0.23f;
		float rightX = pw * 0.25f;
		m_listW = pw * 0.44f;
		float listH = ph - 80.f;
		float listY = -12.f;
		float top = ph / 2.f;

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

		float y = top - 50.f;
		m_modeLabel = addLabel("", rightX, y, 0.65f);

		y -= 32.f;
		addBtn("Record", menu_selector(MacroPopup::onRecord), rightX - 55.f, y, 0.7f);
		addBtn("Play", menu_selector(MacroPopup::onPlay), rightX + 21.f, y, 0.7f);
		addBtn("Off", menu_selector(MacroPopup::onOff), rightX + 81.f, y, 0.7f);

		y -= 32.f;
		m_selLabel = addLabel("", rightX, y, 0.5f);

		y -= 34.f;
		m_input = TextInput::create(130.f, "New name");
		m_input->setMaxCharCount(24);
		m_mainLayer->addChildAtPosition(m_input, Anchor::Center, { rightX - 45.f, y });
		addBtn("Rename", menu_selector(MacroPopup::onRename), rightX + 72.f, y, 0.7f);

		y -= 32.f;
		addBtn("Save", menu_selector(MacroPopup::onSave), rightX - 88.f, y, 0.62f);
		addBtn("Load", menu_selector(MacroPopup::onLoad), rightX - 33.f, y, 0.62f);
		addBtn("Delete", menu_selector(MacroPopup::onDelete), rightX + 30.f, y, 0.62f);
		addBtn("New", menu_selector(MacroPopup::onNew), rightX + 91.f, y, 0.62f);

		// speed label + slider
		y -= 32.f;
		m_speedLabel = addLabel("", rightX, y, 0.6f);

		y -= 24.f;
		m_speedSlider = Slider::create(this, menu_selector(MacroPopup::onSpeedSlider), 0.6f);
		m_speedSlider->setValue(speedToSlider(g_speed));
		m_mainLayer->addChildAtPosition(m_speedSlider, Anchor::Center, { rightX, y });

		// settings (gear) button, top-right of the panel
		auto gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn02_001.png");
		if (gear) {
			gear->setScale(0.7f);
			auto gearBtn = CCMenuItemSpriteExtra::create(gear, this, menu_selector(MacroPopup::onCustomize));
			m_buttonMenu->addChildAtPosition(gearBtn, Anchor::TopRight, { -26.f, -26.f });
		}

		rebuildList();
		refreshLabels();
		g_manager = this;
		return true;
	}

	static float speedToSlider(float s) {
		return std::clamp((s - kSpeedMin) / (kSpeedMax - kSpeedMin), 0.f, 1.f);
	}

	static float sliderToSpeed(float v) {
		float s = kSpeedMin + std::clamp(v, 0.f, 1.f) * (kSpeedMax - kSpeedMin);
		s = std::round(s * 20.f) / 20.f; // snap to 0.05
		return std::clamp(s, kSpeedMin, kSpeedMax);
	}

	CCLabelBMFont* addLabel(char const* text, float x, float y, float scale) {
		auto label = CCLabelBMFont::create(text, "bigFont.fnt");
		label->setScale(scale);
		m_mainLayer->addChildAtPosition(label, Anchor::Center, { x, y });
		return label;
	}

	CCMenuItemSpriteExtra* addBtn(char const* text, SEL_MenuHandler cb, float x, float y, float scale) {
		auto btn = makeButton(text, this, cb, scale);
		m_buttonMenu->addChildAtPosition(btn, Anchor::Center, { x, y });
		return btn;
	}

	void rebuildList() {
		auto content = m_list->m_contentLayer;
		content->removeAllChildrenWithCleanup(true);

		auto names = listMacros();
		if (names.empty()) {
			auto label = CCLabelBMFont::create("No macros yet. Tap Record.", "bigFont.fnt");
			label->setScale(0.5f);
			content->addChild(label);
		}
		for (auto& name : names) {
			auto row = CCMenu::create();
			row->ignoreAnchorPointForPosition(false);
			row->setAnchorPoint({ 0.5f, 0.5f });
			row->setContentSize({ m_listW, 34.f });

			bool sel = (name == g_selected);
			float scale = 0.7f;
			auto spr = ButtonSprite::create(
				name.c_str(), static_cast<int>((m_listW - 24.f) / scale), true,
				"bigFont.fnt", sel ? "GJ_button_02.png" : "GJ_button_01.png", 30.f, 0.8f
			);
			spr->setScale(scale);
			auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroPopup::onPick));
			btn->setID(name);
			row->addChildAtPosition(btn, Anchor::Center);
			content->addChild(row);
		}
		content->updateLayout();
		m_list->scrollToTop();
	}

	void refreshLabels() {
		char const* modes[] = { "Mode: Off", "Mode: Recording", "Mode: Playing" };
		m_modeLabel->setString(modes[g_mode]);

		std::string shown = g_selected;
		if (shown.size() > 18) shown = shown.substr(0, 17) + "...";
		std::string sel = g_selected.empty() ? "Selected: none" : "Selected: " + shown;
		m_selLabel->setString(sel.c_str());

		char buf[32];
		std::snprintf(buf, sizeof(buf), "Speed %.2fx", g_speed);
		m_speedLabel->setString(buf);
	}

	void notify(char const* text) {
		Notification::create(text)->show();
	}

	void onPick(CCObject* sender) {
		g_selected = static_cast<CCNode*>(sender)->getID();
		std::vector<MacroInput> loadedInputs;
		std::vector<MacroAnchor> loadedAnchors;
		if (readMacro(g_selected, loadedInputs, loadedAnchors)) {
			g_inputs = loadedInputs;
			g_anchors = loadedAnchors;
		}
		m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
	}

	void onRecord(CCObject*) {
		g_selected = nextMacroName();
		g_inputs.clear();
		g_anchors.clear();
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

	void onDelete(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		std::error_code ec;
		std::filesystem::remove(macroFile(g_selected), ec);
		g_selected.clear();
		g_inputs.clear();
		g_anchors.clear();
		rebuildList();
		refreshLabels();
		notify("Macro deleted");
	}

	void onNew(CCObject*) {
		g_inputs.clear();
		g_anchors.clear();
		g_selected = nextMacroName();
		writeMacro(g_selected);
		m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
	}

	void onRename(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		auto newName = cleanName(m_input->getString());
		if (newName.empty()) return notify("Type a valid name first");
		if (newName == g_selected) return;
		if (std::filesystem::exists(macroFile(newName))) return notify("That name is taken");
		std::error_code ec;
		std::filesystem::rename(macroFile(g_selected), macroFile(newName), ec);
		if (ec) return notify("Rename failed");
		g_selected = newName;
		rebuildList();
		refreshLabels();
		notify("Renamed");
	}

	void onSpeedSlider(CCObject* sender) {
		auto thumb = static_cast<SliderThumb*>(sender);
		g_speed = sliderToSpeed(thumb->getValue());
		applySpeed();
		refreshLabels();
	}

	void onCustomize(CCObject*);

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

// ---------- customizer popup ----------

class CustomizePopup : public Popup {
protected:
	int m_size = 90;
	int m_shape = 0;
	int m_color = 0;
	CCLabelBMFont* m_sizeLabel = nullptr;
	CCLabelBMFont* m_shapeLabel = nullptr;
	CCLabelBMFont* m_colorLabel = nullptr;
	CCScale9Sprite* m_preview = nullptr;

	bool init() {
		if (!Popup::init(310.f, 220.f)) return false;
		this->setTitle("Customize UI");

		m_size = std::clamp(getInt("ui-size", 90), 85, 100);
		m_shape = std::clamp(getInt("ui-shape", 0), 0, kShapeCount - 1);
		m_color = std::clamp(getInt("ui-color", 0), 0, kColorCount - 1);

		m_sizeLabel = addRow("Size", 45.f, menu_selector(CustomizePopup::onSizeDown), menu_selector(CustomizePopup::onSizeUp));
		m_shapeLabel = addRow("Shape", 10.f, menu_selector(CustomizePopup::onShapePrev), menu_selector(CustomizePopup::onShapeNext));
		m_colorLabel = addRow("Colour", -25.f, menu_selector(CustomizePopup::onColorPrev), menu_selector(CustomizePopup::onColorNext));

		auto apply = makeButton("Apply", this, menu_selector(CustomizePopup::onApply), 0.8f);
		m_buttonMenu->addChildAtPosition(apply, Anchor::Center, { 0.f, -72.f });

		refresh();
		return true;
	}

	CCLabelBMFont* addRow(char const* name, float y, SEL_MenuHandler minus, SEL_MenuHandler plus) {
		auto title = CCLabelBMFont::create(name, "bigFont.fnt");
		title->setScale(0.45f);
		m_mainLayer->addChildAtPosition(title, Anchor::Center, { -115.f, y });

		auto value = CCLabelBMFont::create("", "bigFont.fnt");
		value->setScale(0.5f);
		m_mainLayer->addChildAtPosition(value, Anchor::Center, { 0.f, y });

		auto minusBtn = makeButton("<", this, minus, 0.7f);
		m_buttonMenu->addChildAtPosition(minusBtn, Anchor::Center, { -55.f, y });
		auto plusBtn = makeButton(">", this, plus, 0.7f);
		m_buttonMenu->addChildAtPosition(plusBtn, Anchor::Center, { 55.f, y });
		return value;
	}

	void refresh() {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%d%%", m_size);
		m_sizeLabel->setString(buf);
		m_shapeLabel->setString(kShapes[m_shape].name);
		m_colorLabel->setString(kColors[m_color].name);

		if (m_preview) m_preview->removeFromParent();
		m_preview = makePanel(m_shape, m_color, { 70.f, 52.f });
		m_mainLayer->addChildAtPosition(m_preview, Anchor::Center, { 112.f, 10.f });
	}

	void onSizeDown(CCObject*) { m_size = std::max(85, m_size - 5); refresh(); }
	void onSizeUp(CCObject*) { m_size = std::min(100, m_size + 5); refresh(); }
	void onShapePrev(CCObject*) { m_shape = (m_shape + kShapeCount - 1) % kShapeCount; refresh(); }
	void onShapeNext(CCObject*) { m_shape = (m_shape + 1) % kShapeCount; refresh(); }
	void onColorPrev(CCObject*) { m_color = (m_color + kColorCount - 1) % kColorCount; refresh(); }
	void onColorNext(CCObject*) { m_color = (m_color + 1) % kColorCount; refresh(); }

	void onApply(CCObject*) {
		setInt("ui-size", m_size);
		setInt("ui-shape", m_shape);
		setInt("ui-color", m_color);

		auto old = g_manager;
		if (old) old->closeManager();
		if (auto popup = MacroPopup::create()) popup->show();
		this->keyBackClicked();
	}

public:
	static CustomizePopup* create() {
		auto ret = new CustomizePopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

void MacroPopup::onCustomize(CCObject*) {
	if (auto popup = CustomizePopup::create()) popup->show();
}

// ---------- hooks ----------

class $modify(GJBaseGameLayer) {
	void update(float dt) {
		GJBaseGameLayer::update(dt);
		// keep user speed alive through deaths/restarts/time-warp resets
		enforceSpeed(m_gameState.m_timeWarp);
	}

	void handleButton(bool down, int button, bool p1) {
		if (g_mode == 2 && !g_injecting) return;
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

	// Save a snapshot of the player(s) every kAnchorEvery frames.
	void recordAnchor(int frame) {
		if (!m_player1) return;
		if (frame % kAnchorEvery != 0 || frame == g_lastAnchorFrame) return;
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

	// Snap a player back to the anchor if it has drifted too far.
	void alignPlayer(PlayerObject* p, float x, float y, float v) {
		if (!p) return;
		float dx = p->getPositionX() - x;
		float dy = p->getPositionY() - y;
		if (std::fabs(dx) > kAnchorTol || std::fabs(dy) > kAnchorTol) {
			p->setPosition({ x, y });
			p->m_yVelocity = v;
			g_corrections++;
		}
	}

	void applyAnchors(int frame) {
		while (g_anchorCursor < g_anchors.size() && g_anchors[g_anchorCursor].frame <= frame) {
			auto& a = g_anchors[g_anchorCursor++];
			if (a.frame != frame) continue; // missed this frame, too late to align
			alignPlayer(m_player1, a.x1, a.y1, a.v1);
			if (a.dual && m_gameState.m_isDualMode) alignPlayer(m_player2, a.x2, a.y2, a.v2);
		}
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		int frame = static_cast<int>(m_gameState.m_currentProgress);
		if (g_mode == 1) {
			recordAnchor(frame);
		}
		if (g_mode == 2) {
			applyAnchors(frame);
			while (g_cursor < g_inputs.size() && g_inputs[g_cursor].frame <= frame) {
				auto& i = g_inputs[g_cursor++];
				g_injecting = true;
				GJBaseGameLayer::handleButton(i.down, i.button, i.p1);
				g_injecting = false;
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}
};

class $modify(PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		if (g_mode == 1) {
			g_inputs.clear();
			g_anchors.clear();
		}
		g_cursor = 0;
		g_anchorCursor = 0;
		g_lastAnchorFrame = -1;
		g_corrections = 0;
		// resetLevel clears the scheduler time scale; put our speed back
		enforceSpeed(m_gameState.m_timeWarp);
	}

	void levelComplete() {
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
			if (g_corrections > 0) {
				showSavedBox("Playback done (" + std::to_string(g_corrections) + " corrections)");
			}
			g_mode = 0;
		}
		PlayLayer::levelComplete();
	}

	void onQuit() {
		resetSpeed();
		PlayLayer::onQuit();
	}
};

class $modify(MacroPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		auto menu = CCMenu::create();
		auto btn = makeButton("Macro", this, menu_selector(MacroPause::onOpenMacro), 1.f);
		menu->addChild(btn);
		menu->setPosition(CCDirector::get()->getWinSize().width - 60.f, 40.f);
		this->addChild(menu);
	}

	void onOpenMacro(CCObject*) {
		if (auto popup = MacroPopup::create()) popup->show();
	}
};
