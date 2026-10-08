import myengine as me


class Coin(me.Behaviour):
    spin_speed: float = 90.0
    score_value: int = 1
    manager_name: str = "GameManager"

    def OnStart(self):
        self.collected = False

    def OnUpdate(self, dt):
        transform = self.entity.transform
        transform.rotation = transform.rotation + me.Vec3(0, self.spin_speed * max(0.0, dt), 0)

    def OnTrigger(self, other):
        if self.collected or not other.alive:
            return
        from game_manager import GameManager
        entity = me.world.find(self.manager_name)
        manager = entity.get_script(GameManager) if entity else None
        if manager is None or manager.state != "playing" or other.name != manager.player_name:
            return
        self.collected = True
        me.send(self.manager_name, "add_score", self.score_value)
        self.entity.destroy() # the root and its glow child go away at the end of the step
