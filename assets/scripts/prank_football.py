import myengine as me


class PrankFootball(me.Behaviour):
    """Football of the wheels without bodies (the "naked cars" mini-scene of the easter egg generator): the ball in the goal is a point."""

    ball_name: str = "Мяч"
    goal_z: float = 13.0
    goal_half_width: float = 2.6
    ball_start_x: float = 0.0
    ball_start_y: float = 0.6
    ball_start_z: float = 2.0
    fall_limit: float = -3.0
    manager_name: str = "GameManager"

    def OnUpdate(self, dt):
        manager = self._manager()
        ball = me.world.find(self.ball_name)
        if manager is None or ball is None or manager.state != "playing":
            return
        position = ball.transform.position
        if position.z > self.goal_z and abs(position.x) < self.goal_half_width:
            me.send(self.manager_name, "add_score", 1)
            self._reset(ball)
        elif position.y < self.fall_limit:
            self._reset(ball)

    def _reset(self, ball):
        ball.transform.position = me.Vec3(self.ball_start_x, self.ball_start_y, self.ball_start_z)
        ball.rigidbody.velocity = me.Vec3()

    def _manager(self):
        from game_manager import GameManager
        entity = me.world.find(self.manager_name)
        return entity.get_script(GameManager) if entity else None
