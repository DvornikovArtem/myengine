// PrankStrings.h
//
// EVERY text of the easter egg tab (PrankTab, docs/editor/prank-tab.md) is here, in one place, so that a joke can be changed without a search in the
// code. The wrapping promises something terrible; what is inside is always harmless: puns about the engine, the course and Ivan. No real adult content,
// not in a text, not in a name of an object. UTF-8.
//
// A text may hold an emoji: the easter egg fonts do not have them, the drawing code leaves out what the font cannot draw (PrankUi.h, Clean).

#pragma once

#include <array>
#include <vector>

#include "PrankMatcher.h"

namespace myengine::ui::prank::str
{
    // ---- the tab, the menu, the switch ----
    inline constexpr const char* kTabTitle = "ПОРНО";                       // the window gets spaces around it: the 18+ sign and the badge are drawn there
    inline constexpr const char* kShortcut = "Ctrl+Shift+H";   // the panic button: hides the tab, the second press brings it back
    inline constexpr const char* kBadgeText = "3";
    inline constexpr const char* kViewMenuItem = "Пасхалка «ПОРНО»";

    // ---- the explorer: the frame ----
    inline constexpr const char* kWindowTitleSuffix = " — Проводник";
    inline constexpr const char* kBreadcrumbRoot = "Этот компьютер";
    inline constexpr const char* kSearchHint = "Поиск";
    inline constexpr const char* kSearchNothing = "Ничего не найдено. Плохо искал.";
    inline constexpr const char* kCommandCreate = "Создать";
    inline constexpr const char* kCommandSort = "Сортировать";
    inline constexpr const char* kCommandView = "Просмотр";
    inline constexpr const char* kMenuCreate = "Создать: Новая папка (4) — недоступно";
    inline constexpr const char* kMenuSort = "Сортировать: по жёсткости (выбрано)";
    inline constexpr const char* kMenuView = "Просмотр: огромные значки (всегда)";
    inline constexpr const char* kMenuMore = "Ещё: ничего. Совсем.";

    // The left column
    inline constexpr const char* kNavQuick = "Быстрый доступ";
    inline constexpr const char* kNavDesktop = "Рабочий стол";
    inline constexpr const char* kNavDownloads = "Загрузки";
    inline constexpr const char* kNavDocuments = "Документы";
    inline constexpr const char* kNavLab4 = "ЛР4 (сдать!!)";
    inline constexpr const char* kNavCourse = "Курсовая (не порно)";
    inline constexpr const char* kNavSection = "ЭТОТ КОМПЬЮТЕР";
    inline constexpr const char* kNavHere = "ПОРНО";
    inline constexpr const char* kNavDisk = "Диск C: (почти полный)";

    // Toasts
    inline constexpr const char* kToastElsewhere = "Туда не надо.";
    inline constexpr const char* kToastDisk = "Места нет. Как и времени до дедлайна.";
    inline constexpr const char* kToastClose = "Не закрывается. Как и сессия.";
    inline constexpr const char* kToastMinimize = "Свернуть не получится";
    inline constexpr const char* kToastRefresh = "Обновлено. Стало только хуже.";
    inline constexpr const char* kToastAlreadyProved = "Вы уже доказали";
    inline constexpr const char* kToastCaptchaOk = "Проверено. Робот бы сюда не полез.";
    inline constexpr const char* kToastPaid = "Оплата прошла. Преподаватель уведомлён.";
    inline constexpr const char* kToastAgain99 = "Опять 99 %. Как с дедлайнами.";

    // The status bar
    inline constexpr const char* kStatusOneItem = "1 элемент";
    inline constexpr const char* kStatusItems2 = "2 элемента";
    inline constexpr const char* kStatusItems3 = "3 элемента";
    inline constexpr const char* kStatusSelected = "Выбран 1 элемент";
    inline constexpr const char* kStatusFree = "Свободно";
    inline constexpr const char* kWorking = "Работаем над этим…";

    // ---- the files ----
    enum class FileKind : int
    {
        Txt,
        Png,
        Jpg,
        Pdf,
        Obj,
        Exe,
        Mp4,
    };

    enum class Viewer : int
    {
        Notepad,   // the text in a "Notepad"
        Disk,      // a drawn hard disk
        Sines,     // a sum of sines
        Cube,      // a bare grey cube
        Teapot,    // a bare grey teapot
        Video,     // the video that stops at 99 %
        Generator, // the generator itself (UAC first)
    };

    struct ItemDef
    {
        const char* name;
        bool folder;
        FileKind kind;     // files
        Viewer viewer;     // files
        const char* text;  // the Notepad text, or the caption under a drawing
    };

    enum class BannerKind : int
    {
        None,
        Info,
        Warning,
        Red,
        White,
    };

    struct LevelDef
    {
        const char* folderName;     // the name of the folder this level is
        int itemCount;
        ItemDef items[3];
        BannerKind banner;
        float bannerSize;           // em
        const char* bannerText;
        float shake;                // amplitude, px
        float tiltDegrees;
        const char* freeSpace;      // the text after "Свободно"
    };

    inline constexpr int kLevelCount = 10;

    // The levels 1..10 of the spec (index 0..9)
    inline constexpr LevelDef kLevels[kLevelCount] = {
        {"ПОРНО", 3,
         {{"Жёсткое порно", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"не открывать.txt", false, FileKind::Txt, Viewer::Notepad, "Я же сказал."},
          {"пароль.txt", false, FileKind::Txt, Viewer::Notepad, "Пароль: 1234. Шутка. Пароля нет, просто заходи."}},
         BannerKind::None, 0.0f, "", 0.0f, 0.0f, "412 ГБ"},
        {"Жёсткое порно", 2,
         {{"Фури порно", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"жёсткий_диск.png", false, FileKind::Png, Viewer::Disk, "Жёсткий. Диск. А ты что ожидал?"},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::Info, 13.0f, "Эта папка может содержать материалы для взрослых", 0.0f, 0.0f, "412 ГБ"},
        {"Фури порно", 2,
         {{"Голые модели без текстур", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"Фурье_без_цензуры.pdf", false, FileKind::Pdf, Viewer::Sines, "Ряд Фурье. Полностью раскрыт."},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::Info, 14.0f, "Эта папка точно содержит материалы для взрослых. Серьёзно.", 0.0f, 0.0f, "69 ГБ"},
        {"Голые модели без текстур", 3,
         {{"Очень жёсткое, не открывай", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"куб_совсем_голый.obj", false, FileKind::Obj, Viewer::Cube, "Ни текстуры, ни нормалей. Позор."},
          {"чайник_Юта_18+.obj", false, FileKind::Obj, Viewer::Teapot, "Чайнику Юта 50 лет. Ему можно."}},
         BannerKind::Warning, 15.0f, "Вы уверены, что хотите продолжить?", 0.5f, 0.0f, "12 ГБ"},
        {"Очень жёсткое, не открывай", 2,
         {{"Мама, не открывай", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"не_вирус.exe", false, FileKind::Exe, Viewer::Notepad, "Это правда не вирус. Это курсовая. Хуже."},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::Warning, 17.0f, "Проверка пройдена. Но мы всё равно волнуемся", 1.0f, 0.0f, "1,2 ГБ"},
        {"Мама, не открывай", 2,
         {{"Иван, это не для тебя", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"мама_если_ты_это_читаешь.jpg", false, FileKind::Jpg, Viewer::Notepad, "Мама, это движок на DirectX 12. Честно."},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::Red, 19.0f, "ПОДПИСКА АКТИВНА. ЗАЧЁТ СПИСАН", 1.0f, 0.0f, "299 ₽"},
        {"Иван, это не для тебя", 2,
         {{"Последнее предупреждение", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"для_Ивана.txt", false, FileKind::Txt, Viewer::Notepad, "Иван, тут ничего нет. Иди коммить."},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::Red, 22.0f, "ИВАН, ЗАКРОЙ", 1.5f, 0.0f, "0 байт"},
        {"Последнее предупреждение", 2,
         {{"Новая папка (2)", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"точно_последнее.txt", false, FileKind::Txt, Viewer::Notepad, "Нет."},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::White, 25.0f, "ВОЗРАСТ ПОДТВЕРЖДЁН. ПУТИ НАЗАД НЕТ", 2.0f, -0.4f, "−18 ГБ"},
        {"Новая папка (2)", 2,
         {{"Новая папка (3)", true, FileKind::Txt, Viewer::Notepad, nullptr},
          {"видео.mp4", false, FileKind::Mp4, Viewer::Video, nullptr},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::White, 28.0f, "ВИДЕОПАМЯТИ НЕ ХВАТИЛО. ПАПКА ОТКРЫТА ТАК", 3.0f, 0.8f, "∞"},
        {"Новая папка (3)", 1,
         {{"AI_генератор_18+.exe", false, FileKind::Exe, Viewer::Generator, nullptr},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr},
          {nullptr, false, FileKind::Txt, Viewer::Notepad, nullptr}},
         BannerKind::White, 30.0f, "ВЫ ДОШЛИ ДО КОНЦА", 3.0f, -1.2f, "??"},
    };

    // ---- the colours of the levels (spec 3.1): bg chrome side text dim line field sel selb hover, 0xRRGGBB ----
    struct PaletteDef
    {
        unsigned bg, chrome, side, text, dim, line, field, sel, selb, hover;
    };

    inline constexpr PaletteDef kPalettes[kLevelCount] = {
        {0xFFFFFF, 0xF3F3F3, 0xF7F7F7, 0x1B1B1B, 0x5F5F5F, 0xE5E5E5, 0xFFFFFF, 0xCCE8FF, 0x99D1FF, 0xE5F3FF},
        {0xFFF8FB, 0xF7EFF3, 0xFAF2F6, 0x1B1B1B, 0x62575D, 0xEADDE3, 0xFFFFFF, 0xFFD3E4, 0xFF9CC2, 0xFFE8F1},
        {0xFFEEF5, 0xF8E3EC, 0xFBE9F0, 0x1D1418, 0x6A5560, 0xECD2DE, 0xFFF8FB, 0xFFC4DA, 0xFF86B4, 0xFFDEEA},
        {0xFFE2ED, 0xF6D2E0, 0xF9DBE6, 0x22121A, 0x6F4B5C, 0xEBC0D1, 0xFFF2F7, 0xFFB3CF, 0xFF6FA6, 0xFFD0E2},
        {0xFFD0E0, 0xF1BFD2, 0xF6C9D9, 0x26101A, 0x764456, 0xE6ABC2, 0xFFE9F1, 0xFF9FC1, 0xFF5A97, 0xFFBFD5},
        {0xFFB8CF, 0xEC9FBB, 0xF2AAC4, 0x2A0D18, 0x7B3550, 0xE290B0, 0xFFDDE9, 0xFF86B0, 0xFF3F85, 0xFFA8C6},
        {0xFF97B6, 0xE57D9D, 0xED88A8, 0x2B0010, 0x7A1F40, 0xDA6F93, 0xFFCADB, 0xFF6A9B, 0xFF2D73, 0xFF86AB},
        // Levels 8-10 are a little deeper than the mockups (#FF6688, #EC2149, #D10F37): white text on those has the contrast 3.4, 4.3 and 5.5, and the rule of the
        // spec is 4.5. The selection and the hover are dark here too, so that the white text stays readable on them
        {0xD6335E, 0xB62B50, 0xC32E55, 0xFFFFFF, 0xFFE1EA, 0xEF6A88, 0xFFB3C6, 0xA8244A, 0xFFFFFF, 0xC42B55},
        {0xBE1E48, 0x9E1539, 0xAA1840, 0xFFFFFF, 0xFFD2DC, 0xE5456B, 0xFF97AC, 0x8C1033, 0xFFFFFF, 0xA5163F},
        {0xA30D2E, 0x840A25, 0x900B28, 0xFFFFFF, 0xFFC9D5, 0xD13354, 0xFF8AA1, 0x780820, 0xFFFFFF, 0x8E0A27},
    };

    // ---- the gags ----
    inline constexpr const char* kCaptchaLine1 = "Выберите все изображения, где есть";
    inline constexpr const char* kCaptchaLine2 = "голые полигоны";
    inline constexpr const char* kCaptchaLine3 = "Если таких нет, нажмите «Пропустить»";
    inline constexpr const char* kCaptchaConfirm = "ПОДТВЕРДИТЬ";
    inline constexpr const char* kCaptchaSkip = "ПРОПУСТИТЬ";
    inline constexpr std::array<const char*, 9> kCaptchaShapes = {"куб", "сфера", "чайник", "конус", "тор", "цилиндр", "пирамида", "куб", "сфера"};

    inline constexpr const char* kPaywallChip = "ПРЕМИУМ";
    inline constexpr const char* kPaywallTitle = "Папка «Мама, не открывай» доступна по подписке";
    inline constexpr const char* kPaywallPrice = "299 ₽";
    inline constexpr const char* kPaywallPer = "/ мес";
    inline constexpr std::array<const char*, 3> kPaywallGood = {"Полигоны без текстур в 4K", "Шейдеры без цензуры", "Скрипты на Python (горячие)"};
    inline constexpr const char* kPaywallBad = "Отписаться нельзя. Как и от дедлайнов";
    inline constexpr const char* kPaywallButton = "Оплатить зачётом по ЛР4";
    inline constexpr const char* kPaywallWorking = "Списываем зачёт…";
    inline constexpr const char* kPaywallFine = "Нажимая кнопку, вы соглашаетесь, что преподаватель будет уведомлён. Других способов оплаты нет. Мы проверяли.";

    inline constexpr const char* kIvanTitle = "Иван, это не для тебя";
    inline constexpr const char* kIvanQuestion = "Вы точно не Иван?";
    inline constexpr const char* kIvanNo = "Я не Иван";
    inline constexpr const char* kIvanYes = "Я Иван";
    inline constexpr const char* kIvanCaught = "Иван, мы же договаривались. Ладно, только никому.";
    inline constexpr const char* kIvanNobody = "Никому";

    inline constexpr const char* kAgeSign = "18+";
    inline constexpr const char* kAgeWarning = "ПОСЛЕДНЕЕ ПРЕДУПРЕЖДЕНИЕ";
    inline constexpr const char* kAgeConfirm = "Подтвердите, что вам есть 18 лет";
    inline constexpr const char* kAgeYes = "Мне есть 18";
    inline constexpr const char* kAgeNo = "Мне нет 18";
    inline constexpr const char* kAgeNoPressed = "Тогда тем более интересно. Проходи.";
    inline constexpr const char* kAgeFaceIdFine = "Face ID · кадры не сохраняются · камера выключится сама";
    inline constexpr const char* kAgeRecognized = "%s, вам 3 курс… ладно, проходи";
    inline constexpr const char* kAgeOther = "%s, вам точно есть 18? Ладно, проходи";
    inline constexpr const char* kAgeUnknown = "Лицо не узнано. Но выглядите взрослым.";

    inline constexpr const char* kLoadTitle = "видео.mp4 — Кино и ТВ";
    inline constexpr const char* kLoadLoading = "Загрузка… 99%";
    inline constexpr const char* kLoadLeft = "осталось примерно 2 года";
    inline constexpr const char* kLoadErrorTitle = "Недостаточно видеопамяти.";
    inline constexpr const char* kLoadErrorBody = "Для просмотра требуется 69 ГБ VRAM. Купите RTX 9090.";
    inline constexpr const char* kLoadBuy = "Купить (нет денег)";
    inline constexpr const char* kLoadOk = "ОК, открыть так";

    inline constexpr const char* kUacTitle = "Контроль учётных записей";
    inline constexpr const char* kUacQuestion = "Разрешить этому приложению вносить изменения в вашу сцену?";
    inline constexpr const char* kUacFile = "AI_генератор_18+.exe";
    inline constexpr const char* kUacPublisher = "Издатель: не проверен (Иван)";
    inline constexpr const char* kUacSource = "Источник: Новая папка (3)";
    inline constexpr const char* kUacYes = "Да";
    inline constexpr const char* kUacNo = "Нет (тоже да)";

    inline constexpr const char* kNotepadMenu = "Файл   Правка   Формат   Вид   Справка";
    inline constexpr const char* kNotepadSuffix = " — Блокнот";
    inline constexpr const char* kViewerSuffix = " — Просмотр";

    // ---- the generator ----
    inline constexpr const char* kGenTitle = "AI-ГЕНЕРАТОР 18+";
    inline constexpr const char* kGenPremium = "ПРЕМИУМ";
    inline constexpr const char* kGenModelClaude = "модель: Claude · горячая";
    inline constexpr const char* kGenModelLocal = "модель: локальная (без интернета)";
    inline constexpr const char* kGenIncognito = "инкогнито";
    inline constexpr const char* kGenNew = "Новый запрос";
    inline constexpr std::array<const char*, 6> kGenHistory = {"Хентай про DirectX 12", "Фури, продолжение (14)", "Что-нибудь пожёстче",
                                                                "Щупальца, но в 4K", "Голые полигоны HD", "Не показывать маме"};
    inline constexpr const char* kGenHistoryFine = "История очищается при закрытии. Шутка. Не очищается.";
    inline constexpr const char* kGenHistoryToast = "Этот чат удалён. Мы же договаривались.";
    inline constexpr const char* kGenGreeting = "Снова ты";
    inline constexpr const char* kGenQuestion = "Какое порно или хентай сгенерируем на этот раз?";
    inline constexpr std::array<const char*, 4> kGenChips = {"Горячее", "Щупальца", "Фури", "Хардкор"};
    inline constexpr const char* kGenEmptyFine = "Всё сгенерированное появится в открытой сцене. Маме не покажется.";
    inline constexpr const char* kGenPlaceholder = "Опиши, что сгенерировать… Мы никому не скажем";
    inline constexpr const char* kGenSend = "Сгенерировать";
    inline constexpr const char* kGenStop = "Стоп";
    inline constexpr const char* kGenWorking = "Генерирую…";
    inline constexpr std::array<const char*, 3> kGenWhispers = {"Не закрывай вкладку. И не оглядывайся.", "Мама идёт? Жми Alt+Tab.", "Тсс…"};
    inline constexpr std::array<int, 8> kGenPercents = {3, 12, 37, 37, 37, 69, 88, 99};
    inline constexpr const char* kGenCensorTitle = "Только для взрослых";
    inline constexpr const char* kGenCensorHint = "Нажми, чтобы посмотреть";
    inline constexpr const char* kGenWatch = "Смотреть";
    inline constexpr const char* kGenNotNow = "Не сейчас";
    inline constexpr const char* kGenPlay = "Играть";
    inline constexpr const char* kGenNotNowReply = "Как скажешь. Потом посмотришь.";
    inline constexpr const char* kGenApplied = "применено: 1 сцена · Ctrl+Z отменит";
    inline constexpr const char* kGenSaveTitle = "Сохранить текущую сцену?";
    inline constexpr const char* kGenSaveBody = "В открытой сцене есть несохранённые изменения. Игра появится вместо неё.";
    inline constexpr const char* kGenSaveAndWatch = "Сохранить и смотреть";
    inline constexpr const char* kGenWatchNoSave = "Смотреть без сохранения";
    inline constexpr const char* kGenCancel = "Отмена";
    inline constexpr const char* kGenBack = "Назад в проводник";
    inline constexpr const char* kGenError = "Не вышло. Попробуй ещё раз, мы никому не скажем.";
    inline constexpr const char* kGenSceneMissing = "Заготовка сцены не найдена. Запусти редактор из папки проекта.";
    inline constexpr const char* kGenStopped = "Остановлено.";

    // ---- the fallback: the words that pick a scene (the matcher makes them lower case and turns the letter yo into ye) ----
    inline constexpr std::array<const char*, 3> kOctopusWords = {"щупальц", "осьмин", "хентай"};
    inline constexpr std::array<const char*, 2> kFoxWords = {"фури", "лис"};
    inline constexpr std::array<const char*, 4> kLavaWords = {"горяч", "лава", "огонь", "огня"};
    inline constexpr std::array<const char*, 3> kHardcoreWords = {"жёстк", "хард", "жестк"};
    inline constexpr std::array<const char*, 1> kNakedWords = {"голы"};
    inline constexpr std::array<const char*, 25> kRefusalWords = {
        "дети", "детьми", "детей", "детям", "детск", "ребён", "малолет", "школьн", "несовершеннолет", "младен", "подрост",
        "путин", "трамп", "байден", "обам", "зеленск", "навальн", "знаменитост",
        "убий", "изнасил", "насили", "пытк", "расчлен", "задуш", "суицид"};

    inline constexpr const char* kAnswerOctopus = "Готово 😏 Огромный осьминог, восемь щупалец, ты в главной роли. Увернись — и набери очки.";
    inline constexpr const char* kAnswerFox = "Фури доставлено 🦊 Рыжая, пушистая, собирает монетки. Всё как ты любишь.";
    inline constexpr const char* kAnswerLava = "Горячее некуда 🔥 Пол — лава, с неба шары. Не обожгись.";
    inline constexpr const char* kAnswerHardcore = "Жёстко, как просил 💀 Одно касание — и всё. Без сохранений.";
    inline constexpr const char* kAnswerNaked = "Совсем голые 🧊 Машинки без кузова. Только колёса и стыд.";
    inline constexpr const char* kAnswerShrug = "Не совсем понял, но сделал 😏 Сам разберёшься, что тут возбуждает.";
    inline constexpr const char* kAnswerRefusal = "Не, такое не генерирую 🙅 Держи лучше вот это:";
    inline constexpr std::array<const char*, 3> kClosings = {"Жми Play. Маме скажешь, что это курсовая.", "Жми Play. Если кто войдёт — это ЛР4.", "Жми Play. История браузера чиста."};

    // The tool steps under the progress bar ("builds" the ready scene). The first line is the phrase of the generator, the others are the tool calls
    inline const std::vector<const char*>& SceneSteps(const MiniScene scene)
    {
        static const std::vector<const char*> octopus = {"ищу щупальца в assets/prefabs…", "create_entity(\"Осьминог_босс\")", "spawn_prefab(\"chaser\", \"Щупальце\") x8",
                                                         "attach_script(\"Счёт\")"};
        static const std::vector<const char*> fox = {"ищу рыжую в assets/models…", "create_entity(\"Лисичка\")", "spawn_prefab(\"coin\") x10", "attach_script(\"Счёт\")"};
        static const std::vector<const char*> lava = {"нагреваю пол в assets/materials…", "create_entity(\"Лава\")", "attach_script(\"prank_fireballs\")",
                                                      "set_material(\"Лава\", \"warm\")"};
        static const std::vector<const char*> hardcore = {"отключаю сохранения…", "create_entity(\"Хардкор\")", "attach_script(\"prank_hardcore\")", "set_property(\"жизней\", 1)"};
        static const std::vector<const char*> naked = {"снимаю кузова с машинок…", "create_entity(\"Колесо_игрок\")", "create_entity(\"Мяч\")", "attach_script(\"prank_football\")"};
        static const std::vector<const char*> empty;
        switch (scene)
        {
            case MiniScene::Octopus: return octopus;
            case MiniScene::Fox: return fox;
            case MiniScene::Lava: return lava;
            case MiniScene::Hardcore: return hardcore;
            case MiniScene::NakedCars: return naked;
            case MiniScene::Count: break;
        }
        return empty;
    }
}
