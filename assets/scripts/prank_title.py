import myengine as me


class PrankTitle(me.Behaviour):
    """The title and the hint of a mini-scene of the easter egg generator (docs/editor/prank-tab.md): two lines on the HUD."""

    title: str = ""
    hint: str = ""

    def OnStart(self):
        me.hud.set("prank.title", self.title)
        me.hud.set("prank.hint", self.hint)

    def OnReload(self):
        me.hud.set("prank.title", self.title)
        me.hud.set("prank.hint", self.hint)

    def OnDestroy(self):
        me.hud.clear("prank.title")
        me.hud.clear("prank.hint")
