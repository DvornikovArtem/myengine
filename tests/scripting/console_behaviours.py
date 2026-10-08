import myengine as me


class ConsoleCounter(me.Behaviour):
    count: int = 10

    def OnUpdate(self, dt):
        self.count += 1

    def fail(self):
        raise ValueError("failure inside a game behaviour")


class ConsoleFault(me.Behaviour):
    def OnUpdate(self, dt):
        raise RuntimeError("intentional D6 script fault")


def fail_from_module():
    raise ValueError("failure inside a script file")
