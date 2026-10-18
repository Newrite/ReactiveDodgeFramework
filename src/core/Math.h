#pragma once

#include <algorithm>
#include <cmath>

namespace rdf
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kDegToRad = kPi / 180.0f;
	constexpr float kRadToDeg = 180.0f / kPi;

	// Skyrim planar convention: x = east, y = north; heading (yaw) grows clockwise from north,
	// so forward = (sin h, cos h) and right = (cos h, -sin h).
	struct Vec2
	{
		float x{ 0.0f };
		float y{ 0.0f };
	};

	inline float Length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
	inline Vec2  operator*(Vec2 v, float s) { return { v.x * s, v.y * s }; }
	inline Vec2  FromHeading(float h) { return { std::sin(h), std::cos(h) }; }
	inline float HeadingOf(Vec2 v) { return std::atan2(v.x, v.y); }

	inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

	// Wraps to (-pi, pi].
	inline float WrapAngle(float a)
	{
		a = std::fmod(a, 2.0f * kPi);
		if (a > kPi) {
			a -= 2.0f * kPi;
		} else if (a <= -kPi) {
			a += 2.0f * kPi;
		}
		return a;
	}

	// Signed shortest rotation taking `from` to `to`.
	inline float AngleDelta(float from, float to) { return WrapAngle(to - from); }

	// Rotates `current` toward `target` along the shortest arc by at most `maxStep` (>= 0).
	inline float MoveToward(float current, float target, float maxStep)
	{
		const float delta = AngleDelta(current, target);
		if (std::fabs(delta) <= maxStep) {
			return WrapAngle(target);
		}
		return WrapAngle(current + (delta > 0.0f ? maxStep : -maxStep));
	}

	inline float Smoothstep(float u)
	{
		u = Clamp01(u);
		return u * u * (3.0f - 2.0f * u);
	}

	// Integral of Smoothstep over [0, u].
	inline float SmoothstepIntegral(float u)
	{
		u = Clamp01(u);
		return u * u * u - 0.5f * u * u * u * u;
	}

	// World vector -> actor-local (x = right, y = forward) for an actor facing `heading`.
	inline Vec2 WorldToLocal(Vec2 v, float heading)
	{
		const float s = std::sin(heading);
		const float c = std::cos(heading);
		return { v.x * c - v.y * s, v.x * s + v.y * c };
	}
}
