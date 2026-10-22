#include "InputCollector.h"

#include "Settings.h"

#include <array>
#include <atomic>

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

				if (b.IsDown()) {
					promoted_ = false;
					if (IsDodgeKey(device, code)) {
						Enqueue({ CommandKind::Dodge, false });
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

			bool promoted_{ false };
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
