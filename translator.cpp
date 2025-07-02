#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")

// --- Глобальные переменные ---
HMODULE g_hModule = nullptr; // Хэндл нашей DLL
std::unordered_map<std::string, std::wstring> g_translations; // Хранилище переводов (ключ - UTF-8, значение - UTF-16)

// Указатель на оригинальную функцию DrawTextW
// Мы будем вызывать ее через "трамплин"
typedef HRESULT(WINAPI* DrawTextW_t)(LPD3DXFONT, HDC, LPCWSTR, INT, LPRECT, DWORD, D3DCOLOR);
DrawTextW_t oDrawTextW = nullptr;

// --- Реализация простого хука (замена MinHook) ---

// Эта структура будет хранить информацию о нашем хуке
struct HookInfo {
    BYTE originalBytes[5] = {0}; // Сохраняем первые 5 байт оригинальной функции
    void* targetAddress = nullptr;
};
HookInfo drawTextHookInfo;

// Устанавливает хук на функцию
// pTarget: адрес функции, которую мы хотим перехватить
// pDetour: адрес нашей функции, на которую будет перенаправлен вызов
// ppOriginal: сюда будет записан адрес "трамплина" для вызова оригинальной функции
bool InstallHook(void* pTarget, void* pDetour, void** ppOriginal) {
    if (!pTarget) return false;

    // Сохраняем адрес цели для последующего снятия хука
    drawTextHookInfo.targetAddress = pTarget;

    // Выделяем память для "трамплина". Трамплин - это небольшой кусок кода, который
    // выполняет перезаписанные нами байты и затем прыгает обратно в оригинальную функцию.
    // Это позволяет нам вызывать оригинальную функцию из нашего хука.
    void* trampoline = VirtualAlloc(NULL, 11, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return false;

    // Получаем разрешение на запись в память, где находится целевая функция
    DWORD oldProtect;
    if (!VirtualProtect(pTarget, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    // Сохраняем оригинальные 5 байт
    memcpy(drawTextHookInfo.originalBytes, pTarget, 5);

    // --- Создаем трамплин ---
    // 1. Копируем оригинальные байты в трамплин
    memcpy(trampoline, drawTextHookInfo.originalBytes, 5);
    // 2. Добавляем JMP инструкцию, которая прыгает обратно в оригинальную функцию,
    //    пропуская те 5 байт, что мы скопировали.
    uintptr_t jumpBackAddr = (uintptr_t)pTarget + 5;
    BYTE* trampoline_ptr = (BYTE*)trampoline;
    trampoline_ptr[5] = 0xE9; // JMP opcode
    *(uintptr_t*)(trampoline_ptr + 6) = jumpBackAddr - (uintptr_t)trampoline - 11;

    // --- Устанавливаем хук ---
    // Создаем JMP инструкцию, которая перенаправляет вызовы из оригинальной функции в нашу.
    // Формула для относительного JMP: адрес_цели - адрес_источника - 5
    uintptr_t relativeAddr = (uintptr_t)pDetour - (uintptr_t)pTarget - 5;
    BYTE patch[5] = {0xE9, 0x00, 0x00, 0x00, 0x00}; // 0xE9 = JMP
    memcpy(patch + 1, &relativeAddr, 4);

    // Записываем наш JMP в начало целевой функции
    memcpy(pTarget, patch, 5);

    // Возвращаем старые права на память
    VirtualProtect(pTarget, 5, oldProtect, &oldProtect);
    
    // Сохраняем указатель на трамплин, чтобы мы могли вызывать его
    *ppOriginal = trampoline;
    return true;
}

// Снимает установленный хук
void UninstallHook() {
    if (drawTextHookInfo.targetAddress) {
        DWORD oldProtect;
        VirtualProtect(drawTextHookInfo.targetAddress, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        // Восстанавливаем оригинальные байты
        memcpy(drawTextHookInfo.targetAddress, drawTextHookInfo.originalBytes, 5);
        VirtualProtect(drawTextHookInfo.targetAddress, 5, oldProtect, &oldProtect);

        // Освобождаем память трамплина
        if (oDrawTextW) {
             VirtualFree(oDrawTextW, 0, MEM_RELEASE);
        }
    }
}


// --- Функции для перевода (замена JSON) ---

// Конвертирует строку из UTF-8 в UTF-16 (wstring)
std::wstring Utf8ToUtf16(const std::string& utf8Str) {
    if (utf8Str.empty()) return std::wstring();
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), (int)utf8Str.size(), NULL, 0);
    std::wstring utf16Str(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), (int)utf8Str.size(), &utf16Str[0], sizeNeeded);
    return utf16Str;
}

// Конвертирует строку из UTF-16 (wstring) в UTF-8
std::string Utf16ToUtf8(const std::wstring& utf16Str) {
    if (utf16Str.empty()) return std::string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, utf16Str.c_str(), (int)utf16Str.size(), NULL, 0, NULL, NULL);
    std::string utf8Str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16Str.c_str(), (int)utf16Str.size(), &utf8Str[0], sizeNeeded, NULL, NULL);
    return utf8Str;
}

// Загружает переводы из файла translations.txt
void LoadTranslations() {
    std::ifstream file("translations.txt");
    if (!file.is_open()) {
        // Можно вывести сообщение об ошибке, если нужно для отладки
        // MessageBox(NULL, L"Не удалось открыть translations.txt", L"Ошибка", MB_OK);
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Пропускаем пустые строки или комментарии (начинающиеся с #)
        if (line.empty() || line[0] == '#') {
            continue;
        }

        size_t separatorPos = line.find('=');
        if (separatorPos != std::string::npos) {
            std::string key = line.substr(0, separatorPos);
            std::string value = line.substr(separatorPos + 1);
            
            // Сохраняем ключ как UTF-8, а значение сразу конвертируем в UTF-16 для D3DX
            g_translations[key] = Utf8ToUtf16(value);
        }
    }
    file.close();
}


// --- Наш хук для функции DrawTextW ---

HRESULT WINAPI HookedDrawTextW(LPD3DXFONT pFont, HDC hdc, LPCWSTR pString, INT cchText, LPRECT pRect, DWORD dwFlags, D3DCOLOR color) {
    if (pString) {
        // Конвертируем оригинальный текст (UTF-16) в UTF-8, чтобы использовать как ключ
        std::string originalTextUtf8 = Utf16ToUtf8(pString);

        // Ищем перевод в нашей карте
        auto it = g_translations.find(originalTextUtf8);
        if (it != g_translations.end()) {
            // Если перевод найден, используем его
            const std::wstring& translatedText = it->second;
            // Вызываем оригинальную функцию DrawTextW с переведенным текстом
            return oDrawTextW(pFont, hdc, translatedText.c_str(), -1, pRect, dwFlags, color);
        }
    }

    // Если перевода нет, просто вызываем оригинальную функцию с оригинальными параметрами
    return oDrawTextW(pFont, hdc, pString, cchText, pRect, dwFlags, color);
}


// --- Установка и инициализация ---

DWORD WINAPI InitializationThread(LPVOID lpParam) {
    // Ждем, пока игра загрузит библиотеку d3dx9_43.dll (или другую версию)
    HMODULE hD3dx = NULL;
    while (!hD3dx) {
        hD3dx = GetModuleHandle(L"d3dx9_43.dll"); // Убедитесь, что игра использует именно эту версию!
        if (!hD3dx) hD3dx = GetModuleHandle(L"d3dx9_42.dll"); // Пробуем другие популярные версии
        // ... можно добавить и другие версии
        Sleep(100);
    }

    // Находим адрес функции D3DXCreateFontW, чтобы получить указатель на vtable (виртуальную таблицу методов)
    // Метод DrawTextW находится по индексу 15 в таблице D3DXFont.
    // Это более сложный, но надежный способ, чем искать саму DrawTextW по имени.
    // Однако для простоты примера мы будем искать DrawTextW напрямую, если она экспортируется.
    
    // Способ 1: Прямой поиск по имени (менее надежный, но простой)
    void* drawTextAddr = (void*)GetProcAddress(hD3dx, "D3DXFont_DrawTextW");
    if (!drawTextAddr) {
         // Если не получилось, можно попробовать более сложные методы (например, поиск по сигнатуре)
         // MessageBox(NULL, L"Не удалось найти адрес D3DXFont_DrawTextW", L"Ошибка хука", MB_OK);
         return 1;
    }
    
    // Загружаем наши переводы
    LoadTranslations();

    // Устанавливаем хук
    if (InstallHook(drawTextAddr, HookedDrawTextW, (void**)&oDrawTextW)) {
        // Успех!
        // MessageBox(NULL, L"Хук успешно установлен!", L"Успех", MB_OK);
    } else {
        // Ошибка
        // MessageBox(NULL, L"Не удалось установить хук.", L"Ошибка хука", MB_OK);
    }
    
    return 0;
}


// --- Точка входа DLL ---

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            g_hModule = hModule;
            // Отключаем уведомления о создании/завершении потоков, чтобы избежать лишних вызовов DllMain
            DisableThreadLibraryCalls(hModule); 
            // Создаем отдельный поток для инициализации, чтобы не блокировать загрузку игры
            CreateThread(nullptr, 0, InitializationThread, nullptr, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            // При выгрузке DLL снимаем наш хук, чтобы игра не вылетела
            UninstallHook();
            break;
    }
    return TRUE;
}
