import myengine as me


class Firework(me.Behaviour):
    lifetime: float = 2.0

    def OnStart(self):
        self.remaining = max(0.0, self.lifetime)

    def OnUpdate(self, dt):
        self.remaining -= max(0.0, dt)
        if self.remaining <= 0.0:
            self.entity.destroy()
