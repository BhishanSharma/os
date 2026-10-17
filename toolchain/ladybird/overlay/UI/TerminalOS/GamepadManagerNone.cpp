/*
 * Ladybird on Terminal OS: no gamepads (Ladybird reads them through SDL3,
 * which Terminal OS does not have). Pages see none connected.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWebView/GamepadManager.h>

namespace WebView {

GamepadManager& GamepadManager::the()
{
    static GamepadManager manager;
    return manager;
}

void GamepadManager::client_did_start_using_gamepads(WebContentClient&) { }
void GamepadManager::client_disconnected(WebContentClient&) { }
void GamepadManager::play_effect(WebContentClient&, Web::Gamepad::GamepadHandle, Web::Gamepad::GamepadEffect const&) { }
void GamepadManager::stop_effects(WebContentClient&, Web::Gamepad::GamepadHandle) { }

Optional<Web::Gamepad::VirtualGamepad> GamepadManager::create_virtual_gamepad(WebContentClient&)
{
    return {};
}

void GamepadManager::set_virtual_gamepad_button(WebContentClient&, Web::Gamepad::GamepadHandle, i32, bool) { }
void GamepadManager::set_virtual_gamepad_axis(WebContentClient&, Web::Gamepad::GamepadHandle, i32, i16) { }
void GamepadManager::disconnect_virtual_gamepad(WebContentClient&, Web::Gamepad::GamepadHandle) { }

Vector<Web::Gamepad::GamepadChangeEvent> GamepadManager::pump_gamepad_events(WebContentClient&)
{
    return {};
}

Vector<Web::Gamepad::ReceivedDualRumbleEffect> GamepadManager::virtual_gamepad_received_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle)
{
    return {};
}

Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> GamepadManager::virtual_gamepad_received_trigger_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle)
{
    return {};
}

}
