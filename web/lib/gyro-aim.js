const RADIANS_TO_DEGREES = 180 / Math.PI;
const LOOK_UNITS_PER_DEGREE = 1 / 0.022;

export const DEFAULT_GYRO_AIM_OPTIONS = Object.freeze({
  sensitivity: 1,
  deadZoneDegrees: 0.75,
  maxDeltaSeconds: 0.05,
});

function finite(value) {
  return Number.isFinite(Number(value)) ? Number(value) : 0;
}

function applyDeadZone(value, deadZone) {
  return Math.abs(value) >= deadZone ? value : 0;
}

function clampDeltaSeconds(value, maximum) {
  return Math.max(0, Math.min(value, maximum));
}

export class GyroAim {
  constructor(bridge, options = {}, environment = globalThis) {
    this.bridge = bridge;
    this.options = { ...DEFAULT_GYRO_AIM_OPTIONS, ...options };
    this.environment = environment;
    this.enabled = false;
    this.motionAttached = false;
    this.permission = 'unknown';
    this.frameHandle = null;
    this.lastMotionTimestamp = null;
    this.lastGamepadTimestamp = null;
    this.boundMotion = (event) => this.handleDeviceMotion(event);
    this.boundFrame = (timestamp) => this.pollGamepads(timestamp);
  }

  setEnabled(enabled) {
    this.enabled = Boolean(enabled);
    if (!this.enabled) {
      this.detachMotion();
      this.stopGamepadPolling();
      this.resetTimestamps();
      return;
    }

    const MotionEvent = this.environment.DeviceMotionEvent;
    if (MotionEvent && typeof MotionEvent.requestPermission !== 'function') {
      this.permission = 'granted';
      this.attachMotion();
    }
    this.startGamepadPolling();
  }

  setSensitivity(value) {
    const parsed = Number(value);
    this.options.sensitivity = Number.isFinite(parsed) && parsed > 0
      ? parsed
      : DEFAULT_GYRO_AIM_OPTIONS.sensitivity;
  }

  async requestPermission() {
    if (!this.enabled) return 'disabled';
    const MotionEvent = this.environment.DeviceMotionEvent;
    if (!MotionEvent) {
      this.permission = 'unavailable';
      return this.permission;
    }

    if (typeof MotionEvent.requestPermission === 'function') {
      try {
        this.permission = await MotionEvent.requestPermission();
      } catch {
        this.permission = 'denied';
      }
    } else {
      this.permission = 'granted';
    }

    if (this.permission === 'granted') this.attachMotion();
    return this.permission;
  }

  attachMotion() {
    if (this.motionAttached || !this.enabled) return;
    this.environment.addEventListener?.('devicemotion', this.boundMotion);
    this.motionAttached = true;
  }

  detachMotion() {
    if (!this.motionAttached) return;
    this.environment.removeEventListener?.('devicemotion', this.boundMotion);
    this.motionAttached = false;
  }

  startGamepadPolling() {
    if (this.frameHandle !== null || typeof this.environment.requestAnimationFrame !== 'function') return;
    this.frameHandle = this.environment.requestAnimationFrame(this.boundFrame);
  }

  stopGamepadPolling() {
    if (this.frameHandle === null) return;
    this.environment.cancelAnimationFrame?.(this.frameHandle);
    this.frameHandle = null;
  }

  resetTimestamps() {
    this.lastMotionTimestamp = null;
    this.lastGamepadTimestamp = null;
  }

  isDeviceActive() {
    return this.enabled && (this.bridge.deviceActive?.() ?? true);
  }

  isGamepadActive() {
    return this.enabled && (this.bridge.gamepadActive?.() ?? true);
  }

  emitRates(yawDegreesPerSecond, pitchDegreesPerSecond, deltaSeconds) {
    const deadZone = this.options.deadZoneDegrees;
    const yaw = applyDeadZone(yawDegreesPerSecond, deadZone);
    const pitch = applyDeadZone(pitchDegreesPerSecond, deadZone);
    if (!yaw && !pitch) return;
    const scale = LOOK_UNITS_PER_DEGREE * this.options.sensitivity * deltaSeconds;
    this.bridge.look(yaw * scale, pitch * scale);
  }

  handleDeviceMotion(event) {
    if (!this.isDeviceActive()) {
      this.lastMotionTimestamp = null;
      return;
    }
    const rate = event.rotationRate;
    if (!rate) return;

    const timestamp = finite(event.timeStamp);
    const elapsed = this.lastMotionTimestamp === null
      ? finite(event.interval) / 1000
      : (timestamp - this.lastMotionTimestamp) / 1000;
    this.lastMotionTimestamp = timestamp;
    const deltaSeconds = clampDeltaSeconds(elapsed, this.options.maxDeltaSeconds);
    if (!deltaSeconds) return;

    const angle = finite(this.environment.screen?.orientation?.angle ?? this.environment.orientation);
    const radians = angle * Math.PI / 180;
    const beta = finite(rate.beta);
    const gamma = finite(rate.gamma);
    const pitch = beta * Math.cos(radians) + gamma * Math.sin(radians);
    const yaw = gamma * Math.cos(radians) - beta * Math.sin(radians);
    this.emitRates(yaw, pitch, deltaSeconds);
  }

  gamepadAngularVelocity() {
    let gamepads;
    try {
      gamepads = this.environment.navigator?.getGamepads?.() ?? [];
    } catch {
      return null;
    }
    for (const gamepad of gamepads) {
      const velocity = gamepad?.pose?.angularVelocity;
      if (velocity && velocity.length >= 3) return velocity;
    }
    return null;
  }

  pollGamepads(timestamp) {
    this.frameHandle = null;
    if (!this.enabled) return;

    const velocity = this.isGamepadActive() ? this.gamepadAngularVelocity() : null;
    if (velocity) {
      /* Unlike DeviceMotionEvent, a gamepad snapshot has no sample interval.
       * Prime the timestamp and integrate from the next animation frame. */
      const elapsed = this.lastGamepadTimestamp === null ? 0 : (timestamp - this.lastGamepadTimestamp) / 1000;
      this.lastGamepadTimestamp = timestamp;
      const deltaSeconds = clampDeltaSeconds(elapsed, this.options.maxDeltaSeconds);
      if (deltaSeconds) {
        this.emitRates(
          -finite(velocity[1]) * RADIANS_TO_DEGREES,
          finite(velocity[0]) * RADIANS_TO_DEGREES,
          deltaSeconds,
        );
      }
    } else {
      this.lastGamepadTimestamp = null;
    }

    this.startGamepadPolling();
  }

  destroy() {
    this.setEnabled(false);
  }
}
