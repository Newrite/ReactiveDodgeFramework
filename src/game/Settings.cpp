#include "Settings.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace rdf::game
{
	namespace
	{
		constexpr auto kIniPath = "Data/SKSE/Plugins/ReactiveDodgeFramework.ini";

		Settings                   g_settings;
		std::mutex                 g_snapshotLock;  // Commit (main thread) vs Snapshot (render thread)
		std::atomic<std::uint32_t> g_generation{ 1 };
		std::vector<std::function<void()>> g_observers;  // main thread only

		std::string ReadString(const char* section, const char* key, const std::string& def)
		{
			char buffer[512]{};
			::GetPrivateProfileStringA(section, key, def.c_str(), buffer, sizeof(buffer), kIniPath);
			return buffer;
		}

		float ReadFloat(const char* section, const char* key, float def, float lo, float hi)
		{
			const auto text = ReadString(section, key, "");
			if (text.empty()) {
				return def;
			}
			char* end = nullptr;
			const float value = std::strtof(text.c_str(), &end);
			if (end == text.c_str() || !std::isfinite(value)) {
				logger::warn("[{}] {}: '{}' is not a number, using {}", section, key, text, def);
				return def;
			}
			const float clamped = std::clamp(value, lo, hi);
			if (clamped != value) {
				logger::warn("[{}] {}: {} out of range [{}, {}], using {}", section, key, value, lo, hi, clamped);
			}
			return clamped;
		}

		// Accepts decimal or 0x hex.
		int ReadInt(const char* section, const char* key, int def, int lo, int hi)
		{
			const auto text = ReadString(section, key, "");
			if (text.empty()) {
				return def;
			}
			char* end = nullptr;
			const long value = std::strtol(text.c_str(), &end, 0);
			if (end == text.c_str()) {
				logger::warn("[{}] {}: '{}' is not an integer, using {}", section, key, text, def);
				return def;
			}
			const int clamped = static_cast<int>(std::clamp<long>(value, lo, hi));
			if (clamped != value) {
				logger::warn("[{}] {}: {} out of range [{}, {}], using {}", section, key, value, lo, hi, clamped);
			}
			return clamped;
		}

		bool ReadBool(const char* section, const char* key, bool def)
		{
			return ReadInt(section, key, def ? 1 : 0, 0, 1) != 0;
		}

		std::vector<std::string> Split(const std::string& text)
		{
			std::vector<std::string> out;
			std::stringstream        ss(text);
			std::string              item;
			while (std::getline(ss, item, ',')) {
				const auto b = item.find_first_not_of(" \t");
				const auto e = item.find_last_not_of(" \t");
				if (b != std::string::npos) {
					out.push_back(item.substr(b, e - b + 1));
				}
			}
			return out;
		}

		std::string Join(const std::vector<std::string>& items)
		{
			std::string out;
			for (const auto& i : items) {
				out += (out.empty() ? "" : ",") + i;
			}
			return out;
		}

		// Names accepted in sAttackStates -> RE::ATTACK_STATE_ENUM values (core mirrors the numbers).
		struct StateName
		{
			const char*   name;
			std::uint8_t  value;
			RE::ATTACK_STATE_ENUM engine;
		};
		constexpr StateName kStates[] = {
			{ "None", attack_state::kNone, RE::ATTACK_STATE_ENUM::kNone },
			{ "Draw", attack_state::kDraw, RE::ATTACK_STATE_ENUM::kDraw },
			{ "Swing", attack_state::kSwing, RE::ATTACK_STATE_ENUM::kSwing },
			{ "Hit", attack_state::kHit, RE::ATTACK_STATE_ENUM::kHit },
			{ "NextAttack", attack_state::kNextAttack, RE::ATTACK_STATE_ENUM::kNextAttack },
			{ "FollowThrough", attack_state::kFollowThrough, RE::ATTACK_STATE_ENUM::kFollowThrough },
			{ "Bash", attack_state::kBash, RE::ATTACK_STATE_ENUM::kBash },
			{ "BowDraw", attack_state::kBowDraw, RE::ATTACK_STATE_ENUM::kBowDraw },
			{ "BowAttached", attack_state::kBowAttached, RE::ATTACK_STATE_ENUM::kBowAttached },
			{ "BowDrawn", attack_state::kBowDrawn, RE::ATTACK_STATE_ENUM::kBowDrawn },
			{ "BowReleasing", attack_state::kBowReleasing, RE::ATTACK_STATE_ENUM::kBowReleasing },
			{ "BowReleased", attack_state::kBowReleased, RE::ATTACK_STATE_ENUM::kBowReleased },
			{ "BowNextAttack", attack_state::kBowNextAttack, RE::ATTACK_STATE_ENUM::kBowNextAttack },
			{ "BowFollowThrough", attack_state::kBowFollowThrough, RE::ATTACK_STATE_ENUM::kBowFollowThrough },
			{ "Fire", attack_state::kFire, RE::ATTACK_STATE_ENUM::kFire },
			{ "Firing", attack_state::kFiring, RE::ATTACK_STATE_ENUM::kFiring },
			{ "Fired", attack_state::kFired, RE::ATTACK_STATE_ENUM::kFired },
		};

		constexpr bool StatesMatchEngine()
		{
			for (const auto& s : kStates) {
				if (static_cast<std::uint32_t>(s.engine) != s.value) {
					return false;
				}
			}
			return true;
		}
		static_assert(StatesMatchEngine(), "core attack_state values must mirror RE::ATTACK_STATE_ENUM");

		std::uint32_t ParseAttackStates(const std::string& text)
		{
			std::uint32_t mask = 0;
			for (const auto& item : Split(text)) {
				const auto it = std::find_if(std::begin(kStates), std::end(kStates), [&](const StateName& s) {
					return _stricmp(s.name, item.c_str()) == 0;
				});
				if (it == std::end(kStates)) {
					logger::warn("[Timing] sAttackStates: unknown state '{}'", item);
				} else {
					mask |= attack_state::Bit(it->value);
				}
			}
			return mask;
		}

		std::string AttackStatesToString(std::uint32_t mask)
		{
			std::vector<std::string> names;
			for (const auto& s : kStates) {
				if (s.value != attack_state::kNone && (mask & attack_state::Bit(s.value))) {
					names.push_back(s.name);
				}
			}
			return Join(names);
		}

		OrientationMode ParseOrientation(const std::string& text)
		{
			if (_stricmp(text.c_str(), "FaceMotion") == 0) return OrientationMode::FaceMotion;
			if (_stricmp(text.c_str(), "KeepFacing") == 0) return OrientationMode::KeepFacing;
			if (_stricmp(text.c_str(), "Auto") != 0) {
				logger::warn("[Steering] sOrientation: unknown mode '{}', using Auto", text);
			}
			return OrientationMode::Auto;
		}

		std::optional<RE::ActorValue> ParseResource(const std::string& text)
		{
			if (_stricmp(text.c_str(), "None") == 0) return std::nullopt;
			if (_stricmp(text.c_str(), "Magicka") == 0) return RE::ActorValue::kMagicka;
			if (_stricmp(text.c_str(), "Stamina") != 0) {
				logger::warn("[Cost] sResource: unknown resource '{}', using Stamina", text);
			}
			return RE::ActorValue::kStamina;
		}

		CostShortfall ParseShortfall(const std::string& text)
		{
			if (_stricmp(text.c_str(), "Refuse") == 0) return CostShortfall::Refuse;
			if (_stricmp(text.c_str(), "Always") == 0) return CostShortfall::Always;
			if (_stricmp(text.c_str(), "Minimum") != 0) {
				logger::warn("[Cost] sShortfall: unknown '{}', using Minimum", text);
			}
			return CostShortfall::Minimum;
		}

		void LoadTiming(Settings& s)
		{
			auto& t = s.core.timing;
			t.startup = ReadFloat("Timing", "fStartup", t.startup, 0.0f, 2.0f);
			t.active = ReadFloat("Timing", "fActive", t.active, 0.0f, 3.0f);
			t.recovery = ReadFloat("Timing", "fRecovery", t.recovery, 0.0f, 3.0f);
			t.exitBlend = ReadFloat("Timing", "fExitBlend", t.exitBlend, 0.02f, 1.0f);
			t.actionExitBlend = ReadFloat("Timing", "fActionExitBlend", t.actionExitBlend, 0.02f, 1.0f);
			t.attackExitBlend = ReadFloat("Timing", "fAttackExitBlend", t.attackExitBlend, 0.0f, 1.0f);
			if (t.Total() < 0.1f) {
				logger::warn("[Timing] total duration {} too short, using defaults", t.Total());
				t = Timing{};
			}
			auto& p = s.core.policy;
			p.attackFrom = ReadFloat("Timing", "fAttackFrom", p.attackFrom, 0.0f, 5.0f);
			p.chainFrom = ReadFloat("Timing", "fChainFrom", p.chainFrom, 0.0f, 5.0f);
			p.blockFrom = ReadFloat("Timing", "fBlockFrom", p.blockFrom, 0.0f, 5.0f);
			p.exitFrom = ReadFloat("Timing", "fExitFrom", p.exitFrom, 0.0f, 5.0f);
			// iChainDodges: chained dodges after the first (-1 = unlimited). An ini without it still has the old
			// iMaxChain (dodges in a row, 0 = unlimited).
			const int legacy = ReadInt("Timing", "iMaxChain", p.chainDodges + 1, 0, 100);
			p.chainDodges = ReadInt("Timing", "iChainDodges", legacy == 0 ? -1 : legacy - 1, -1, 100);
			p.attackStateMask = ParseAttackStates(ReadString("Timing", "sAttackStates", AttackStatesToString(p.attackStateMask)));
		}

		void LoadMotionAndSteering(Settings& s)
		{
			auto& m = s.core.motion;
			m.distance = ReadFloat("Motion", "fDistance", m.distance, 10.0f, 2000.0f);
			m.runSpeed = ReadFloat("Motion", "fRunSpeed", m.runSpeed, 0.0f, 1500.0f);
			m.maxCarrySpeed = ReadFloat("Motion", "fMaxCarrySpeed", m.maxCarrySpeed, 0.0f, 1500.0f);
			m.backstepWithoutInput = ReadBool("Motion", "bBackstepWithoutInput", m.backstepWithoutInput);
			s.carryDecay = ReadFloat("Motion", "fCarryDecay", s.carryDecay, 0.0f, 100000.0f);
			s.yawSign = ReadInt("Motion", "iYawSign", s.yawSign, -1, 1);
			if (s.yawSign == 0) {
				logger::warn("[Motion] iYawSign 0 is invalid, using 1");
				s.yawSign = 1;
			}

			auto& st = s.core.steering;
			st.rateStartup = ReadFloat("Steering", "fRateStartup", st.rateStartup, 1.0f, 5000.0f);
			st.rateActive = ReadFloat("Steering", "fRateActive", st.rateActive, 1.0f, 5000.0f);
			st.rateRecovery = ReadFloat("Steering", "fRateRecovery", st.rateRecovery, 1.0f, 5000.0f);
			st.deadzone = ReadFloat("Steering", "fDeadzone", st.deadzone, 0.0f, 0.9f);
			st.responseExponent = ReadFloat("Steering", "fResponseExponent", st.responseExponent, 0.1f, 5.0f);
			st.startupYawRate = ReadFloat("Steering", "fStartupYawRate", st.startupYawRate, 1.0f, 100000.0f);
			st.faceYawRate = ReadFloat("Steering", "fFaceYawRate", st.faceYawRate, 1.0f, 5000.0f);
			st.faceMotionMaxAngle = ReadFloat("Steering", "fFaceMotionMaxAngle", st.faceMotionMaxAngle, 0.0f, 180.0f);
			st.maxDeviation = ReadFloat("Steering", "fMaxDeviation", st.maxDeviation, 0.0f, 180.0f);
			st.inputCone = ReadFloat("Steering", "fInputCone", st.inputCone, 0.0f, 180.0f);
			st.holdFacing = ReadBool("Steering", "bHoldFacing", st.holdFacing);
			st.commitTime = ReadFloat("Steering", "fDirectionCommitTime", st.commitTime, 0.0f, 0.5f);
			st.orientationWarpMax = ReadFloat("Steering", "fOrientationWarpMax", st.orientationWarpMax, 0.0f, 90.0f);
			s.inputFromStartCamera = ReadBool("Steering", "bInputFromStartCamera", s.inputFromStartCamera);
			st.orientation = ParseOrientation(ReadString("Steering", "sOrientation", "Auto"));
		}

		void LoadInputCostGraph(Settings& s)
		{
			s.dodgeKeyboard = ReadInt("Input", "iDodgeKeyboard", s.dodgeKeyboard, -1, 0xFF);
			s.dodgeMouse = ReadInt("Input", "iDodgeMouse", s.dodgeMouse, -1, 0xFF);
			s.dodgeGamepad = ReadInt("Input", "iDodgeGamepad", s.dodgeGamepad, -1, 0xFFFF);
			s.core.input.bufferWindow = ReadFloat("Input", "fBufferWindow", s.core.input.bufferWindow, 0.0f, 2.0f);
			s.core.input.debounce = ReadFloat("Input", "fDebounce", s.core.input.debounce, 0.0f, 1.0f);
			s.core.input.holdDuringDodge = ReadBool("Input", "bHoldPressesDuringDodge", s.core.input.holdDuringDodge);
			s.powerAttackHold = ReadFloat("Input", "fPowerAttackHold", s.powerAttackHold, 0.05f, 2.0f);
			s.idleReplayChains = Split(ReadString("Input", "sIdleReplayChains", Join(s.idleReplayChains)));

			s.costResource = ParseResource(ReadString("Cost", "sResource", "Stamina"));
			auto& c = s.price;
			c.flat = ReadFloat("Cost", "fCost", c.flat, 0.0f, 1000.0f);
			c.maxPercent = ReadFloat("Cost", "fCostMaxPercent", c.maxPercent, 0.0f, 100.0f);
			c.weightMult = ReadFloat("Cost", "fCostWeightMult", c.weightMult, 0.0f, 10.0f);
			c.chainStep = ReadFloat("Cost", "fChainCostStep", c.chainStep, 0.0f, 10.0f);
			c.overencumberedMult = ReadFloat("Cost", "fOverencumberedMult", c.overencumberedMult, 0.0f, 10.0f);
			c.min = ReadFloat("Cost", "fCostMin", c.min, 0.0f, 1000.0f);
			c.max = ReadFloat("Cost", "fCostMax", c.max, 0.0f, 1000.0f);
			c.shortfall = ParseShortfall(ReadString("Cost", "sShortfall", "Minimum"));
			c.minimum = ReadFloat("Cost", "fMinimum", c.minimum, 0.0f, 1000.0f);
			c.perfectRefund = ReadFloat("Cost", "fPerfectRefund", c.perfectRefund, 0.0f, 100.0f);
			s.costKeyword = ReadString("Cost", "sCostKeyword", s.costKeyword);
			s.costMultKeyword = ReadString("Cost", "sCostMultKeyword", s.costMultKeyword);
			s.chainCostKeyword = ReadString("Cost", "sChainCostKeyword", s.chainCostKeyword);
			s.chainDodgesKeyword = ReadString("Timing", "sChainDodgesKeyword", s.chainDodgesKeyword);

			auto& g = s.graph;
			g.start = ReadString("Graph", "sEventStart", g.start);
			g.startB = ReadString("Graph", "sEventStartB", g.startB);
			g.chain = ReadString("Graph", "sEventChain", g.chain);
			g.exit = ReadString("Graph", "sEventExit", g.exit);
			g.oneHand = ReadBool("Graph", "bOneHandDodge", g.oneHand);
			g.start1H = ReadString("Graph", "sEventStart1H", g.start1H);
			g.startB1H = ReadString("Graph", "sEventStartB1H", g.startB1H);
			g.startBow = ReadString("Graph", "sEventStartBow", g.startBow);
			g.startBowB = ReadString("Graph", "sEventStartBowB", g.startBowB);
			g.exitBow = ReadString("Graph", "sEventExitBow", g.exitBow);
			g.startMagic = ReadString("Graph", "sEventStartMagic", g.startMagic);
			g.startMagicB = ReadString("Graph", "sEventStartMagicB", g.startMagicB);
			g.exitMagic = ReadString("Graph", "sEventExitMagic", g.exitMagic);
			g.layer = ReadString("Graph", "sVarDodgeLayer", g.layer);
			g.holdEvents = Split(ReadString("Graph", "sHoldEvents", Join(g.holdEvents)));
			g.entered = ReadString("Graph", "sEventEntered", g.entered);
			g.left = ReadString("Graph", "sEventLeft", g.left);
			g.clip = ReadString("Graph", "sVarClip", g.clip);
			g.speed = ReadString("Graph", "sVarSpeed", g.speed);
			g.armBlend = ReadString("Graph", "sVarArmBlend", g.armBlend);
			g.isDodging = ReadString("Graph", "sVarIsDodging", g.isDodging);
			g.chainIndex = ReadString("Graph", "sVarChainIndex", g.chainIndex);
			g.attackFromDodge = ReadString("Graph", "sVarAttackFromDodge", g.attackFromDodge);
			g.attackFromDodgeClip = ReadString("Graph", "sVarAttackFromDodgeClip", g.attackFromDodgeClip);
			g.windowOpen = Split(ReadString("Graph", "sDodgeWindowOpenEvents", Join(g.windowOpen)));
			g.windowClose = Split(ReadString("Graph", "sDodgeWindowCloseEvents", Join(g.windowClose)));
			s.clipLength = ReadFloat("Graph", "fClipLength", s.clipLength, 0.05f, 10.0f);
			s.animSpeed = ReadFloat("Graph", "fAnimSpeed", s.animSpeed, 0.1f, 5.0f);
			// fArmStanceStep: its name before the roll / step clips became one set (an older ini)
			s.armStance = ReadFloat("Graph", "fArmStance", ReadFloat("Graph", "fArmStanceStep", s.armStance, 0.0f, 1.0f), 0.0f, 1.0f);
			s.armStanceFirstPerson = ReadFloat("Graph", "fArmStanceFirstPerson", s.armStanceFirstPerson, 0.0f, 1.0f);
			s.firstPersonLookScale = ReadFloat("Graph", "fFirstPersonLookScale", s.firstPersonLookScale, -5.0f, 5.0f);
			s.clipLengthFirstPerson = ReadFloat("Graph", "fClipLengthFirstPerson", s.clipLengthFirstPerson, 0.05f, 10.0f);
			s.armStanceWhenSheathed = ReadBool("Graph", "bArmStanceWhenSheathed", s.armStanceWhenSheathed);
			s.clipMotion = ReadBool("Graph", "bClipMotion", s.clipMotion);
			s.enterTimeoutFrames = ReadInt("Graph", "iEnterTimeoutFrames", s.enterTimeoutFrames, 1, 120);
		}
	}

	const Settings& Settings::Get()
	{
		return g_settings;
	}

	void Settings::Commit(const Settings& s)
	{
		{
			std::scoped_lock lock(g_snapshotLock);
			g_settings = s;
		}
		g_generation.fetch_add(1, std::memory_order_acq_rel);
		for (const auto& observer : g_observers) {
			observer();
		}
	}

	void Settings::Observe(std::function<void()> observer)
	{
		g_observers.push_back(std::move(observer));
	}

	std::uint32_t Settings::Generation()
	{
		return g_generation.load(std::memory_order_acquire);
	}

	Settings Settings::Snapshot()
	{
		std::scoped_lock lock(g_snapshotLock);
		return g_settings;
	}

	void Settings::Load()
	{
		Settings s;
		LoadTiming(s);
		LoadMotionAndSteering(s);
		LoadInputCostGraph(s);
		s.iFrames = ReadBool("IFrames", "bIFrames", s.iFrames);
		s.projectileIFrames = ReadBool("IFrames", "bProjectileIFrames", s.projectileIFrames);
		s.explosionIFrames = ReadBool("IFrames", "bExplosionIFrames", s.explosionIFrames);
		s.iFrameStart = ReadFloat("IFrames", "fIFrameStart", s.iFrameStart, 0.0f, 5.0f);
		s.iFrameEnd = ReadFloat("IFrames", "fIFrameEnd", s.iFrameEnd, 0.0f, 5.0f);
		s.iFrameKeyword = ReadString("IFrames", "sIFrameKeyword", s.iFrameKeyword);
		s.allowFirstPerson = ReadBool("Compat", "bAllowFirstPerson", s.allowFirstPerson);
		s.blockJumpDuringDodge = ReadBool("Jump", "bBlockDuringDodge", s.blockJumpDuringDodge);
		s.dodgeFromStagger = ReadBool("Stagger", "bDodgeFromStagger", s.dodgeFromStagger);
		s.staggerDodgeDelay = ReadFloat("Stagger", "fDodgeDelay", s.staggerDodgeDelay, -1.0f, 10.0f);
		{
			auto& st = s.stagger;
			st.noEscapeKeyword = ReadString("Stagger", "sNoEscapeKeyword", st.noEscapeKeyword);
			st.anyTimeKeyword = ReadString("Stagger", "sAnyTimeKeyword", st.anyTimeKeyword);
			st.delayKeyword = ReadString("Stagger", "sDelayKeyword", st.delayKeyword);
			st.recoveryVariables = Split(ReadString("Stagger", "sRecoveryVariables", Join(st.recoveryVariables)));
			st.recoveryOpenEvents = Split(ReadString("Stagger", "sRecoveryOpenEvents", Join(st.recoveryOpenEvents)));
			st.recoveryCloseEvents = Split(ReadString("Stagger", "sRecoveryCloseEvents", Join(st.recoveryCloseEvents)));
			st.staggerStartEvents = Split(ReadString("Stagger", "sStaggerStartEvents", Join(st.staggerStartEvents)));
			s.price.escapeFlat = ReadFloat("Stagger", "fEscapeCost", s.price.escapeFlat, 0.0f, 1000.0f);
			s.price.escapeMult = ReadFloat("Stagger", "fEscapeCostMult", s.price.escapeMult, 0.0f, 10.0f);
			st.escapeCostKeyword = ReadString("Stagger", "sEscapeCostKeyword", st.escapeCostKeyword);
			st.escapeCostMultKeyword = ReadString("Stagger", "sEscapeCostMultKeyword", st.escapeCostMultKeyword);
		}
		s.casting.drawKeyword = ReadString("Casting", "sDrawDuringDodgeKeyword", s.casting.drawKeyword);
		s.casting.castKeyword = ReadString("Casting", "sCastDuringDodgeKeyword", s.casting.castKeyword);
		s.casting.castingVariables = Split(ReadString("Casting", "sCastingVariables", Join(s.casting.castingVariables)));
		s.castOn = ReadBool("CastOn", "bCastOn", s.castOn);
		s.suppressLegacyDodge = ReadBool("Compat", "bSuppressLegacyDodge", s.suppressLegacyDodge);
		s.useTrueDirectionalMovement = ReadBool("Compat", "bUseTrueDirectionalMovement", s.useTrueDirectionalMovement);
		s.usePrecision = ReadBool("Compat", "bUsePrecision", s.usePrecision);
		auto& ac = s.attackCancel;
		s.core.policy.cancelInRecovery = ReadBool("AttackCancel", "bDodgeInRecovery", s.core.policy.cancelInRecovery);
		ac.recoveryEvents = Split(ReadString("AttackCancel", "sRecoveryEvents", Join(ac.recoveryEvents)));
		ac.recoveryEndEvents = Split(ReadString("AttackCancel", "sRecoveryEndEvents", Join(ac.recoveryEndEvents)));
		ac.recoveryVariables = Split(ReadString("AttackCancel", "sRecoveryVariables", Join(ac.recoveryVariables)));
		ac.attackStartEvents = Split(ReadString("AttackCancel", "sAttackStartEvents", Join(ac.attackStartEvents)));
		ac.anyTimeKeyword = ReadString("AttackCancel", "sAnyTimeKeyword", ac.anyTimeKeyword);
		ac.anyTimePowerKeyword = ReadString("AttackCancel", "sAnyTimePowerKeyword", ac.anyTimePowerKeyword);
		ac.noDodgeKeyword = ReadString("Rules", "sNoDodgeKeyword", ac.noDodgeKeyword);
		ac.perfectOnlyKeyword = ReadString("Rules", "sPerfectOnlyKeyword", ac.perfectOnlyKeyword);
		ac.undodgeableKeyword = ReadString("Rules", "sUndodgeableKeyword", ac.undodgeableKeyword);
		auto& pf = s.perfect;
		pf.enabled = ReadBool("Perfect", "bPerfectDodge", pf.enabled);
		pf.windowStart = ReadFloat("Perfect", "fPerfectWindowStart", pf.windowStart, 0.0f, 3.0f);
		pf.windowEnd = ReadFloat("Perfect", "fPerfectWindowEnd", pf.windowEnd, 0.0f, 3.0f);
		pf.keyword = ReadString("Perfect", "sPerfectDodgeKeyword", pf.keyword);
		pf.windowKeyword = ReadString("Perfect", "sPerfectWindowKeyword", pf.windowKeyword);
		pf.slowMotion = ReadFloat("Perfect", "fPerfectSlowMotion", pf.slowMotion, 0.05f, 1.0f);
		pf.slowMotionPlayer = ReadFloat("Perfect", "fPerfectSlowMotionPlayer", pf.slowMotionPlayer, 0.05f, 1.0f);
		pf.slowMotionDuration = ReadFloat("Perfect", "fPerfectSlowMotionDuration", pf.slowMotionDuration, 0.0f, 5.0f);
		pf.slowMotionEaseIn = ReadFloat("Perfect", "fPerfectSlowMotionEaseIn", pf.slowMotionEaseIn, 0.0f, 2.0f);
		pf.slowMotionEaseOut = ReadFloat("Perfect", "fPerfectSlowMotionEaseOut", pf.slowMotionEaseOut, 0.0f, 2.0f);
		pf.slowMotionCooldown = ReadFloat("Perfect", "fPerfectSlowMotionCooldown", pf.slowMotionCooldown, 0.0f, 30.0f);
		pf.playerEvent = ReadString("Perfect", "sPlayerEvent", pf.playerEvent);
		pf.attackerEvent = ReadString("Perfect", "sAttackerEvent", pf.attackerEvent);
		pf.modEvent = ReadString("Perfect", "sModEvent", pf.modEvent);
		s.devLog = ReadBool("Dev", "bDevLog", s.devLog);
		Commit(s);

		logger::info("settings: dodge keys kb={} mouse={} pad={} total={:.3f}s distance={:.0f} chainDodges={} iframes={} devLog={}",
			g_settings.dodgeKeyboard, g_settings.dodgeMouse, g_settings.dodgeGamepad, g_settings.core.timing.Total(),
			g_settings.core.motion.distance, g_settings.core.policy.chainDodges, g_settings.iFrames, g_settings.devLog);
	}
}
