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

// colour bucket for a window size (1, 2, 3, 4, 5-6, 7-8, 9+)
static int winBucket(int w) {
	if (w <= 4) return std::max(0, w - 1);
	if (w <= 6) return 4;
	if (w <= 8) return 5;
	return 6;
}

// ---------- frame stepper panel (on-screen buttons, works with touch) ----------

class StepPanel : public CCNode {
protected:
	CCLabelBMFont* m_info = nullptr;
	ButtonSprite* m_holdSpr = nullptr;

	CCMenuItemSpriteExtra* addBtn(CCMenu* menu, char const* text, SEL_MenuHandler cb, float x, float w, float scale) {
		auto spr = ButtonSprite::create(text, static_cast<int>(w / scale), true, "bigFont.fnt", "GJ_button_01.png", 38.f, 0.8f);
		spr->setScale(scale);
		auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
		btn->setPosition({ x, 0.f });
		menu->addChild(btn);
		if (std::strcmp(text, "Hold") == 0) m_holdSpr = spr;
		return btn;
	}

	bool init() {
		if (!CCNode::init()) return false;
		float sc = S("step-size") / 100.f;
		float w = 62.f * sc;
		float gap = 6.f * sc;
		float scale = 0.8f * sc;
		auto menu = CCMenu::create();
		menu->setPosition({ 0.f, 0.f });
		this->addChild(menu);

		m_info = CCLabelBMFont::create("", "bigFont.fnt");
		m_info->setScale(0.38f * sc);
		m_info->setPosition({ 0.f, 30.f * sc });
		this->addChild(m_info);

		float x = -2.f * (w + gap);
		addBtn(menu, "Undo", menu_selector(StepPanel::onUndo), x, w, scale); x += w + gap;
		addBtn(menu, "Step", menu_selector(StepPanel::onStep), x, w, scale); x += w + gap;
		addBtn(menu, "+5", menu_selector(StepPanel::onStep5), x, w, scale); x += w + gap;
		addBtn(menu, "Hold", menu_selector(StepPanel::onHold), x, w, scale); x += w + gap;
		addBtn(menu, "Exit", menu_selector(StepPanel::onExit), x, w, scale);
		return true;
	}

public:
	void onUndo(CCObject*) { if (g_stepUndoReq < 20) g_stepUndoReq++; }
	void onStep(CCObject*) { if (g_stepPending < 50) g_stepPending++; }
	void onStep5(CCObject*) { g_stepPending = std::min(50, g_stepPending + 5); }
	void onHold(CCObject*) { g_stepHoldReq = !g_stepHoldReq; }
	void onExit(CCObject*) { g_stepOn = false; }

	void refresh() {
		auto pl = PlayLayer::get();
		int f = pl ? static_cast<int>(pl->m_gameState.m_currentProgress) : 0;
		std::string t = "Frame " + std::to_string(f) + "   undo " + std::to_string(g_stepHist.size());
		if (!g_stepMsg.empty()) t += "   " + g_stepMsg;
		if (t != std::string(m_info->getString())) m_info->setString(t.c_str());
		if (m_holdSpr) {
			bool armed = g_stepHeld != g_stepHoldReq; // what the button will be after the next step
			m_holdSpr->setColor(armed ? accentColor() : ccColor3B{ 255, 255, 255 });
		}
	}

	static StepPanel* create() {
		auto ret = new StepPanel();
		if (ret->init()) { ret->autorelease(); return ret; }
		delete ret;
		return nullptr;
	}
};

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
		// the stepper only works while recording / idle
		if (g_stepOn && (g_mode == 2 || g_mode == 3 || g_mode == 4)) { g_stepOn = false; stepClear(); }
		if (g_stepOn && PlayLayer::get()) this->stepperTick();
		else GJBaseGameLayer::update(dt);
		// keep user speed alive through deaths/restarts/time-warp resets
		enforceSpeed(m_gameState.m_timeWarp);
		updateHud();
		updateWindowHud();
		updatePath();
		updateStepperUI();
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

	// ----- frame stepper -----
	// Advances the game by exactly one physics step. The leftover-time counter is cleared first
	// (when the field exists) so no time carries over; if no step happened the time is nudged up.
	void stepOnce() {
		float tw = m_gameState.m_timeWarp;
		if (tw <= 0.f) tw = 1.f;
		float stepDt = tw / 240.f;
		int before = static_cast<int>(m_gameState.m_currentProgress);
		static const float nudge[3] = { 1.0005f, 1.02f, 1.1f };
		for (int a = 0; a < 3; a++) {
			[&](auto* q) {
				if constexpr (requires { q->m_extraDelta; }) q->m_extraDelta = 0;
			}(this);
			GJBaseGameLayer::update(stepDt * nudge[a]);
			if (static_cast<int>(m_gameState.m_currentProgress) != before) break;
		}
	}

	void stepperTick() {
		auto pl = PlayLayer::get();
		if (!pl) return;

		if (g_stepUndoReq > 0) {
			g_stepUndoReq--;
			g_stepPending = 0;
			if (g_stepHist.empty()) {
				g_stepMsg = "nothing to undo";
			} else {
				auto e = g_stepHist.back();
				g_stepHist.pop_back();
				if (spLoad(pl, e.cp)) {
					int f = e.frame;
					if (g_mode == 1) {
						// forget what was recorded from the undone step on
						g_inputs.erase(std::remove_if(g_inputs.begin(), g_inputs.end(),
							[&](MacroInput const& i) { return i.frame >= f; }), g_inputs.end());
						g_anchors.erase(std::remove_if(g_anchors.begin(), g_anchors.end(),
							[&](MacroAnchor const& a) { return a.frame >= f; }), g_anchors.end());
						for (auto it = g_windows.begin(); it != g_windows.end();) {
							if (it->first >= static_cast<int>(g_inputs.size())) it = g_windows.erase(it);
							else ++it;
						}
						g_pendStamp.clear();
						g_lastAnchorFrame = f - 1;
					}
					g_lastRawStep = -1;
					g_callIdx = 0;
					g_frame = f;
					g_stepHoldReq = false;
					// a checkpoint may not remember the held button: put it back as it was
					if (playerHolding(m_player1) != e.held) {
						g_injecting = true;
						GJBaseGameLayer::handleButton(e.held, 1, true);
						g_injecting = false;
					}
					g_stepHeld = e.held;
					g_stepDead = false;
					g_stepMsg = "back at " + std::to_string(f);
				} else {
					g_stepMsg = "undo failed";
				}
				if (e.cp) e.cp->release();
			}
		}

		while (g_stepPending > 0) {
			g_stepPending--;
			if (g_stepDead) { g_stepPending = 0; g_stepMsg = "dead: tap Undo"; break; }
			auto cp = spCreate(pl);
			if (!cp) { g_stepPending = 0; g_stepMsg = "no checkpoint: use practice mode"; break; }
			cp->retain();
			size_t maxH = static_cast<size_t>(S("step-max"));
			while (g_stepHist.size() >= maxH) {
				if (g_stepHist.front().cp) g_stepHist.front().cp->release();
				g_stepHist.erase(g_stepHist.begin());
			}
			g_stepHist.push_back({ static_cast<int>(m_gameState.m_currentProgress), cp, g_stepHeld });
			if (g_stepHoldReq) {
				g_stepHoldReq = false;
				g_stepHeld = !g_stepHeld;
				// goes through the hooked handleButton so it is recorded while recording
				g_injecting = true;
				this->handleButton(g_stepHeld, 1, true);
				g_injecting = false;
			}
			g_stepMsg.clear();
			this->stepOnce();
		}
	}

	void updateStepperUI() {
		if (!m_uiLayer || !PlayLayer::get()) return;
		auto node = m_uiLayer->getChildByID("macro-stepper");
		if (!g_stepOn) {
			if (node) node->removeFromParent();
			return;
		}
		if (!node) {
			auto panel = StepPanel::create();
			if (!panel) return;
			panel->setID("macro-stepper");
			m_uiLayer->addChild(panel, 160);
			node = panel;
		}
		auto ws = CCDirector::get()->getWinSize();
		node->setPosition({ ws.width / 2.f, 26.f * S("step-size") / 100.f });
		static_cast<StepPanel*>(node)->refresh();
	}

	// ----- tally + rings (window counter, tally style) -----
	void updateTally(bool on) {
		static const ccColor3B cols[7] = {
			{ 240, 75, 75 }, { 250, 160, 60 }, { 250, 225, 70 }, { 255, 255, 255 },
			{ 85, 210, 105 }, { 140, 215, 255 }, { 110, 110, 250 }
		};
		auto root = m_uiLayer->getChildByID("macro-tally");
		auto rings = m_objectLayer ? m_objectLayer->getChildByID("macro-rings") : nullptr;
		if (!on) {
			if (root) root->setVisible(false);
			if (rings) rings->removeAllChildrenWithCleanup(true);
			return;
		}

		bool wide = S("win-max") * 2 + 1 > 12;
		static char const* const names[7] = { "1", "2", "3", "4", "5-6", "7-8", "9-12" };
		if (!root) {
			root = CCNode::create();
			root->setID("macro-tally");
			for (int i = 0; i < 7; i++) {
				auto nm = CCLabelBMFont::create("", "bigFont.fnt");
				nm->setAnchorPoint({ 0.f, 0.5f });
				nm->setTag(100 + i);
				nm->setColor(cols[i]);
				root->addChild(nm);
				auto ct = CCLabelBMFont::create("0", "bigFont.fnt");
				ct->setAnchorPoint({ 1.f, 0.5f });
				ct->setTag(200 + i);
				ct->setColor(cols[i]);
				root->addChild(ct);
			}
			m_uiLayer->addChild(root, 151);
		}
		root->setVisible(true);

		// count the clicks passed so far in this run
		if (g_cursor < g_tallyCursor) {
			for (auto& c : g_tally) c = 0;
			g_tallyCursor = 0;
		}
		while (g_tallyCursor < g_cursor && g_tallyCursor < g_inputs.size()) {
			size_t idx = g_tallyCursor++;
			if (!g_inputs[idx].down) continue;
			auto it = g_windows.find(static_cast<int>(idx));
			if (it != g_windows.end()) g_tally[winBucket(it->second.first + it->second.second + 1)]++;
		}

		auto ws = CCDirector::get()->getWinSize();
		float sc = S("win-size") / 100.f * 0.9f;
		float topY = ws.height * 0.8f;
		for (int i = 0; i < 7; i++) {
			int row = 6 - i; // biggest windows on top
			float y = topY - row * 30.f * sc;
			auto nm = static_cast<CCLabelBMFont*>(root->getChildByTag(100 + i));
			auto ct = static_cast<CCLabelBMFont*>(root->getChildByTag(200 + i));
			if (!nm || !ct) continue;
			std::string label = std::string(i == 6 && wide ? "9+" : names[i]) + ":";
			if (label != std::string(nm->getString())) nm->setString(label.c_str());
			std::string count = std::to_string(g_tally[i]);
			if (count != std::string(ct->getString())) ct->setString(count.c_str());
			nm->setScale(sc);
			ct->setScale(sc);
			nm->setPosition({ 8.f, y });
			ct->setPosition({ 8.f + 245.f * sc, y });
			nm->setOpacity(235);
			ct->setOpacity(235);
		}

		// rings are added when a click is registered (addRingMark) and removed here
		// once they have scrolled off the left side of the screen
		if (!m_objectLayer || !rings) return;
		if (S("win-ring") == 0) {
			rings->removeAllChildrenWithCleanup(true);
			return;
		}
		if (auto kids = rings->getChildren()) {
			for (int i = static_cast<int>(kids->count()) - 1; i >= 0; i--) {
				auto n = static_cast<CCNode*>(kids->objectAtIndex(i));
				if (m_objectLayer->convertToWorldSpace(n->getPosition()).x < -60.f) n->removeFromParent();
			}
		}
	}

	// A ring with the window size next to it, placed on the player where a click
	// was just registered. It stays in the level, so it scrolls away with it.
	void addRingMark(float x, float y, int w) {
		if (!m_objectLayer) return;
		static const ccColor3B cols[7] = {
			{ 240, 75, 75 }, { 250, 160, 60 }, { 250, 225, 70 }, { 255, 255, 255 },
			{ 85, 210, 105 }, { 140, 215, 255 }, { 110, 110, 250 }
		};
		auto root = m_objectLayer->getChildByID("macro-rings");
		if (!root) {
			root = CCNode::create();
			root->setID("macro-rings");
			m_objectLayer->addChild(root, 501);
		}
		if (root->getChildrenCount() > 120) return;

		auto col = cols[winBucket(w)];
		ccColor4F c4 = ccc4f(col.r / 255.f, col.g / 255.f, col.b / 255.f, 0.95f);
		auto mark = CCNode::create();
		mark->setPosition({ x, y });

		auto draw = CCDrawNode::create();
		float r = 14.f;
		const int segs = 24;
		for (int k = 0; k < segs; k++) {
			float a0 = k * 6.2831853f / segs;
			float a1 = (k + 1) * 6.2831853f / segs;
			draw->drawSegment(
				{ r * std::cos(a0), r * std::sin(a0) },
				{ r * std::cos(a1), r * std::sin(a1) }, 1.8f, c4);
		}
		mark->addChild(draw);

		auto lab = CCLabelBMFont::create(std::to_string(w).c_str(), "bigFont.fnt");
		lab->setScale(0.5f);
		lab->setColor(col);
		lab->setPosition({ -r - 12.f, 0.f });
		mark->addChild(lab);

		root->addChild(mark);
	}

	// ----- frame window counter -----
	// Shows the fixed window (in frames) of the next click while playing.
	void updateWindowHud() {
		if (!m_uiLayer || !PlayLayer::get()) return;
		auto label = static_cast<CCLabelBMFont*>(m_uiLayer->getChildByID("macro-wnd"));
		updateTally(g_mode == 2 && S("win-hud") != 0 && !g_windows.empty() && S("win-style") == 1);
		std::string text;
		char buf[96];
		if (g_mode == 3 && g_wt.active) {
			if (g_wt.phase == 0) {
				std::snprintf(buf, sizeof(buf), "Window test: checking macro");
			} else {
				std::snprintf(buf, sizeof(buf), "Testing click %d/%d  shift %c%d%s",
					static_cast<int>(g_wt.ci) + 1, static_cast<int>(g_wt.clicks.size()),
					g_wt.phase == 1 ? '-' : '+', g_wt.s, g_wt.sp >= 0 ? "  (save point)" : "");
			}
			text = buf;
		} else if (g_mode == 4 && g_sol.active) {
			std::snprintf(buf, sizeof(buf), "Solving  frame %d  tries %d  option %d/%d",
				g_sol.frame, g_sol.attempts, g_sol.k + 1, kSolCands);
			text = buf;
		} else if (g_mode == 2 && S("win-hud") && !g_windows.empty() && S("win-style") == 0) {
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
		if ((g_mode == 2 || g_mode == 3 || g_mode == 4 || g_stepOn) && !g_injecting) return;
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
		// "Snap to route" off: playback is left alone so the finder shows real drift
		// (the solver always needs the snapping)
		if (g_mode != 4 && !S("snap")) return;
		if (S("exact-route") || g_mode == 4) {
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
		if (g_mode == 3 || g_mode == 4) {
			if (g_spNeedBase) {
				g_spNeedBase = false;
				g_frameBase = g_spExpect - frame;
				if (g_spStrategy == 1 || g_mode == 4) {
					// the save point may not remember a held button: press it again if the macro holds it here
					bool held = false;
					for (size_t k = g_cursor; k > 0 && k <= g_work.size(); k--) {
						auto& in = g_work[k - 1];
						if (in.p1 && in.button == 1) { held = in.down; break; }
					}
					if (held && (g_mode == 3 || !playerHolding(m_player1))) {
						g_injecting = true;
						GJBaseGameLayer::handleButton(true, 1, true);
						g_injecting = false;
					}
				}
			}
			frame += g_frameBase;
		}
		g_frame = frame;
		if (g_mode == 1 && !g_pendStamp.empty()) {
			for (auto idx : g_pendStamp) {
				if (idx < g_inputs.size()) {
					g_inputs[idx].frame = frame;
					g_inputs[idx].sub = g_callIdx;
				}
			}
			g_pendStamp.clear();
		}
		if (g_diagPending && m_player1) {
			g_diagPending = false;
			char b[200];
			std::snprintf(b, sizeof(b), "start: x %.1f y %.1f vy %.2f prog %d",
				static_cast<float>(m_player1->getPositionX()), static_cast<float>(m_player1->getPositionY()),
				static_cast<float>(m_player1->m_yVelocity), frame);
			g_diag = b;
			if (!g_anchors.empty()) {
				auto& a = g_anchors.front();
				std::snprintf(b, sizeof(b), "\nrecorded: x %.1f y %.1f vy %.2f frame %d", a.x1, a.y1, a.v1, a.frame);
				g_diag += b;
			}
		}
		if (g_mode == 3 && g_wt.active && m_player1) {
			float px = static_cast<float>(m_player1->getPositionX());
			float py = static_cast<float>(m_player1->getPositionY());
			if (g_wt.phase == 0 && !g_wt.resolved && frame >= 0 && frame < 600000) {
				if (g_baseTrace.size() <= static_cast<size_t>(frame)) g_baseTrace.resize(static_cast<size_t>(frame) + 1, { 1e9f, 1e9f });
				g_baseTrace[frame] = { px, py };
				if (g_wt.spOk && S("win-save") && frame > 0 && g_sps.size() < 150 && frame % S("win-gap") == 0) {
					auto pl = PlayLayer::get();
					if (pl && pl->m_isPracticeMode) {
						if (auto cp = spCreate(pl)) {
							cp->retain();
							g_sps.push_back({ frame, cp });
						}
					}
				}
			}
			if (g_spVerify > 0 && !g_wt.resolved) {
				g_spVerify--;
				if (frame >= 0 && static_cast<size_t>(frame) < g_baseTrace.size()) {
					auto& tr = g_baseTrace[frame];
					if (tr[0] < 1e8f && (std::fabs(px - tr[0]) > 0.02f || std::fabs(py - tr[1]) > 0.02f)) spFail();
				}
			}
		}
		if (g_mode == 1) {
			recordAnchor(frame);
		}
		if (g_mode == 2 || g_mode == 3 || g_mode == 4) {
			// anchors are off while testing: the test needs the real physics
			if (g_mode == 2) applyAnchors(frame);
			// solver: replay the proven route exactly, then explore past its end
			if (g_mode == 4 && frame < g_sol.frame) applyAnchors(frame);
			// solver: take a checkpoint at the end of this chunk, before this step's inputs
			if (g_mode == 4 && g_sol.active && !g_sol.resolved && frame == g_sol.frame + g_sol.chunk) solMaybeCp(frame);
			auto& src = (g_mode == 3 || g_mode == 4) ? g_work : g_inputs;
			while (g_cursor < src.size() && (src[g_cursor].frame < frame || (src[g_cursor].frame == frame && src[g_cursor].sub <= g_callIdx))) {
				auto i = src[g_cursor++];
				g_injecting = true;
				GJBaseGameLayer::handleButton(i.down, i.button, i.p1);
				g_injecting = false;
				if (g_mode == 2 && i.down && m_player1 && S("win-hud") != 0 && S("win-style") == 1 && S("win-ring") != 0) {
					auto wit = g_windows.find(static_cast<int>(g_cursor) - 1);
					if (wit != g_windows.end()) {
						addRingMark(static_cast<float>(m_player1->getPositionX()), static_cast<float>(m_player1->getPositionY()),
							wit->second.first + wit->second.second + 1);
					}
				}
			}
			if (g_mode == 3 && g_wt.active && g_wt.phase != 0 && !g_wt.resolved && frame >= g_wt.endFrame) {
				wtResolve(true);
			}
			if (g_mode == 4 && g_sol.active && !g_sol.resolved) {
				g_solSteps++;
				if (frame >= g_sol.frame + g_sol.chunk) solResolve(1);
				else if (frame >= g_sol.frame) recordSolAnchor(frame);
			}
		}
		g_inCmd = true;
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		g_inCmd = false;
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

		if (g_stepOn) stepClear();
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
		g_tpsExtra = 0.f;
		if (g_mode == 2) divReset();
		g_diagPending = (g_mode == 3 || g_mode == 4);
		if (m_objectLayer) {
			if (auto rr = m_objectLayer->getChildByID("macro-rings")) rr->removeAllChildrenWithCleanup(true);
		}
		g_frameBase = 0;
		g_spNeedBase = false;
		g_spVerify = 0;
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
			if (g_wt.phase == 0) spClear();
			wtBuildWork();
			g_wt.sp = -1;
			if (g_wt.phase > 0 && g_wt.spOk && S("win-save") && m_isPracticeMode && !g_sps.empty()) {
				size_t ci = static_cast<size_t>(g_wt.clicks[g_wt.ci]);
				if (ci < g_inputs.size()) {
					int need = g_inputs[ci].frame - S("win-max") - 3;
					int best = -1;
					for (size_t i = 0; i < g_sps.size(); i++) {
						if (g_sps[i].frame <= need) best = static_cast<int>(i);
					}
					if (best >= 0 && spLoad(this, g_sps[best].cp)) {
						g_wt.sp = best;
						g_spNeedBase = true;
						g_spExpect = g_sps[best].frame;
						g_spVerify = 20;
						while (g_cursor < g_work.size() && g_work[g_cursor].frame < g_spExpect) g_cursor++;
					}
				}
			}
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
			if (g_sol.attempts == 0 && g_sol.frame == 0 && startFrame > 0) g_sol.frame = startFrame;
			solBuildWork();
			g_sol.resolved = false;
			g_solSteps = 0;
			g_lastAnchorFrame = -1;
			// fast path: reload the newest checkpoint on the proven route instead of replaying
			// the level from the start (a checkpoint is only kept while the route is in sync)
			int loadFrame = startFrame;
			if (S("solve-cp") && m_isPracticeMode && g_sol.desync == 0 && !g_solCps.empty()) {
				int best = -1;
				for (size_t i = 0; i < g_solCps.size(); i++) {
					if (g_solCps[i].frame <= g_sol.frame) best = static_cast<int>(i);
				}
				if (best >= 0 && spLoad(this, g_solCps[best].cp)) {
					g_spNeedBase = true;
					g_spExpect = g_solCps[best].frame;
					loadFrame = g_spExpect;
				}
			}
			while (g_cursor < g_work.size() && g_work[g_cursor].frame < loadFrame) g_cursor++;
			while (g_anchorCursor < g_anchors.size() && g_anchors[g_anchorCursor].frame < loadFrame) g_anchorCursor++;
		}
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		// frame stepper: don't die and restart, flag it and let the user Undo the step
		if (g_stepOn) {
			if (!g_stepDead) {
				g_stepDead = true;
				g_stepPending = 0;
				g_stepMsg = "died: tap Undo";
			}
			return;
		}
		// window test / solver: a death only decides the attempt, then the level is restarted
		if ((g_mode == 3 && g_wt.active) || (g_mode == 4 && g_sol.active)) {
			char b[200];
			std::snprintf(b, sizeof(b), "death: x %.1f y %.1f vy %.2f frame %d obj %d",
				player ? static_cast<float>(player->getPositionX()) : 0.f,
				player ? static_cast<float>(player->getPositionY()) : 0.f,
				player ? static_cast<float>(player->m_yVelocity) : 0.f,
				g_frame, obj ? static_cast<int>(obj->m_objectID) : -1);
			if (g_mode == 3) {
				g_diagDeath = b;
				if (!g_wt.resolved) {
					if (g_wt.phase == 0) {
						g_wt.baseEnd = g_frame;
						wtResolve(true);
					} else {
						wtResolve(false);
					}
				}
			} else if (!g_sol.resolved) {
				g_diagDeath = b;
				g_solDeathFrame = g_frame;
				g_solDeathX = player ? static_cast<float>(player->getPositionX()) : 0.f;
				g_solDeathObj = obj ? static_cast<int>(obj->m_objectID) : -1;
				// dying inside the part that was already proven means the replay drifted
				if (g_frame < g_sol.frame) solResolve(2);
				else solResolve(0);
			}
			return;
		}
		PlayLayer::destroyPlayer(player, obj);
	}

	void levelComplete() {
		if (g_mode == 3 && g_wt.active) {
			if (!g_wt.resolved) {
				if (g_wt.phase == 0) g_wt.baseEnd = 0x7fffffff;
				wtResolve(true);
			}
			return;
		}
		if (g_mode == 4 && g_sol.active) {
			if (!g_sol.resolved) {
				g_sol.resolved = true;
				solCommit();
				solFinish(0);
			}
			PlayLayer::levelComplete();
			return;
		}
		if (g_mode == 1 && !g_inputs.empty() && !g_selected.empty()) {
			writeMacro(g_selected);
		}
		PlayLayer::levelComplete();
	}

	void onQuit() {
		g_stepOn = false;
		stepClear();
		if (g_mode == 1 && !g_inputs.empty() && !g_selected.empty()) {
			writeMacro(g_selected);
		} else if (g_mode == 3) {
			g_mode = 0;
			wtRestoreSpeed();
			spClear();
			g_wt.active = false;
			g_wt.pendingReset = false;
			g_wt.pendingAdvance = false;
		} else if (g_mode == 4) {
			solFinish(3);
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

		auto sbtn = makeButton(g_stepOn ? "Step off" : "Stepper", this, menu_selector(MacroPauseLayer::onStepperToggle), 0.6f);
		sbtn->setPosition({ left ? x + 78.f : x - 78.f, y });
		menu->addChild(sbtn);

		this->addChild(menu, 100);
	}

	void onStepperToggle(CCObject*) {
		if (g_stepOn) {
			g_stepOn = false;
			stepClear();
		} else {
			if (g_mode == 2 || g_mode == 3 || g_mode == 4) {
				Notification::create("Set the macro mode to Off or Record first")->show();
				return;
			}
			stepClear();
			g_stepOn = true;
			g_stepMsg = "tap Step";
		}
		this->onResume(nullptr);
	}

	void onMacroOpen(CCObject*) {
		if (auto popup = MacroPopup::create()) popup->show();
	}
};
