#include "SlowMotion.h"

#include "Diagnostics.h"
#include "Settings.h"

#include <chrono>

namespace rdf::game::slowmo
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// VATS +0x80 world multiplier, +0x84 player multiplier (CommonLib's layout; in IDA SetMagicTimeSlowdown 0x140759360
		// writes a1+128 / a1+132, and a world value <= 0 means "none", restoring the global multiplier to 1).
		bool              g_active{ false };
		Clock::time_point g_start{};
		Clock::time_point g_cooldownUntil{};
		float             g_lastWorld{ 0.0f };  // what we last wrote, to see someone else's write

		float Seconds(Clock::duration d) { return std::chrono::duration<float>(d).count(); }

		float Smooth(float x)
		{
			x = std::clamp(x, 0.0f, 1.0f);
			return x * x * (3.0f - 2.0f * x);
		}

		// 0 = normal time, 1 = full slow motion.
		float Envelope(float t, const Settings::PerfectSettings& p)
		{
			if (t < p.slowMotionEaseIn) {
				return Smooth(t / p.slowMotionEaseIn);
			}
			t -= p.slowMotionEaseIn;
			if (t < p.slowMotionDuration) {
				return 1.0f;
			}
			t -= p.slowMotionDuration;
			return p.slowMotionEaseOut > 0.0f ? 1.0f - Smooth(t / p.slowMotionEaseOut) : 0.0f;
		}

		float TotalLength(const Settings::PerfectSettings& p)
		{
			return p.slowMotionEaseIn + p.slowMotionDuration + p.slowMotionEaseOut;
		}

		void Write(RE::VATS* vats, float k)
		{
			const auto& p = Settings::Get().perfect;
			const float world = std::lerp(1.0f, p.slowMotion, k);
			const float player = std::lerp(1.0f, p.slowMotionPlayer, k);
			vats->SetMagicTimeSlowdown(world, player);
			g_lastWorld = world;
		}

		void End(RE::VATS* vats, const char* why)
		{
			if (vats && vats->magicTimeSlowdown == g_lastWorld) {
				vats->SetMagicTimeSlowdown(0.0f, 1.0f);  // what the Slow Time effect writes when it ends
			}
			g_active = false;
			g_cooldownUntil = Clock::now() + std::chrono::duration_cast<Clock::duration>(
				std::chrono::duration<float>(Settings::Get().perfect.slowMotionCooldown));
			DevLog("perfect slow motion ended ({})", why);
		}
	}

	void Start()
	{
		const auto& p = Settings::Get().perfect;
		auto*       vats = RE::VATS::GetSingleton();
		if (!vats || p.slowMotion >= 1.0f || TotalLength(p) <= 0.0f) {
			return;
		}
		const auto now = Clock::now();
		if (g_active) {
			// Another perfect dodge inside the slow motion: hold it again from where it is, without a new ease-in.
			const float t = Seconds(now - g_start);
			const float holdFrom = p.slowMotionEaseIn;
			if (t > holdFrom) {
				g_start = now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(holdFrom));
			}
			DevLog("{}", "perfect slow motion extended");
			return;
		}
		if (now < g_cooldownUntil) {
			DevLog("perfect slow motion on cooldown for {:.2f}s", Seconds(g_cooldownUntil - now));
			return;
		}
		if (vats->magicTimeSlowdown > 0.0f || vats->mode != RE::VATS::VATS_MODE::kNone) {
			DevLog("{}", "perfect slow motion skipped: another time slowdown (Slow Time shout, kill cam) runs");
			return;
		}
		g_active = true;
		g_start = now;
		Write(vats, p.slowMotionEaseIn > 0.0f ? 0.0f : 1.0f);
		DevLog("perfect slow motion: world x{:.2f}, player x{:.2f}, in {:.2f}s, hold {:.2f}s, out {:.2f}s", p.slowMotion, p.slowMotionPlayer,
			p.slowMotionEaseIn, p.slowMotionDuration, p.slowMotionEaseOut);
	}

	void Update()
	{
		if (!g_active) {
			return;
		}
		auto* vats = RE::VATS::GetSingleton();
		if (!vats) {
			g_active = false;
			return;
		}
		if (vats->magicTimeSlowdown != g_lastWorld) {
			End(nullptr, "taken over by another time slowdown");  // theirs now: leave it alone
			return;
		}
		const auto& p = Settings::Get().perfect;
		const float t = Seconds(Clock::now() - g_start);
		if (t >= TotalLength(p)) {
			End(vats, "done");
			return;
		}
		Write(vats, Envelope(t, p));
	}

	void Stop()
	{
		if (g_active) {
			End(RE::VATS::GetSingleton(), "load");
		}
		g_cooldownUntil = {};
	}

	float PlayerDtScale()
	{
		const auto* vats = RE::VATS::GetSingleton();
		if (!vats || !(vats->magicTimeSlowdown > 0.0f)) {
			return 1.0f;
		}
		return vats->playerMagicTimeSlowdown / vats->magicTimeSlowdown;
	}
}
