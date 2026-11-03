#include "InputCollector.h"

#include "Settings.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

// FLICK's API: its menu is one of the menus a dodge must not fire in (this plugin's settings pages live in it).
#include "FUCK_API.h"

#include <array>
#include <atomic>
#include <chrono>

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

		void TrackMoveKey(const RE::ButtonEvent& b, const RE::UserEvents& events, bool blocked);

		// Wheeler's wheel, only while that DLL is loaded: it publishes IsWheelerOpen for exactly this question (the same
		// check TrueFlasksNG makes). Resolved once; a missing DLL is not an error, it just means "never open".
		using IsWheelerOpen_t = bool (*)();

		IsWheelerOpen_t WheelerIsOpen()
		{
			static const IsWheelerOpen_t fn = [] {
				const ::HMODULE wheeler = ::GetModuleHandleA("wheeler.dll");
				return wheeler ? reinterpret_cast<IsWheelerOpen_t>(::GetProcAddress(wheeler, "IsWheelerOpen")) : nullptr;
			}();
			return fn;
		}

		// A menu that keeps the player's hands off the dodge. UI::GameIsPaused() is the cheap catch-all first (the game's
		// own paused-by-a-menu counter, UI::numPausesGame); the named menus follow it, so the answer does not depend on
		// whether a given menu raises that counter: the vanilla ones that are known not to pause (console, dialogue) and,
		// for the rest, the same list as TrueFlasksNG's is_any_menu_open() so a pausing menu is named as well.
		// Mod menus a dodge must not fire in: QuickLoot's loot window (IE / EE), FLICK's own menu -- this plugin's
		// settings pages live in it -- and Wheeler's wheel while that DLL is loaded.
		bool GameIsInputBlocked()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return true;
			}
			if (ui->GameIsPaused()) {
				return true;
			}
			static constexpr std::array kMenus{
				RE::Console::MENU_NAME,       // does not pause: typing must not dodge
				RE::DialogueMenu::MENU_NAME,  // does not pause: the world runs while talking
				RE::TweenMenu::MENU_NAME,
				RE::MagicMenu::MENU_NAME,
				RE::InventoryMenu::MENU_NAME,
				RE::LockpickingMenu::MENU_NAME,
				RE::RaceSexMenu::MENU_NAME,
				RE::StatsMenu::MENU_NAME,
				RE::LoadingMenu::MENU_NAME,
				RE::FaderMenu::MENU_NAME,
				RE::FavoritesMenu::MENU_NAME,
				RE::SleepWaitMenu::MENU_NAME,
				RE::JournalMenu::MENU_NAME,
				RE::BarterMenu::MENU_NAME,
				RE::MainMenu::MENU_NAME,
				RE::BookMenu::MENU_NAME,
				RE::ContainerMenu::MENU_NAME,
				RE::GiftMenu::MENU_NAME,
				RE::MessageBoxMenu::MENU_NAME,
				RE::TrainingMenu::MENU_NAME,
				RE::MapMenu::MENU_NAME,
				RE::TutorialMenu::MENU_NAME,
				RE::LevelUpMenu::MENU_NAME,
				RE::CreditsMenu::MENU_NAME,
				RE::CraftingMenu::MENU_NAME,
				RE::CursorMenu::MENU_NAME,
			};
			for (const auto& name : kMenus) {
				if (ui->IsMenuOpen(name)) {
					return true;
				}
			}
			// QuickLoot IE / EE: the loot window keeps the world running while the player picks an item, so a dodge would
			// start from it. [Input] bBlockDodgeInLootMenu=0 lets it through for players who want to dodge from there.
			if (Settings::Get().blockDodgeInLootMenu && (ui->IsMenuOpen("LootMenuIE") || ui->IsMenuOpen("LootMenu"))) {
				return true;
			}
			if (FUCK::IsMenuOpen()) {   // FLICK's menu (F7), where this plugin's settings are edited
				return true;
			}
			const auto wheeler = WheelerIsOpen();
			return wheeler && wheeler();
		}

		// sDodgeKeyAction's ini name as the game's user event ("ReadyWeapon" -> "Ready Weapon").
		std::string KeyActionEvent(const std::string& name)
		{
			if (_stricmp(name.c_str(), "ReadyWeapon") == 0) {
				return "Ready Weapon";
			}
			if (_stricmp(name.c_str(), "TogglePOV") == 0) {
				return "Toggle POV";
			}
			return name;
		}

		bool IsMapped(const RE::BSFixedString& userEvent, RE::INPUT_DEVICE device, std::uint32_t code);

		bool IsDodgeKey(RE::INPUT_DEVICE device, std::uint32_t code)
		{
			const auto& s = Settings::Get();
			if (!s.dodgeKeyAction.empty()) {  // the button the game binds to that action, whatever the player made it
				return IsMapped(RE::BSFixedString(KeyActionEvent(s.dodgeKeyAction)), device, code);
			}
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
				const bool blocked = GameIsInputBlocked();
				if (blocked) {
					// A menu consumes the whole gesture: its release after closing must not turn an earlier press into a
					// Tap dodge, and nothing pressed in it may queue a command. The keys themselves are still tracked
					// below, so a movement or block key released inside the menu does not stay "held" for the next dodge.
					tapSeen_.fill(false);
					promoted_ = false;
				}
				if (!events) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* e = *events; e; e = e->next) {
					if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
						Handle(*e->AsButtonEvent(), blocked);
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
			void Handle(const RE::ButtonEvent& b, bool blocked)
			{
				const auto device = b.GetDevice();
				const auto code = b.GetIDCode();
				const auto* events = RE::UserEvents::GetSingleton();
				if (!events) {
					return;
				}
				TrackMoveKey(b, *events, blocked);
				if (IsMapped(events->leftAttack, device, code)) {
					g_blockHeld.store(b.IsPressed(), std::memory_order_relaxed);
				}
				if (blocked) {
					return;   // the menu has the player's hands: no dodge / attack / block command, no tap, no promotion
				}

				const auto& s = Settings::Get();
				const bool  dodgeKey = IsDodgeKey(device, code);
				if (dodgeKey && s.dodgeTrigger == Settings::DodgeTrigger::Tap) {
					// On the release, if the button was held at most fTapMax. A release whose press this sink did not see
					// (pressed in a menu, or while the game was paused) is not a tap.
					auto& seen = tapSeen_[DeviceIndex(device)];
					if (b.IsDown()) {
						seen = true;
					} else if (b.IsUp()) {
						if (seen && b.HeldDuration() <= s.tapMax) {
							Enqueue({ CommandKind::Dodge, false });
						}
						seen = false;
					}
				}
				if (b.IsDown()) {
					promoted_ = false;
					if (dodgeKey) {
						if (s.dodgeTrigger == Settings::DodgeTrigger::Press) {
							Enqueue({ CommandKind::Dodge, false });
						}
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

			static std::size_t DeviceIndex(RE::INPUT_DEVICE device)
			{
				switch (device) {
				case RE::INPUT_DEVICE::kKeyboard: return 0;
				case RE::INPUT_DEVICE::kMouse: return 1;
				default: return 2;
				}
			}

			bool                promoted_{ false };
			std::array<bool, 3> tapSeen_{};  // per device: the dodge button's press was seen (Tap)
		};

		Sink g_sink;

		// [Input] bDoubleTapDodge: the last movement key pressed and when (the input sink's thread only).
		std::uint32_t                         g_lastMoveBit{ 0 };
		std::chrono::steady_clock::time_point g_lastMoveDown{};

		void TrackMoveKey(const RE::ButtonEvent& b, const RE::UserEvents& events, bool blocked)
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
			if (blocked) {
				return;   // a key pressed in a menu is tracked, but it is not the first tap of a double tap
			}
			// A second press of the same movement key soon after the first dodges that way: the dodge reads the move input,
			// which that key holds now.
			if (b.IsDown() && device == RE::INPUT_DEVICE::kKeyboard) {
				const auto& s = Settings::Get();
				const auto  now = std::chrono::steady_clock::now();
				if (s.doubleTap && bit == g_lastMoveBit && now - g_lastMoveDown <= std::chrono::duration<float>(s.doubleTapWindow)) {
					Enqueue({ CommandKind::Dodge, false });
					g_lastMoveBit = 0;  // a third press starts over
				} else {
					g_lastMoveBit = bit;
					g_lastMoveDown = now;
				}
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

	bool IsBlocked()
	{
		return GameIsInputBlocked();
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
