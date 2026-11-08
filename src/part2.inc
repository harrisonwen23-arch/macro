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
	else if (code == -1) {
		std::string msg =
			"No route. Died after " + std::to_string(g_solSteps) + " steps" +
			" at frame " + std::to_string(g_solDeathFrame) +
			", x " + std::to_string(static_cast<int>(g_solDeathX)) +
			", object " + std::to_string(g_solDeathObj) +
			", tries " + std::to_string(g_sol.attempts);
		if (g_solSteps <= 1) msg += ". Dies instantly here: use an earlier checkpoint";
		msg += "\n" + g_diag + "\n" + g_diagDeath;
		showSavedBox(msg);
	}
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
			// g_mode can be 3 (window test) or 4 (solver), so the table needs 5 entries
			char const* modes[] = { "Mode: Off", "Mode: Recording", "Mode: Playing", "Mode: Testing", "Mode: Solving" };
			m_modeLabel->setString(modes[std::clamp(g_mode, 0, 4)]);
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
		int keepTps = g_macroTps;
		if (!readMacro(second, in2, an2)) return notify("Macro not found");
		g_macroTps = keepTps;

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
		g_solDeathObj = -1;
		g_solSteps = 0;
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
		g_spStrategy = 0;
		spClear();
		wtBeginSpeed();
		notify("Window test: restart the level");
		this->keyBackClicked();
	}

	void onStopTest(CCObject*) {
		if (g_mode != 3) return notify("No test running");
		g_mode = 0;
		wtRestoreSpeed();
		spClear();
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

