import math

import myengine as me


class EnemySpawner(me.Behaviour):
    wave_size: int = 1
    wave_growth: int = 1
    wave_interval: float = 10.0
    initial_delay: float = 4.0
    max_enemies: int = 8
    spawn_radius: float = 1.5
    prefab_name: str = "chaser"
    manager_name: str = "GameManager"

    def OnStart(self):
        self._prefix = f"Chaser_{self.entity.id}_"
        self._enemies = me.world.find_all(self._prefix)
        self._round_id = None
        self._wave = 0
        self._timer = max(0.0, self.initial_delay)

    def OnUpdate(self, dt):
        manager = self._manager()
        if manager is None:
            self.stop()
            return
        if self._round_id != manager.round_id:
            self.reset_round(manager.round_id)
        self._enemies = [entity for entity in self._enemies if entity.alive]
        if manager.state != "playing":
            self.stop()
            return

        limit = max(0, self.max_enemies)
        for entity in self._enemies[limit:]:
            entity.destroy()
        self._enemies = self._enemies[:limit]
        self._timer -= max(0.0, dt)
        if self._timer > 0.0:
            return

        size = max(0, self.wave_size + self._wave * self.wave_growth)
        count = min(size, max(0, limit - len(self._enemies)))
        center = self.entity.transform.position
        for index in range(count):
            angle = math.tau * index / count + self._wave
            radius = max(0.0, self.spawn_radius)
            position = center + me.Vec3(math.cos(angle) * radius, 0, math.sin(angle) * radius)
            enemy = me.world.spawn(self.prefab_name, position)
            enemy.name = self._prefix + str(enemy.id)
            self._enemies.append(enemy)
        self._wave += 1
        self._timer = max(0.05, self.wave_interval)

    def reset_round(self, round_id):
        self._clear()
        self._round_id = round_id
        self._wave = 0
        self._timer = max(0.0, self.initial_delay)

    def stop(self):
        for entity in self._enemies:
            if entity.alive:
                velocity = entity.rigidbody.velocity
                entity.rigidbody.velocity = me.Vec3(0, velocity.y, 0)

    def OnDestroy(self):
        self._clear()

    def _clear(self):
        for entity in self._enemies:
            if entity.alive:
                entity.destroy()
        self._enemies = []

    def _manager(self):
        from game_manager import GameManager
        entity = me.world.find(self.manager_name)
        return entity.get_script(GameManager) if entity else None
