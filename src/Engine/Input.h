#pragma once

#include "Core/InputState.h"

// S2 / S2b: the input hook, InputState, and the input-rewriting experiments.
//
// The hook replaces slot 1 (PerformInputProcessing) in the vtable of PlayerCamera's BSInputEventReceiver
// sub-object, after checking at runtime that the live PlayerCamera really uses that vtable (see
// Engine/Offsets.h). It sees every input event the game queues, builds InputState from them, and can
// rewrite or swallow events for the S2b experiments.

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::Input
{
	// Called every tick after S1. Installs the hook once the PlayerCamera exists, and finishes the jump
	// check that S2b starts when a Jump press arrives.
	void OnTick(const Frame::Snapshot& a_snap);

	// True once the hook has been installed on the PlayerCamera's input receiver.
	[[nodiscard]] bool HookInstalled() noexcept;

	// T3.3b: the movement vector of PlayerControls::Manager as the game computed it in the last call of the rewrite hook (read after the original returned, before our write).
	struct MovementVector
	{
		float         x{ 0.0f };      // +0x58, strafe, right positive
		float         y{ 0.0f };      // +0x5C, forward positive
		float         z{ 0.0f };      // +0x60
		float         prevX{ 0.0f };  // the copy at +0x7C
		float         prevY{ 0.0f };  // +0x80
		std::uint64_t calls{ 0 };
		std::uint64_t writes{ 0 };  // calls in which a rewritten vector was written back
		float         wroteX{ 0.0f }; // the vector left in the manager by the last call (the written one, or the game's when nothing was written)
		float         wroteY{ 0.0f };
		bool          wroteLast{ false };  // the last call wrote a vector different from the game's
		bool          faulted{ false };
	};
	[[nodiscard]] MovementVector LastMovementVector() noexcept;
	// Rig write test: from the next call on the hook writes this vector after the original, whatever the game computed (and whatever the cover filter says).
	void SetMovementOverride(bool a_on, float a_x, float a_y) noexcept;
	// Rig probe: log every change of the vector (`S3 manager vector ...`, at most 160 lines).
	void SetMovementProbeLog(bool a_on) noexcept;

	// T3.4: asks the rewrite hook to deliver one Sneak press (and, a call later, its release) to the game, as a copy of a real button event linked in for
	// that one call. Thread-safe; the press arrives in the next call of the hook. Does nothing until a real button event has passed the hook once.
	void RequestSneakPress() noexcept;
	// T5.4: a fire press made by the plugin, with the device and id of the real control, held for a_seconds (events with a growing held time in every call of the hook, then the release).
	// Does nothing until a real button event has passed the hook once.
	void RequestMadeFire(int a_device, int a_id, float a_seconds) noexcept;
	// T5.6: the same for any user event: a_namePtr is the pooled string pointer copied from a real event of that control (its first word), a_device and a_id those of the real control.
	// T5.7: a tap of the ready/reload control made by the plugin (keyboard R as device and id), a reload on a full magazine does nothing.
	void RequestReloadTap() noexcept;
	// T5.8: the look input seen at the controls hook since the last call: the two ints of the mouse move events (+0x38, +0x3C; which one is the vertical one is learned by the camera hook from the
	// view's own pitch on the own graph) summed, and the latest right stick (user event Look, idCode 0x0C) x and y. Called from the camera's call-site hook (the engine's thread).
	void TakeLook(float& a_mouseA, float& a_mouseB, float& a_stickX, float& a_stickY) noexcept;
	void RequestMadeButton(const void* a_namePtr, int a_device, int a_id, float a_seconds) noexcept;
	struct MadeUpStats
	{
		std::uint32_t requested{ 0 };
		std::uint32_t delivered{ 0 };
		std::uint32_t noTemplate{ 0 };  // requests dropped because no real button event had been seen yet
		bool          haveTemplate{ false };
		std::uint32_t expired{ 0 };         // requests dropped because no hook call delivered them within 1 s
		std::uint64_t nullHeadCalls{ 0 };   // calls of the rewrite hook with an empty queue (when this stays 0 the hook is not called while nothing is pressed)
	};
	[[nodiscard]] MadeUpStats MadeUpPressStats() noexcept;
	// Drops every made-up press that has not been delivered yet.
	void ClearMadeUpRequests() noexcept;

	// True once the PlayerControls::Manager rewrite hook is installed and has not faulted (Phase 3 only runs on top of it).
	[[nodiscard]] bool ControlsHookWorking() noexcept;

	// S2b: the left stick as the PlayerControls rewrite hook saw it BEFORE it zeroed or scaled it (the S2b movement test must not decide
	// "released" from InputState, which can read the rewritten value). True when a "Move" thumbstick event with a magnitude above 0.05
	// arrived within the last a_seconds. Thread-safe.
	[[nodiscard]] bool RawMoveRecent(double a_seconds) noexcept;

	// Rig: force the movement rewrite for a while (0 none, 1 zero, 2 scale). Thread-safe.
	void SetRigMoveStage(int a_stage) noexcept;

	// True once after the S3 probe key was pressed (thread-safe).
	[[nodiscard]] bool ConsumeProbeRequest();

	// True once after the T3.5 reload key was pressed (thread-safe).
	[[nodiscard]] bool ConsumeReloadRequest();

	// True once after the T2.3 sense key was pressed (thread-safe).
	[[nodiscard]] bool ConsumeSenseRequest();

	// A copy of the current input state (thread-safe).
	[[nodiscard]] Core::InputState State();

	// One-line summary of the held keys, event counts and active S2b modes, for the S1 log line.
	[[nodiscard]] std::string StatusLine();
}
