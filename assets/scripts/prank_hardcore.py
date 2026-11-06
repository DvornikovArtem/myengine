import myengine as me


class PrankHardcore(me.Behaviour):
    """One touch and it is over (the "hardcore" mini-scene of the easter egg generator): a touch of a chaser, or a fall, loses the round at once."""

    touch_radius: float = 1.0
    fall_limit: float = -2.0
    chaser_prefix: str = "Chaser_"
    manager_name: str = "GameManager"
    player_name: str = "Controlled_1"

    def OnUpdate(self, dt):
        manager = self._manager()
        if manager is None or manager.state != "playing":
            return
        player = me.world.find(self.player_name)
        if player is None:
            return
        position = player.transform.position
        if position.y < self.fall_limit:
            self._lose(manager)
            return
        for chaser in me.world.find_all(self.chaser_prefix):
            if chaser.alive and (chaser.transform.position - position).length() < max(0.1, self.touch_radius):
                self._lose(manager)
                return

    def _lose(self, manager):
        manager._finish("lost")
        me.hud.set("prank.hint", "Одно касание. Без сохранений. R - ещё раз.")

    def _manager(self):
        from game_manager import GameManager
        entity = me.world.find(self.manager_name)
        return entity.get_script(GameManager) if entity else None
