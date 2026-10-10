import math
import random

import myengine as me

V = me.Vec3

HX, HZ = 20.0, 30.0                 # half size of the field (goal lines at z = +-HZ)
WALL_VIS, WALL_TOP = 4.0, 10.0      # visible wall height; the invisible collider goes up to the ceiling
GOAL_W, GOAL_H, GOAL_D = 6.0, 5.0, 5.0
BALL_R = 1.4
CAR_Y = 0.3

# Textured materials: assets/materials/rl/<name>.material.json, made by tools/gen_rl_assets.py
def _mat(name):
    return "assets/materials/rl/%s.material.json" % name


MAT = {
    # effects and glow (the key is also the colour name used by the effect code)
    "blue": _mat("fx_blue"), "orange": _mat("fx_orange"), "gold": _mat("fx_gold"), "white": _mat("fx_white"),
    "car_blue": _mat("car_blue"), "car_orange": _mat("car_orange"),
    "cabin_gold": _mat("cabin_gold"), "cabin_glass": _mat("cabin_glass"),
    "pad_big": _mat("pad_big"), "pad_small": _mat("pad_small"),
    "goal_blue": _mat("goal_blue"), "goal_orange": _mat("goal_orange"),
    "crowd_blue": _mat("crowd_blue"), "crowd_orange": _mat("crowd_orange"), "crowd_white": _mat("crowd_white"),
    "neon_blue": _mat("neon_blue"), "neon_orange": _mat("neon_orange"),
    "sandstone": _mat("sandstone"), "rock": _mat("rock"),
}
# "key" selects <key>_field / <key>_wall_left / <key>_wall_right / <key>_wall_end_<team> textures
ARENAS = [
    {"name": "CLASSIC STADIUM", "key": "classic", "extra": "stands"},
    {"name": "NEON ARENA", "key": "neon", "extra": "neon"},
    {"name": "DESERT ARENA", "key": "desert", "extra": "desert"},
]
SPOTS = [(-6.0, -20.0), (6.0, -22.0), (0.0, -27.0), (-13.0, -25.0)]
BIG_PADS = [(-17.0, -26.0), (17.0, -26.0), (-17.0, 26.0), (17.0, 26.0), (-17.0, 0.0), (17.0, 0.0)]
SMALL_PADS = [(0.0, -11.0), (0.0, 11.0), (-9.0, -17.0), (9.0, -17.0), (-9.0, 17.0), (9.0, 17.0),
              (-10.0, 0.0), (10.0, 0.0), (-12.0, -8.0), (12.0, -8.0), (-12.0, 8.0), (12.0, 8.0),
              (0.0, -23.0), (0.0, 23.0)]

# Car tuning
ACCEL, BRAKE, COAST = 16.0, 38.0, 0.8
MAX_DRIVE, MAX_BOOST, MAX_REVERSE = 20.0, 32.0, 12.0
BOOST_ACC, BOOST_USE = 26.0, 33.0
JUMP_V, DOUBLE_V, DODGE_V = 8.5, 7.0, 11.0
DEMO_SPEED = 26.0


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def wrap(a):
    return (a + 180.0) % 360.0 - 180.0


def flat(v):
    return V(v.x, 0.0, v.z)


class Car:
    def __init__(self, entity, team, index, is_player):
        self.e = entity
        self.team = team
        self.index = index
        self.player = is_player
        self.yaw = 0.0
        self.pitch = 0.0
        self.roll = 0.0
        self.boost = 33.0
        self.jumps = 0
        self.air_time = 0.0
        self.flip = 0.0
        self.flip_dir = (0.0, 0.0)
        self.dodge_t = 0.0
        self.dead = 0.0
        self.hit_cd = 0.0
        self.bump_cd = 0.0
        self.trail_cd = 0.0
        self.boosting = False
        self.grounded = True
        self.was_grounded = True
        self.jump_lock = 0.0
        self.role = "attacker"

    @property
    def sign(self):
        return 1.0 if self.team == "blue" else -1.0  # blue attacks +z

    def forward(self):
        r = math.radians(self.yaw)
        return V(math.sin(r), 0.0, math.cos(r))

    def place(self, x, z):
        self.yaw = 0.0 if self.team == "blue" else 180.0
        self.pitch = self.roll = 0.0
        self.jumps, self.flip, self.dodge_t = 0, 0.0, 0.0
        self.was_grounded, self.jump_lock = True, 0.0
        rb = self.e.rigidbody
        rb.is_kinematic = False
        rb.velocity = V(0.0, 0.0, 0.0)
        self.e.transform.position = V(x, CAR_Y + 0.05, z)
        self.e.transform.rotation = V(0.0, self.yaw, 0.0)

    def drive(self, inp, dt):
        rb, t = self.e.rigidbody, self.e.transform
        v, p = rb.velocity, t.position
        # "On the ground" has hysteresis. A car that has just jumped is airborne however its tilted body touches the
        # floor, and a car that sits on the floor stays "on the ground" through the small bounces of the contact and
        # while the settling tilt lowers it. Without this the state flips every frame and the car rocks.
        self.jump_lock = max(0.0, self.jump_lock - dt)
        tilt_p, tilt_r = math.radians(abs(wrap(self.pitch))), math.radians(abs(wrap(self.roll)))
        # height of the body centre when it lies on the floor with this tilt (half sizes of the car 0.7 x 0.3 x 1.2)
        rest_y = 0.3 * math.cos(tilt_p) * math.cos(tilt_r) + 1.2 * math.sin(tilt_p) + 0.7 * math.sin(tilt_r)
        if self.was_grounded:
            touching = p.y < rest_y + 1.5 and v.y < 3.0
        else:
            touching = v.y < 1.0 and (rb.is_grounded or (p.y < rest_y + 0.25 and abs(v.y) < 1.5))
        self.grounded = self.flip <= 0.0 and self.jump_lock <= 0.0 and touching
        self.was_grounded = self.grounded
        self.boosting = inp["boost"] and self.boost > 0.0
        self.dodge_t = max(0.0, self.dodge_t - dt)
        if self.grounded:
            self.jumps, self.air_time = 0, 0.0
            # settle the tilt of the air smoothly (no snap), the script is the only owner of the orientation
            ease = min(1.0, 16.0 * dt)
            self.pitch = wrap(self.pitch) * (1.0 - ease)
            self.roll = wrap(self.roll) * (1.0 - ease)
            if abs(self.pitch) < 0.3:
                self.pitch = 0.0
            if abs(self.roll) < 0.3:
                self.roll = 0.0
            fwd0 = self.forward()
            speed = abs(v.dot(fwd0))
            turn = (190.0 - 3.5 * min(speed, MAX_BOOST)) * min(1.0, speed / 4.0 + 0.15)
            if inp["slide"]:
                turn *= 1.6
            self.yaw += inp["steer"] * turn * dt * (1.0 if v.dot(fwd0) >= -0.5 else -1.0)
            fwd = self.forward()
            right = V(fwd.z, 0.0, -fwd.x)
            vf, vl = v.dot(fwd), v.dot(right)
            th = inp["throttle"]
            if th > 0:
                vf += (ACCEL if vf >= 0 else BRAKE) * th * dt
            elif th < 0:
                vf += (BRAKE if vf > 0 else ACCEL * 0.8) * th * dt
            else:
                vf -= vf * min(1.0, COAST * dt)
            if self.boosting:
                vf += BOOST_ACC * dt
                self.boost = max(0.0, self.boost - BOOST_USE * dt)
            elif vf > MAX_DRIVE:
                vf = max(MAX_DRIVE, vf - 12.0 * dt)
            vf = clamp(vf, -MAX_REVERSE, MAX_BOOST)
            grip = 1.8 if inp["slide"] else 11.0
            vl -= vl * min(1.0, grip * dt)
            nv = fwd * vf + right * vl
            vy = v.y
            if inp["jump"]:
                vy = JUMP_V
                self.jumps, self.air_time = 1, 0.0
                self.grounded = self.was_grounded = False
                self.jump_lock = 0.15
            rb.velocity = V(nv.x, vy, nv.z)
        else:
            self.air_time += dt
            if self.jumps == 0:
                self.jumps = 1  # drove off something: one jump is still left
            if self.flip > 0.0:
                step = 360.0 / 0.55 * dt
                self.pitch += self.flip_dir[0] * step
                self.roll += self.flip_dir[1] * step
                self.flip -= dt
                if self.flip <= 0.0:
                    self.pitch = self.roll = 0.0  # a dodge is a full turn
            else:
                # W / S tilt the nose by up to 45 degrees and let go means level again: an exponential approach, so
                # it cannot overshoot or swing
                self.pitch += (inp["throttle"] * 45.0 - wrap(self.pitch)) * min(1.0, 7.0 * dt)
                if inp["roll"] != 0:
                    self.roll += inp["roll"] * 260.0 * dt
                elif inp["slide"]:
                    self.roll += inp["steer"] * 260.0 * dt  # free air roll on Ctrl
                else:
                    self.yaw += inp["steer"] * 170.0 * dt
                    self.roll -= wrap(self.roll) * min(1.0, 5.0 * dt)
            if self.boosting:
                pr, yr = math.radians(self.pitch), math.radians(self.yaw)
                d = V(math.sin(yr) * math.cos(pr), -math.sin(pr), math.cos(yr) * math.cos(pr))
                v = v + d * (BOOST_ACC * dt)
                self.boost = max(0.0, self.boost - BOOST_USE * dt)
            if inp["jump"] and self.jumps == 1 and self.air_time < 1.5:
                self.jumps = 2
                th, st = inp["throttle"], inp["steer"]
                if th != 0 or st != 0:
                    fwd = self.forward()
                    right = V(fwd.z, 0.0, -fwd.x)
                    d = (fwd * th + right * st).normalized()
                    v = V(v.x + d.x * DODGE_V, max(v.y, 2.5), v.z + d.z * DODGE_V)
                    self.flip, self.dodge_t = 0.55, 0.6
                    self.flip_dir = (float(th), float(-st))  # front/back flip = pitch, side flip = roll
                else:
                    v = V(v.x, max(v.y, 0.0) + DOUBLE_V, v.z)
            if v.length() > MAX_BOOST + 6.0:
                v = v * ((MAX_BOOST + 6.0) / v.length())
            rb.velocity = v
        t.rotation = V(self.pitch, self.yaw, self.roll)


class RocketLeague(me.Behaviour):
    match_minutes: float = 5.0
    start_mode: int = 0          # 0 = menu, 1..4 = start NvN at once
    bot_skill: float = 1.0
    top_view: bool = False       # start with the view over the whole arena (V toggles it)

    def OnStart(self):
        if hasattr(me, "debug"):
            me.debug.show_colliders(False)  # the wireframes cover the arena
        self.arena = []
        self.cars = []
        self.pads = []
        self.fx = []
        self.ball = None
        self.state = "menu"
        self.timer = 0.0
        self.clock = 0.0
        self.overtime = False
        self.score = {"blue": 0, "orange": 0}
        self.mode = 0
        self.role_cd = 0.0
        self.theme = ARENAS[0]
        self.ball_cam = False
        self.overview = bool(self.top_view)
        self.cam_pos = None
        self._build_arena(ARENAS[0])
        if 1 <= self.start_mode <= 4:
            self._start_match(self.start_mode)
        else:
            self._show_menu()

    def OnDestroy(self):
        if hasattr(me, "debug"):
            me.debug.show_colliders(True)
        for key in ("1title", "2score", "3time", "4boost", "5msg", "9help"):
            me.hud.clear(key)

    # ------------------------------------------------------------------ main loop
    def OnUpdate(self, dt):
        dt = min(dt, 0.05)
        self._update_camera(dt)
        if self.state == "menu":
            for n in (1, 2, 3, 4):
                if me.input.was_key_pressed(str(n)):
                    self._start_match(n)
                    return
            return
        if me.input.was_key_pressed("Esc") or me.input.was_key_pressed("M"):
            self._show_menu()
            return
        self._update_fx(dt)
        self._update_pads(dt)
        if self.state == "countdown":
            self.timer -= dt
            self._hold_kickoff()
            left = int(math.ceil(self.timer))
            me.hud.set("5msg", "- %d -" % left if left > 0 else "GO!")
            if self.timer <= 0.0:
                self.state = "play"
                me.hud.set("5msg", "GO!")
                self.timer = 0.7
        elif self.state == "play":
            if self.timer > 0.0:
                self.timer -= dt
                if self.timer <= 0.0:
                    me.hud.clear("5msg")
            if self.overtime:
                self.clock += dt
            else:
                self.clock -= dt
                if self.clock <= 0.0:
                    self.clock = 0.0
                    if self.score["blue"] == self.score["orange"]:
                        self.overtime = True
                        me.hud.set("5msg", "OVERTIME! Next goal wins")
                        self.timer = 2.0
                    else:
                        self._end_match()
                        return
            self._step_cars(dt)
            self._step_ball(dt)
            self._check_goal()
        elif self.state == "goal":
            self.timer -= dt
            self._step_cars(dt)
            if self.timer <= 0.0:
                if self.overtime or (self.clock <= 0.0 and self.score["blue"] != self.score["orange"]):
                    self._end_match()
                else:
                    self._kickoff()
        elif self.state == "end":
            if me.input.was_key_pressed("Enter"):
                self._show_menu()
        self._update_hud()

    # ------------------------------------------------------------------ match flow
    def _show_menu(self):
        self._clear_match()
        self.state = "menu"
        me.hud.set("1title", "=== ROCKET LEAGUE ===")
        me.hud.set("5msg", "Press  1 = 1v1    2 = 2v2    3 = 3v3    4 = 4v4")
        me.hud.set("9help", "W/S gas/brake  A/D steer  Space jump, x2 + direction = dodge  Shift boost  Ctrl slide  C ball cam  V arena  M menu")
        for key in ("2score", "3time", "4boost"):
            me.hud.clear(key)

    def _start_match(self, n):
        self._clear_match()
        self.mode = n
        self.theme = random.choice(ARENAS)
        self._build_arena(self.theme)
        self.ball = me.world.spawn("rl_ball", V(0.0, BALL_R, 0.0))
        self.ball.name = "RlBallMain"
        for team in ("blue", "orange"):
            for i in range(n):
                e = me.world.spawn("rl_car", V(0.0, CAR_Y, 0.0))
                e.name = "Car_%s_%d" % (team, i)
                e.mesh.material = MAT["car_" + team]
                player = team == "blue" and i == 0
                for cabin in me.world.find_all("RlCarCabin"):
                    cabin.name = "Cabin_%s_%d" % (team, i)
                    if player:
                        cabin.mesh.material = MAT["cabin_gold"]
                self.cars.append(Car(e, team, i, player))
        self.score = {"blue": 0, "orange": 0}
        self.clock = max(0.1, self.match_minutes) * 60.0
        self.overtime = False
        me.hud.set("1title", "ROCKET LEAGUE  %dv%d  -  %s" % (n, n, self.theme["name"]))
        me.hud.set("9help", "You = blue car with GOLD roof. Shift boost, Space x2 + direction = dodge. C ball cam, V arena view. Esc/M menu")
        self._kickoff()

    def _kickoff(self):
        self.state = "countdown"
        self.timer = 3.0
        self.ball.mesh.visible = True
        self.ball.rigidbody.is_kinematic = False
        for c in self.cars:
            c.dead = 0.0
            c.boost = 33.0
        self._hold_kickoff()

    def _hold_kickoff(self):
        for c in self.cars:
            x, z = SPOTS[c.index % len(SPOTS)]
            c.place(x * c.sign, z * c.sign)
        self.ball.transform.position = V(0.0, BALL_R + 0.05, 0.0)
        self.ball.rigidbody.velocity = V(0.0, 0.0, 0.0)

    def _end_match(self):
        self.state = "end"
        b, o = self.score["blue"], self.score["orange"]
        winner = "BLUE TEAM WINS!" if b > o else "ORANGE TEAM WINS!"
        me.hud.set("5msg", "%s  %d : %d   -  Enter / Esc for menu" % (winner, b, o))
        for c in self.cars:
            c.e.rigidbody.velocity = V(0.0, 0.0, 0.0)

    def _clear_match(self):
        for c in self.cars:
            if c.e.alive:
                c.e.destroy()
        self.cars = []
        if self.ball is not None and self.ball.alive:
            self.ball.destroy()
        self.ball = None
        for f in self.fx:
            if f[0].alive:
                f[0].destroy()
        self.fx = []

    def _update_hud(self):
        if self.state == "menu":
            return
        me.hud.set("2score", "BLUE %d  :  %d ORANGE" % (self.score["blue"], self.score["orange"]))
        secs = int(math.ceil(self.clock))
        prefix = "OVERTIME +" if self.overtime else "TIME "
        me.hud.set("3time", "%s%d:%02d" % (prefix, secs // 60, secs % 60))
        player = next((c for c in self.cars if c.player), None)
        if player is not None:
            bar = "#" * int(player.boost / 10) + "." * (10 - int(player.boost / 10))
            state = "  DEMOLISHED" if player.dead > 0 else ""
            me.hud.set("4boost", "BOOST [%s] %d%s" % (bar, int(player.boost), state))

    # ------------------------------------------------------------------ camera
    def _update_camera(self, dt):
        cam = getattr(me, "camera", None)  # needs the engine-side me.camera.set(position, rotation)
        if cam is None:
            return
        if me.input.was_key_pressed("C"):
            self.ball_cam = not self.ball_cam
            self.overview = False
        if me.input.was_key_pressed("V"):
            self.overview = not self.overview
        player = next((c for c in self.cars if c.player and c.dead <= 0.0), None)
        if player is None or self.ball is None or self.overview:
            want, look = V(0.0, 46.0, -40.0), V(0.0, 0.0, 0.0)
        else:
            p = player.e.transform.position
            p = V(p.x, 0.3 + (p.y - 0.3) * 0.5, p.z)  # a jump moves the camera half as much
            if self.ball_cam:
                d = flat(p - self.ball.transform.position)
                back = d.normalized() if d.length() > 0.5 else player.forward() * -1.0
                look = self.ball.transform.position
            else:
                back = player.forward() * -1.0
                look = p + player.forward() * 6.0 + V(0.0, 1.0, 0.0)
            want = p + back * 9.0 + V(0.0, 4.0, 0.0)
            want = V(want.x, max(want.y, 1.5), want.z)
        if self.cam_pos is None:
            self.cam_pos = want
        k = 1.0 - math.exp(-8.0 * dt)
        self.cam_pos = self.cam_pos + (want - self.cam_pos) * k
        d = look - self.cam_pos
        yaw = math.degrees(math.atan2(d.x, d.z))
        pitch = math.degrees(math.atan2(-d.y, max(0.01, flat(d).length())))
        cam.set(self.cam_pos, V(pitch, yaw, 0.0), 75.0)

    # ------------------------------------------------------------------ cars
    def _step_cars(self, dt):
        self.role_cd -= dt
        if self.role_cd <= 0.0:
            self.role_cd = 0.3
            self._assign_roles()
        for c in self.cars:
            c.hit_cd = max(0.0, c.hit_cd - dt)
            c.bump_cd = max(0.0, c.bump_cd - dt)
            if c.dead > 0.0:
                c.dead -= dt
                if c.dead <= 0.0:
                    x, z = SPOTS[2]
                    c.place((c.index * 4.0 - 6.0) * c.sign, z * c.sign)
                    c.boost = 33.0
                continue
            if c.e.transform.position.y < -5.0:
                c.place(0.0, -20.0 * c.sign)
            if self.state != "play":
                inp = {"throttle": 0, "steer": 0, "pitch": 0, "roll": 0, "jump": False, "boost": False, "slide": False}
            elif c.player:
                inp = self._player_input()
            else:
                inp = self._bot_input(c)
            c.drive(inp, dt)
            top = WALL_TOP - 1.0  # there is no ceiling collider
            cp = c.e.transform.position
            if cp.y > top:
                c.e.transform.position = V(cp.x, top, cp.z)
                cv = c.e.rigidbody.velocity
                c.e.rigidbody.velocity = V(cv.x, min(cv.y, 0.0), cv.z)
            if c.boosting:
                c.trail_cd -= dt
                if c.trail_cd <= 0.0:
                    c.trail_cd = 0.04
                    back = c.e.transform.position - c.forward() * 1.5
                    self._spawn_fx("rl_orb", back, c.forward() * -4.0, 0.35, -0.6, 0.35, "gold" if c.team == "blue" else "orange", False)
        if self.state == "play":
            self._car_ball_hits()
            self._car_car_hits()

    def _player_input(self):
        k = me.input.is_key_down
        th = (1 if k("W") else 0) - (1 if k("S") else 0)
        st = (1 if k("D") else 0) - (1 if k("A") else 0)
        roll = (1 if k("E") else 0) - (1 if k("Q") else 0)
        return {"throttle": th, "steer": st, "roll": roll, "jump": me.input.was_key_pressed("Space"),
                "boost": k("Shift"), "slide": k("Ctrl")}

    def _car_ball_hits(self):
        bp = self.ball.transform.position
        brb = self.ball.rigidbody
        for c in self.cars:
            if c.dead > 0.0 or c.hit_cd > 0.0:
                continue
            cp = c.e.transform.position
            d = bp - cp
            dist = d.length()
            if dist > BALL_R + 1.6 or dist < 0.01:
                continue
            n = (d.normalized() + c.forward() * 0.35).normalized()
            cv = c.e.rigidbody.velocity
            bv = brb.velocity
            closing = (cv - bv).dot(d.normalized())
            if closing < 0.5:
                continue
            power = closing * (1.7 if c.dodge_t > 0.0 else 1.2) + 3.0
            lift = 2.5 if c.grounded else 4.0
            brb.velocity = bv + n * power + V(0.0, lift, 0.0)
            c.e.rigidbody.velocity = cv * 0.8
            c.hit_cd = 0.2

    def _car_car_hits(self):
        alive = [c for c in self.cars if c.dead <= 0.0]
        for i in range(len(alive)):
            for j in range(i + 1, len(alive)):
                a, b = alive[i], alive[j]
                if a.bump_cd > 0.0 and b.bump_cd > 0.0:
                    continue
                d = b.e.transform.position - a.e.transform.position
                dist = d.length()
                if dist > 2.6 or dist < 0.01:
                    continue
                n = d.normalized()
                av, bv = a.e.rigidbody.velocity, b.e.rigidbody.velocity
                closing = (av - bv).dot(n)
                if closing <= 1.0:
                    continue
                if a.team != b.team and av.length() > DEMO_SPEED and av.normalized().dot(n) > 0.6:
                    self._demolish(b, a)
                elif a.team != b.team and bv.length() > DEMO_SPEED and bv.normalized().dot(n * -1.0) > 0.6:
                    self._demolish(a, b)
                else:
                    b.e.rigidbody.velocity = bv + n * (closing * 0.8) + V(0.0, 3.0, 0.0)
                    a.e.rigidbody.velocity = av - n * (closing * 0.5)
                a.bump_cd = b.bump_cd = 0.4

    def _demolish(self, victim, by):
        p = victim.e.transform.position
        self._explosion(p, victim.team, False)
        victim.dead = 3.0
        victim.e.rigidbody.is_kinematic = True
        victim.e.rigidbody.velocity = V(0.0, 0.0, 0.0)
        victim.e.transform.position = V(self.cars.index(victim) * 5.0 - 20.0, -40.0, 0.0)
        if victim.player or by.player:
            me.hud.set("5msg", "DEMOLISHED!" if victim.player else "DEMOLITION!")
            self.timer = 1.5

    # ------------------------------------------------------------------ bots
    def _assign_roles(self):
        bp = self.ball.transform.position
        for team in ("blue", "orange"):
            members = [c for c in self.cars if c.team == team and c.dead <= 0.0]
            if not members:
                continue
            sign = members[0].sign
            own_z = -HZ * sign

            def attack_cost(c):
                p = c.e.transform.position
                wrong_side = (p.z - bp.z) * sign > 2.0
                return flat(bp - p).length() + (10.0 if wrong_side else 0.0) - c.boost * 0.03

            members.sort(key=attack_cost)
            members[0].role = "attacker"
            rest = sorted(members[1:], key=lambda c: abs(c.e.transform.position.z - own_z))
            for k, c in enumerate(rest):
                c.role = "defender" if k == 0 else "support"

    def _bot_input(self, c):
        p = c.e.transform.position
        fwd = c.forward()
        sign = c.sign
        own_z, enemy_z = -HZ * sign, HZ * sign
        bp = self.ball.transform.position
        bv = self.ball.rigidbody.velocity
        t = clamp(flat(bp - p).length() / 25.0, 0.0, 1.2) * self.bot_skill
        pred = V(clamp(bp.x + bv.x * t, -HX + 1.5, HX - 1.5), bp.y, clamp(bp.z + bv.z * t, -HZ + 1.0, HZ - 1.0))
        ball_dist = flat(bp - p).length()
        stop_at_target = False
        role = c.role
        danger = (bp.z - own_z) * sign < 14.0
        if role == "defender" and danger:
            role = "attacker"
        if role == "support" and c.boost < 35.0:
            role = "boost"

        if role == "attacker":
            to_goal = flat(V(0.0, 0.0, enemy_z) - pred).normalized()
            if (p.z - pred.z) * sign > 1.0:
                side = 4.0 if p.x > pred.x else -4.0
                target = V(pred.x + side, 0.0, pred.z - 6.0 * sign)  # get back goal-side of the ball
            elif ball_dist < 8.0:
                target = pred - to_goal * 0.6
            else:
                target = pred - to_goal * 2.5
        elif role == "defender":
            target = V(clamp(pred.x * 0.35, -GOAL_W, GOAL_W), 0.0, own_z + 6.0 * sign)
            stop_at_target = True
        elif role == "boost":
            best, best_d = None, 1e9
            for pad in self.pads:
                if pad[3] <= 0.0:
                    d = flat(pad[1] - p).length() - (8.0 if pad[2] else 0.0)
                    if d < best_d:
                        best, best_d = pad, d
            target = best[1] if best is not None else V(0.0, 0.0, own_z * 0.5)
        else:
            lane = 9.0 if c.index % 2 else -9.0
            target = V(clamp(pred.x * 0.5 + lane, -HX + 3.0, HX - 3.0), 0.0, clamp(pred.z - 12.0 * sign, -HZ + 4.0, HZ - 4.0))
            stop_at_target = True

        d = flat(target - p)
        dist = d.length()
        desired = math.degrees(math.atan2(d.x, d.z))
        diff = wrap(desired - c.yaw)
        steer = clamp(diff / 25.0, -1.0, 1.0)
        throttle = 1
        if stop_at_target and dist < 3.0:
            throttle = 0
            steer = clamp(wrap(math.degrees(math.atan2(bp.x - p.x, bp.z - p.z)) - c.yaw) / 25.0, -1.0, 1.0)
        # keep away from teammates that are right in front of us
        for m in self.cars:
            if m is c or m.team != c.team or m.dead > 0.0:
                continue
            dm = flat(m.e.transform.position - p)
            if dm.length() < 5.0 and dm.normalized().dot(fwd) > 0.7 and role != "attacker":
                right = V(fwd.z, 0.0, -fwd.x)
                steer = clamp(steer - (1.0 if dm.dot(right) > 0 else -1.0), -1.0, 1.0)
        boost = c.boost > 0.0 and abs(diff) < 15.0 and (dist > 12.0 or (role == "attacker" and ball_dist < 10.0))
        jump = False
        if role == "attacker":
            if c.grounded and ball_dist < 4.5 and 2.6 < bp.y < 6.0 and abs(diff) < 30.0:
                jump = True
            elif c.jumps == 1 and 0.1 < c.air_time < 0.6 and ball_dist < 5.0:
                jump = True  # dodge into the ball
            elif c.grounded and ball_dist < 7.0 and bp.y < 2.5 and abs(diff) < 10.0 and c.e.rigidbody.velocity.length() > 15.0:
                jump = random.random() < 0.05  # occasional flip into a ground ball
        if not c.grounded and c.jumps == 1 and c.air_time > 0.1 and role == "attacker" and ball_dist < 6.0:
            throttle, steer = 1, 0
        return {"throttle": throttle, "steer": steer, "roll": 0, "jump": jump, "boost": boost,
                "slide": c.grounded and abs(diff) > 75.0 and dist > 4.0}

    # ------------------------------------------------------------------ ball and goals
    def _step_ball(self, dt):
        rb = self.ball.rigidbody
        t = self.ball.transform
        v = rb.velocity
        damp = 1.0 - 0.15 * dt
        v = V(v.x * damp, v.y, v.z * damp)
        if v.length() > 42.0:
            v = v * (42.0 / v.length())
        p = t.position
        if p.y > WALL_TOP - BALL_R and v.y > 0.0:  # there is no ceiling collider: bounce off the top
            t.position = V(p.x, WALL_TOP - BALL_R, p.z)
            p = t.position
            v = V(v.x, -v.y * 0.5, v.z)
        rb.velocity = v
        r = t.rotation
        t.rotation = V(r.x + v.z * 45.0 * dt, r.y, r.z - v.x * 45.0 * dt)
        if abs(p.x) > HX + 3.0 or p.y < -3.0 or p.y > WALL_TOP + 3.0 or abs(p.z) > HZ + GOAL_D + 3.0:
            t.position = V(0.0, 6.0, 0.0)  # escaped through a gap: drop it back in the middle
            rb.velocity = V(0.0, 0.0, 0.0)

    def _check_goal(self):
        p = self.ball.transform.position
        if abs(p.x) < GOAL_W and p.y < GOAL_H and abs(p.z) > HZ + BALL_R:
            team = "blue" if p.z > 0 else "orange"
            self.score[team] += 1
            self._explosion(p, "orange" if p.z > 0 else "blue", True)
            self.ball.mesh.visible = False
            self.ball.rigidbody.is_kinematic = True
            self.ball.rigidbody.velocity = V(0.0, 0.0, 0.0)
            self.ball.transform.position = V(0.0, -30.0, 0.0)
            me.hud.set("5msg", "*** GOAL!  %s scores ***" % team.upper())
            self.state = "goal"
            self.timer = 3.0

    # ------------------------------------------------------------------ boost pads
    def _update_pads(self, dt):
        for pad in self.pads:
            e, pos, big, cd = pad
            if cd > 0.0:
                pad[3] = cd - dt
                if pad[3] <= 0.0:
                    e.mesh.visible = True
                continue
            for c in self.cars:
                if c.dead > 0.0 or c.boost >= 100.0:
                    continue
                cp = c.e.transform.position
                if cp.y < 2.5 and flat(cp - pos).length() < (2.2 if big else 1.5):
                    c.boost = 100.0 if big else min(100.0, c.boost + 12.0)
                    pad[3] = 10.0 if big else 4.0
                    e.mesh.visible = False
                    break

    # ------------------------------------------------------------------ effects
    def _spawn_fx(self, prefab, pos, vel, scale, grow, life, mat, gravity):
        if len(self.fx) > 260:
            return
        e = me.world.spawn(prefab, pos)
        e.transform.scale = V(scale, scale, scale)
        e.mesh.material = MAT[mat]
        self.fx.append([e, vel, scale, grow, life, gravity])

    def _explosion(self, pos, team, big):
        n = 36 if big else 16
        self._spawn_fx("rl_orb", pos, V(0.0, 0.0, 0.0), 1.0, 24.0 if big else 12.0, 0.5, "gold", False)
        self._spawn_fx("rl_orb", pos, V(0.0, 0.0, 0.0), 0.5, 16.0 if big else 8.0, 0.8, team, False)
        for _ in range(n):
            d = V(random.uniform(-1, 1), random.uniform(0.2, 1.2), random.uniform(-1, 1)).normalized()
            speed = random.uniform(10.0, 26.0) if big else random.uniform(6.0, 14.0)
            mat = random.choice((team, "gold", "white"))
            self._spawn_fx("rl_deco", pos, d * speed, random.uniform(0.3, 0.7), -0.25, random.uniform(1.0, 1.8), mat, True)

    def _update_fx(self, dt):
        keep = []
        for f in self.fx:
            e, vel, scale, grow, life, gravity = f
            if not e.alive:
                continue
            life -= dt
            scale += grow * dt
            if life <= 0.0 or scale <= 0.02:
                e.destroy()
                continue
            if gravity:
                vel = V(vel.x, vel.y - 20.0 * dt, vel.z)
            t = e.transform
            t.position = t.position + vel * dt
            t.scale = V(scale, scale, scale)
            if gravity:
                r = t.rotation
                t.rotation = V(r.x + 400.0 * dt, r.y + 250.0 * dt, r.z)
            keep.append([e, vel, scale, grow, life, gravity])
        self.fx = keep

    # ------------------------------------------------------------------ arena
    def _box(self, x, y, z, sx, sy, sz, mat, solid=True, yaw=0.0, visible=True, mesh=None):
        e = me.world.spawn("rl_block" if solid else "rl_deco", V(x, y, z))
        e.transform.scale = V(sx, sy, sz)
        if yaw:
            e.transform.rotation = V(0.0, yaw, 0.0)
        e.mesh.material = mat if mat.startswith("assets/") else MAT[mat]
        if mesh:
            e.mesh.mesh = mesh
        if not visible:
            e.mesh.visible = False
        self.arena.append(e)
        return e

    def _wall(self, x, z, sx, sz, mat, yaw=0.0, vis_h=WALL_VIS):
        # the collider is one invisible block up to the ceiling, the visible part is a block without a collider
        self._box(x, WALL_TOP * 0.5, z, sx, WALL_TOP, sz, mat, yaw=yaw, visible=False)
        self._box(x, vis_h * 0.5, z, sx, vis_h, sz, mat, solid=False, yaw=yaw)

    def _build_arena(self, theme):
        for e in self.arena:
            if e.alive:
                e.destroy()
        self.arena = []
        self.pads = []
        key, extra = theme["key"], theme["extra"]
        tex = lambda name: _mat("%s_%s" % (key, name))
        depth = HZ + GOAL_D
        # the floor texture carries the markings (centre line and circle, penalty areas, team goal lines)
        # the top is 2 cm below zero on purpose: the box then stays in one layer of the broad-phase grid (half the cells)
        self._box(0.0, -0.52, 0.0, 2 * HX + 2, 1.0, 2 * depth + 4, tex("field"))
        # long walls: the texture of the right one is mirrored so that the blue half stays on the blue side
        self._wall(-(HX + 0.5), 0.0, 1.0, 2 * HZ, tex("wall_left"))
        self._wall(HX + 0.5, 0.0, 1.0, 2 * HZ, tex("wall_right"))
        for sz in (-1.0, 1.0):
            team = "blue" if sz < 0 else "orange"
            end = tex("wall_end_" + team)
            ez = sz * (HZ + 0.5)
            for sx in (-1.0, 1.0):
                self._wall(sx * (GOAL_W + (HX - GOAL_W) * 0.5), ez, HX - GOAL_W, 1.0, end)
                # rounded-ish corners: a 45 degree wall across each corner
                self._wall(sx * (HX - 2.0), sz * (HZ - 2.0), 6.5, 1.0, end, yaw=45.0 if sx * sz > 0 else -45.0)
            goal = "goal_" + team
            self._box(0.0, (GOAL_H + WALL_TOP) * 0.5, ez, 2 * GOAL_W, WALL_TOP - GOAL_H, 1.0, end, visible=False)
            self._box(0.0, GOAL_H + 0.5, ez, 2 * GOAL_W, 1.0, 1.0, goal, solid=False)          # crossbar
            gz = sz * (HZ + GOAL_D * 0.5)
            self._box(0.0, GOAL_H * 0.5, sz * (depth + 0.5), 2 * GOAL_W + 2, GOAL_H, 1.0, goal)  # back of the net
            for sx in (-1.0, 1.0):
                self._box(sx * (GOAL_W + 0.5), GOAL_H * 0.5, gz, 1.0, GOAL_H + 2, GOAL_D + 1, goal)  # posts / sides
            self._box(0.0, GOAL_H + 0.5, gz, 2 * GOAL_W + 2, 1.0, GOAL_D, goal, visible=False)
        for x, z in BIG_PADS:
            e = me.world.spawn("rl_orb", V(x, 0.6, z))
            e.transform.scale = V(0.7, 0.7, 0.7)
            e.mesh.material = MAT["orange"]
            self.arena.append(e)
            self._box(x, 0.03, z, 2.2, 0.06, 2.2, "pad_big", solid=False)
            self.pads.append([e, V(x, 0.0, z), True, 0.0])
        for x, z in SMALL_PADS:
            e = self._box(x, 0.1, z, 0.9, 0.2, 0.9, "pad_small", solid=False)
            self.pads.append([e, V(x, 0.0, z), False, 0.0])
        if extra == "stands":
            for sx in (-1.0, 1.0):
                for k in range(3):
                    crowd = "crowd_white" if k % 2 else ("crowd_blue" if sx < 0 else "crowd_orange")
                    self._box(sx * (HX + 3 + k * 2.5), 1.0 + k * 1.5, 0.0, 2.5, 2.0 + k * 3.0, 2 * HZ, crowd, solid=False)
        elif extra == "neon":
            for sx in (-1.0, 1.0):
                for z in range(-25, 30, 10):
                    self._box(sx * (HX + 0.5), WALL_VIS + 0.15, float(z), 1.2, 0.3, 6.0, "neon_blue" if z < 0 else "neon_orange", solid=False)
            for sz in (-1.0, 1.0):
                for sx in (-1.0, 1.0):
                    e = me.world.spawn("rl_orb", V(sx * (GOAL_W + 0.5), GOAL_H + 2.0, sz * (HZ + 0.5)))
                    e.transform.scale = V(1.2, 1.2, 1.2)
                    e.mesh.material = MAT["blue" if sz < 0 else "orange"]
                    self.arena.append(e)
        elif extra == "desert":
            for k in range(8):
                sx = -1.0 if k % 2 else 1.0
                z = -30.0 + k * 8.5
                self._box(sx * (HX + 6 + (k % 3) * 3), 2.5 + k % 2, z, 6.0, 5.0 + (k % 3) * 2, 6.0, "sandstone", solid=False, mesh="assets/models/pyramid.obj")
                self._box(sx * (HX + 2.5), 0.6, z + 3.0, 1.6, 1.2, 1.4, "rock", solid=False, yaw=k * 37.0)
