ROCKET ARENA  {VERSION}
=======================
Steam в этой сборке / Steam in this build: {STEAM}

Машинки, мяч и ворота: матч в духе Rocket League на учебном движке myengine
(ИТМО, курс по архитектуре игровых движков). Играть можно одному против ботов
и по сети с друзьями до 4 на 4.
Cars, a ball and goals: a Rocket League-style match on the myengine student engine
(ITMO). Play alone against bots or online with friends, up to 4 vs 4.

Содержание / Contents
  1. Русский: запуск, управление, игра с другом, брандмауэр, файлы, если что-то не так
  2. English: the same in English (below, after the line of "=")


=====================================================================
РУССКИЙ
=====================================================================

1. ТРЕБОВАНИЯ
-------------
- Windows 10 или 11, 64 бит.
- Видеокарта с DirectX 12 (Feature Level 12_0, любая из последних лет) и свежий драйвер.
- Процессор с AVX2 (Intel с 2013 г., AMD Ryzen и новее): на нём строится физика.
- 4 ГБ оперативной памяти, около 150 МБ на диске.
Ничего ставить не нужно: библиотеки Visual C++ и Python лежат рядом с игрой.

2. ЗАПУСК
---------
1. Распакуйте архив целиком в любую папку (не запускайте RocketArena.exe прямо из zip).
2. Запустите RocketArena.exe. Первый запуск чуть дольше: игра один раз строит кэш моделей и текстур.
3. Windows SmartScreen может написать «Windows защитила ваш компьютер», потому что exe не подписан:
   «Подробнее» > «Выполнить в любом случае». Файл не меняется; если сомневаетесь, сверьте размер со своим источником.
4. При первом запуске брандмауэр Windows спросит про доступ к сети: нажмите «Разрешить доступ» (см. раздел 4).

3. УПРАВЛЕНИЕ
-------------
Клавиатура и мышь                                    Геймпад Xbox
  W / S         газ и тормоз (в воздухе: тангаж)       правый / левый триггер
  A / D         руль (в воздухе: рысканье)             левый стик по горизонтали
  Q / E         крен в воздухе                         LB / RB
  ЛКМ           буст                                   B
  ПКМ           прыжок (второй раз с направлением:     A
                додж)
  Пробел        камера «на мяч» / свободная            Y
  Shift         дрифт (powerslide)                     X
  Tab           таблица счёта                          Back
  Esc (или M)   пауза                                  Start

Меню: стрелки или мышь, Enter - выбрать, Esc - назад, Q / E - вкладки (геймпад: D-pad, A, B, LB / RB).
Клавиши и кнопки геймпада переназначаются в Settings > Controls; там же камера, видео (окно, VSync,
лимит кадров, тени) и звук.

4. ИГРА С ДРУГОМ
----------------
Play > PRIVATE MATCH. Один создаёт лобби (CREATE LOBBY), остальные входят в него; когда все нажали
«I AM READY», хозяин лобби жмёт START MATCH. Свободные места занимают боты. Способ связи выбирается
в строке Network: LAN или Steam. Версии игры у всех должны совпадать (число в правом верхнем углу меню).

4.1. Через Steam (проще всего, если оба в Steam)
- Оба запускают Steam и входят в аккаунт, и вы друзья в Steam.
- Игра использует тестовый номер приложения Valve (Spacewar, 480): в Steam у вас будет написано «Играет в Spacewar».
  Это нормально. Ничего покупать и добавлять в библиотеку не нужно.
- Оба запускают RocketArena.exe. Хозяин: Play > PRIVATE MATCH > Network: Steam > CREATE LOBBY.
  Лобби видно только друзьям.
- Друг: на том же экране в списке лобби друзей нажать на лобби (REFRESH обновляет список, друг должен
  быть уже в игре) либо принять приглашение: хозяин жмёт INVITE FRIENDS (оверлей Steam, Shift+Tab >
  друг > «Пригласить в игру») или правой кнопкой по другу в списке Steam > «Пригласить в игру».
  Принимайте приглашение, когда игра уже запущена.
- Не получается: Steam должен быть запущен до игры, от того же пользователя Windows; не удаляйте рядом с
  игрой steam_api64.dll и steam_appid.txt. Причина, если Steam недоступен, написана в окне при выборе Network: Steam.
  Игра без Steam всё равно работает (LAN, IP, боты).

4.2. По локальной сети (LAN)
- Оба в одной сети (один роутер, Wi-Fi или кабель). Хозяин: Network: LAN > CREATE LOBBY. Друг: Network: LAN,
  лобби хозяина появится в списке через секунду-две. Не видно? Введите адрес хозяина вручную (поле Address, JOIN ADDRESS):
  адрес показан у хозяина в комнате (COPY ADDRESS), это число вида 192.168.1.5.

4.3. По интернету с пробросом портов
- Хозяин: на роутере пробросить на свой компьютер два UDP-порта: 27015 (матч; это поле Match port при создании
  лобби, его можно сменить, тогда пробрасывайте выбранный) и 47778 (лобби). Порт 47779 нужен только для списка лобби
  в LAN, через интернет он не нужен.
- Узнать свой внешний адрес можно в поиске «what is my ip». Друг вводит его в Address (JOIN ADDRESS) на экране Network: LAN.
- Если провайдер выдаёт серый (общий) адрес, проброс не заработает: используйте VPN из следующего пункта.

4.4. Через VPN (Radmin VPN, Tailscale, ZeroTier): без проброса портов
- Оба ставят одну программу и входят в одну сеть (Radmin VPN: «Создать сеть» у одного, «Присоединиться» у другого;
  Tailscale: приглашение в tailnet).
- Хозяин создаёт лобби в Network: LAN, узнаёт адрес своего компьютера в VPN (Radmin: вида 26.x.x.x,
  Tailscale: 100.x.x.x), друг вводит этот адрес в Address (JOIN ADDRESS). Список лобби через VPN обычно не работает,
  поэтому адрес вводят руками.

5. БРАНДМАУЭР WINDOWS
---------------------
При первом запуске Windows спросит, разрешить ли RocketArena.exe доступ к сети: отметьте «Частные сети»
(для VPN и интернета ещё и «Общественные») и нажмите «Разрешить доступ». Хозяину это нужно обязательно.
Если нажали «Отмена»: Пуск > «Разрешение взаимодействия с приложением через брандмауэр Windows» > «Изменить параметры» >
найдите RocketArena (или «Разрешить другое приложение» и выберите RocketArena.exe) и отметьте нужные сети.
Либо одной командой в PowerShell от администратора (поправьте путь):
  New-NetFirewallRule -DisplayName "Rocket Arena" -Direction Inbound -Program "C:\Games\RocketArena\RocketArena.exe" -Action Allow -Protocol UDP
Если после переезда папки правило перестало работать, повторите команду с новым путём.

6. ГДЕ ФАЙЛЫ И ЧТО ПРИСЛАТЬ, ЕСЛИ ЧТО-ТО НЕ ТАК
-----------------------------------------------
Игра ничего не пишет в свою папку. Всё лежит в %LOCALAPPDATA%\RocketArena (Win+R, вставьте путь):
  logs\myengine.log      лог последнего запуска (при каждом запуске перезаписывается: скопируйте его сразу)
  crashes\               при падении игры: файл crash-<дата>-<время>.txt (версия, ошибка, конец лога) и .dmp
  game_settings.json     настройки, rl_controls.json - клавиши, Cache - кэш моделей
Если игра упала, при следующем запуске она сама скажет об этом в главном меню и назовёт файл. Пришлите .txt и .dmp
из crashes тому, кто дал вам игру. Если игра вообще не открывается, пришлите logs\myengine.log.
Чтобы начать с нуля, закройте игру и удалите папку %LOCALAPPDATA%\RocketArena.

7. ЛИЦЕНЗИИ
-----------
Игра использует стороннее программное обеспечение. Тексты лицензий лежат в папке licenses
(начните с THIRD_PARTY_NOTICES.txt). Модели, текстуры и звуки игры сгенерированы
процедурно скриптами из репозитория; чужих сэмплов и моделей в них нет.


=====================================================================
ENGLISH
=====================================================================

1. REQUIREMENTS
---------------
- Windows 10 or 11, 64 bit.
- A DirectX 12 graphics card (Feature Level 12_0, any from the last years) and an up-to-date driver.
- A CPU with AVX2 (Intel since 2013, AMD Ryzen and newer): the physics is built on it.
- 4 GB of RAM, about 150 MB of disk.
Nothing to install: the Visual C++ libraries and Python are next to the game.

2. RUNNING THE GAME
-------------------
1. Unpack the whole archive into any folder (do not start RocketArena.exe from inside the zip).
2. Start RocketArena.exe. The first start is a bit longer: the game builds a cache of models and textures once.
3. Windows SmartScreen may say "Windows protected your PC" because the exe is not code-signed:
   "More info" > "Run anyway".
4. On the first start the Windows Firewall asks about network access: click "Allow access" (see section 4).

3. CONTROLS
-----------
Keyboard and mouse                                   Xbox gamepad
  W / S         throttle and brake (air: pitch)        right / left trigger
  A / D         steering (air: yaw)                    left stick horizontal
  Q / E         air roll                               LB / RB
  LMB           boost                                  B
  RMB           jump (a second one with a direction:   A
                dodge)
  Space         ball cam / free camera                 Y
  Shift         powerslide                             X
  Tab           scoreboard                             Back
  Esc (or M)    pause                                  Start

Menus: arrows or mouse, Enter to select, Esc to go back, Q / E for tabs (gamepad: D-pad, A, B, LB / RB).
Keys and gamepad buttons are rebound in Settings > Controls; the camera, video (window, VSync, frame limit,
shadows) and audio are there too.

4. PLAYING WITH A FRIEND
------------------------
Play > PRIVATE MATCH. One player creates the lobby (CREATE LOBBY), the others join it; when everybody pressed
"I AM READY" the host presses START MATCH. Free slots are filled with bots. The way to connect is the Network
row: LAN or Steam. Everybody needs the same version of the game (the number in the top right corner of the menu).

4.1. Through Steam (the easiest if both of you have Steam)
- Both run Steam and are logged in, and you are friends on Steam.
- The game uses the test application id of Valve (Spacewar, 480): Steam shows "Playing Spacewar". That is
  normal. Nothing to buy or add to the library.
- Both start RocketArena.exe. Host: Play > PRIVATE MATCH > Network: Steam > CREATE LOBBY. The lobby is
  visible to friends only.
- Friend: on the same screen click the lobby in the list of your friends' lobbies (REFRESH reloads the list,
  the host must already be in the game) or accept an invitation: the host presses INVITE FRIENDS (the Steam
  overlay, Shift+Tab > friend > "Invite to game") or right-clicks the friend in the Steam friends list >
  "Invite to game". Accept the invitation while the game is already running.
- Does not work: Steam must be running before the game, under the same Windows user; do not delete steam_api64.dll
  and steam_appid.txt next to the game. If Steam is not available the reason is written on the screen when you
  pick Network: Steam. The game still works without Steam (LAN, IP, bots).

4.2. On the local network (LAN)
- Both on the same network (one router, Wi-Fi or cable). Host: Network: LAN > CREATE LOBBY. Friend: Network: LAN,
  the lobby of the host appears in the list in a second or two. Not there? Type the address of the host by hand
  (the Address field, JOIN ADDRESS): the host sees it in the lobby room (COPY ADDRESS), a number like 192.168.1.5.

4.3. Over the internet with port forwarding
- Host: forward two UDP ports on the router to your PC: 27015 (the match: the Match port field when you create
  the lobby, forward the one you chose if you change it) and 47778 (the lobby). Port 47779 is only for the lobby
  list on a LAN and is not needed over the internet.
- Find your external address by searching "what is my ip". The friend types it into Address (JOIN ADDRESS) on the Network: LAN screen.
- If your provider gives you a shared (CGNAT) address forwarding cannot work: use a VPN, the next item.

4.4. Through a VPN (Radmin VPN, Tailscale, ZeroTier): no port forwarding
- Both install the same program and join one network (Radmin VPN: "Create network" on one side, "Join network"
  on the other; Tailscale: an invitation to the tailnet).
- The host creates the lobby with Network: LAN, finds the address of the PC inside the VPN (Radmin: 26.x.x.x,
  Tailscale: 100.x.x.x), the friend types it into Address (JOIN ADDRESS). The lobby list does not usually work over a VPN, so the
  address is typed by hand.

5. WINDOWS FIREWALL
-------------------
On the first start Windows asks whether RocketArena.exe may use the network: tick "Private networks" (and
"Public networks" for a VPN or the internet) and click "Allow access". The host needs this.
If you clicked "Cancel": Start > "Allow an app through Windows Firewall" > "Change settings" > find RocketArena
(or "Allow another app" and pick RocketArena.exe) and tick the networks.
Or one command in PowerShell as administrator (fix the path):
  New-NetFirewallRule -DisplayName "Rocket Arena" -Direction Inbound -Program "C:\Games\RocketArena\RocketArena.exe" -Action Allow -Protocol UDP
If the rule stops working after you moved the folder, run the command again with the new path.

6. WHERE THE FILES ARE AND WHAT TO SEND IF SOMETHING GOES WRONG
---------------------------------------------------------------
The game writes nothing to its own folder. Everything is in %LOCALAPPDATA%\RocketArena (Win+R, paste the path):
  logs\myengine.log      the log of the last run (overwritten on every start: copy it right away)
  crashes\               after a crash: crash-<date>-<time>.txt (version, the error, the end of the log) and .dmp
  game_settings.json     settings, rl_controls.json the keys, Cache the cache of the models
If the game crashed, on the next start it says so in the main menu and names the file. Send the .txt and .dmp from
crashes to the person who gave you the game. If the game does not open at all, send logs\myengine.log.
To start from scratch, close the game and delete the folder %LOCALAPPDATA%\RocketArena.

7. LICENSES
-----------
The game uses third-party software. The license texts are in the licenses folder (start with
THIRD_PARTY_NOTICES.txt). The models, textures and sounds of the game are generated procedurally by scripts
of the repository; there are no third-party samples or models in them.
