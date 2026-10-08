import myengine as me


class Chaser(me.Behaviour):
    speed: float = 3.0
    damage: int = 1
    aggressive: bool = True
    target_name: str = "Controlled_1"
    detection_radius: float = 20.0
    catch_radius: float = 0.8
    manager_name: str = "GameManager"

    def OnUpdate(self, dt):
        manager = self._manager()
        target = me.world.find(self.target_name)
        velocity = self.entity.rigidbody.velocity
        direction = me.Vec3()
        if self.aggressive and manager is not None and manager.state == "playing" and target is not None:
            delta = target.transform.position - self.entity.transform.position
            if delta.length() <= max(0.0, self.detection_radius):
                direction = me.Vec3(delta.x, 0, delta.z).normalized() * max(0.0, self.speed)
                if delta.length() <= max(0.0, self.catch_radius):
                    self._hit(target)
                    direction = me.Vec3()
        # Physics still owns gravity, vertical motion and contact resolution.
        self.entity.rigidbody.velocity = me.Vec3(direction.x, velocity.y, direction.z)

    def OnCollision(self, other, point, normal, impulse):
        self._hit(other)

    def OnTrigger(self, other):
        self._hit(other)

    def _hit(self, other):
        if self.aggressive and other.alive and other.name == self.target_name:
            me.send(self.manager_name, "hit_player", self.damage)

    def _manager(self):
        from game_manager import GameManager
        entity = me.world.find(self.manager_name)
        return entity.get_script(GameManager) if entity else None
