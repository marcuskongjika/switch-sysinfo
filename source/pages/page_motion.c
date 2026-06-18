#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_motion(int cy, const SysData *d) {
    int r = 0;
    char buf[256];

    if (!d->gyroOk) {
        ROW("Gyro", "No sensor available — connect a Joy-Con", RED);
        return;
    }

    float ax = d->gyroState.acceleration.x;
    float ay = d->gyroState.acceleration.y;
    float az = d->gyroState.acceleration.z;
    float gx = d->gyroState.angular_velocity.x;
    float gy = d->gyroState.angular_velocity.y;
    float gz = d->gyroState.angular_velocity.z;
    float rx = d->gyroState.angle.x;
    float ry2= d->gyroState.angle.y;
    float rz = d->gyroState.angle.z;

    snprintf(buf, sizeof(buf), "X %+.4f   Y %+.4f   Z %+.4f  G", ax, ay, az);
    ROW("Accelerometer", buf, CYAN);
    BAR((ax + 2.f) / 4.f, CYAN);
    BAR((ay + 2.f) / 4.f, GREEN);
    BAR((az + 2.f) / 4.f, PURPLE);

    snprintf(buf, sizeof(buf), "X %+.4f   Y %+.4f   Z %+.4f  rad/s", gx, gy, gz);
    ROW("Angular Velocity", buf, ORANGE);
    BAR((gx + 5.f) / 10.f, ORANGE);
    BAR((gy + 5.f) / 10.f, YELLOW);
    BAR((gz + 5.f) / 10.f, RED);

    snprintf(buf, sizeof(buf), "Roll %+.3f   Pitch %+.3f   Yaw %+.3f  rad", rx, ry2, rz);
    ROW("Angle", buf, YELLOW);
}
