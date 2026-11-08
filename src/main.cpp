#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

struct MacroInput { int frame; int button; bool down; bool p1; };

static int g_mode = 0; // 0 = off, 1 = record, 2 = play
static std::vector<MacroInput> g_inputs;
static size_t g_cursor = 0;
static bool g_injecting = false;

class $modify(GJBaseGameLayer) {
	void handleButton(bool down, int button, bool p1) {
		if (g_mode == 2 && !g_injecting) return;
		if (g_mode == 1) g_inputs.push_back({ m_gameState.m_currentProgress, button, down, p1 });
		GJBaseGameLayer::handleButton(down, button, p1);
	}

	void processCommands(float dt) {
		if (g_mode == 2) {
			while (g_cursor < g_inputs.size() && g_inputs[g_cursor].frame <= m_gameState.m_currentProgress) {
				auto& i = g_inputs[g_cursor++];
				g_injecting = true;
				GJBaseGameLayer::handleButton(i.down, i.button, i.p1);
				g_injecting = false;
			}
		}
		GJBaseGameLayer::processCommands(dt);
	}
};

class $modify(PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		if (g_mode == 1) g_inputs.clear();
		g_cursor = 0;
	}
};

class $modify(MacroPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		auto menu = CCMenu::create();
		auto btn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Macro"), this, menu_selector(MacroPause::onMacro)
		);
		menu->addChild(btn);
		menu->setPosition(CCDirector::get()->getWinSize().width - 60.f, 40.f);
		this->addChild(menu);
	}

	void onMacro(CCObject*) {
		g_mode = (g_mode + 1) % 3;
		if (g_mode == 1) g_inputs.clear();
		char const* names[] = { "Macro off", "Recording: restart level", "Playing: restart level" };
		Notification::create(names[g_mode])->show();
	}
};
