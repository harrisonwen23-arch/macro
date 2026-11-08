#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

using namespace geode::prelude;

struct MacroInput { int frame; int button; bool down; bool p1; };

static int g_mode = 0; // 0 = off, 1 = record, 2 = play
static std::vector<MacroInput> g_inputs;
static size_t g_cursor = 0;
static bool g_injecting = false;
static float g_speed = 1.f;
static std::string g_selected;

static void applySpeed() {
	CCDirector::get()->getScheduler()->setTimeScale(g_speed);
}

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

static bool writeMacro(std::string const& name) {
	std::ofstream out(macroFile(name));
	if (!out) return false;
	for (auto& i : g_inputs) {
		out << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0) << '\n';
	}
	return true;
}

static bool readMacro(std::string const& name, std::vector<MacroInput>& out) {
	std::ifstream in(macroFile(name));
	if (!in) return false;
	out.clear();
	int frame, button, down, p1;
	while (in >> frame >> button >> down >> p1) {
		out.push_back({ frame, button, down != 0, p1 != 0 });
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

class MacroPopup : public Popup {
protected:
	CCLabelBMFont* m_modeLabel = nullptr;
	CCLabelBMFont* m_selLabel = nullptr;
	CCLabelBMFont* m_speedLabel = nullptr;
	TextInput* m_input = nullptr;
	ScrollLayer* m_list = nullptr;
	float m_listW = 0.f;

	bool init() {
		auto ws = CCDirector::get()->getWinSize();
		if (!Popup::init(ws.width, ws.height)) return false;

		this->setTitle("Macro Manager");

		float leftX = -ws.width * 0.2f;
		float rightX = ws.width * 0.26f;
		m_listW = ws.width * 0.47f;
		float listH = ws.height - 110.f;
		float listY = -10.f;

		auto bg = CCScale9Sprite::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
		bg->setColor({ 0, 0, 0 });
		bg->setOpacity(110);
		bg->setContentSize({ m_listW + 10.f, listH + 10.f });
		m_mainLayer->addChildAtPosition(bg, Anchor::Center, { leftX, listY });

		m_list = ScrollLayer::create(CCSize{ m_listW, listH });
		m_list->ignoreAnchorPointForPosition(false);
		m_list->setAnchorPoint({ 0.5f, 0.5f });
		m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout());
		m_mainLayer->addChildAtPosition(m_list, Anchor::Center, { leftX, listY });

		m_modeLabel = addLabel("", rightX, 110.f, 0.55f);

		addBtn("Record", menu_selector(MacroPopup::onRecord), rightX - 75.f, 85.f);
		addBtn("Play", menu_selector(MacroPopup::onPlay), rightX, 85.f);
		addBtn("Off", menu_selector(MacroPopup::onOff), rightX + 60.f, 85.f);

		m_selLabel = addLabel("", rightX, 55.f, 0.45f);

		m_input = TextInput::create(140.f, "New name");
		m_input->setMaxCharCount(24);
		m_mainLayer->addChildAtPosition(m_input, Anchor::Center, { rightX - 40.f, 28.f });
		addBtn("Rename", menu_selector(MacroPopup::onRename), rightX + 80.f, 28.f);

		addBtn("Save", menu_selector(MacroPopup::onSave), rightX - 93.f, 0.f);
		addBtn("Load", menu_selector(MacroPopup::onLoad), rightX - 31.f, 0.f);
		addBtn("Delete", menu_selector(MacroPopup::onDelete), rightX + 31.f, 0.f);
		addBtn("New", menu_selector(MacroPopup::onNew), rightX + 93.f, 0.f);

		addBtn("-", menu_selector(MacroPopup::onSpeedDown), rightX - 70.f, -40.f);
		m_speedLabel = addLabel("", rightX, -40.f, 0.5f);
		addBtn("+", menu_selector(MacroPopup::onSpeedUp), rightX + 70.f, -40.f);

		rebuildList();
		refreshLabels();
		return true;
	}

	CCLabelBMFont* addLabel(char const* text, float x, float y, float scale) {
		auto label = CCLabelBMFont::create(text, "bigFont.fnt");
		label->setScale(scale);
		m_mainLayer->addChildAtPosition(label, Anchor::Center, { x, y });
		return label;
	}

	CCMenuItemSpriteExtra* addBtn(char const* text, SEL_MenuHandler cb, float x, float y) {
		auto btn = CCMenuItemSpriteExtra::create(ButtonSprite::create(text), this, cb);
		btn->setScale(0.55f);
		m_buttonMenu->addChildAtPosition(btn, Anchor::Center, { x, y });
		return btn;
	}

	void rebuildList() {
		auto content = m_list->m_contentLayer;
		content->removeAllChildrenWithCleanup(true);

		auto names = listMacros();
		if (names.empty()) {
			auto label = CCLabelBMFont::create("No macros yet. Tap New.", "bigFont.fnt");
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
			auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroPopup::onPick));
			btn->setScale(scale);
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

		std::string sel = g_selected.empty() ? "Selected: none" : "Selected: " + g_selected;
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
		std::vector<MacroInput> loaded;
		if (readMacro(g_selected, loaded)) g_inputs = loaded;
		m_input->setString(g_selected);
		rebuildList();
		refreshLabels();
	}

	void onRecord(CCObject*) {
		g_inputs.clear();
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
		g_mode = 2;
		notify("Playing: restart the level");
		this->keyBackClicked();
	}

	void onOff(CCObject*) {
		g_mode = 0;
		refreshLabels();
		notify("Macro off");
	}

	void onSave(CCObject*) {
		if (g_selected.empty()) return notify("Tap New or pick a macro first");
		if (g_inputs.empty()) return notify("Nothing recorded yet");
		notify(writeMacro(g_selected) ? "Macro saved" : "Save failed");
	}

	void onLoad(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		std::vector<MacroInput> loaded;
		if (!readMacro(g_selected, loaded)) return notify("Could not load");
		g_inputs = loaded;
		notify(g_inputs.empty() ? "Macro is empty" : "Macro loaded");
	}

	void onDelete(CCObject*) {
		if (g_selected.empty()) return notify("Pick a macro first");
		std::error_code ec;
		std::filesystem::remove(macroFile(g_selected), ec);
		g_selected.clear();
		g_inputs.clear();
		rebuildList();
		refreshLabels();
		notify("Macro deleted");
	}

	void onNew(CCObject*) {
		std::string name;
		for (int n = 1; n < 1000; n++) {
			name = "Macro " + std::to_string(n);
			if (!std::filesystem::exists(macroFile(name))) break;
		}
		g_inputs.clear();
		g_selected = name;
		writeMacro(name);
		m_input->setString(name);
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

	void onSpeedDown(CCObject*) {
		g_speed = std::max(0.1f, g_speed - 0.25f);
		if (g_speed < 0.2f) g_speed = 0.1f;
		applySpeed();
		refreshLabels();
	}

	void onSpeedUp(CCObject*) {
		g_speed = std::min(5.f, g_speed + 0.25f);
		applySpeed();
		refreshLabels();
	}

public:
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

class $modify(GJBaseGameLayer) {
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

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (g_mode == 2) {
			int frame = static_cast<int>(m_gameState.m_currentProgress);
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
		if (g_mode == 1) g_inputs.clear();
		g_cursor = 0;
	}

	void onQuit() {
		g_speed = 1.f;
		applySpeed();
		PlayLayer::onQuit();
	}
};

class $modify(MacroPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		auto menu = CCMenu::create();
		auto btn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Macro"), this, menu_selector(MacroPause::onOpenMacro)
		);
		menu->addChild(btn);
		menu->setPosition(CCDirector::get()->getWinSize().width - 60.f, 40.f);
		this->addChild(menu);
	}

	void onOpenMacro(CCObject*) {
		if (auto popup = MacroPopup::create()) popup->show();
	}
};
