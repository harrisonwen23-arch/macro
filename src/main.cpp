#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fstream>
#include <cstdio>

using namespace geode::prelude;

struct MacroInput { int frame; int button; bool down; bool p1; };

static int g_mode = 0; // 0 = off, 1 = record, 2 = play
static std::vector<MacroInput> g_inputs;
static size_t g_cursor = 0;
static bool g_injecting = false;
static int g_slot = 1;
static float g_speed = 1.f;

static std::filesystem::path slotPath() {
	return Mod::get()->getSaveDir() / ("macro" + std::to_string(g_slot) + ".txt");
}

static bool saveMacro() {
	if (g_inputs.empty()) return false;
	std::ofstream out(slotPath());
	if (!out) return false;
	for (auto& i : g_inputs) {
		out << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0) << '\n';
	}
	return true;
}

static bool loadMacro() {
	std::ifstream in(slotPath());
	if (!in) return false;
	std::vector<MacroInput> loaded;
	int frame, button, down, p1;
	while (in >> frame >> button >> down >> p1) {
		loaded.push_back({ frame, button, down != 0, p1 != 0 });
	}
	if (loaded.empty()) return false;
	g_inputs = loaded;
	return true;
}

static void applySpeed() {
	CCDirector::get()->getScheduler()->setTimeScale(g_speed);
}

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
	struct Fields {
		CCLabelBMFont* status = nullptr;
		CCLabelBMFont* slot = nullptr;
		CCLabelBMFont* speed = nullptr;
	};

	void customSetup() {
		PauseLayer::customSetup();

		auto winSize = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		menu->setPosition({ winSize.width - 95.f, winSize.height / 2.f });

		auto addButton = [&](char const* text, SEL_MenuHandler cb, float x, float y) {
			auto btn = CCMenuItemSpriteExtra::create(ButtonSprite::create(text), this, cb);
			btn->setScale(0.5f);
			btn->setPosition({ x, y });
			menu->addChild(btn);
		};

		auto addLabel = [&](float x, float y) {
			auto label = CCLabelBMFont::create("", "bigFont.fnt");
			label->setScale(0.4f);
			label->setPosition({ x, y });
			menu->addChild(label);
			return label;
		};

		m_fields->status = addLabel(0.f, 95.f);

		addButton("Record", menu_selector(MacroPause::onRecord), -48.f, 65.f);
		addButton("Play", menu_selector(MacroPause::onPlay), 0.f, 65.f);
		addButton("Off", menu_selector(MacroPause::onOff), 40.f, 65.f);

		addButton("Save", menu_selector(MacroPause::onSave), -30.f, 30.f);
		addButton("Load", menu_selector(MacroPause::onLoad), 30.f, 30.f);

		addButton("-", menu_selector(MacroPause::onSlotDown), -45.f, 0.f);
		m_fields->slot = addLabel(0.f, 0.f);
		addButton("+", menu_selector(MacroPause::onSlotUp), 45.f, 0.f);

		addButton("-", menu_selector(MacroPause::onSpeedDown), -45.f, -35.f);
		m_fields->speed = addLabel(0.f, -35.f);
		addButton("+", menu_selector(MacroPause::onSpeedUp), 45.f, -35.f);

		this->addChild(menu);
		refreshLabels();
	}

	void refreshLabels() {
		char const* modes[] = { "Mode: Off", "Mode: Recording", "Mode: Playing" };
		m_fields->status->setString(modes[g_mode]);

		char buf[32];
		std::snprintf(buf, sizeof(buf), "Slot %d", g_slot);
		m_fields->slot->setString(buf);

		std::snprintf(buf, sizeof(buf), "Speed %.2fx", g_speed);
		m_fields->speed->setString(buf);
	}

	void onRecord(CCObject*) {
		g_inputs.clear();
		g_mode = 1;
		refreshLabels();
		Notification::create("Recording: restart the level")->show();
	}

	void onPlay(CCObject*) {
		if (g_inputs.empty()) {
			Notification::create("No macro loaded")->show();
			return;
		}
		g_cursor = 0;
		g_mode = 2;
		refreshLabels();
		Notification::create("Playing: restart the level")->show();
	}

	void onOff(CCObject*) {
		g_mode = 0;
		refreshLabels();
		Notification::create("Macro off")->show();
	}

	void onSave(CCObject*) {
		Notification::create(saveMacro() ? "Macro saved" : "Nothing to save")->show();
	}

	void onLoad(CCObject*) {
		Notification::create(loadMacro() ? "Macro loaded" : "No macro in this slot")->show();
	}

	void onSlotDown(CCObject*) {
		if (g_slot > 1) g_slot--;
		refreshLabels();
	}

	void onSlotUp(CCObject*) {
		if (g_slot < 99) g_slot++;
		refreshLabels();
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
};
