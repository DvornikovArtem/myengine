"""Test behaviours only: copied into a temporary scripts directory by the C++ test."""
import myengine as me

events = []


class Counter(me.Behaviour):
    count: int = 2
    label: str = "counter"
    enabled: bool = True

    def OnStart(self):
        events.append(("start", self.entity.name))

    def OnUpdate(self, dt):
        if self.enabled:
            self.count += 1

    def add(self, amount):
        self.count += amount
        me.hud.set("counter", f"Count: {self.count}")

    def fail(self):
        raise RuntimeError("message failure")

    def OnDestroy(self):
        events.append(("destroy", self.entity.name))


class Listener(me.Behaviour):
    collisions: int = 0
    triggers: int = 0

    def OnCollision(self, other, point, normal, impulse):
        self.collisions += 1
        self.normal = normal
        self.other = other

    def OnTrigger(self, other):
        self.triggers += 1


class Broken(me.Behaviour):
    def OnUpdate(self, dt):
        raise ValueError("update failure")


class ReloadProbe(me.Behaviour):
    speed: float = 3.0
    ticks: int = 0
    reloads: int = 0

    def OnStart(self):
        self.state = 42

    def OnUpdate(self, dt):
        self.ticks += 1

    def OnReload(self):
        self.reloads += 1


class PrefabSpawner(me.Behaviour):
    def OnUpdate(self, dt):
        if not hasattr(self, "spawned"):
            self.spawned = me.world.spawn("test", me.Vec3(4, 2, 3))
            self.waiting_for_start = self.spawned.get_script(Counter) is None
