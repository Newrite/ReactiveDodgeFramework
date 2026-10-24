#include "SettingsSchema.h"

#include "core/IniWriter.h"

#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace rdf::game::schema
{
	namespace
	{
		constexpr auto kIniPath = "Data/SKSE/Plugins/ReactiveDodgeFramework.ini";

#define RDF_F(member) +[](Settings& s) -> float* { return &s.member; }
#define RDF_I(member) +[](Settings& s) -> int* { return &s.member; }
#define RDF_B(member) +[](Settings& s) -> bool* { return &s.member; }
#define RDF_S(member) +[](Settings& s) -> std::string* { return &s.member; }

		Field Float(Page page, const char* section, const char* key, float* (*f)(Settings&), float min, float max, const char* format)
		{
			Field x{ page, section, key, Kind::Float, min, max, format };
			x.f = f;
			return x;
		}

		Field Int(Page page, const char* section, const char* key, int* (*i)(Settings&), int min, int max)
		{
			Field x{ page, section, key, Kind::Int, static_cast<float>(min), static_cast<float>(max), "%d" };
			x.i = i;
			return x;
		}

		Field Bool(Page page, const char* section, const char* key, bool* (*b)(Settings&))
		{
			Field x{ page, section, key, Kind::Bool };
			x.b = b;
			return x;
		}

		Field Form(Page page, const char* section, const char* key, std::string* (*str)(Settings&), RE::FormType type)
		{
			Field x{ page, section, key, Kind::Form };
			x.str = str;
			x.formType = type;
			return x;
		}

		Field Color(Page page, const char* section, const char* key, float* (*rgba)(Settings&))
		{
			Field x{ page, section, key, Kind::Color };
			x.f = rgba;  // four floats from here on
			return x;
		}

		Field Named(Field f, const char* textKey)
		{
			f.textKey = textKey;
			return f;
		}

		Field Custom(Field f)
		{
			f.custom = true;
			return f;
		}

		constexpr std::array<const char*, 3> kOrientations{ "Auto", "FaceMotion", "KeepFacing" };
		constexpr std::array<const char*, 3> kResources{ "None", "Stamina", "Magicka" };
		constexpr std::array<const char*, 3> kShortfalls{ "Refuse", "Minimum", "Always" };  // CostShortfall order
		constexpr std::array<const char*, 2> kTriggers{ "Press", "Tap" };                   // DodgeTrigger order
		// sDodgeKeyAction: the game's user events by their ini names (KeyActionEvent turns them into the game's own)
		constexpr std::array<const char*, 8> kKeyActions{ "None", "Sprint", "Sneak", "Shout", "Activate", "ReadyWeapon", "TogglePOV", "Jump" };

		Field Orientation()
		{
			Field x{ Page::Steering, "Steering", "sOrientation", Kind::Choice };
			x.getChoice = [](const Settings& s) { return static_cast<int>(s.core.steering.orientation); };
			x.setChoice = [](Settings& s, int v) { s.core.steering.orientation = static_cast<OrientationMode>(v); };
			x.choices = kOrientations;
			return x;
		}

		Field Resource()
		{
			Field x{ Page::Rules, "Cost", "sResource", Kind::Choice };
			x.getChoice = [](const Settings& s) {
				if (!s.costResource) {
					return 0;
				}
				return *s.costResource == RE::ActorValue::kMagicka ? 2 : 1;
			};
			x.setChoice = [](Settings& s, int v) {
				s.costResource = v == 0 ? std::nullopt :
				                 v == 2 ? std::optional<RE::ActorValue>(RE::ActorValue::kMagicka) :
				                          std::optional<RE::ActorValue>(RE::ActorValue::kStamina);
			};
			x.choices = kResources;
			return x;
		}

		Field Trigger()
		{
			Field x{ Page::Input, "Input", "sDodgeTrigger", Kind::Choice };
			x.getChoice = [](const Settings& s) { return static_cast<int>(s.dodgeTrigger); };
			x.setChoice = [](Settings& s, int v) { s.dodgeTrigger = v == 1 ? Settings::DodgeTrigger::Tap : Settings::DodgeTrigger::Press; };
			x.choices = kTriggers;
			return x;
		}

		Field KeyAction()
		{
			Field x{ Page::Input, "Input", "sDodgeKeyAction", Kind::Choice };
			x.getChoice = [](const Settings& s) {
				for (int i = 1; i < static_cast<int>(kKeyActions.size()); ++i) {
					if (_stricmp(s.dodgeKeyAction.c_str(), kKeyActions[i]) == 0) {
						return i;
					}
				}
				return 0;
			};
			x.setChoice = [](Settings& s, int v) { s.dodgeKeyAction = v > 0 && v < static_cast<int>(kKeyActions.size()) ? kKeyActions[v] : ""; };
			x.choices = kKeyActions;
			return x;
		}

		Field Shortfall()
		{
			Field x{ Page::Rules, "Cost", "sShortfall", Kind::Choice };
			x.getChoice = [](const Settings& s) { return static_cast<int>(s.price.shortfall); };
			x.setChoice = [](Settings& s, int v) { s.price.shortfall = static_cast<CostShortfall>(std::clamp(v, 0, 2)); };
			x.choices = kShortfalls;
			return x;
		}

		// Ranges are the menu's slider ranges; the ini loader's own clamps are wider or equal.
		const std::vector<Field>& Table()
		{
			static const std::vector<Field> fields{
				// --- input: the dodge keys are drawn as one FLICK hotkey widget (Menu.cpp)
				Custom(Int(Page::Input, "Input", "iDodgeKeyboard", RDF_I(dodgeKeyboard), -1, 0xFF)),
				Custom(Int(Page::Input, "Input", "iDodgeMouse", RDF_I(dodgeMouse), -1, 0xFF)),
				Custom(Int(Page::Input, "Input", "iDodgeGamepad", RDF_I(dodgeGamepad), -1, 0xFFFF)),
				Float(Page::Input, "Input", "fBufferWindow", RDF_F(core.input.bufferWindow), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Input, "Input", "fDebounce", RDF_F(core.input.debounce), 0.0f, 0.3f, "%.2f s"),
				Bool(Page::Input, "Input", "bHoldPressesDuringDodge", RDF_B(core.input.holdDuringDodge)),
				Float(Page::Input, "Input", "fPowerAttackHold", RDF_F(powerAttackHold), 0.05f, 1.0f, "%.2f s"),
				KeyAction(),
				Trigger(),
				Float(Page::Input, "Input", "fTapMax", RDF_F(tapMax), 0.05f, 1.0f, "%.2f s"),
				Bool(Page::Input, "Input", "bDoubleTapDodge", RDF_B(doubleTap)),
				Float(Page::Input, "Input", "fDoubleTapWindow", RDF_F(doubleTapWindow), 0.05f, 1.0f, "%.2f s"),
				// --- timing and windows
				Float(Page::Timing, "Timing", "fStartup", RDF_F(core.timing.startup), 0.0f, 0.5f, "%.3f s"),
				Float(Page::Timing, "Timing", "fActive", RDF_F(core.timing.active), 0.0f, 1.0f, "%.3f s"),
				Float(Page::Timing, "Timing", "fRecovery", RDF_F(core.timing.recovery), 0.0f, 1.0f, "%.3f s"),
				Float(Page::Timing, "Timing", "fExitBlend", RDF_F(core.timing.exitBlend), 0.02f, 1.0f, "%.2f s"),
				Float(Page::Timing, "Timing", "fActionExitBlend", RDF_F(core.timing.actionExitBlend), 0.02f, 1.0f, "%.2f s"),
				Float(Page::Timing, "Timing", "fAttackExitBlend", RDF_F(core.timing.attackExitBlend), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Timing, "Timing", "fChainFrom", RDF_F(core.policy.chainFrom), 0.0f, 1.5f, "%.2f s"),
				Float(Page::Timing, "Timing", "fAttackFrom", RDF_F(core.policy.attackFrom), 0.0f, 1.5f, "%.2f s"),
				Float(Page::Timing, "Timing", "fBlockFrom", RDF_F(core.policy.blockFrom), 0.0f, 1.5f, "%.2f s"),
				Float(Page::Timing, "Timing", "fExitFrom", RDF_F(core.policy.exitFrom), 0.0f, 1.5f, "%.2f s"),
				Int(Page::Timing, "Timing", "iChainDodges", RDF_I(core.policy.chainDodges), -1, 10),
				Float(Page::Timing, "Timing", "fChainPressFrom", RDF_F(core.policy.chainPressFrom), 0.0f, 1.0f, "%.2f s"),
				Form(Page::Timing, "Timing", "sChainDodgesKeyword", RDF_S(chainDodgesKeyword), RE::FormType::Keyword),
				Float(Page::Timing, "IFrames", "fIFrameStart", RDF_F(iFrameStart), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Timing, "IFrames", "fIFrameEnd", RDF_F(iFrameEnd), 0.0f, 1.5f, "%.2f s"),
				Form(Page::Timing, "IFrames", "sIFrameKeyword", RDF_S(iFrameKeyword), RE::FormType::Keyword),
				// --- motion
				Float(Page::Motion, "Motion", "fDistance", RDF_F(core.motion.distance), 50.0f, 1000.0f, "%.0f"),
				Float(Page::Motion, "Motion", "fRunSpeed", RDF_F(core.motion.runSpeed), 0.0f, 800.0f, "%.0f"),
				Float(Page::Motion, "Motion", "fMaxCarrySpeed", RDF_F(core.motion.maxCarrySpeed), 0.0f, 1000.0f, "%.0f"),
				Float(Page::Motion, "Motion", "fCarryDecay", RDF_F(carryDecay), 0.0f, 5000.0f, "%.0f"),
				Bool(Page::Motion, "Motion", "bBackstepWithoutInput", RDF_B(core.motion.backstepWithoutInput)),
				// --- steering
				Float(Page::Steering, "Steering", "fDirectionCommitTime", RDF_F(core.steering.commitTime), 0.0f, 0.3f, "%.2f s"),
				Float(Page::Steering, "Steering", "fMaxDeviation", RDF_F(core.steering.maxDeviation), 0.0f, 180.0f, "%.0f deg"),
				Float(Page::Steering, "Steering", "fInputCone", RDF_F(core.steering.inputCone), 0.0f, 180.0f, "%.0f deg"),
				Bool(Page::Steering, "Steering", "bInputFromStartCamera", RDF_B(inputFromStartCamera)),
				Bool(Page::Steering, "Steering", "bHoldFacing", RDF_B(core.steering.holdFacing)),
				Float(Page::Steering, "Steering", "fOrientationWarpMax", RDF_F(core.steering.orientationWarpMax), 0.0f, 90.0f, "%.1f deg"),
				Orientation(),
				Float(Page::Steering, "Steering", "fFaceMotionMaxAngle", RDF_F(core.steering.faceMotionMaxAngle), 0.0f, 180.0f, "%.0f deg"),
				Float(Page::Steering, "Steering", "fRateStartup", RDF_F(core.steering.rateStartup), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fRateActive", RDF_F(core.steering.rateActive), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fRateRecovery", RDF_F(core.steering.rateRecovery), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fStartupYawRate", RDF_F(core.steering.startupYawRate), 1.0f, 10000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fFaceYawRate", RDF_F(core.steering.faceYawRate), 1.0f, 3000.0f, "%.0f deg/s"),
				Float(Page::Steering, "Steering", "fDeadzone", RDF_F(core.steering.deadzone), 0.0f, 0.9f, "%.2f"),
				Float(Page::Steering, "Steering", "fResponseExponent", RDF_F(core.steering.responseExponent), 0.1f, 5.0f, "%.2f"),
				// --- animation
				Float(Page::Animation, "Graph", "fAnimSpeed", RDF_F(animSpeed), 0.1f, 3.0f, "%.2f"),
				Float(Page::Animation, "Graph", "fClipLength", RDF_F(clipLength), 0.1f, 3.0f, "%.3f s"),
				Bool(Page::Animation, "Graph", "bClipMotion", RDF_B(clipMotion)),
				Float(Page::Animation, "Graph", "fArmStance", RDF_F(armStance), 0.0f, 1.0f, "%.2f"),
				Bool(Page::Animation, "Graph", "bArmStanceWhenSheathed", RDF_B(armStanceWhenSheathed)),
				Float(Page::Animation, "Graph", "fArmStanceFirstPerson", RDF_F(armStanceFirstPerson), 0.0f, 1.0f, "%.2f"),
				Float(Page::Animation, "Graph", "fClipLengthFirstPerson", RDF_F(clipLengthFirstPerson), 0.1f, 3.0f, "%.2f s"),
				Float(Page::Animation, "Graph", "fFirstPersonLookScale", RDF_F(firstPersonLookScale), -2.0f, 2.0f, "%.2f"),
				// --- rules and cost
				Resource(),
				Float(Page::Rules, "Cost", "fCost", RDF_F(price.flat), 0.0f, 200.0f, "%.0f"),
				Float(Page::Rules, "Cost", "fCostMaxPercent", RDF_F(price.maxPercent), 0.0f, 50.0f, "%.1f %%"),
				Float(Page::Rules, "Cost", "fCostWeightMult", RDF_F(price.weightMult), 0.0f, 2.0f, "%.2f"),
				Float(Page::Rules, "Cost", "fChainCostStep", RDF_F(price.chainStep), 0.0f, 2.0f, "%.2f"),
				Float(Page::Rules, "Cost", "fOverencumberedMult", RDF_F(price.overencumberedMult), 0.0f, 5.0f, "%.2f"),
				Float(Page::Rules, "Cost", "fCostMin", RDF_F(price.min), 0.0f, 200.0f, "%.0f"),
				Float(Page::Rules, "Cost", "fCostMax", RDF_F(price.max), 0.0f, 500.0f, "%.0f"),
				Shortfall(),
				Float(Page::Rules, "Cost", "fMinimum", RDF_F(price.minimum), 0.0f, 200.0f, "%.0f"),
				Float(Page::Rules, "Cost", "fPerfectRefund", RDF_F(price.perfectRefund), 0.0f, 100.0f, "%.0f %%"),
				Float(Page::Rules, "Cost", "fRegenDelay", RDF_F(regenDelay), 0.0f, 5.0f, "%.2f s"),
				Float(Page::Rules, "Cost", "fRegenDelayEmpty", RDF_F(regenDelayEmpty), 0.0f, 10.0f, "%.2f s"),
				Bool(Page::Rules, "Cost", "bFlashMeter", RDF_B(flashMeter)),
				Form(Page::Rules, "Cost", "sStaminaKeyword", RDF_S(staminaCostKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Cost", "sMagickaKeyword", RDF_S(magickaCostKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Cost", "sCostKeyword", RDF_S(costKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Cost", "sCostMultKeyword", RDF_S(costMultKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Cost", "sChainCostKeyword", RDF_S(chainCostKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "Perfect", "bPerfectDodge", RDF_B(perfect.enabled)),
				Float(Page::Rules, "Perfect", "fPerfectWindowStart", RDF_F(perfect.windowStart), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectWindowEnd", RDF_F(perfect.windowEnd), 0.0f, 1.0f, "%.2f s"),
				Form(Page::Rules, "Perfect", "sPerfectDodgeKeyword", RDF_S(perfect.keyword), RE::FormType::Keyword),
				Form(Page::Rules, "Perfect", "sPerfectWindowKeyword", RDF_S(perfect.windowKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "Perfect", "bPerfectGhost", RDF_B(perfect.ghost)),
				Bool(Page::Rules, "Perfect", "bPerfectGhostNPC", RDF_B(perfect.ghostNPC)),
				Color(Page::Rules, "Perfect", "sPerfectGhostColor", +[](Settings& s) -> float* { return s.perfect.ghostColor.data(); }),
				Float(Page::Rules, "Perfect", "fPerfectGhostIntensity", RDF_F(perfect.ghostIntensity), 0.0f, 10.0f, "%.2f"),
				Float(Page::Rules, "Perfect", "fPerfectGhostRimPower", RDF_F(perfect.ghostRimPower), 0.0f, 10.0f, "%.2f"),
				Float(Page::Rules, "Perfect", "fPerfectGhostLifetime", RDF_F(perfect.ghostLifetime), 0.05f, 5.0f, "%.2f s"),
				Int(Page::Rules, "Perfect", "iPerfectGhostCount", RDF_I(perfect.ghostCount), 1, 10),
				Float(Page::Rules, "Perfect", "fPerfectGhostInterval", RDF_F(perfect.ghostInterval), 0.0f, 0.5f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotion", RDF_F(perfect.slowMotion), 0.05f, 1.0f, "%.2f"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionPlayer", RDF_F(perfect.slowMotionPlayer), 0.05f, 1.0f, "%.2f"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionDuration", RDF_F(perfect.slowMotionDuration), 0.0f, 2.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionEaseIn", RDF_F(perfect.slowMotionEaseIn), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionEaseOut", RDF_F(perfect.slowMotionEaseOut), 0.0f, 1.0f, "%.2f s"),
				Float(Page::Rules, "Perfect", "fPerfectSlowMotionCooldown", RDF_F(perfect.slowMotionCooldown), 0.0f, 10.0f, "%.1f s"),
				Form(Page::Rules, "Rules", "sNoDodgeKeyword", RDF_S(attackCancel.noDodgeKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "Rules", "bRespectDisabledControls", RDF_B(respectDisabledControls)),
				Form(Page::Rules, "Rules", "sPerfectOnlyKeyword", RDF_S(attackCancel.perfectOnlyKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Rules", "sUndodgeableKeyword", RDF_S(attackCancel.undodgeableKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "AttackCancel", "bDodgeInRecovery", RDF_B(core.policy.cancelInRecovery)),
				Form(Page::Rules, "AttackCancel", "sAnyTimeKeyword", RDF_S(attackCancel.anyTimeKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "AttackCancel", "sAnyTimePowerKeyword", RDF_S(attackCancel.anyTimePowerKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "Jump", "bBlockDuringDodge", RDF_B(blockJumpDuringDodge)),
				Bool(Page::Rules, "Stagger", "bDodgeFromStagger", RDF_B(dodgeFromStagger)),
				Float(Page::Rules, "Stagger", "fDodgeDelay", RDF_F(staggerDodgeDelay), -1.0f, 3.0f, "%.2f s"),
				Float(Page::Rules, "Stagger", "fEscapeCost", RDF_F(price.escapeFlat), 0.0f, 100.0f, "%.0f"),
				Float(Page::Rules, "Stagger", "fEscapeCostMult", RDF_F(price.escapeMult), 0.0f, 5.0f, "%.2f"),
				Form(Page::Rules, "Stagger", "sNoEscapeKeyword", RDF_S(stagger.noEscapeKeyword), RE::FormType::Keyword),
				Named(Form(Page::Rules, "Stagger", "sAnyTimeKeyword", RDF_S(stagger.anyTimeKeyword), RE::FormType::Keyword), "Stagger_sAnyTimeKeyword"),
				Form(Page::Rules, "Stagger", "sDelayKeyword", RDF_S(stagger.delayKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Stagger", "sEscapeCostKeyword", RDF_S(stagger.escapeCostKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Stagger", "sEscapeCostMultKeyword", RDF_S(stagger.escapeCostMultKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Casting", "sDrawDuringDodgeKeyword", RDF_S(casting.drawKeyword), RE::FormType::Keyword),
				Form(Page::Rules, "Casting", "sCastDuringDodgeKeyword", RDF_S(casting.castKeyword), RE::FormType::Keyword),
				Bool(Page::Rules, "CastOn", "bCastOn", RDF_B(castOn)),
				// --- debug
				Bool(Page::Debug, "Dev", "bDevLog", RDF_B(devLog)),
			};
			return fields;
		}

#undef RDF_F
#undef RDF_I
#undef RDF_B
#undef RDF_S

		std::string ValueText(const Field& f, const Settings& s)
		{
			auto& m = const_cast<Settings&>(s);  // the accessors take a mutable Settings; nothing is written
			switch (f.kind) {
			case Kind::Float:
				{
					const float v = *f.f(m);
					return std::format("{}", std::round(v * 10000.0f) / 10000.0f);
				}
			case Kind::Int: return std::format("{}", *f.i(m));
			case Kind::Bool: return *f.b(m) ? "1" : "0";
			case Kind::Choice:
				{
					const int v = f.getChoice(s);
					return v >= 0 && v < static_cast<int>(f.choices.size()) ? f.choices[v] : f.choices[0];
				}
			case Kind::Form: return *f.str(m);
			case Kind::Color:
				{
					const float* c = f.f(m);
					const auto   v = [](float x) { return std::round(x * 1000.0f) / 1000.0f; };
					return std::format("{},{},{},{}", v(c[0]), v(c[1]), v(c[2]), v(c[3]));
				}
			}
			return {};
		}
	}

	std::span<const Field> Fields()
	{
		return Table();
	}

	void CopyPage(Page page, const Settings& from, Settings& to)
	{
		auto& src = const_cast<Settings&>(from);
		for (const auto& f : Table()) {
			if (f.page != page) {
				continue;
			}
			switch (f.kind) {
			case Kind::Float: *f.f(to) = *f.f(src); break;
			case Kind::Int: *f.i(to) = *f.i(src); break;
			case Kind::Bool: *f.b(to) = *f.b(src); break;
			case Kind::Choice: f.setChoice(to, f.getChoice(from)); break;
			case Kind::Form: *f.str(to) = *f.str(src); break;
			case Kind::Color: std::copy_n(f.f(src), 4, f.f(to)); break;
			}
		}
	}

	bool SaveToIni(const Settings& s, std::string* error)
	{
		std::string text;
		{
			std::ifstream in(kIniPath, std::ios::binary);
			if (in) {
				std::ostringstream ss;
				ss << in.rdbuf();
				text = ss.str();
			}
		}
		std::vector<ini::Entry> entries;
		for (const auto& f : Table()) {
			entries.push_back({ f.section, f.key, ValueText(f, s) });
		}
		const auto    out = ini::Update(text, entries);
		std::ofstream file(kIniPath, std::ios::binary | std::ios::trunc);
		if (!file) {
			if (error) {
				*error = std::format("cannot write {}", kIniPath);
			}
			return false;
		}
		file << out;
		return static_cast<bool>(file);
	}
}
