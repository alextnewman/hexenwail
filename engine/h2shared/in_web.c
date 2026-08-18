#include "quakedef.h"

#include <ctype.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

cvar_t in_gamepad = {"in_gamepad", "1", CVAR_ARCHIVE};
cvar_t joy_deadzone_look = {"joy_deadzone_look", "0.2", CVAR_ARCHIVE};
cvar_t joy_deadzone_move = {"joy_deadzone_move", "0.2", CVAR_ARCHIVE};
cvar_t joy_deadzone_trigger = {"joy_deadzone_trigger", "0.2", CVAR_ARCHIVE};
cvar_t joy_sensitivity_yaw = {"joy_sensitivity_yaw", "240", CVAR_ARCHIVE};
cvar_t joy_sensitivity_pitch = {"joy_sensitivity_pitch", "180", CVAR_ARCHIVE};
cvar_t joy_exponent = {"joy_exponent", "2", CVAR_ARCHIVE};
cvar_t joy_exponent_move = {"joy_exponent_move", "1.5", CVAR_ARCHIVE};
cvar_t joy_invert = {"joy_invert", "0", CVAR_ARCHIVE};
cvar_t joy_swapmovelook = {"joy_swapmovelook", "0", CVAR_ARCHIVE};
cvar_t joy_rumble = {"joy_rumble", "1", CVAR_ARCHIVE};
cvar_t m_filter = {"m_filter", "1", CVAR_ARCHIVE};
cvar_t _enable_mouse = {"_enable_mouse", "1", CVAR_ARCHIVE};

/* Menu mouse cursor position, in menu canvas coordinates.  Fed by the
 * pointer callbacks below; consumed by the menu hit-testing code. */
int menu_mouse_x, menu_mouse_y;

/* Gamepad "alt" modifier chord, mirrored from the SDL input backend so
 * keys.c can share its gamepad binding logic. */
qboolean joy_altmodifier_pressed = false;

static double look_x, look_y;
static qboolean mouse_active;

static int Web_KeyCode (const EmscriptenKeyboardEvent *event)
{
	const char *key = event->key;
	if (key[0] && !key[1])
		return tolower((unsigned char)key[0]);
	if (!strcmp(key, "Tab")) return K_TAB;
	if (!strcmp(key, "Enter")) return K_ENTER;
	if (!strcmp(key, "Escape")) return K_ESCAPE;
	if (!strcmp(key, " ")) return K_SPACE;
	if (!strcmp(key, "Backspace")) return K_BACKSPACE;
	if (!strcmp(key, "ArrowUp")) return K_UPARROW;
	if (!strcmp(key, "ArrowDown")) return K_DOWNARROW;
	if (!strcmp(key, "ArrowLeft")) return K_LEFTARROW;
	if (!strcmp(key, "ArrowRight")) return K_RIGHTARROW;
	if (!strcmp(key, "Shift")) return K_SHIFT;
	if (!strcmp(key, "Control")) return K_CTRL;
	if (!strcmp(key, "Alt")) return K_ALT;
	if (!strcmp(key, "Delete")) return K_DEL;
	if (!strcmp(key, "Home")) return K_HOME;
	if (!strcmp(key, "End")) return K_END;
	if (!strcmp(key, "PageUp")) return K_PGUP;
	if (!strcmp(key, "PageDown")) return K_PGDN;
	if (key[0] == 'F' && isdigit((unsigned char)key[1]) && !key[3])
	{
		int number = atoi(key + 1);
		if (number >= 1 && number <= 12)
			return K_F1 + number - 1;
	}
	return 0;
}

static EM_BOOL Web_KeyboardCallback (int event_type,
	const EmscriptenKeyboardEvent *event, void *user_data)
{
	int key = Web_KeyCode(event);
	(void)user_data;
	if (!key)
		return EM_FALSE;
	Key_Event(key, event_type == EMSCRIPTEN_EVENT_KEYDOWN);
	if (event_type == EMSCRIPTEN_EVENT_KEYDOWN && event->key[0] && !event->key[1] &&
		event->key[0] >= 32 && event->key[0] < 127)
		Key_CharEvent(event->key);
	return EM_TRUE;
}

static EM_BOOL Web_MouseCallback (int event_type,
	const EmscriptenMouseEvent *event, void *user_data)
{
	int key;
	(void)user_data;
	if (event_type == EMSCRIPTEN_EVENT_MOUSEMOVE)
	{
		if (mouse_active)
		{
			look_x += event->movementX;
			look_y += event->movementY;
		}
		return EM_TRUE;
	}
	key = event->button == 0 ? K_MOUSE1 :
		(event->button == 1 ? K_MOUSE3 : K_MOUSE2);
	Key_Event(key, event_type == EMSCRIPTEN_EVENT_MOUSEDOWN);
	return EM_TRUE;
}

static EM_BOOL Web_WheelCallback (int event_type,
	const EmscriptenWheelEvent *event, void *user_data)
{
	int key = event->deltaY < 0 ? K_MWHEELUP : K_MWHEELDOWN;
	(void)event_type;
	(void)user_data;
	Key_Event(key, true);
	Key_Event(key, false);
	return EM_TRUE;
}

EMSCRIPTEN_KEEPALIVE int Web_TouchKey (int key, int down)
{
	Key_Event(key, down ? true : false);
	return 1;
}

EMSCRIPTEN_KEEPALIVE void Web_TouchLook (double dx, double dy)
{
	look_x += dx;
	look_y += dy;
}

void IN_Init (void)
{
	Cvar_RegisterVariable(&in_gamepad);
	Cvar_RegisterVariable(&joy_deadzone_look);
	Cvar_RegisterVariable(&joy_deadzone_move);
	Cvar_RegisterVariable(&joy_deadzone_trigger);
	Cvar_RegisterVariable(&joy_sensitivity_yaw);
	Cvar_RegisterVariable(&joy_sensitivity_pitch);
	Cvar_RegisterVariable(&joy_exponent);
	Cvar_RegisterVariable(&joy_exponent_move);
	Cvar_RegisterVariable(&joy_invert);
	Cvar_RegisterVariable(&joy_swapmovelook);
	Cvar_RegisterVariable(&joy_rumble);
	Cvar_RegisterVariable(&m_filter);
	Cvar_RegisterVariable(&_enable_mouse);
	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, true,
		Web_KeyboardCallback);
	emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, true,
		Web_KeyboardCallback);
	emscripten_set_mousedown_callback("#canvas", NULL, true, Web_MouseCallback);
	emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, true,
		Web_MouseCallback);
	emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, true,
		Web_MouseCallback);
	emscripten_set_wheel_callback("#canvas", NULL, true, Web_WheelCallback);
	mouse_active = true;
}

void IN_ReInit (void) {}
void IN_Shutdown (void) { mouse_active = false; }
void IN_Commands (void) {}
void IN_SendKeyEvents (void) {}
void IN_ClearStates (void) { look_x = look_y = 0; }
void IN_ActivateMouse (void) { mouse_active = true; }
void IN_DeactivateMouse (void) { mouse_active = false; }
void IN_ShowMouse (void) {}
void IN_HideMouse (void) {}
void IN_UpdateViewAngles (void) {}
qboolean IN_HasGamepad (void) { return false; }
gamepad_type_t IN_GetGamepadType (void) { return GAMEPAD_TYPE_UNKNOWN; }
void IN_GPRumble (float low_freq, float high_freq, unsigned int duration_ms) {}

void IN_Move (usercmd_t *cmd)
{
	double dx = look_x * sensitivity.value;
	double dy = look_y * sensitivity.value;
	look_x = look_y = 0;
	if (cl.v.cameramode)
		return;
	if ((in_strafe.state & 1) || (lookstrafe.integer && (in_mlook.state & 1)))
		cmd->sidemove += m_side.value * dx;
	else
		cl.viewangles[YAW] -= m_yaw.value * dx;
	if ((in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		V_StopPitchDrift();
		cl.viewangles[PITCH] += m_pitch.value * dy;
		cl.viewangles[PITCH] = q_max(-70, q_min(80, cl.viewangles[PITCH]));
	}
	else
		cmd->forwardmove -= m_forward.value * dy;
}
