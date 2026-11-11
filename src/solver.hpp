#pragma once
// =====================================================================================
// solver.hpp - deterministic sandbox simulator + path search for the macro mod.
//
// RULES FOR THIS FILE
//   * No cocos2d / Geode / GD types anywhere. Everything here works on plain-old-data that
//     the main thread copies out of the game once, so the whole file is safe to run on a
//     worker thread. Nothing in here can touch (or crash) the live game.
//   * No randomness, no clocks in the physics: same Level + same State + same Cfg always
//     gives the same input list.
//
// ACCURACY WARNING
//   This is a re-implementation of GD physics, not the game's own code. The constants in
//   namespace `phys` are the community-documented values for the 60Hz-frame model and the
//   ship/wave/pad/orb numbers are APPROXIMATE. Use the mod's existing desync finder (write
//   the route as anchors, turn "Snap to route" off) to measure drift and tune them.
//   Supported: cube, ship, wave, gravity/size/speed/gamemode portals, yellow+pink pads
//   and orbs, solid + hazard hitboxes (axis-aligned). Not supported: slopes (treated as
//   boxes), ball/ufo/robot/spider/swing (the solver stops there and reports it), moving
//   objects/triggers, blue/green/red pads and orbs, dual mode.
// =====================================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace pf {

enum class Mode : uint8_t { Cube, Ship, Wave, Unsupported };
enum class Kind : uint8_t { Solid, Hazard, Trigger };
enum class Eff : uint8_t {
	None, ToCube, ToShip, ToWave, ToUnsupported,
	GravNormal, GravFlip, SizeNormal, SizeMini, Speed,
	PadYellow, PadPink, OrbYellow, OrbPink
};

struct Box {
	float x0, y0, x1, y1;   // hitbox in object-layer coordinates (what GameObject::getObjectRect gives)
	Kind kind;
	Eff eff = Eff::None;
	uint8_t arg = 0;        // speed index for Eff::Speed
};

// units per second for the five speed portals (0.5x, 1x, 2x, 3x, 4x)
inline constexpr float kSpeedUps[5] = { 251.16f, 311.58f, 387.42f, 468.0f, 576.0f };

namespace phys {
	// velocities are in "units per 60Hz frame", accelerations in "units per frame^2".
	// Each simulation tick advances by ts = 60 / tps of a frame.
	inline constexpr float kGravity   = 0.958199f;
	inline constexpr float kJump      = 11.180032f;
	inline constexpr float kTerminal  = 15.0f;
	// Ship. Release gravity, the velocity caps and the mini scaling come from the reconstructed
	// PlayerObject::updateJump (fall: 0.4 * gravity; caps: +8.0 / -6.4; mini divides all of them
	// by the vehicle size 0.85). The HOLD acceleration is still an estimate (0.8 * gravity).
	inline constexpr float kShipUp    = 0.8f * kGravity;   // hold acceleration - ESTIMATE, calibrate
	inline constexpr float kShipDown  = 0.4f * kGravity;   // release acceleration (0.38328)
	inline constexpr float kShipMaxUp = 8.0f;              // max rise speed
	inline constexpr float kShipMaxDn = 6.4f;              // max fall speed
	inline constexpr float kMiniScale = 0.85f;             // vehicle size of the mini ship
	inline constexpr float kPadYellow = 16.0f;   // APPROXIMATE - calibrate
	inline constexpr float kPadPink   = 10.0f;   // APPROXIMATE - calibrate
	inline constexpr float kOrbYellow = 11.18f;  // APPROXIMATE - calibrate
	inline constexpr float kOrbPink   = 7.0f;    // APPROXIMATE - calibrate
	inline constexpr float kHalf      = 15.0f;   // player half-size used against solids (mini: 9)
	inline constexpr float kSmallHalf = 5.0f;    // smaller hitbox used against hazards (mini: 3)
	inline constexpr float kStepTol   = 2.0f;    // how furthest a corner may clip before it is a crash
}

struct Level {
	std::vector<Box> boxes;
	float floorY = 0.f;     // top surface of the ground
	float ceilY = 300.f;    // ceiling height (absolute)
	float endX = 0.f;       // x where the level ends
	float originX = 0.f;

	// x-buckets so a collision query only looks at the boxes closest the player
	// (the same idea as the game's own section array)
	static constexpr float kCol = 60.f;
	std::vector<std::vector<int>> cols;

	void build(float fromX) {
		originX = fromX;
		int n = std::max(1, static_cast<int>((endX - originX) / kCol) + 3);
		cols.assign(n, {});
		for (int i = 0; i < static_cast<int>(boxes.size()); i++) {
			int c0 = std::clamp(static_cast<int>(std::floor((boxes[i].x0 - originX) / kCol)), 0, n - 1);
			int c1 = std::clamp(static_cast<int>(std::floor((boxes[i].x1 - originX) / kCol)), 0, n - 1);
			for (int c = c0; c <= c1; c++) cols[c].push_back(i);
		}
	}

	template <class F>
	void query(float x0, float x1, F&& f) const {
		if (cols.empty()) return;
		int n = static_cast<int>(cols.size());
		int c0 = std::clamp(static_cast<int>(std::floor((x0 - originX) / kCol)), 0, n - 1);
		int c1 = std::clamp(static_cast<int>(std::floor((x1 - originX) / kCol)), 0, n - 1);
		for (int c = c0; c <= c1; c++)
			for (int idx : cols[c]) f(boxes[idx], idx);
	}
};

struct State {
	float x = 0.f, y = 0.f, vy = 0.f;
	Mode mode = Mode::Cube;
	bool flip = false, mini = false, onGround = false, held = false;
	uint8_t speed = 1;
	int32_t lastPad = -1;   // pad touched last tick, so one pad fires once per touch
};

struct Cfg {
	int tps = 240;                 // physics ticks per second (matches the mod's TPS setting)
	int decisionTicks = 4;         // one hold/release decision lasts this many ticks
	float window = 600.f;          // lookahead per search window, in units
	size_t budget = 400000;        // max nodes per window
	float maxSeconds = 60.f;       // wall-clock cap for the whole solve
	int traceEvery = 4;            // ticks between recorded trace points
	bool preferTaps = true;        // try 'released' first so the route taps only when it must, instead of holding
};

enum class Step { Alive, Crashed, Stopped };

// Why the last step() crashed, plus the box involved. Diagnostics only.
struct Why { const char* what = "none"; float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
inline thread_local Why g_why;

// Advances `s` by exactly one physics tick with the button held (`hold`) or released.
// Works in a "gravity space" where gravity always points down; a flipped player is
// mirrored in y on the way in and mirrored back on the way out.
inline Step step(State& s, bool hold, Level const& L, Cfg const& c) {
	using namespace phys;
	const float ts = 60.f / static_cast<float>(c.tps);
	const float dx = kSpeedUps[s.speed] / static_cast<float>(c.tps);
	const bool press = hold && !s.held;
	s.held = hold;

	const float sg = s.flip ? -1.f : 1.f;
	const float half = s.mini ? 9.f : kHalf;
	const float tiny = s.mini ? 3.f : kSmallHalf;
	const float groundM = s.flip ? -L.ceilY : L.floorY;
	// An upright cube has no ceiling (tall pad/orb jumps are legal); ship, wave and a flipped
	// cube are bounded by it.
	const bool useCeil = s.mode != Mode::Cube || s.flip;
	const float ceilM = !useCeil ? 1.0e9f : (s.flip ? -L.floorY : L.ceilY);
	auto yRange = [&](Box const& b, float& lo, float& hi) {
		lo = s.flip ? -b.y1 : b.y0;
		hi = s.flip ? -b.y0 : b.y1;
	};

	float vm = s.vy * sg;
	float ym = sg * s.y;
	s.x += dx;

	// ---- mode physics ----
	switch (s.mode) {
		case Mode::Cube:
			if (s.onGround && hold) { vm = kJump; s.onGround = false; }
			vm = std::max(vm - kGravity * ts, -kTerminal);
			break;
		case Mode::Ship: {
			// mini ship: acceleration and both speed caps are divided by the vehicle size
			const float vs = s.mini ? kMiniScale : 1.f;
			vm += (hold ? kShipUp : -kShipDown) * ts / vs;
			vm = std::clamp(vm, -kShipMaxDn / vs, kShipMaxUp / vs);
			break;
		}
		case Mode::Wave:
			vm = (hold ? 1.f : -1.f) * kSpeedUps[s.speed] / 60.f; // 45 degree slope
			break;
		default: return Step::Stopped;
	}

	const bool wave = s.mode == Mode::Wave;
	const float hh = wave ? tiny : half;   // wave touches solids with its tiny hitbox

	// ---- horizontal pass: moving forward into a block's side kills ----
	bool dead = false;
	L.query(s.x - hh, s.x + hh, [&](Box const& b, int) {
		if (b.kind != Kind::Solid || dead) return;
		if (s.x + hh <= b.x0 || s.x - hh >= b.x1) return;
		float lo, hi; yRange(b, lo, hi);
		if (ym + hh <= lo || ym - hh >= hi) return;
		if (wave) { dead = true; g_why = { "wave touched solid (side)", b.x0, b.y0, b.x1, b.y1 }; return; }
		float depthBottom = hi - (ym - hh), depthTop = (ym + hh) - lo;
		if (std::min(depthBottom, depthTop) > kStepTol) { dead = true; g_why = { "side hit on solid", b.x0, b.y0, b.x1, b.y1 }; }
	});
	if (dead) return Step::Crashed;

	// ---- vertical pass ----
	ym += vm * ts;
	bool ground = false;
	if (ym - hh <= groundM) { ym = groundM + hh; ground = true; if (!wave) vm = std::max(vm, 0.f); }
	if (ym + hh >= ceilM) { ym = ceilM - hh; if (vm > 0.f && !wave) vm = 0.f; }
	float tol = kStepTol + std::fabs(vm * ts);
	L.query(s.x - hh, s.x + hh, [&](Box const& b, int) {
		if (b.kind != Kind::Solid || dead) return;
		if (s.x + hh <= b.x0 || s.x - hh >= b.x1) return;
		float lo, hi; yRange(b, lo, hi);
		if (ym + hh <= lo || ym - hh >= hi) return;
		if (wave) { dead = true; g_why = { "wave touched solid (vertical)", b.x0, b.y0, b.x1, b.y1 }; return; }
		float depthBottom = hi - (ym - hh), depthTop = (ym + hh) - lo;
		if (std::min(depthBottom, depthTop) > tol) { dead = true; g_why = { "embedded in solid", b.x0, b.y0, b.x1, b.y1 }; return; }
		if (depthBottom <= depthTop) { ym = hi + hh; vm = std::max(vm, 0.f); ground = true; }   // land on top
		else { ym = lo - hh; vm = std::min(vm, 0.f); }                                          // bonk underside
	});
	if (dead) return Step::Crashed;
	s.onGround = ground;

	// ---- hazards (spikes, saws ...) use the tiny player hitbox ----
	L.query(s.x - tiny, s.x + tiny, [&](Box const& b, int) {
		if (b.kind != Kind::Hazard || dead) return;
		if (s.x + tiny <= b.x0 || s.x - tiny >= b.x1) return;
		float lo, hi; yRange(b, lo, hi);
		if (ym + tiny > lo && ym - tiny < hi) { dead = true; g_why = { "hazard", b.x0, b.y0, b.x1, b.y1 }; }
	});
	if (dead) return Step::Crashed;

	// back to real space
	s.y = sg * ym;
	s.vy = sg * vm;

	// ---- portals / pads / orbs (real-space overlap with the full hitbox) ----
	bool touchedPad = false;
	Step result = Step::Alive;
	L.query(s.x - half, s.x + half, [&](Box const& b, int idx) {
		if (b.kind != Kind::Trigger) return;
		if (s.x + half <= b.x0 || s.x - half >= b.x1) return;
		if (s.y + half <= b.y0 || s.y - half >= b.y1) return;
		float up = s.flip ? -1.f : 1.f;
		switch (b.eff) {
			case Eff::ToCube: s.mode = Mode::Cube; break;
			case Eff::ToShip: s.mode = Mode::Ship; break;
			case Eff::ToWave: s.mode = Mode::Wave; break;
			case Eff::ToUnsupported: s.mode = Mode::Unsupported; result = Step::Stopped; break;
			case Eff::GravNormal: if (s.flip) { s.flip = false; s.onGround = false; } break;
			case Eff::GravFlip:   if (!s.flip) { s.flip = true; s.onGround = false; } break;
			case Eff::SizeNormal: s.mini = false; break;
			case Eff::SizeMini:   s.mini = true; break;
			case Eff::Speed:      s.speed = std::min<uint8_t>(b.arg, 4); break;
			case Eff::PadYellow: case Eff::PadPink:
				touchedPad = true;
				if (s.lastPad != idx) {
					s.vy = up * (b.eff == Eff::PadYellow ? kPadYellow : kPadPink);
					s.onGround = false; s.lastPad = idx;
				}
				break;
			case Eff::OrbYellow: case Eff::OrbPink:
				if (press) {
					s.vy = up * (b.eff == Eff::OrbYellow ? kOrbYellow : kOrbPink);
					s.onGround = false;
				}
				break;
			default: break;
		}
	});
	if (!touchedPad) s.lastPad = -1;
	return result;
}

// ------------------------------------- search -------------------------------------

inline uint64_t keyOf(State const& s) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](int64_t v) { h ^= static_cast<uint64_t>(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 1099511628211ull; };
	mix(static_cast<int64_t>(std::floor(s.x / 3.f)));
	mix(static_cast<int64_t>(std::floor(s.y / 2.f)));
	mix(static_cast<int64_t>(std::floor(s.vy / 0.5f)));
	mix(static_cast<int>(s.mode) | (s.flip << 3) | (s.mini << 4) | (s.onGround << 5) | (s.held << 6) | (s.speed << 7));
	return h;
}

struct Node { State s; int parent; uint8_t hold; int toggles; int holds; };

struct QItem {
	float prio; int idx;
	bool operator<(QItem const& o) const { return prio != o.prio ? prio < o.prio : idx > o.idx; }
};

// Best-first search for one window: expand the furthest-right node first (so it behaves like
// depth-first with backtracking), keep the current hold state as the first choice, never
// revisit a quantised state. Returns the decision list (1 = hold) that reaches targetX.
inline bool searchWindow(Level const& L, State const& start, float targetX, Cfg const& c,
	std::unordered_set<uint64_t> const& dead, std::atomic<bool> const& cancel,
	std::atomic<float>& prog, std::vector<uint8_t>& out, bool& stopped, State& furthest)
{
	std::vector<Node> nodes;
	nodes.reserve(std::min<size_t>(c.budget, 1u << 16));
	std::unordered_set<uint64_t> seen;
	std::priority_queue<QItem> pq;
	nodes.push_back({ start, -1, 0, 0, 0 });
	seen.insert(keyOf(start));
	pq.push({ start.x, 0 });

	auto finish = [&](int idx, uint8_t lastHold) {
		out.clear();
		out.push_back(lastHold);
		for (int i = idx; nodes[i].parent >= 0; i = nodes[i].parent)
			if (nodes[nodes[i].parent].parent >= 0) out.push_back(nodes[nodes[i].parent].hold);
		std::reverse(out.begin(), out.end());
	};

	size_t expanded = 0;
	while (!pq.empty()) {
		if ((++expanded & 255) == 0 && cancel.load(std::memory_order_relaxed)) return false;
		if (nodes.size() >= c.budget) return false;
		int cur = pq.top().idx; pq.pop();
		Node n = nodes[cur];
		for (int k = 0; k < 2; k++) {
			bool hold = k == 0 ? n.s.held : !n.s.held;
			State t = n.s;
			Step r = Step::Alive;
			for (int i = 0; i < c.decisionTicks; i++) {
				r = step(t, hold, L, c);
				if (r != Step::Alive || t.x >= L.endX) break; // never cut a decision short at the window edge: replay runs whole decisions
			}
			if (r == Step::Crashed) continue;
			bool goal = t.x >= targetX || r == Step::Stopped;
			uint64_t key = keyOf(t);
			if (!goal && (seen.count(key) || dead.count(key))) continue;
			seen.insert(key);
			nodes.push_back({ t, cur, static_cast<uint8_t>(hold), n.toggles + (hold != n.s.held), n.holds + (hold ? 1 : 0) });
			int idx = static_cast<int>(nodes.size()) - 1;
			if (goal) { stopped = r == Step::Stopped; finish(idx, hold); return true; }
			// Sibling order. Default: keep the current button state (fewest toggles) first, which makes
			// routes that hold through many jumps. preferTaps: fewest held chunks first, so the button is
			// released unless the search needs a jump. Penalties stay far below one node's x advance
			// (~5 units) so the search remains depth-first.
			float pen = c.preferTaps ? 1e-4f * nodes[idx].holds : 0.01f * nodes[idx].toggles;
			pq.push({ t.x - pen, idx });
			prog.store(std::max(prog.load(std::memory_order_relaxed), t.x), std::memory_order_relaxed);
			if (t.x > furthest.x) furthest = t;
		}
	}
	return false;
}

// Human-readable report: the state the search got stuck in and what kills it next.
inline const char* effName(Eff e) {
	switch (e) {
		case Eff::ToCube: return "cube"; case Eff::ToShip: return "SHIP"; case Eff::ToWave: return "WAVE";
		case Eff::ToUnsupported: return "UNSUPPORTED"; case Eff::GravNormal: return "grav-normal";
		case Eff::GravFlip: return "grav-FLIP"; case Eff::SizeNormal: return "size-normal"; case Eff::SizeMini: return "size-mini";
		case Eff::Speed: return "speed"; case Eff::PadYellow: return "pad-yellow"; case Eff::PadPink: return "pad-pink";
		case Eff::OrbYellow: return "orb-yellow"; case Eff::OrbPink: return "orb-pink"; default: return "?";
	}
}

inline std::string diag(Level const& L, State const& s, Cfg const& c) {
	char b[320];
	std::snprintf(b, sizeof b, "stuck at x=%.1f y=%.1f vy=%.2f mode=%d flip=%d mini=%d speed=%d ground=%d\nfloor=%.1f ceil=%.0f end=%.0f boxes=%zu",
		s.x, s.y, s.vy, static_cast<int>(s.mode), s.flip, s.mini, s.speed, s.onGround, L.floorY, L.ceilY, L.endX, L.boxes.size());
	std::string out = b;
	for (int h = 0; h < 2; h++) {
		State t = s; Step r = Step::Alive; int n = 0;
		for (; n < c.decisionTicks * 3; n++) { g_why = {}; r = step(t, h != 0, L, c); if (r != Step::Alive) break; }
		if (r == Step::Crashed)
			std::snprintf(b, sizeof b, "\nhold=%d: %s after %d ticks; box x%.0f..%.0f y%.0f..%.0f; player x=%.1f y=%.1f",
				h, g_why.what, n + 1, g_why.x0, g_why.x1, g_why.y0, g_why.y1, t.x, t.y);
		else
			std::snprintf(b, sizeof b, "\nhold=%d: survives the next %d ticks", h, n);
		out += b;
	}
	// the 6 triggers closest to (at or just before) the stuck spot, to show which portal the sim saw last
	std::vector<Box const*> closest;
	for (auto const& bx : L.boxes)
		if (bx.kind == Kind::Trigger && bx.x0 <= s.x + 150.f && bx.x0 >= s.x - 1500.f) closest.push_back(&bx);
	std::sort(closest.begin(), closest.end(), [](Box const* a, Box const* z) { return a->x0 > z->x0; });
	out += "\ntriggers:";
	for (size_t i = 0; i < closest.size() && i < 6; i++) {
		std::snprintf(b, sizeof b, " %s@%.0f,%.0f", effName(closest[i]->eff), closest[i]->x0, (closest[i]->y0 + closest[i]->y1) * 0.5f);
		out += b;
	}
	return out;
}

struct Input { int tick; bool down; };
struct TracePt { int tick; float x, y, vy; };

struct Result {
	bool ok = false;        // a route to the end of the level was found
	bool partial = false;   // route is valid up to an unsupported gamemode
	std::string msg;
	std::vector<Input> inputs;   // tick 0 = the step the solve started on
	std::vector<TracePt> trace;
	float reachedX = 0.f;
	int ticks = 0;
	int tps = 0;            // physics rate the route was simulated at; macro frames are steps at this rate
};

// Rolling-horizon solve: search a window, commit the first half of it, continue from there.
// If a window has no way through, the state it started from is marked dead and the solver
// backs up one commit and must find a different way into that spot.
inline Result solve(Level const& L, State start, Cfg const& c, std::atomic<bool> const& cancel, std::atomic<float>& prog) {
	Result R;
	auto t0 = std::chrono::steady_clock::now();
	std::unordered_set<uint64_t> dead;
	struct Ck { State s; size_t decCount; };
	std::vector<Ck> ck{ { start, 0 } };
	std::vector<uint8_t> decisions;
	bool partial = false;
	State farS = start;

	while (true) {
		if (cancel.load()) { R.msg = "cancelled"; return R; }
		if (std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() > c.maxSeconds) {
			R.msg = "time limit reached\n" + diag(L, farS, c); R.reachedX = prog.load(); return R;
		}
		State cur = ck.back().s;
		if (cur.x >= L.endX) break;

		float target = std::min(cur.x + c.window, L.endX);
		std::vector<uint8_t> win;
		bool stopped = false;
		if (searchWindow(L, cur, target, c, dead, cancel, prog, win, stopped, farS)) {
			size_t commit = (stopped || target >= L.endX) ? win.size() : std::max<size_t>(1, win.size() / 2);
			State s = cur;
			for (size_t i = 0; i < commit; i++) {
				for (int t = 0; t < c.decisionTicks; t++) {
					Step r = step(s, win[i] != 0, L, c);
					if (r != Step::Alive || s.x >= L.endX) break; // identical rule to searchWindow and the final replay
				}
			}
			decisions.insert(decisions.end(), win.begin(), win.begin() + commit);
			ck.push_back({ s, decisions.size() });
			if (stopped) { partial = true; break; }
		} else {
			if (cancel.load()) { R.msg = "cancelled"; return R; }
			dead.insert(keyOf(cur));
			if (ck.size() == 1) { R.msg = "no route found\n" + diag(L, farS, c); R.reachedX = prog.load(); return R; }
			ck.pop_back();
			decisions.resize(ck.back().decCount);
		}
	}

	// final replay: turn decisions into press/release edges and a position trace
	State s = start;
	bool held = start.held;
	int tick = 0;
	for (size_t i = 0; i < decisions.size(); i++) {
		bool hold = decisions[i] != 0;
		if (hold != held) { R.inputs.push_back({ tick, hold }); held = hold; }
		for (int t = 0; t < c.decisionTicks; t++, tick++) {
			if (tick % std::max(1, c.traceEvery) == 0) R.trace.push_back({ tick, s.x, s.y, s.vy });
			Step r = step(s, hold, L, c);
			if (r != Step::Alive || s.x >= L.endX) { tick++; goto done; }
		}
	}
done:
	R.ok = !partial;
	R.partial = partial;
	R.ticks = tick;
	R.reachedX = s.x;
	R.tps = c.tps;
	R.msg = partial ? "route stops at an unsupported gamemode" : "route found";
	return R;
}

} // namespace pf
