/*
 * page_motion.c - "Motion" tab: live accelerometer, gyroscope, and angle data.
 *
 * Data comes from HidSixAxisSensorState via services_gather_data(). A Joy-Con
 * (or the built-in IMU on some models) must be connected; if gyroOk is false
 * we bail early with an error row.
 *
 * Units:
 *   acceleration  - G (1.0 = Earth gravity, ≈9.8 m/s²)
 *   angular_velocity - rad/s
 *   angle (integrated attitude) - radians
 *
 * The bar charts are scaled so that a "typical resting" value sits near the
 * middle of the bar rather than at zero, which makes small motions visible:
 *
 *   Accelerometer: resting ~1G on one axis. Bar maps [-2G .. +2G] to [0..1].
 *                  Formula: (value + 2) / 4
 *
 *   Angular velocity: quiet resting is ~0 rad/s. Bar maps [-5 .. +5] to [0..1].
 *                  Formula: (value + 5) / 10
 *
 *   Angle: no bar (unbounded accumulator; would be meaningless).
 *
 * Three bars are drawn for each vector, one per axis (X/Y/Z), using three
 * distinct colors so you can tell them apart without reading the label.
 */
#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_motion(int cy, const SysData *d) {
    int r = 0;      // row counter — advanced by ROW/BAR macros
    char buf[256];

    // If no Joy-Con is connected (or the gyro service didn't open), show a
    // helpful error instead of zeroes that look like "it's working".
    if (!d->gyroOk) {
        ROW("Gyro", "No sensor available — connect a Joy-Con", RED);
        return;
    }

    // Pull each axis out into locals for readability — the struct paths are long.
    float ax = d->gyroState.acceleration.x;
    float ay = d->gyroState.acceleration.y;
    float az = d->gyroState.acceleration.z;
    float gx = d->gyroState.angular_velocity.x;
    float gy = d->gyroState.angular_velocity.y;
    float gz = d->gyroState.angular_velocity.z;
    float rx = d->gyroState.angle.x;
    float ry2= d->gyroState.angle.y;   // ry2 avoids shadowing the 'r' row counter
    float rz = d->gyroState.angle.z;

    // Accelerometer row + one bar per axis.
    // Bars map [-2G, +2G] → [0, 1]: (value + 2) / 4. At rest with the console
    // flat, az ≈ 1 → bar ≈ 0.75, which shows up as a filled, non-zero bar.
    snprintf(buf, sizeof(buf), "X %+.4f   Y %+.4f   Z %+.4f  G", ax, ay, az);
    ROW("Accelerometer", buf, CYAN);
    BAR((ax + 2.f) / 4.f, CYAN);
    BAR((ay + 2.f) / 4.f, GREEN);
    BAR((az + 2.f) / 4.f, PURPLE);

    // Gyroscope row + one bar per axis.
    // Bars map [-5, +5] rad/s → [0, 1]: (value + 5) / 10. At rest everything
    // is ~0 so bars sit at 0.5 (midpoint), giving headroom in both directions.
    snprintf(buf, sizeof(buf), "X %+.4f   Y %+.4f   Z %+.4f  rad/s", gx, gy, gz);
    ROW("Angular Velocity", buf, ORANGE);
    BAR((gx + 5.f) / 10.f, ORANGE);
    BAR((gy + 5.f) / 10.f, YELLOW);
    BAR((gz + 5.f) / 10.f, RED);

    // Integrated angle (roll/pitch/yaw accumulator). No bar: the value
    // grows without bound as the device rotates, so a fixed-range bar
    // would be useless. Display as text only.
    snprintf(buf, sizeof(buf), "Roll %+.3f   Pitch %+.3f   Yaw %+.3f  rad", rx, ry2, rz);
    ROW("Angle", buf, YELLOW);
}
