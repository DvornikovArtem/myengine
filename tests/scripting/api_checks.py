"""Assertions run inside the engine's embedded interpreter, not an installed Python."""
import myengine as me


def components():
    player = me.world.find("Player")
    assert player is not None and player.alive
    assert player == me.world.find("Player")
    assert len({player, me.world.find("Player")}) == 1
    assert player != object()
    assert me.world.find("missing") is None
    original = player.transform.position
    player.transform.position.x += 100
    assert player.transform.position == original
    player.transform.position = me.Vec3(2, 3, 4)
    player.transform.rotation = me.Vec3(0, 90, 0)
    player.transform.scale = me.Vec3(2, 2, 2)
    player.rigidbody.velocity = me.Vec3(1, 2, 3)
    player.rigidbody.mass = 2
    player.rigidbody.add_impulse(me.Vec3(4, 0, 0))
    assert player.rigidbody.velocity == me.Vec3(3, 2, 3)
    player.rigidbody.use_gravity = False
    player.rigidbody.is_kinematic = True
    assert not player.rigidbody.is_grounded
    try:
        player.rigidbody.is_grounded = True
        assert False, "is_grounded must be read-only"
    except AttributeError:
        pass
    player.collider.is_trigger = True
    player.collider.radius = 1.5
    player.collider.half_extents = me.Vec3(1, 2, 3)
    player.mesh.mesh = "assets/models/sphere.obj"
    player.mesh.material = "assets/materials/warm.material.json"
    player.mesh.visible = False
    player.name = "RenamedPlayer"
    assert me.world.find("Player") is None
    assert me.world.find("RenamedPlayer") == player
    player.name = "Player"
    assert me.Vec3(1, 0, 0).cross(me.Vec3(0, 1, 0)) == me.Vec3(0, 0, 1)
    assert me.Vec3(3, 0, 0).normalized() == me.Vec3(1, 0, 0)
    for operation in (
        lambda: setattr(player.rigidbody, "mass", 0),
        lambda: setattr(player.collider, "radius", -1),
        lambda: setattr(player.transform, "position", me.Vec3(float("nan"), 0, 0)),
    ):
        try:
            operation()
            assert False, "Invalid component data was accepted"
        except ValueError:
            pass
    try:
        me.world.find("Manager").transform
        assert False, "A missing component was accepted"
    except AttributeError:
        pass


def queries():
    assert [e.name for e in me.world.find_all("Query")] == ["QueryFar", "QueryNear", "QueryEdge", "QueryChild"]
    hits = me.world.find_in_radius(me.Vec3(), 3, "Query")
    assert [e.name for e in hits] == ["QueryNear", "QueryEdge", "QueryFar"]
    assert me.world.find_in_radius(me.Vec3(), 0, "Query") == []
    # QueryChild.position is local (2, 0, 0); its parent's world position is (100, 0, 0).
    assert [e.name for e in me.world.find_in_radius(me.Vec3(102, 0, 0), 0.1, "Query")] == ["QueryChild"]
    for radius in (-1, float("nan"), float("inf")):
        try:
            me.world.find_in_radius(me.Vec3(), radius)
            assert False, "An invalid query radius was accepted"
        except ValueError:
            pass


def input_and_time():
    assert me.input.is_down("test_action") and me.input.was_pressed("test_action")
    assert me.input.is_key_down("r") and me.input.was_key_pressed("R")
    assert me.input.is_key_down("Space") and me.input.was_key_pressed("F1")
    assert not me.input.is_down("missing_action")
    try:
        me.input.is_key_down("UnknownKey")
        assert False, "An unknown key was accepted"
    except ValueError:
        pass
    assert abs(me.time.dt - 0.25) < 0.00001
    assert abs(me.time.total - 0.25) < 0.00001
    assert me.time.frame == 1
    try:
        me.time.dt = 1.0
        assert False, "Script time must be read-only"
    except AttributeError:
        pass
