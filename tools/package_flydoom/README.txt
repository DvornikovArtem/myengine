FLYDOOM  {VERSION}
==================

Классический DOOM (1993) на учебном движке myengine (ИТМО, курс по архитектуре игровых движков): настоящий код игры id Software
(порт doomgeneric), кадр рисуется через DirectX 12. Дальше в серии у игры появится муха с настоящим мозгом (коннектом взрослой дрозофилы).
The classic DOOM (1993) on the educational engine myengine (ITMO, a course on game engine architecture): the real code of id Software
(the doomgeneric port), the frame is drawn through DirectX 12. In the next steps of the series the game gets a fly with a real brain
(the connectome of an adult fruit fly).

Содержание / Contents
  1. Русский
  2. English (below, after the line of "=")


=====================================================================
РУССКИЙ
=====================================================================

1. ТРЕБОВАНИЯ
-------------
- Windows 10 или 11, 64 бит; видеокарта с DirectX 12 (Feature Level 12_0) и свежий драйвер.
- Процессор с AVX2 (Intel с 2013 г., AMD Ryzen и новее).
- Около 60 МБ на диске (если в папке есть мозг мухи, assets\flybrain, — около 210 МБ) и 4 ГБ оперативной памяти.
Ничего ставить не нужно: библиотеки Visual C++ и Python лежат рядом с игрой.

2. ЗАПУСК
---------
1. Распакуйте архив целиком в любую папку (не запускайте FlyDoom.exe прямо из zip).
2. Запустите FlyDoom.exe.
3. Windows SmartScreen может написать «Windows защитила ваш компьютер», потому что exe не подписан:
   «Подробнее» > «Выполнить в любом случае».
Игра сама находит данные (WAD). Порядок: файл из --iwad; папки assets\doom, wads и doom рядом с exe; %LOCALAPPDATA%\FlyDoom\wads;
DOOM из вашей библиотеки Steam; переменная DOOMWADDIR. Из найденного берётся полная игра (DOOM.WAD, DOOM2.WAD, plutonia.wad, tnt.wad),
потом shareware doom1.wad (первый эпизод, лежит в папке wads: id Software разрешила раздавать его свободно), потом Freedoom
(свободная замена данных, assets\doom). Если у вас есть купленный DOOM в Steam, игра возьмёт его, а не shareware.
Свой WAD можно указать явно: FlyDoom.exe --iwad "D:\Games\DOOM2.WAD".

3. УПРАВЛЕНИЕ
-------------
  W S или стрелки вверх/вниз    вперёд / назад          Пробел или E    открыть, нажать
  A D                           боковые шаги            Shift           бег
  стрелки влево/вправо или мышь поворот                 1 ... 7         оружие
  Ctrl или левая кнопка мыши    огонь                   Tab             карта
  Esc                           меню (стрелки, Enter, Y/N)
Выход: Esc > Quit Game > Y. Размер экрана и яркость: - = F11, остальные F1...F12 как в DOOM.
Ключи командной строки: --iwad <файл.wad>; --doom-args "-warp 1 1 -skill 3" (командная строка самого DOOM: уровень, сложность, -nomonsters,
-playdemo demo1); --doom-square-pixels (квадратные пиксели вместо растянутых до 4:3); --doom-no-music (без музыки).

4. ГДЕ ФАЙЛЫ И ЧТО ПРИСЛАТЬ, ЕСЛИ ЧТО-ТО НЕ ТАК
-----------------------------------------------
Игра ничего не пишет в свою папку. Всё лежит в %LOCALAPPDATA%\FlyDoom (Win+R, вставьте путь):
  logs\myengine.log      лог последнего запуска (перезаписывается при каждом запуске: скопируйте его сразу; в нём написано, какой WAD выбран)
  crashes\               при падении: crash-<дата>-<время>.txt (версия, ошибка, конец лога) и .dmp
  doom\                  настройки DOOM и сохранения (savegame\<имя wad>)
Если игра упала или не открывается, пришлите logs\myengine.log и файлы из crashes тому, кто дал вам игру.
Чтобы начать с нуля, закройте игру и удалите папку %LOCALAPPDATA%\FlyDoom.

5. ЛИЦЕНЗИИ И ИСХОДНЫЙ КОД
--------------------------
FlyDoom.exe содержит код DOOM и поэтому распространяется по лицензии GNU GPL v2 (файл COPYING). Где лежит исходный код этой версии
(репозиторий и коммит) и как собрать то же самое: SOURCE.txt. Что ещё входит в пакет и по каким лицензиям (данные Freedoom и shareware DOOM,
коннектом мухи CC BY 4.0, библиотеки): THIRD_PARTY.txt и папка licenses. Список файлов, которые попали именно в этот пакет: version.txt.


=====================================================================
ENGLISH
=====================================================================

1. REQUIREMENTS
---------------
- Windows 10 or 11, 64 bit; a DirectX 12 graphics card (Feature Level 12_0) and an up-to-date driver.
- A CPU with AVX2 (Intel since 2013, AMD Ryzen and newer).
- About 60 MB of disk (about 210 MB when the fly brain, assets\flybrain, is in the folder) and 4 GB of RAM.
Nothing to install: the Visual C++ and Python libraries are next to the game.

2. RUNNING THE GAME
-------------------
1. Unpack the whole archive into any folder (do not start FlyDoom.exe from inside the zip).
2. Start FlyDoom.exe.
3. Windows SmartScreen may say "Windows protected your PC" because the exe is not code-signed: "More info" > "Run anyway".
The game finds its data (the WAD) by itself. The order: the file of --iwad; the folders assets\doom, wads and doom next to the exe;
%LOCALAPPDATA%\FlyDoom\wads; the DOOM of your Steam library; the variable DOOMWADDIR. Among the files found a full game (DOOM.WAD, DOOM2.WAD,
plutonia.wad, tnt.wad) is taken first, then the shareware doom1.wad (the first episode, in the folder wads: id Software allowed to distribute it
freely), then Freedoom (a free replacement of the game data, assets\doom). If you own DOOM on Steam the game takes that one, not the shareware.
To name a WAD yourself: FlyDoom.exe --iwad "D:\Games\DOOM2.WAD".

3. CONTROLS
-----------
  W S or up/down arrows          forward / back          Space or E      open, press
  A D                            strafe                  Shift           run
  left/right arrows or the mouse turn                    1 ... 7         weapon
  Ctrl or the left mouse button  fire                    Tab             map
  Esc                            menu (arrows, Enter, Y/N)
Quit: Esc > Quit Game > Y. Screen size and brightness: - = F11, the other F1...F12 as in DOOM.
Command line: --iwad <file.wad>; --doom-args "-warp 1 1 -skill 3" (the command line of DOOM itself: level, skill, -nomonsters, -playdemo demo1);
--doom-square-pixels (square pixels instead of the 4:3 stretch); --doom-no-music (no music).

4. WHERE THE FILES ARE AND WHAT TO SEND IF SOMETHING GOES WRONG
---------------------------------------------------------------
The game writes nothing to its own folder. Everything is in %LOCALAPPDATA%\FlyDoom (Win+R, paste the path):
  logs\myengine.log      the log of the last run (overwritten on every start: copy it right away; it says which WAD was chosen)
  crashes\               after a crash: crash-<date>-<time>.txt (version, the error, the end of the log) and .dmp
  doom\                  the DOOM settings and the saved games (savegame\<name of the wad>)
If the game crashed or does not open, send logs\myengine.log and the files from crashes to the person who gave you the game.
To start from scratch, close the game and delete the folder %LOCALAPPDATA%\FlyDoom.

5. LICENSES AND THE SOURCE CODE
-------------------------------
FlyDoom.exe contains the code of DOOM and is therefore distributed under the GNU GPL v2 (the file COPYING). Where the source code of this
version is (the repository and the commit) and how to build the same: SOURCE.txt. What else is in the package and under which licenses (the data
of Freedoom and of the shareware DOOM, the fly connectome under CC BY 4.0, the libraries): THIRD_PARTY.txt and the folder licenses. The files
that are in this very package: version.txt.
