import math

import myengine as me


class CoinSpawner(me.Behaviour):
    max_coins: int = 6
    spawn_interval: float = 1.5
    spawn_radius: float = 4.0
    prefab_name: str = "coin"
    manager_name: str = "GameManager"

    def OnStart(self):
        self._prefix = f"Coin_{self.entity.id}_"
        # Adopt our old roots after an L1 reload, so they cannot become orphaned.
        self._coins = me.world.find_all(self._prefix)
        self._round_id = None
        self._spawn_index = 0
        self._timer = 0.0
        self._fill = True

    def OnUpdate(self, dt):
        manager = self._manager()
        if manager is None:
            return
        if self._round_id != manager.round_id:
            self.reset_round(manager.round_id)
        self._coins = [entity for entity in self._coins if entity.alive]
        if manager.state != "playing":
            return

        limit = max(0, self.max_coins)
        # A reduced inspector value takes effect without waiting for collection.
        for entity in self._coins[limit:]:
            entity.destroy()
        self._coins = self._coins[:limit]
        if self._fill:
            while len(self._coins) < limit:
                self._spawn_one()
            self._fill = False
            self._timer = max(0.05, self.spawn_interval)
        else:
            self._timer -= max(0.0, dt)
            if self._timer <= 0.0 and len(self._coins) < limit:
                self._spawn_one()
                self._timer = max(0.05, self.spawn_interval)

    def reset_round(self, round_id):
        self._clear()
        self._round_id = round_id
        self._spawn_index = 0
        self._timer = 0.0
        self._fill = True

    def OnDestroy(self):
        self._clear()

    def _spawn_one(self):
        # Golden-angle placement is reproducible and stays inside the demo floor.
        angle = self._spawn_index * math.pi * (3.0 - math.sqrt(5.0))
        radius = max(0.0, self.spawn_radius) * (0.5 + 0.25 * (self._spawn_index % 3))
        center = self.entity.transform.position
        position = center + me.Vec3(math.cos(angle) * radius, 0, math.sin(angle) * radius)
        coin = me.world.spawn(self.prefab_name, position)
        coin.name = self._prefix + str(coin.id)
        self._coins.append(coin)
        self._spawn_index += 1

    def _clear(self):
        for entity in self._coins:
            if entity.alive:
                entity.destroy()
        self._coins = []

    def _manager(self):
        # Resolve the current class, not a stale imported class from before reload.
        from game_manager import GameManager
        entity = me.world.find(self.manager_name)
        return entity.get_script(GameManager) if entity else None
