#include "InputCollector.h"

#include "Settings.h"

#include <array>
#include <atomic>
#include <bitset>
#include <filesystem>
#include <mutex>
#include <vector>

#include <Windows.h>

namespace rdf::game::input
{
	namespace
	{
		// Single-producer (input sink) / single-consumer (update tick) ring; a full ring drops the newest press.
		constexpr std::size_t kRing = 32;

		std::array<Press, kRing>      g_ring{};
		std::atomic<std::uint32_t>    g_head{ 0 };  // next write
		std::atomic<std::uint32_t>    g_tail{ 0 };  // next read

		void Enqueue(Press p)
		{
			const auto head = g_head.load(std::memory_order_relaxed);
			const auto tail = g_tail.load(std::memory_order_acquire);
			if (head - tail >= kRing) {
				return;
			}
			g_ring[head % kRing] = p;
			g_head.store(head + 1, std::memory_order_release);
		}

		// Move input as the player gives it. PlayerControls::moveInputVec is not usable: in 360 mode
		// TDM rewrites it to "forward" relative to the character it turns. Keys go through the control
		// map, so remapped movement keys work.
		enum MoveKey : std::uint32_t
		{
			kForward = 1 << 0,
			kBack = 1 << 1,
			kLeft = 1 << 2,
			kRight = 1 << 3,
		};
		std::atomic<std::uint32_t> g_moveKeys{ 0 };
		std::atomic<bool>          g_blockHeld{ false };
		std::atomic<float>         g_stickX{ 0.0f };
		std::atomic<float>         g_stickY{ 0.0f };

		void TrackMoveKey(const RE::ButtonEvent& b, const RE::UserEvents& events);

		// SKSE key code of a button: keyboard scan code, 256 + mouse button, 266 + gamepad button.
		std::uint32_t SkseCode(RE::INPUT_DEVICE device, std::uint32_t code)
		{
			switch (device) {
			case RE::INPUT_DEVICE::kKeyboard: return code;
			case RE::INPUT_DEVICE::kMouse: return SKSE::InputMap::kMacro_MouseButtonOffset + code;
			case RE::INPUT_DEVICE::kGamepad: return SKSE::InputMap::GamepadMaskToKeycode(code);
			default: return 0;
			}
		}

		// Separate power-attack buttons: the ini's, plus BFCO's (main key and optional modifier) from its MCM settings.
		struct PowerKey
		{
			std::uint32_t key{ 0 };
			std::uint32_t modifier{ 0 };  // 0 = none
		};
		std::mutex            g_powerLock;
		std::vector<PowerKey> g_powerKeys;

		// GetPrivateProfileInt looks a relative path up in the Windows directory: make it absolute (the game's folder,
		// where MO2's virtual Data holds the mods' MCM files).
		int ReadMcmInt(const char* relative, const char* key, int def)
		{
			const auto path = (std::filesystem::current_path() / relative).string();
			return static_cast<int>(::GetPrivateProfileIntA("Controls", key, def, path.c_str()));
		}

		void LoadPowerKeys()
		{
			std::vector<PowerKey> keys;
			const auto& s = Settings::Get();
			for (const auto k : s.powerAttackKeys) {
				keys.push_back({ k, 0 });
			}
			if (s.readBfcoPowerKey) {
				// MCM Helper: the user's values in MCM\Settings, the defaults in MCM\Config\<mod>\settings.ini.
				constexpr auto kUser = "Data\\MCM\\Settings\\SCSI-ACTbfco-Main.ini";
				constexpr auto kDefault = "Data\\MCM\\Config\\SCSI-ACTbfco-Main\\settings.ini";
				const int key = ReadMcmInt(kUser, "iKeyAttackPowerNUM", ReadMcmInt(kDefault, "iKeyAttackPowerNUM", -1));
				const int mod = ReadMcmInt(kUser, "iKeyAttackPowerNUMmodf", ReadMcmInt(kDefault, "iKeyAttackPowerNUMmodf", -1));
				if (key > 0) {
					keys.push_back({ static_cast<std::uint32_t>(key), mod > 0 ? static_cast<std::uint32_t>(mod) : 0u });
					logger::info("input: BFCO power-attack key {} (modifier {})", key, mod);
				}
			}
			std::scoped_lock lock(g_powerLock);
			g_powerKeys = std::move(keys);
		}

		class JournalSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (e && !e->opening && e->menuName == RE::JournalMenu::MENU_NAME) {
					LoadPowerKeys();  // MCM settings may have changed
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		JournalSink g_journalSink;

		bool GameIsInputBlocked()
		{
			auto* ui = RE::UI::GetSingleton();
			return !ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME);
		}

		bool IsDodgeKey(RE::INPUT_DEVICE device, std::uint32_t code)
		{
			const auto& s = Settings::Get();
			switch (device) {
			case RE::INPUT_DEVICE::kKeyboard:
				return s.dodgeKeyboard >= 0 && code == static_cast<std::uint32_t>(s.dodgeKeyboard);
			case RE::INPUT_DEVICE::kMouse:
				return s.dodgeMouse >= 0 && code == static_cast<std::uint32_t>(s.dodgeMouse);
			case RE::INPUT_DEVICE::kGamepad:
				return s.dodgeGamepad >= 0 && code == static_cast<std::uint32_t>(s.dodgeGamepad);
			default:
				return false;
			}
		}

		bool IsMapped(const RE::BSFixedString& userEvent, RE::INPUT_DEVICE device, std::uint32_t code)
		{
			const auto* map = RE::ControlMap::GetSingleton();
			if (!map) {
				return false;
			}
			const auto key = map->GetMappedKey(userEvent, device);
			return key != RE::ControlMap::kInvalid && key == code;
		}

		class Sink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!events || GameIsInputBlocked()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* e = *events; e; e = e->next) {
					if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
						Handle(*e->AsButtonEvent());
					} else if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick) {
						const auto* stick = static_cast<const RE::ThumbstickEvent*>(e);
						if (stick->IsLeft()) {
							g_stickX.store(stick->xValue, std::memory_order_relaxed);
							g_stickY.store(stick->yValue, std::memory_order_relaxed);
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			void Handle(const RE::ButtonEvent& b)
			{
				const auto device = b.GetDevice();
				const auto code = b.GetIDCode();
				const auto* events = RE::UserEvents::GetSingleton();
				if (!events) {
					return;
				}
				TrackMoveKey(b, *events);
				if (IsMapped(events->leftAttack, device, code)) {
					g_blockHeld.store(b.IsPressed(), std::memory_order_relaxed);
				}

				const auto skse = SkseCode(device, code);
				if (skse < held_.size()) {
					held_[skse] = b.IsPressed();
				}
				if (b.IsDown()) {
					promoted_ = false;
					if (IsDodgeKey(device, code)) {
						Enqueue({ CommandKind::Dodge, false });
					} else if (IsPowerKey(skse)) {
						Enqueue({ CommandKind::PowerAttack, false });
					} else if (IsMapped(events->rightAttack, device, code)) {
						Enqueue({ CommandKind::Attack, false });
					} else if (IsMapped(events->leftAttack, device, code)) {
						Enqueue({ CommandKind::Block, false });
					}
				} else if (b.IsHeld() && b.HeldDuration() >= Settings::Get().powerAttackHold && !promoted_ &&
						   IsMapped(events->rightAttack, device, code)) {
					promoted_ = true;
					Enqueue({ CommandKind::PowerAttack, true });
				}
				if (b.IsUp()) {
					promoted_ = false;
				}
			}

			bool IsPowerKey(std::uint32_t skse) const
			{
				std::scoped_lock lock(g_powerLock);
				for (const auto& k : g_powerKeys) {
					if (k.key == skse && (k.modifier == 0 || (k.modifier < held_.size() && held_[k.modifier]))) {
						return true;
					}
				}
				return false;
			}

			bool               promoted_{ false };
			std::bitset<512>   held_;  // SKSE codes held down (power-key modifiers)
		};

		Sink g_sink;

		void TrackMoveKey(const RE::ButtonEvent& b, const RE::UserEvents& events)
		{
			const auto device = b.GetDevice();
			const auto code = b.GetIDCode();
			std::uint32_t bit = 0;
			if (IsMapped(events.forward, device, code)) {
				bit = kForward;
			} else if (IsMapped(events.back, device, code)) {
				bit = kBack;
			} else if (IsMapped(events.strafeLeft, device, code)) {
				bit = kLeft;
			} else if (IsMapped(events.strafeRight, device, code)) {
				bit = kRight;
			}
			if (bit == 0) {
				return;
			}
			if (b.IsPressed()) {
				g_moveKeys.fetch_or(bit, std::memory_order_relaxed);
			} else {
				g_moveKeys.fetch_and(~bit, std::memory_order_relaxed);
			}
		}

		RE::NiPoint2 PlayerMoveInput()
		{
			const float sx = g_stickX.load(std::memory_order_relaxed);
			const float sy = g_stickY.load(std::memory_order_relaxed);
			if (sx * sx + sy * sy > 1e-4f) {
				return { sx, sy };
			}
			const auto keys = g_moveKeys.load(std::memory_order_relaxed);
			const float x = ((keys & kRight) ? 1.0f : 0.0f) - ((keys & kLeft) ? 1.0f : 0.0f);
			const float y = ((keys & kForward) ? 1.0f : 0.0f) - ((keys & kBack) ? 1.0f : 0.0f);
			return { x, y };
		}
	}

	void Install()
	{
		auto* devices = RE::BSInputDeviceManager::GetSingleton();
		if (!devices) {
			logger::error("input: BSInputDeviceManager not available, buttons will not be recorded");
			return;
		}
		devices->AddEventSink(&g_sink);
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_journalSink);
		}
		LoadPowerKeys();
		logger::info("input sink registered");
	}

	namespace detail
	{
		bool Pop(Press& out)
		{
			const auto tail = g_tail.load(std::memory_order_relaxed);
			if (tail == g_head.load(std::memory_order_acquire)) {
				return false;
			}
			out = g_ring[tail % kRing];
			g_tail.store(tail + 1, std::memory_order_release);
			return true;
		}
	}

	void DiscardQueued()
	{
		Press p;
		while (detail::Pop(p)) {
		}
	}

	bool BlockHeld()
	{
		return g_blockHeld.load(std::memory_order_relaxed);
	}

	float CameraYaw()
	{
		const auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->cameraRoot) {
			return 0.0f;
		}
		// Camera forward is the local Y axis: column 1 of the world rotation (x = east, y = north).
		const auto& m = camera->cameraRoot->world.rotate;
		return std::atan2(m.entry[0][1], m.entry[1][1]);
	}

	Vec2 WorldMoveInput(std::optional<float> cameraYaw)
	{
		const auto* controls = RE::PlayerControls::GetSingleton();
		if (!controls) {
			return {};
		}
		// The game's own vector says whether the player is moving at all (it is zero in menus and after
		// a release the sink may have missed); the direction comes from the tracked keys / stick.
		const auto& game = controls->data.moveInputVec;
		if (game.x * game.x + game.y * game.y <= 1e-8f) {
			return {};
		}
		const auto raw = PlayerMoveInput();
		const float length = std::min(std::sqrt(raw.x * raw.x + raw.y * raw.y), 1.0f);
		if (length <= 1e-4f) {
			return {};
		}
		// Heading of the stick relative to "camera forward": right is clockwise-positive.
		const float heading = cameraYaw.value_or(CameraYaw()) + std::atan2(raw.x, raw.y);
		return FromHeading(heading) * length;
	}
}
