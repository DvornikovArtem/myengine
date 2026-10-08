import json
import math

import myengine as me


def manager():
    from game_manager import GameManager
    return me.world.find("GameManager").get_script(GameManager)


def coins():
    return me.world.find_all("Coin_5_")


def enemies():
    return me.world.find_all("Chaser_6_")


def fireworks():
    return me.world.find_all("Firework_4_")


def start(coins_count=3, with_enemies=False, duration=1000.0):
    scene = json.loads(base_scene)
    settings = {
        "GameManager": {"target_score": 20, "round_duration": duration, "firework_count": 6},
        "CoinSpawner": {"max_coins": coins_count, "spawn_interval": 0.05},
        "EnemySpawner": {"initial_delay": 0.05 if with_enemies else 9999.0, "wave_interval": 0.1, "max_enemies": 3},
    }
    for entity in scene["entities"]:
        if entity["Tag"]["name"] in settings:
            entity["Script"]["scripts"][0]["props"].update(settings[entity["Tag"]["name"]])
    load_scene(json.dumps(scene))
    tick(0)


def test_prefabs_and_pickup():
    from coin import Coin
    start()
    assert len(coins()) == 3 and len(me.world.find_all("CoinGlow")) == 3
    assert manager().target_score == 20 and manager().score == 0
    assert "0 / 20" in hud_line("t6.score") and "1000" in hud_line("t6.time")
    first = coins()[0]
    assert first.get_script(Coin) is None, "Prefab scripts must start next frame"
    tick(0)
    assert first.get_script(Coin).spin_speed == 120 and first.get_script(Coin).score_value == 1
    rotation = first.transform.rotation.y
    tick(0.25)
    assert math.isclose(first.transform.rotation.y - rotation, 30.0, abs_tol=0.001)
    trigger(first.id, me.world.find("Floor_1").id)
    tick(0)
    assert first.alive and manager().score == 0, "Only the player may collect a coin"

    # Real physics publishes the trigger; the script awards a point and destroys the subtree.
    player = me.world.find("Controlled_1")
    player.transform.position = first.transform.position
    player.rigidbody.velocity = me.Vec3()
    tick(1 / 60, physics=True)
    assert not first.alive and manager().score == 1
    assert len(me.world.find_all("CoinGlow")) == 2
    tick(0.06)
    assert len(coins()) == 3

    second = coins()[0]
    second_id = second.id
    trigger(second_id, player.id)
    trigger(second_id, player.id)
    tick(0)
    assert not second.alive and manager().score == 2, "Duplicate events awarded points twice"
    trigger(second_id, player.id)
    tick(0.06)
    assert manager().score == 2 and len(coins()) == 3


def test_waves_chase_and_hits():
    start(coins_count=0, with_enemies=True)
    tick(0.06)
    assert len(enemies()) == 1
    tick(0)
    first = enemies()[0]
    velocity = first.rigidbody.velocity
    assert math.isclose(math.hypot(velocity.x, velocity.z), 3.0, abs_tol=0.001)
    player = me.world.find("Controlled_1")
    before = (player.transform.position - first.transform.position).length()
    tick(1 / 60, physics=True)
    assert (player.transform.position - first.transform.position).length() < before
    tick(0.11)
    assert len(enemies()) == 3, "Wave growth must add two enemies after the first one"
    tick(0.11)
    assert len(enemies()) == 3, "The enemy limit was exceeded"

    for enemy in enemies():
        enemy.transform.position = me.Vec3(100, 0.5, 100)
    tick(0)
    assert first.rigidbody.velocity.x == 0 and first.rigidbody.velocity.z == 0
    assert me.send("GameManager", "add_score", 5)
    player.transform.position = me.Vec3(2, 0.5, 2)
    player.rigidbody.velocity = me.Vec3(1, 2, 3)
    collision(first.id, player.id)
    collision(enemies()[1].id, player.id)
    tick(0)
    assert manager().score == 4, "Several enemies must share the hit cooldown"
    assert player.transform.position == manager()._start_position
    assert player.rigidbody.velocity == me.Vec3()
    collision(first.id, player.id)
    tick(0)
    assert manager().score == 4
    tick(1.6)
    collision(first.id, player.id)
    tick(0)
    assert manager().score == 3
    tick(1.6)
    first.transform.position = player.transform.position + me.Vec3(0.5, 0, 0)
    tick(0)
    assert manager().score == 2, "The Python catch-radius rule did not catch a nearby player"


def test_victory_lifetime_and_restart():
    start(coins_count=2, with_enemies=True)
    tick(0.06)
    tick(0)
    old_coins = coins()
    old_enemies = enemies()
    assert me.send("GameManager", "add_score", 20)
    assert manager().state == "won" and len(fireworks()) == 6
    assert "won" in hud_line("t6.state")
    assert all(enemy.rigidbody.velocity.x == 0 and enemy.rigidbody.velocity.z == 0 for enemy in enemies())
    tick(0)
    particles = fireworks()
    assert all(particle.rigidbody.velocity.length() > 0 for particle in particles)
    remaining = manager().remaining
    tick(2.1)
    assert not fireworks() and all(not particle.alive for particle in particles)
    assert manager().remaining == remaining and manager().state == "won"
    assert me.send("GameManager", "add_score", 20) and manager().score == 20

    round_id = manager().round_id
    press_r()
    assert manager().round_id == round_id + 1 and manager().state == "playing"
    assert manager().score == 0 and manager().remaining == 1000
    assert all(not entity.alive for entity in old_coins + old_enemies)
    tick(0)
    assert len(coins()) == 2 and not enemies() and not fireworks()
    for _ in range(20):
        old = coins()
        press_r()
        tick(0)
        assert len(coins()) == 2 and entity_count() == 10
        assert all(not entity.alive for entity in old)


def test_loss_fall_and_missing_player():
    start(coins_count=0, duration=0.05)
    tick(0.06)
    assert manager().state == "lost" and manager().remaining == 0
    assert not fireworks() and "lost" in hud_line("t6.state")
    me.send("GameManager", "add_score", 99)
    assert manager().score == 0
    press_r()
    assert manager().state == "playing" and math.isclose(manager().remaining, 0.05)

    start(coins_count=0, with_enemies=True)
    tick(0.06)
    tick(0)
    me.send("GameManager", "add_score", 4)
    player = me.world.find("Controlled_1")
    player.transform.position = me.Vec3(0, -10, 2)
    tick(0)
    assert manager().score == 3 and player.transform.position == manager()._start_position
    player.destroy()
    tick(0)
    tick(0)
    assert manager().state == "lost", "A missing target must not fault the other behaviours"


def test_reload_and_play_stop():
    start(coins_count=2)
    tick(0)
    me.send("GameManager", "add_score", 3)
    original_coins = coins()
    change_coin_rule(2)
    wait_for(lambda: getattr(__import__("coin"), "TEST_RULE_VERSION", 0) == 2)
    assert manager().score == 3 and all(entity.alive for entity in original_coins)
    assert len(coins()) == 2, "L2 reload duplicated the spawner's tracked roots"
    first = coins()[0]
    trigger(first.id, me.world.find("Controlled_1").id)
    tick(0)
    assert manager().score == 5, "The edited Python rule did not take effect"

    keep_reload_state(False)
    old_coins = coins()
    change_coin_rule(3)
    wait_for(lambda: getattr(__import__("coin"), "TEST_RULE_VERSION", 0) == 3)
    tick(0)
    assert manager().score == 0 and len(coins()) == 2
    assert all(not entity.alive for entity in old_coins), "L1 reload left orphaned prefab instances"
    for _ in range(5):
        old = coins()
        stop()
        assert instance_count() == 0 and all(not entity.alive for entity in old)
        play()
        tick(0)
        assert len(coins()) == 2 and instance_count() == 5 and entity_count() == 10


def run_checks():
    test_prefabs_and_pickup()
    test_waves_chase_and_hits()
    test_victory_lifetime_and_restart()
    test_loss_fall_and_missing_player()
    test_reload_and_play_stop()
