import re

files_simple = [
    'completed_todos_window.cpp',
    'stats_window.cpp',
    'ui.cpp',
]

for fname in files_simple:
    with open(fname, 'r', encoding='utf-8') as f:
        c = f.read()
    c = c.replace('FontFamily ff(L"Microsoft YaHei");', 'FontFamily& ff = *g_MiSansFamily;')
    # also remove stale comments about the replacement
    c = c.replace('        // --- 修改点：将 MiSans 替换为 Microsoft YaHei ---\n', '')
    with open(fname, 'w', encoding='utf-8', newline='\n') as f:
        f.write(c)
    print(f'{fname}: done')

# settings_window.cpp - line-by-line
with open('settings_window.cpp', 'r', encoding='utf-8') as f:
    lines = f.readlines()

out = []
i = 0
while i < len(lines):
    line = lines[i]
    if 'FontFamily ff(L"Microsoft YaHei");' in line:
        out.append(line.replace('FontFamily ff(L"Microsoft YaHei");', 'FontFamily& ff = *g_MiSansFamily;'))
        i += 1
        continue
    if 'CreateFontW(S(13)' in line:
        indent = line[:len(line) - len(line.lstrip())]
        j = i + 1
        while j < len(lines):
            if 'Microsoft YaHei' in lines[j]:
                break
            j += 1
        out.append(indent + 's_hFont = GetMiSansFont(13);\n')
        i = j + 1
        continue
    out.append(line)
    i += 1

with open('settings_window.cpp', 'w', encoding='utf-8', newline='\n') as f:
    f.writelines(out)
print('settings_window.cpp: done')

# utils.cpp - GetMiSansFont body
with open('utils.cpp', 'r', encoding='utf-8') as f:
    c = f.read()
# replace the old single-line CreateFontW return with the new multi-line version
old = 'HFONT GetMiSansFont(int s) {\n    return CreateFontW(S(s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");\n}'
new = '''HFONT GetMiSansFont(int s) {
    static std::wstring fontPath;
    static bool fontRegistered = false;
    static bool fontAvailable  = false;
    if (!fontRegistered) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        PathAppendW(exePath, L"MiSans-Regular.ttf");
        fontPath = exePath;
        DWORD cnt = 0;
        fontAvailable  = (AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) != 0);
        fontRegistered = true;
    }
    const wchar_t* faceName = fontAvailable ? L"MiSans" : L"Microsoft YaHei";
    return CreateFontW(S(s), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, faceName);
}'''
if old in c:
    c = c.replace(old, new)
    print('utils.cpp: replaced old GetMiSansFont')
else:
    # already modified - just ensure faceName logic is present
    print('utils.cpp: old pattern not found, check manually')
with open('utils.cpp', 'w', encoding='utf-8', newline='\n') as f:
    f.write(c)

# main.cpp - fix Clone() and remove duplicate g_MiSansFamily definition
with open('main.cpp', 'r', encoding='utf-8') as f:
    c = f.read()
# Fix: families[0].Clone(g_MiSansFamily) -> g_MiSansFamily = families[0].Clone()
c = c.replace(
    'g_MiSansFamily = new Gdiplus::FontFamily();\n                families[0].Clone(g_MiSansFamily);',
    'g_MiSansFamily = families[0].Clone();'
)
with open('main.cpp', 'w', encoding='utf-8', newline='\n') as f:
    f.write(c)
print('main.cpp: done')

# Verify all
for fname in ['settings_window.cpp', 'utils.cpp', 'main.cpp', 'completed_todos_window.cpp', 'stats_window.cpp', 'ui.cpp']:
    with open(fname, 'r', encoding='utf-8') as f:
        c = f.read()
    yh = c.count('Microsoft YaHei')
    ms = c.count('g_MiSansFamily') + c.count('GetMiSansFont')
    print(f'{fname}: YaHei={yh}, MiSans refs={ms}')

