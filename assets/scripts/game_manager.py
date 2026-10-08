import math

import myengine as me


class GameManager(me.Behaviour):
    target_score: int = 10
    round_duration: float = 60.0
    player_name: str = "Controlled_1"
    coin_spawner_name: str = "CoinSpawner"
    enemy_spawner_name: str = "EnemySpawner"
    hit_cooldown: float = 1.5
    fall_height: float = -4.0
    fall_damage: int = 1
    firework_count: int = 20
    firework_speed: float = 5.0

    def OnStart(self):
        player = me.world.find(self.player_name)
        self._start_position = player.transform.position if player else me.Vec3(0, 0.5, 2)
        self._firework_prefix = f"Firework_{self.entity.id}_"
        self._fireworks = me.world.find_all(self._firework_prefix)
        self._clear_fireworks()
        self.round_id = 1
        self.score = 0
        self.remaining = max(0.0, self.round_duration)
        self.state = "playing"
        self._hit_timer = 0.0
        self._publish_hud()
        me.log.info("GameManager: round started")

    def OnUpdate(self, dt):
        self._fireworks = [entity for entity in self._fireworks if entity.alive]
        if me.input.was_key_pressed("R"):
            self.restart()
            return
        if self.state != "playing":
            return

        self._hit_timer = max(0.0, self._hit_timer - max(0.0, dt))
        self.remaining = max(0.0, self.remaining - max(0.0, dt))
        player = me.world.find(self.player_name)
        if player is None:
            self._finish("lost")
        elif self.score >= max(1, self.target_score):
            self._finish("won")
        elif self.remaining <= 0.0:
            self._finish("lost")
        elif player.transform.position.y < self.fall_height:
            self.hit_player(self.fall_damage)
        self._publish_hud()

    def add_score(self, value):
        if self.state != "playing":
            return
        self.score += max(0, int(value))
        if self.score >= max(1, self.target_score):
            self._finish("won")
        self._publish_hud()

    def hit_player(self, damage):
        if self.state != "playing" or self._hit_timer > 0.0:
            return
        self.score = max(0, self.score - max(0, int(damage)))
        self._reset_player()
        # Several enemies touching in the same frame count as one hit.
        self._hit_timer = max(0.0, self.hit_cooldown)
        self._publish_hud()

    def restart(self):
        self.round_id += 1
        self.score = 0
        self.remaining = max(0.0, self.round_duration)
        self.state = "playing"
        self._hit_timer = 0.0
        self._clear_fireworks()
        self._reset_player()
        me.send(self.coin_spawner_name, "reset_round", self.round_id)
        me.send(self.enemy_spawner_name, "reset_round", self.round_id)
        self._publish_hud()
        me.log.info("GameManager: round restarted")

    def OnReload(self):
        # L2 keeps score, timers and owned handles; only the rules/defaults change.
        self._publish_hud()

    def OnDestroy(self):
        self._clear_fireworks()
        for key in ("t6.score", "t6.time", "t6.state", "t6.controls"):
            me.hud.clear(key)

    def _reset_player(self):
        player = me.world.find(self.player_name)
        if player is not None:
            player.transform.position = self._start_position
            player.rigidbody.velocity = me.Vec3()

    def _finish(self, state):
        if self.state != "playing":
            return
        self.state = state
        me.send(self.enemy_spawner_name, "stop")
        if state == "won":
            player = me.world.find(self.player_name)
            center = player.transform.position if player else self._start_position
            count = max(0, self.firework_count)
            speed = max(0.0, self.firework_speed)
            for index in range(count):
                angle = math.tau * index / count
                particle = me.world.spawn("firework", center + me.Vec3(0, 1, 0))
                particle.name = self._firework_prefix + str(particle.id)
                particle.rigidbody.velocity = me.Vec3(math.cos(angle) * speed, speed, math.sin(angle) * speed)
                self._fireworks.append(particle)
        me.log.info(f"GameManager: {state}, score={self.score}")

    def _clear_fireworks(self):
        for entity in self._fireworks:
            if entity.alive:
                entity.destroy()
        self._fireworks = []

    def _publish_hud(self):
        me.hud.set("t6.score", f"Score: {self.score} / {max(1, self.target_score)}")
        me.hud.set("t6.time", f"Time: {math.ceil(max(0.0, self.remaining))} s")
        status = {"playing": "Collect the gold coins, avoid the chasers!", "won": "You won! Press R to restart.", "lost": "Round lost. Press R to restart."}
        me.hud.set("t6.state", status[self.state])
        me.hud.set("t6.controls", "WASD: move | Space: jump | R: restart")
