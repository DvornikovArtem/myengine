import math
import random

import myengine as me


class PrankFireballs(me.Behaviour):
    """Fireballs fall on the arena (the "lava" mini-scene of the easter egg generator). A ball that touches the player is a hit of the game manager."""

    interval: float = 0.55
    radius: float = 6.5
    height: float = 10.0
    hit_radius: float = 0.9
    fall_limit: float = -2.0
    ball_scale: float = 0.55
    manager_name: str = "GameManager"
    player_name: str = "Controlled_1"

    def OnStart(self):
        self._prefix = f"Fireball_{self.entity.id}_"
        self._balls = me.world.find_all(self._prefix)
        self._timer = 1.0
        self._round_id = None
        self._rng = random.Random(7)

    def OnUpdate(self, dt):
        manager = self._manager()
        if manager is None:
            return
        if self._round_id != manager.round_id:
            self._clear()
            self._round_id = manager.round_id
        self._balls = [ball for ball in self._balls if ball.alive]
        if manager.state != "playing":
            return
        player = me.world.find(self.player_name)
        for ball in self._balls:
            position = ball.transform.position
            if position.y < self.fall_limit:
                ball.destroy()
            elif player is not None and (position - player.transform.position).length() < max(0.1, self.hit_radius):
                me.send(self.manager_name, "hit_player", 1)
                ball.destroy()
        self._timer -= max(0.0, dt)
        if self._timer <= 0.0:
            self._timer = max(0.05, self.interval)
            self._spawn()

    def OnDestroy(self):
        self._clear()

    def _spawn(self):
        center = self.entity.transform.position
        angle = self._rng.random() * math.tau
        distance = math.sqrt(self._rng.random()) * max(0.0, self.radius)
        ball = me.world.spawn("firework", me.Vec3(center.x + math.cos(angle) * distance, self.height, center.z + math.sin(angle) * distance))
        ball.name = self._prefix + str(ball.id)
        size = max(0.1, self.ball_scale)
        ball.transform.scale = me.Vec3(size, size, size)
        self._balls.append(ball)

    def _clear(self):
        for ball in self._balls:
            if ball.alive:
                ball.destroy()
        self._balls = []

    def _manager(self):
        from game_manager import GameManager
        entity = me.world.find(self.manager_name)
        return entity.get_script(GameManager) if entity else None
