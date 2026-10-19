#include "Settings.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace rdf::game
{
	namespace
	{
		constexpr auto kIniPath = "Data/SKSE/Plugins/ReactiveDodgeFramework.ini";

		Settings g_settings;

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

		void LoadTiming(Settings& s)
		{
			auto& t = s.core.timing;
			t.startup = ReadFloat("Timing", "fStartup", t.startup, 0.0f, 2.0f);
			t.active = ReadFloat("Timing", "fActive", t.active, 0.0f, 3.0f);
			t.recovery = ReadFloat("Timing", "fRecovery", t.recovery, 0.0f, 3.0f);
			t.exitBlend = ReadFloat("Timing", "fExitBlend", t.exitBlend, 0.02f, 1.0f);
			if (t.Total() < 0.1f) {
				logger::warn("[Timing] total duration {} too short, using defaults", t.Total());
				t = Timing{};
			}
			auto& p = s.core.policy;
			p.attackFrom = ReadFloat("Timing", "fAttackFrom", p.attackFrom, 0.0f, 5.0f);
			p.chainFrom = ReadFloat("Timing", "fChainFrom", p.chainFrom, 0.0f, 5.0f);
			p.blockFrom = ReadFloat("Timing", "fBlockFrom", p.blockFrom, 0.0f, 5.0f);
			p.exitFrom = ReadFloat("Timing", "fExitFrom", p.exitFrom, 0.0f, 5.0f);
			p.maxChain = ReadInt("Timing", "iMaxChain", p.maxChain, 0, 100);
			p.attackStateMask = ParseAttackStates(ReadString("Timing", "sAttackStates", AttackStatesToString(p.attackStateMask)));
		}

		void LoadMotionAndSteering(Settings& s)
		{
			auto& m = s.core.motion;
			m.distance = ReadFloat("Motion", "fDistance", m.distance, 10.0f, 2000.0f);
			m.runSpeed = ReadFloat("Motion", "fRunSpeed", m.runSpeed, 0.0f, 1500.0f);
			m.maxCarrySpeed = ReadFloat("Motion", "fMaxCarrySpeed", m.maxCarrySpeed, 0.0f, 1500.0f);
			m.backstepWithoutInput = ReadBool("Motion", "bBackstepWithoutInput", m.backstepWithoutInput);
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
			st.orientation = ParseOrientation(ReadString("Steering", "sOrientation", "Auto"));
		}

		void LoadInputCostGraph(Settings& s)
		{
			s.dodgeKeyboard = ReadInt("Input", "iDodgeKeyboard", s.dodgeKeyboard, -1, 0xFF);
			s.dodgeMouse = ReadInt("Input", "iDodgeMouse", s.dodgeMouse, -1, 0xFF);
			s.dodgeGamepad = ReadInt("Input", "iDodgeGamepad", s.dodgeGamepad, -1, 0xFFFF);
			s.core.input.bufferWindow = ReadFloat("Input", "fBufferWindow", s.core.input.bufferWindow, 0.0f, 2.0f);
			s.core.input.debounce = ReadFloat("Input", "fDebounce", s.core.input.debounce, 0.0f, 1.0f);
			s.powerAttackHold = ReadFloat("Input", "fPowerAttackHold", s.powerAttackHold, 0.05f, 2.0f);

			s.staminaCost = ReadFloat("Cost", "fStaminaCost", s.staminaCost, 0.0f, 1000.0f);
			s.minStamina = ReadFloat("Cost", "fMinStamina", s.minStamina, 0.0f, 1000.0f);

			auto& g = s.graph;
			g.start = ReadString("Graph", "sEventStart", g.start);
			g.chain = ReadString("Graph", "sEventChain", g.chain);
			g.exit = ReadString("Graph", "sEventExit", g.exit);
			g.entered = ReadString("Graph", "sEventEntered", g.entered);
			g.left = ReadString("Graph", "sEventLeft", g.left);
			g.clipEnd = ReadString("Graph", "sEventClipEnd", g.clipEnd);
			g.clip = ReadString("Graph", "sVarClip", g.clip);
			g.speed = ReadString("Graph", "sVarSpeed", g.speed);
			g.isDodging = ReadString("Graph", "sVarIsDodging", g.isDodging);
			g.windowOpen = Split(ReadString("Graph", "sDodgeWindowOpenEvents", Join(g.windowOpen)));
			g.windowClose = Split(ReadString("Graph", "sDodgeWindowCloseEvents", Join(g.windowClose)));
			s.clipLength = ReadFloat("Graph", "fClipLength", s.clipLength, 0.05f, 10.0f);
			s.animSpeed = ReadFloat("Graph", "fAnimSpeed", s.animSpeed, 0.1f, 5.0f);
			s.sidestepWhenKeepFacing = ReadBool("Graph", "bSidestepWhenKeepFacing", s.sidestepWhenKeepFacing);
			s.enterTimeoutFrames = ReadInt("Graph", "iEnterTimeoutFrames", s.enterTimeoutFrames, 1, 120);
		}
	}

	const Settings& Settings::Get()
	{
		return g_settings;
	}

	void Settings::Load()
	{
		Settings s;
		LoadTiming(s);
		LoadMotionAndSteering(s);
		LoadInputCostGraph(s);
		s.iFrames = ReadBool("IFrames", "bIFrames", s.iFrames);
		s.iFrameStart = ReadFloat("IFrames", "fIFrameStart", s.iFrameStart, 0.0f, 5.0f);
		s.iFrameEnd = ReadFloat("IFrames", "fIFrameEnd", s.iFrameEnd, 0.0f, 5.0f);
		s.allowFirstPerson = ReadBool("Compat", "bAllowFirstPerson", s.allowFirstPerson);
		s.suppressLegacyDodge = ReadBool("Compat", "bSuppressLegacyDodge", s.suppressLegacyDodge);
		s.useTrueDirectionalMovement = ReadBool("Compat", "bUseTrueDirectionalMovement", s.useTrueDirectionalMovement);
		s.devLog = ReadBool("Dev", "bDevLog", s.devLog);
		g_settings = std::move(s);

		logger::info("settings: dodge keys kb={} mouse={} pad={} total={:.3f}s distance={:.0f} maxChain={} iframes={} devLog={}",
			g_settings.dodgeKeyboard, g_settings.dodgeMouse, g_settings.dodgeGamepad, g_settings.core.timing.Total(),
			g_settings.core.motion.distance, g_settings.core.policy.maxChain, g_settings.iFrames, g_settings.devLog);
	}
}
