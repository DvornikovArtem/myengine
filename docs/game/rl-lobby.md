# Rocket Arena: лобби (G6a)

Сервис лобби — то, что вызывает экран Private match (G1-flow): где игроки находят друг друга, видят состав, готовность и стартуют матч. Матч дальше идёт обычным `RlNetHost`/`RlNetClient` (`docs/game/rl-netcode.md`). Заголовки: `game/rl/include/rl/net/LobbyService.h` (интерфейс), `LobbyLan.h`, `LobbySteam.h`; код — `game/rl/src/net/LobbyLan.cpp`, `LobbySteamStub.cpp`, `steam/*`; тест — `rl_net` (`ctest -R rl_net`).

## Интерфейс (`rl::net::ILobbyService`)

Один интерфейс на LAN и Steam; вызывается раз в кадр `Update(now)` и читается `TakeEvents()`. Ничего не блокирует и не бросает исключений: ошибка — в `std::string error` или в событии `Error`.

| Что | Вызовы |
| --- | --- |
| Хост | `Create({hostName, mode, maxPlayers, lobbyPort, gamePort, friendsOnly})`, `InviteText()` (LAN: текст для друга), `InviteFriends()` (Steam: оверлей), `StartMatch(error)` — отказывает, пока кто-то не готов |
| Гость | `RefreshList()` → `Lobbies()` (id, имя хоста, режим, игроки/мест, ping, `joinable` + `note`: другая версия, лобби полное, матч уже идёт), `Join(id)` или `Join("192.168.1.5:47778")` вручную, `SetReady(bool)` |
| Состояние | `State()` (Idle, Creating, Hosting, Joining, Joined, Starting, Failed), `Members()`, `Current()`, `StartInfo()`, `LastError()`, `Leave()` |
| События | Created, Joined, MemberJoined, MemberLeft, MemberChanged, ListUpdated, MatchStarting, HostLeft, InviteReceived, Error |
| Матч | `CreateMatchTransport(sim)`: UDP в LAN, `SteamTransport` в Steam; `StartInfo()` = {isHost, hostAddress, gamePort, mode, members} |

Порядок на экране Private match: хост `Create` → показывает состав → когда все готовы `StartMatch` → на `MatchStarting` создаёт `RlNetHost` на `CreateMatchTransport`, `Listen(info.gamePort)`; гость на `MatchStarting` делает `RlNetClient` на своём `CreateMatchTransport` и `Connect(info.hostAddress)`. Гости входят в уже идущий матч (подключение посреди матча из G4f, слоты ботов), хост может подождать, пока `RlNetHost::ClientCount()` сравняется с числом членов лобби.

## LAN-лобби

- **Маяк.** Хост раз в 1 с рассылает датаграмму по `255.255.255.255:47779` (`'R' 'L' 1`, версия, режим, мест, игроков, флаг «матч идёт», порт лобби, имя). Гость (`RefreshList`) слушает этот порт (общий `SO_REUSEADDR`, поэтому два окна на одном ПК не мешают друг другу), собирает записи, через 4 с без маяка запись пропадает.
- **Ping.** На первый маяк и затем раз в 2 с гость шлёт хосту `Ping` (на адрес, откуда пришёл маяк), хост отвечает `Pong` с тем же числом; в списке — время круга. (Steam ping до входа в лобби не даёт: у записей Steam `pingMs = -1`.)
- **Лобби** — надёжное соединение (тот же `ReliableTransport`, что у матча) гостя к порту лобби хоста `47778`: `Hello` (версия, имя) → `Welcome` или `Reject` (другая версия, полное, матч идёт; соединение живёт ещё 0.6 с, чтобы отказ дошёл) → `Members` всем при любом изменении, `Ready`, `Rename`, `Start{gamePort}`. Ушедший гость (или потерявшийся по таймауту) пропадает из состава у всех; хост закрыл лобби — гости получают `HostLeft`.
- Порты: `47778` лобби, `47779` маяк, `47777` матч (`kDefaultLobbyPort`, `kLanBeaconPort`, `kDefaultGamePort`).
- Ручной вход по `ip[:порт]` остаётся (`Join("192.168.0.160")` добавит порт лобби).

## Steam

Каркас `SteamLobbyService` и `SteamTransport` под `MYENGINE_STEAM` (по умолчанию OFF; `docs/game/steam-setup.md`: как включить и проверить вдвоём). В сборке без SDK `MakeSteamLobbyService()` возвращает сервис, у которого `Available(whyNot)` говорит «в этой сборке нет Steam», поэтому экран может предлагать Steam без `#ifdef`. `FindConnectLobbyInCommandLine` разбирает `+connect_lobby <id>` и работает без SDK.

## Что проверено

`rl_net` (778 проверок): параметры `--net-sim`; LAN-лобби на loopback-сети с виртуальным часом (маяк → список с ping → вход по записи → состав → готовность → старт → и сам матч на транспортах сервисов: 0 жёстких поправок), полное лобби, другая версия, ручной вход, уход гостя, исчезновение записи, уход хоста, молчащий хост (отказ с текстом); **то же по настоящему UDP на этом ПК** (маяк по broadcast дошёл, ping 3 мс); `FindConnectLobbyInCommandLine`; сервис Steam в сборке без SDK. Код Steam собирается против наброска `game/rl/steam_mock` (`-DMYENGINE_STEAM_CHECK=ON`, ни одной ошибки и предупреждения), с настоящим SDK и двумя аккаунтами **не проверялся**.

## Ограничения

- LAN: маяк идёт по IPv4-броадкасту стандартного интерфейса; в сети с изоляцией клиентов (гостевой Wi-Fi) или при брандмауэре, режущем UDP 47779, список пуст — тогда остаётся ввод адреса вручную.
- Состав лобби и готовность не защищены от подделки (LAN-доверие).
- В списке нет сортировки по версиям кроме «играбельные выше», нет пароля у лобби.
